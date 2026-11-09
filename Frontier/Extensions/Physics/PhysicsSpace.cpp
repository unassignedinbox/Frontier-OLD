/*==========================================================================================================================================
                                                              PHYSICSSPACE.CPP
==========================================================================================================================================*/
// 🧩 The PhysicsSpace on Jolt: one Jolt solver with the extension's layers, box and sphere bodies, contact tally and stepping.

#include "Frontier/Extensions/Physics/PhysicsSpace.h"

#include "Frontier/Extensions/Physics/JoltExtension.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace Frontier
{

namespace
{

// 📝 Counts the contacts Jolt adds (callbacks arrive on the job threads).
class ContactTally final : public JPH::ContactListener
{
  public:
    void OnContactAdded(const JPH::Body&, const JPH::Body&, const JPH::ContactManifold&, JPH::ContactSettings&) override
    {
        Added.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> Added{ 0 };
};

JPH::Vec3 ToVector(const std::array<double, 3>& Value)
{
    return JPH::Vec3(static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2]));
}

} // namespace

struct PhysicsSpace::Implementation
{
    JoltExtension          Backend;
    JPH::PhysicsSystem     Solver;
    ContactTally           Contacts;
    std::vector<JPH::BodyID> Bodies;

    explicit Implementation(uint32_t Workers) : Backend(Workers) {}
};

PhysicsSpace::PhysicsSpace(const PhysicsSpaceSettings& Settings) : Detail(std::make_unique<Implementation>(Settings.Workers))
{
    const JPH::uint Bodies = std::max<JPH::uint>(16, Settings.MaximumBodies);
    Detail->Solver.Init(Bodies, 0, Bodies, Bodies, Detail->Backend.QueryLayers(), Detail->Backend.QueryLayerFilter(), Detail->Backend.QueryPairFilter());
    Detail->Solver.SetGravity(ToVector(Settings.GravityMps2));
    Detail->Solver.SetContactListener(&Detail->Contacts);
}

PhysicsSpace::~PhysicsSpace()
{
    JPH::BodyInterface& Interface = Detail->Solver.GetBodyInterface();
    for (const JPH::BodyID& Body : Detail->Bodies)
    {
        Interface.RemoveBody(Body);
        Interface.DestroyBody(Body);
    }
}

