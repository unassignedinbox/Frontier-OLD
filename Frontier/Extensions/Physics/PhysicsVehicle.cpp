/*==========================================================================================================================================
                                                            PHYSICSVEHICLE.CPP
==========================================================================================================================================*/
// 🧩 The chassis body and its hull, the solver's hooks (chassis, forces, torque, ground rays, parallel carcasses), the step, the
//    reset and the view the editor draws.

#include "Frontier/Extensions/Physics/PhysicsVehicle.h"

#include <algorithm>
#include <cmath>

namespace Frontier
{

namespace
{

using Vehicle::Quat;
using Vehicle::Vec3;

std::array<double, 3> ToArray(const Vec3& Value)
{
    return { Value.x, Value.y, Value.z };
}

Vec3 ToVec(const std::array<double, 3>& Value)
{
    return Vec3{ static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2]) };
}

// 📝 Column-major world-from-local for a position and a rotation.
std::array<float, 16> Compose(const Vec3& Position, const Quat& Rotation)
{
    const Vec3 X = Rotation.Rotate({ 1.0f, 0.0f, 0.0f }), Y = Rotation.Rotate({ 0.0f, 1.0f, 0.0f }), Z = Rotation.Rotate({ 0.0f, 0.0f, 1.0f });
    return { X.x, X.y, X.z, 0.0f, Y.x, Y.y, Y.z, 0.0f, Z.x, Z.y, Z.z, 0.0f, Position.x, Position.y, Position.z, 1.0f };
}

} // namespace

PhysicsVehicle::PhysicsVehicle()  = default;
PhysicsVehicle::~PhysicsVehicle() = default;

bool PhysicsVehicle::Construct(PhysicsSpace& InSpace, RecordToken InRecord, const Vehicle::VehicleSpecification& Specification,
                               const Vehicle::VehicleSolverConfiguration& InConfiguration, const std::array<double, 3>& StartMetres,
                               double StartYawDegrees, std::string* Error, PhysicsVehiclePlacementOptions Placement)
{
    Space                   = &InSpace;
    Record                  = InRecord;
    Configuration           = InConfiguration;
    PlacementOptions        = Placement;
    AuthoredStartMetres     = StartMetres;
    AuthoredStartYawDegrees = StartYawDegrees;
    const Vehicle::VehicleGeometry& Geometry = Specification.Geometry;
    AuthoredCoMHeight = Geometry.CoMHeight;
    OriginBelowCentre = Geometry.CoMAuthoredZ();
    Body              = PhysicsNoBody;

    // ① 4-corner slope-aligned equilibrium pose (or vertical ray drop when EquilibriumPlacement is off)
    Start              = ResolveEquilibriumPose(StartMetres, StartYawDegrees, Geometry.CoMHeight, FittedGroundNormal);
    ParkedBrakeLatched = PlacementOptions.EngageBrakesOnPlacement;

    // ② the chassis hull about the centre of mass: flat floor pan between the axles (0.18 m clearance, ~15° breakover) with
    //    upswept front splitter & rear diffuser overhangs (±2.28 m, 0.36 m clearance, ~22° approach/departure) so kerbs and
    //    12.7° proving-ground ramps clear cleanly without grounding the bumpers, heights from the ground at rest
    struct Corner { float X, Y, Height; };
    const Corner Wedge[] = { { 1.42f, 0.96f, 0.18f },  { 1.42f, -0.96f, 0.18f },  { -1.42f, 0.96f, 0.18f },  { -1.42f, -0.96f, 0.18f },
                             { 2.28f, 0.92f, 0.36f },  { 2.28f, -0.92f, 0.36f },  { -2.28f, 0.92f, 0.36f },  { -2.28f, -0.92f, 0.36f },
                             { 2.28f, 0.92f, 0.62f },  { 2.28f, -0.92f, 0.62f },  { 1.0f, 0.98f, 0.85f },    { 1.0f, -0.98f, 0.85f },
                             { 0.6f, 0.75f, 1.35f },   { 0.6f, -0.75f, 1.35f },   { -1.8f, 0.75f, 1.35f },   { -1.8f, -0.75f, 1.35f },
                             { -2.28f, 0.92f, 0.95f }, { -2.28f, -0.92f, 0.95f } };
    PhysicsBodyDescriptor Chassis;
    Chassis.Record = Record.Value;
    Chassis.VehicleChassis = true;
    Chassis.Shape  = PhysicsShape::Hull;
    for (const Corner& Point : Wedge) Chassis.Points.push_back({ Point.X, Point.Y, Point.Height - Geometry.CoMHeight });
    Chassis.PositionMetres = ToArray(Start.Position);
    Chassis.Orientation    = { Start.Orientation.x, Start.Orientation.y, Start.Orientation.z, Start.Orientation.w };
    Chassis.Motion         = PhysicsMotion::Moving;
    Chassis.MassKilograms  = Configuration.ChassisMass;
    // 📝 The solver predicts the chassis from ChassisMass, InvChassisInertia and gravity alone, so the body carries exactly those
    //    and no damping of its own.
    Chassis.InertiaKgm2    = { 1.0 / std::max(1.0e-9f, Configuration.InvChassisInertia.x), 1.0 / std::max(1.0e-9f, Configuration.InvChassisInertia.y),
                               1.0 / std::max(1.0e-9f, Configuration.InvChassisInertia.z) };
    Chassis.Friction       = 0.6;
    Chassis.Restitution    = 0.1;
    Chassis.LinearDamping  = 0.0;
    Chassis.AngularDamping = 0.0;
    Chassis.Sleeps         = false;
    Chassis.Swept          = true;
    Body = Space->AddBody(Chassis);
    if (Body == PhysicsNoBody)
    {
        if (Error) *Error = "the physics space refused the chassis body";
        return false;
    }
    LastMass = Configuration.ChassisMass;

    // ③ the solver, starting at the chassis' pose at rest
    BuildSolver(Start);
    return Solver && Solver->Constructed();
}

Vehicle::ChassisKinematics PhysicsVehicle::ResolveEquilibriumPose(const std::array<double, 3>& StartMetres, double StartYawDegrees,
                                                                  double FallbackCoMHeight, Vec3& OutNormal) const
{
    const double YawRad = StartYawDegrees * 3.14159265358979323846 / 180.0;
    const Quat   YawOrientation{ 0.0f, 0.0f, static_cast<float>(std::sin(0.5 * YawRad)), static_cast<float>(std::cos(0.5 * YawRad)) };

    Vehicle::ChassisKinematics Pose;
    Pose.Orientation     = YawOrientation;
    Pose.LinearVelocity  = Vec3{};
    Pose.AngularVelocity = Vec3{};
    OutNormal            = Vec3{ 0.0f, 0.0f, 1.0f };

    if (!PlacementOptions.EquilibriumPlacement || Configuration.Wheels.empty())
    {
        double        Ground = StartMetres[2];
        PhysicsRayHit Hit;
        if (Space && Space->CastRay(PhysicsRay{ { StartMetres[0], StartMetres[1], StartMetres[2] + 50.0 }, { 0.0, 0.0, -1.0 }, 100.0, Body }, Hit))
            Ground = Hit.PointMetres[2];
        Pose.Position = Vec3{ static_cast<float>(StartMetres[0]), static_cast<float>(StartMetres[1]),
                              static_cast<float>(Ground + FallbackCoMHeight + 0.01) };
        return Pose;
    }

    // 📝 Cast 4 downward probes at the yaw-rotated wheel mounts to fit the local terrain plane (slope pitch + banking roll).
    const size_t WheelCount = std::min<size_t>(4u, Configuration.Wheels.size());
    const double CosYaw     = std::cos(YawRad);
    const double SinYaw     = std::sin(YawRad);

    std::array<Vec3, 4> HitPoints{};
    std::array<Vec3, 4> HitNormals{};
    std::array<bool, 4> HitValid{};
    size_t              ValidCount = 0u;
    Vec3                Centroid{};
    Vec3                AverageNormal{};

    for (size_t CornerIdx = 0u; CornerIdx < WheelCount; ++CornerIdx)
    {
        const Vehicle::WheelMount& Mount = Configuration.Wheels[CornerIdx];
        const Vec3 LocalAnchor = Configuration.SuspensionEnabled ? Mount.SuspensionMount : Mount.LocalOffset;
        const double WorldX    = StartMetres[0] + static_cast<double>(LocalAnchor.x) * CosYaw - static_cast<double>(LocalAnchor.y) * SinYaw;
        const double WorldY    = StartMetres[1] + static_cast<double>(LocalAnchor.x) * SinYaw + static_cast<double>(LocalAnchor.y) * CosYaw;

        PhysicsRayHit Hit;
        if (Space && Space->CastRay(PhysicsRay{ { WorldX, WorldY, StartMetres[2] + 30.0 }, { 0.0, 0.0, -1.0 }, 70.0, Body }, Hit))
        {
            HitPoints[CornerIdx]  = ToVec(Hit.PointMetres);
            HitNormals[CornerIdx] = ToVec(Hit.Normal).Normalized();
            HitValid[CornerIdx]   = true;
            Centroid += HitPoints[CornerIdx];
            AverageNormal += HitNormals[CornerIdx];
            ++ValidCount;
        }
    }

    if (ValidCount == 0u)
    {
        Pose.Position = Vec3{ static_cast<float>(StartMetres[0]), static_cast<float>(StartMetres[1]),
                              static_cast<float>(StartMetres[2] + FallbackCoMHeight) };
        return Pose;
    }

    Centroid = Centroid * (1.0f / static_cast<float>(ValidCount));
    Vec3 PlaneNormal{ 0.0f, 0.0f, 1.0f };
    if (WheelCount >= 4u && HitValid[0] && HitValid[1] && HitValid[2] && HitValid[3])
    {
        // Symmetric bilinear quad diagonal cross product across FL(0), FR(1), RL(2), RR(3):
        const Vec3 DiagA = HitPoints[1] - HitPoints[2];   // FR - RL
        const Vec3 DiagB = HitPoints[0] - HitPoints[3];   // FL - RR
        Vec3       Cross = Vehicle::Cross(DiagA, DiagB);
        if (Cross.LengthSq() > 1.0e-8f)
        {
            if (Cross.z < 0.0f) Cross = Cross * -1.0f;
            PlaneNormal = Cross.Normalized();
        }
        else if (AverageNormal.LengthSq() > 1.0e-8f)
        {
            PlaneNormal = AverageNormal.Normalized();
        }
    }
    else if (AverageNormal.LengthSq() > 1.0e-8f)
    {
        PlaneNormal = AverageNormal.Normalized();
    }
    if (PlaneNormal.z < 0.0f) PlaneNormal = PlaneNormal * -1.0f;
    OutNormal = PlaneNormal;

    // 📝 Shortest-arc tilt from world +Z onto PlaneNormal composed with StartYawDegrees (zero heading twist).
    const Vec3 WorldUp{ 0.0f, 0.0f, 1.0f };
    const Vec3 TiltAxis  = Vehicle::Cross(WorldUp, PlaneNormal);
    const float TiltW    = 1.0f + Vehicle::Dot(WorldUp, PlaneNormal);
    const Quat TiltQuat  = (TiltW > 1.0e-5f) ? Quat{ TiltAxis.x, TiltAxis.y, TiltAxis.z, TiltW }.Normalized() : Quat{};
    Pose.Orientation     = (TiltQuat * YawOrientation).Normalized();

    // 📝 Static equilibrium CoM position: place hubs at authored StaticCompression sag and tyres at static normal deflection.
    const float CosSlope  = std::max(0.25f, PlaneNormal.z);
    const float RimMass   = std::max(1.0f, Configuration.UnsprungMass - Configuration.Tyre.TotalMass);
    const float TotalLoad = (Configuration.ChassisMass + static_cast<float>(WheelCount) * RimMass) * Configuration.Gravity.Length() * CosSlope;
    const float CornerLoad = TotalLoad / static_cast<float>(WheelCount);

    const float TyreRadius = Configuration.Tyre.Radius;
    Vec3        MeanHubLocal{};
    float       MeanDeflection = 0.0f;
    for (size_t CornerIdx = 0u; CornerIdx < WheelCount; ++CornerIdx)
    {
        MeanHubLocal += Vehicle::ResolveRestingHubLocal(Configuration, static_cast<uint32_t>(CornerIdx));

        const float ColdPa    = (CornerIdx < Configuration.CornerPressures.size()) ? Configuration.CornerPressures[CornerIdx]
                                                                                   : Configuration.Tyre.InflationPressure;
        const float Stiffness = std::max(50000.0f, Vehicle::PressureStiffness(Configuration.TraceContact, ColdPa));
        MeanDeflection += std::clamp(CornerLoad / Stiffness, 0.0f, TyreRadius * 0.25f);
    }
    MeanHubLocal   = MeanHubLocal * (1.0f / static_cast<float>(WheelCount));
    MeanDeflection = MeanDeflection / static_cast<float>(WheelCount);

    // Keep horizontal (StartX, StartY) anchored over the requested station while matching the fitted plane height & normal offset:
    Vec3 GroundAnchor = Centroid;
    if (PlaneNormal.z > 0.25f)
    {
        const float TargetX = static_cast<float>(StartMetres[0]);
        const float TargetY = static_cast<float>(StartMetres[1]);
        const float PlaneZ  = Centroid.z - (PlaneNormal.x * (TargetX - Centroid.x) + PlaneNormal.y * (TargetY - Centroid.y)) / PlaneNormal.z;
        GroundAnchor        = Vec3{ TargetX, TargetY, PlaneZ };
    }
    const Vec3 HubOffsetWorld = Pose.Orientation.Rotate(MeanHubLocal);
    Pose.Position = GroundAnchor + PlaneNormal * (TyreRadius - MeanDeflection) - HubOffsetWorld;
    return Pose;
}