uint32_t PhysicsSpace::AddBody(const PhysicsBodyDescriptor& Descriptor)
{
    constexpr uint32_t Refused = std::numeric_limits<uint32_t>::max();
    JPH::RefConst<JPH::Shape> Shape;
    if (Descriptor.Shape == PhysicsShape::Sphere)
    {
        if (!(Descriptor.HalfExtentMetres[0] > 1.0e-3)) return Refused;
        Shape = new JPH::SphereShape(static_cast<float>(Descriptor.HalfExtentMetres[0]));
    }
    else
    {
        const JPH::Vec3 Half = ToVector(Descriptor.HalfExtentMetres);
        if (!(Half.ReduceMin() > 1.0e-3f)) return Refused;
        // 📝 The convex radius rounds the box's edges; it must stay below half the smallest side.
        Shape = new JPH::BoxShape(Half, std::min(JPH::cDefaultConvexRadius, 0.5f * Half.ReduceMin()));
    }
    const bool                Moving = Descriptor.Motion == PhysicsMotion::Moving;
    JPH::BodyCreationSettings Settings(Shape, JPH::RVec3(ToVector(Descriptor.PositionMetres)),
                                       JPH::Quat(static_cast<float>(Descriptor.Orientation[0]), static_cast<float>(Descriptor.Orientation[1]),
                                                 static_cast<float>(Descriptor.Orientation[2]), static_cast<float>(Descriptor.Orientation[3]))
                                           .Normalized(),
                                       Moving ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, Moving ? JoltLayers::Moving : JoltLayers::Fixed);
    Settings.mFriction       = static_cast<float>(Descriptor.Friction);
    Settings.mRestitution    = static_cast<float>(Descriptor.Restitution);
    Settings.mLinearDamping  = static_cast<float>(Descriptor.LinearDamping);
    Settings.mUserData       = Descriptor.Record;
    if (Moving)
    {
        Settings.mLinearVelocity  = ToVector(Descriptor.LinearVelocityMps);
        Settings.mAngularVelocity = ToVector(Descriptor.AngularVelocityRads);
        if (Descriptor.MassKilograms > 0.0)
        {
            Settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
            Settings.mMassPropertiesOverride.mMass = static_cast<float>(Descriptor.MassKilograms);
        }
    }
    JPH::BodyInterface& Interface = Detail->Solver.GetBodyInterface();
    const JPH::BodyID   Body      = Interface.CreateAndAddBody(Settings, Moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (Body.IsInvalid()) return Refused;
    Detail->Bodies.push_back(Body);
    Descriptors.push_back(Descriptor);
    return static_cast<uint32_t>(Descriptors.size() - 1);
}

void PhysicsSpace::Prepare()
{
    Detail->Solver.OptimizeBroadPhase();
}

void PhysicsSpace::Step(double DeltaSeconds, uint32_t CollisionSteps)
{
    if (!(DeltaSeconds > 0.0)) return;
    (void)Detail->Solver.Update(static_cast<float>(DeltaSeconds), static_cast<int>(std::max(1u, CollisionSteps)), &Detail->Backend.AccessTemporary(),
                                &Detail->Backend.AccessJobs());
    SimulatedSeconds += DeltaSeconds;
}

PhysicsBodyReading PhysicsSpace::ReadBody(uint32_t Index) const
{
    PhysicsBodyReading Reading;
    if (Index >= Detail->Bodies.size()) return Reading;
    const JPH::BodyInterface& Interface = Detail->Solver.GetBodyInterfaceNoLock();
    const JPH::BodyID         Body      = Detail->Bodies[Index];
    JPH::RVec3                Position;
    JPH::Quat                 Rotation;
    Interface.GetPositionAndRotation(Body, Position, Rotation);
    const JPH::Vec3 Linear  = Interface.GetLinearVelocity(Body);
    const JPH::Vec3 Angular = Interface.GetAngularVelocity(Body);
    Reading.PositionMetres      = { Position.GetX(), Position.GetY(), Position.GetZ() };
    Reading.Orientation         = { Rotation.GetX(), Rotation.GetY(), Rotation.GetZ(), Rotation.GetW() };
    Reading.LinearVelocityMps   = { Linear.GetX(), Linear.GetY(), Linear.GetZ() };
    Reading.AngularVelocityRads = { Angular.GetX(), Angular.GetY(), Angular.GetZ() };
    Reading.Awake               = Interface.IsActive(Body);
    return Reading;
}

size_t PhysicsSpace::QueryAwakeCount() const
{
    return Detail->Solver.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

uint64_t PhysicsSpace::QueryContactsAdded() const noexcept
{
    return Detail->Contacts.Added.load(std::memory_order_relaxed);
}

uint32_t PhysicsSpace::QueryWorkers() const noexcept
{
    return Detail->Backend.QueryWorkers();
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    ORIENTATION
//------------------------------------------------------------------------------------------------------------------------

std::array<double, 4> ResolvePhysicsOrientation(const std::array<double, 3>& OrientationDegrees) noexcept
{
    // 📝 q = qz(γ) · qy(β) · qx(α), half angles.
    constexpr double Radians = 3.14159265358979323846 / 360.0;
    const double     Cx = std::cos(OrientationDegrees[0] * Radians), Sx = std::sin(OrientationDegrees[0] * Radians);
    const double     Cy = std::cos(OrientationDegrees[1] * Radians), Sy = std::sin(OrientationDegrees[1] * Radians);
    const double     Cz = std::cos(OrientationDegrees[2] * Radians), Sz = std::sin(OrientationDegrees[2] * Radians);
    return { Sx * Cy * Cz - Cx * Sy * Sz, Cx * Sy * Cz + Sx * Cy * Sz, Cx * Cy * Sz - Sx * Sy * Cz, Cx * Cy * Cz + Sx * Sy * Sz };
}

std::array<double, 3> ResolvePhysicsEulerDegrees(const std::array<double, 4>& Q) noexcept
{
    // 📝 From the rotation's columns: β = asin(−R₂₀), α = atan2(R₂₁, R₂₂), γ = atan2(R₁₀, R₀₀).
    constexpr double Degrees = 180.0 / 3.14159265358979323846;
    const double     X = Q[0], Y = Q[1], Z = Q[2], W = Q[3];
    const double     R00 = 1.0 - 2.0 * (Y * Y + Z * Z), R10 = 2.0 * (X * Y + Z * W), R20 = 2.0 * (X * Z - Y * W);
    const double     R21 = 2.0 * (Y * Z + X * W), R22 = 1.0 - 2.0 * (X * X + Y * Y);
    return { std::atan2(R21, R22) * Degrees, std::asin(std::clamp(-R20, -1.0, 1.0)) * Degrees, std::atan2(R10, R00) * Degrees };
}

std::array<double, 3> RotatePhysicsVector(const std::array<double, 4>& Q, const std::array<double, 3>& V) noexcept
{
    // 📝 v' = v + 2 w (q × v) + 2 q × (q × v)
    const double Tx = 2.0 * (Q[1] * V[2] - Q[2] * V[1]), Ty = 2.0 * (Q[2] * V[0] - Q[0] * V[2]), Tz = 2.0 * (Q[0] * V[1] - Q[1] * V[0]);
    return { V[0] + Q[3] * Tx + (Q[1] * Tz - Q[2] * Ty), V[1] + Q[3] * Ty + (Q[2] * Tx - Q[0] * Tz), V[2] + Q[3] * Tz + (Q[0] * Ty - Q[1] * Tx) };
}

} // namespace Frontier