void PhysicsVehicle::BuildSolver(const Vehicle::ChassisKinematics& At)
{
    Solver = std::make_unique<Vehicle::VehicleSolver>();
    Vehicle::VehicleSolver::Hooks Hooks;
    Hooks.ReadChassis       = [this] { return ReadChassis(); };
    Hooks.ApplyForceAtPoint = [this](const Vec3& Force, const Vec3& Point) { Space->ApplyForce(Body, ToArray(Force), ToArray(Point)); };
    Hooks.ApplyTorque       = [this](const Vec3& Torque) { Space->ApplyTorque(Body, ToArray(Torque)); };
    Hooks.Ground            = [this](const Vec3& Point, Vec3& Surface, Vec3& Normal) { return QueryGround(Point, Surface, Normal); };
    Hooks.Dispatch          = [this](uint32_t Count, const std::function<void(uint32_t)>& Task) { Space->Dispatch(Count, Task); };
    Solver->Build(Configuration, Hooks, At);
}

Vehicle::ChassisKinematics PhysicsVehicle::ReadChassis() const
{
    const PhysicsBodyReading Reading = Space->ReadBody(Body);
    Vehicle::ChassisKinematics Chassis;
    Chassis.Position        = ToVec(Reading.PositionMetres);
    Chassis.Orientation     = Quat{ static_cast<float>(Reading.Orientation[0]), static_cast<float>(Reading.Orientation[1]),
                                    static_cast<float>(Reading.Orientation[2]), static_cast<float>(Reading.Orientation[3]) };
    Chassis.LinearVelocity  = ToVec(Reading.LinearVelocityMps);
    Chassis.AngularVelocity = ToVec(Reading.AngularVelocityRads);
    return Chassis;
}

// 📝 The road at a point: the column above and below it (1 m each way), the chassis itself passed through. Other bodies count
//    (a tyre rides over a fallen cone or a ramp).
bool PhysicsVehicle::QueryGround(const Vec3& Point, Vec3& Surface, Vec3& Normal) const
{
    PhysicsRayHit Hit;
    if (!Space->CastRay(PhysicsRay{ { Point.x, Point.y, Point.z + 1.5 }, { 0.0, 0.0, -1.0 }, 3.0, Body }, Hit)) return false;
    Surface = ToVec(Hit.PointMetres);
    Normal  = ToVec(Hit.Normal);
    return true;
}

void PhysicsVehicle::Step(double Seconds)
{
    if (!Solver) return;
    if (ParkedBrakeLatched && Controls.Throttle > 0.02f)
    {
        ParkedBrakeLatched = false;
    }
    Vehicle::DriverInput Input;
    Input.Throttle    = std::clamp(Controls.Throttle, 0.0f, 1.0f);
    Input.Brake       = ParkedBrakeLatched ? 1.0f : std::clamp(Controls.Brake, 0.0f, 1.0f);
    Input.Steer       = std::clamp(Controls.Steer, -1.0f, 1.0f);
    Input.Handbrake   = ParkedBrakeLatched ? true : Controls.Handbrake;
    Input.ClutchPedal = std::clamp(Controls.ClutchPedal, 0.0f, 1.0f);
    Solver->AssignInput(Input);
    Solver->Step(static_cast<float>(Seconds));
    // 📝 Fuel leaves the chassis: the body follows the solver's sprung mass (inertia at the same radii of gyration).
    const float Mass = Solver->Telemetry().ChassisMass;
    if (Mass > 1.0f && std::fabs(Mass - LastMass) > 0.25f)
    {
        const double Scale = static_cast<double>(Mass) / static_cast<double>(Configuration.ChassisMass);
        Space->AssignMass(Body, Mass, { Scale / Configuration.InvChassisInertia.x, Scale / Configuration.InvChassisInertia.y,
                                        Scale / Configuration.InvChassisInertia.z });
        LastMass = Mass;
    }
}

void PhysicsVehicle::Reset()
{
    Start              = ResolveEquilibriumPose(AuthoredStartMetres, AuthoredStartYawDegrees, AuthoredCoMHeight, FittedGroundNormal);
    ParkedBrakeLatched = PlacementOptions.EngageBrakesOnPlacement;
    PhysicsBodyReading Pose;
    Pose.PositionMetres      = ToArray(Start.Position);
    Pose.Orientation         = { Start.Orientation.x, Start.Orientation.y, Start.Orientation.z, Start.Orientation.w };
    Pose.LinearVelocityMps   = { 0.0, 0.0, 0.0 };
    Pose.AngularVelocityRads = { 0.0, 0.0, 0.0 };
    Space->PlaceBody(Body, Pose);
    if (LastMass != Configuration.ChassisMass)
    {
        Space->AssignMass(Body, Configuration.ChassisMass, { 1.0 / Configuration.InvChassisInertia.x, 1.0 / Configuration.InvChassisInertia.y,
                                                             1.0 / Configuration.InvChassisInertia.z });
        LastMass = Configuration.ChassisMass;
    }
    BuildSolver(Start);
}

void PhysicsVehicle::ReadView(PhysicsVehicleView& View) const
{
    View.Record = Record;
    View.Input  = Controls;
    if (!Solver) return;
    View.Telemetry = Solver->Telemetry();
    const Vehicle::ChassisKinematics Chassis = ReadChassis();
    // 📝 The authored model's origin sits OriginBelowCentre under the centre of mass in the body's own axes.
    const Vec3 Origin = Chassis.Position - Chassis.Orientation.Rotate({ 0.0f, 0.0f, static_cast<float>(OriginBelowCentre) });
    View.Body = Compose(Origin, Chassis.Orientation);
    const float Cold = Configuration.Tyre.InflationPressure;
    for (uint32_t Wheel = 0; Wheel < 4; ++Wheel)
    {
        View.FieldValid[Wheel] = false;
        if (Wheel >= View.Telemetry.WheelCount) continue;
        const Vehicle::WheelTelemetry& Reading = View.Telemetry.Wheels[Wheel];
        const Quat Material = (Reading.HubRotation * Quat::AxisAngle(Vec3{ 0.0f, 1.0f, 0.0f }, Reading.SpinAngle)).Normalized();
        View.Wheels[Wheel]  = Compose(Reading.HubPosition, Material);
        View.FieldValid[Wheel] = Solver->SampleCarcassField(Wheel, View.Fields[Wheel]);
        // 📝 The analytic profile's inputs (VehicleVisual.slang): the road's direction in the material axes, deflection, pressure, shear.
        const Vec3 Down = Material.Conjugate().Rotate(Vec3{ -Reading.ContactNormal.x, -Reading.ContactNormal.y, -Reading.ContactNormal.z });
        View.ContactAngle[Wheel]  = std::atan2(Down.z, Down.x);
        View.Deflection[Wheel]    = Reading.InContact ? std::max(0.0f, Reading.TyreDeflection) : 0.0f;
        View.GaugePressure[Wheel] = Reading.Thermal.Valid && Reading.Thermal.GaugePressure > 0.0 ? static_cast<float>(Reading.Thermal.GaugePressure) : Cold;
        View.LateralShift[Wheel]  = std::clamp(Reading.LateralForce / 145000.0f, -0.042f, 0.042f);
    }
}

} // namespace Frontier
