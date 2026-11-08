/*==========================================================================================================================================
                                                              JOLTADAPTER.CPP
==========================================================================================================================================*/
// 🧩 The PhysicsAdapter on Jolt: body shapes and mass, stepping, readings, placement, forces and the nearest-hit ray query.

#include "Frontier/Extensions/Physics/JoltAdapter.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Frontier
{

namespace
{

JPH::Vec3 ToVector(const std::array<double, 3>& Vector)
{
    return JPH::Vec3(static_cast<float>(Vector[0]), static_cast<float>(Vector[1]), static_cast<float>(Vector[2]));
}

JPH::RVec3 ToPosition(const std::array<double, 3>& Position)
{
    return JPH::RVec3(static_cast<JPH::Real>(Position[0]), static_cast<JPH::Real>(Position[1]), static_cast<JPH::Real>(Position[2]));
}

JPH::Quat ToRotation(const std::array<double, 4>& Rotation)
{
    return JPH::Quat(static_cast<float>(Rotation[0]), static_cast<float>(Rotation[1]), static_cast<float>(Rotation[2]), static_cast<float>(Rotation[3]))
        .Normalized();
}

} // namespace

void JoltContactTally::ConfigureBody(uint32_t BodyIndex, uint64_t Record, PhysicsImpactSurface Surface, bool VehicleChassis)
{
    std::lock_guard Lock(Mutex);
    if (Metadata.size() <= BodyIndex) Metadata.resize(static_cast<size_t>(BodyIndex) + 1u);
    Metadata[BodyIndex] = BodyMetadata{ Record, Surface, VehicleChassis };
}

void JoltContactTally::RemoveBody(uint32_t BodyIndex)
{
    std::lock_guard Lock(Mutex);
    if (BodyIndex < Metadata.size()) Metadata[BodyIndex] = {};
}

void JoltContactTally::ClearImpactEvents()
{
    std::lock_guard Lock(Mutex);
    PendingImpacts.clear();
    LastImpactByPair.clear();
    NextImpactSeed = 1;
}

std::vector<PhysicsImpactEvent> JoltContactTally::DrainImpactEvents()
{
    std::lock_guard Lock(Mutex);
    std::vector<PhysicsImpactEvent> Events;
    Events.swap(PendingImpacts);
    return Events;
}

void JoltContactTally::OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold& Manifold,
                                     JPH::ContactSettings& Settings)
{
    Added.fetch_add(1, std::memory_order_relaxed);
    (void)Settings;

    if (Manifold.mRelativeContactPointsOn1.empty() || Manifold.mRelativeContactPointsOn2.empty()) return;
    const uint32_t Index1 = Body1.GetID().GetIndex();
    const uint32_t Index2 = Body2.GetID().GetIndex();
    const JPH::Vec3 Normal12 = Manifold.mWorldSpaceNormal;
    const JPH::Vec3 RelativePoint = 0.5f * (Manifold.mRelativeContactPointsOn1[0] + Manifold.mRelativeContactPointsOn2[0]);
    const JPH::RVec3 Point = Manifold.mBaseOffset + RelativePoint;
    // 📝 Include angular motion at the actual contact point: a spinning wheel/rim can strike hard while its centre barely moves.
    const JPH::Vec3 RelativeVelocity12 = Body1.GetPointVelocity(Point) - Body2.GetPointVelocity(Point);
    const float ClosingSpeed = std::abs(RelativeVelocity12.Dot(Normal12));
    constexpr float ImpactSpeedThresholdMps = 4.5f;
    if (!(ClosingSpeed >= ImpactSpeedThresholdMps)) return;
    const auto Now = std::chrono::steady_clock::now();

    std::lock_guard Lock(Mutex);
    if (Index1 >= Metadata.size() || Index2 >= Metadata.size()) return;
    const BodyMetadata& Meta1 = Metadata[Index1];
    const BodyMetadata& Meta2 = Metadata[Index2];
    const bool VehicleFirst = Meta1.VehicleChassis && (Meta2.Surface == PhysicsImpactSurface::Metal || Meta2.Surface == PhysicsImpactSurface::Concrete);
    const bool VehicleSecond = Meta2.VehicleChassis && (Meta1.Surface == PhysicsImpactSurface::Metal || Meta1.Surface == PhysicsImpactSurface::Concrete);
    if (!VehicleFirst && !VehicleSecond) return;

    const uint64_t VehicleRecord = VehicleFirst ? Meta1.Record : Meta2.Record;
    const uint64_t SurfaceRecord = VehicleFirst ? Meta2.Record : Meta1.Record;
    const PhysicsImpactSurface Surface = VehicleFirst ? Meta2.Surface : Meta1.Surface;
    if (VehicleRecord == 0 || SurfaceRecord == 0) return;
    const uint64_t LowRecord = std::min(VehicleRecord, SurfaceRecord);
    const uint64_t HighRecord = std::max(VehicleRecord, SurfaceRecord);
    uint64_t PairKey = LowRecord + 0x9E3779B97F4A7C15ull;
    PairKey ^= HighRecord + (PairKey << 6u) + (PairKey >> 2u);
    if (const auto Last = LastImpactByPair.find(PairKey); Last != LastImpactByPair.end() && Now - Last->second < std::chrono::milliseconds(250)) return;
    LastImpactByPair[PairKey] = Now;

    // 📝 Keep the queue bounded if a scene contains many simultaneously colliding vehicles.
    if (PendingImpacts.size() >= 64u) PendingImpacts.erase(PendingImpacts.begin());
    PhysicsImpactEvent Event;
    Event.VehicleRecord = VehicleRecord;
    Event.SurfaceRecord = SurfaceRecord;
    Event.Surface = Surface;
    Event.PointMetres = { static_cast<double>(Point.GetX()), static_cast<double>(Point.GetY()), static_cast<double>(Point.GetZ()) };
    const float NormalDirection = VehicleFirst ? -1.0f : 1.0f;
    const float VelocityDirection = VehicleFirst ? 1.0f : -1.0f;
    Event.Normal = { NormalDirection * static_cast<double>(Normal12.GetX()), NormalDirection * static_cast<double>(Normal12.GetY()),
                     NormalDirection * static_cast<double>(Normal12.GetZ()) };
    Event.RelativeVelocityMps = { VelocityDirection * static_cast<double>(RelativeVelocity12.GetX()),
                                  VelocityDirection * static_cast<double>(RelativeVelocity12.GetY()),
                                  VelocityDirection * static_cast<double>(RelativeVelocity12.GetZ()) };
    Event.ClosingSpeedMps = ClosingSpeed;
    uint32_t Seed = static_cast<uint32_t>(VehicleRecord) ^ static_cast<uint32_t>(VehicleRecord >> 32u) ^
                    (static_cast<uint32_t>(SurfaceRecord) * 0x9E3779B9u) ^ static_cast<uint32_t>(SurfaceRecord >> 32u) ^ NextImpactSeed++;
    if (NextImpactSeed == 0) NextImpactSeed = 1;
    Seed ^= Seed >> 16u;
    Seed *= 0x7FEB352Du;
    Seed ^= Seed >> 15u;
    Seed *= 0x846CA68Bu;
    Seed ^= Seed >> 16u;
    Event.EventSeed = Seed != 0 ? Seed : 1u;
    PendingImpacts.push_back(Event);

    if (LastImpactByPair.size() > 512u)
    {
        for (auto It = LastImpactByPair.begin(); It != LastImpactByPair.end();)
            if (Now - It->second > std::chrono::seconds(2)) It = LastImpactByPair.erase(It);
            else ++It;
        while (LastImpactByPair.size() > 512u) LastImpactByPair.erase(LastImpactByPair.begin());
    }
}

std::unique_ptr<PhysicsAdapter> ConstructJoltAdapter(const PhysicsSpaceSettings& Settings)
{
    return std::make_unique<JoltAdapter>(Settings);
}

JoltAdapter::JoltAdapter(const PhysicsSpaceSettings& Settings) : Backend(Settings.Workers)
{
    const JPH::uint Capacity = std::max<JPH::uint>(16, Settings.MaximumBodies);
    Solver.Init(Capacity, 0, Capacity, Capacity, Backend.QueryLayers(), Backend.QueryLayerFilter(), Backend.QueryPairFilter());
    Solver.SetGravity(ToVector(Settings.GravityMps2));
    Solver.SetContactListener(&Contacts);
    BodyIndices.assign(Capacity, PhysicsNoBody);
}

JoltAdapter::~JoltAdapter()
{
    JPH::BodyInterface& Interface = Solver.GetBodyInterface();
    for (const JPH::BodyID& Body : Bodies)
    {
        if (Body.IsInvalid()) continue;
        Interface.RemoveBody(Body);
        Interface.DestroyBody(Body);
    }
}

JPH::RefConst<JPH::Shape> JoltAdapter::ConstructShape(const PhysicsBodyDescriptor& Descriptor) const
{
    constexpr double Pi = 3.14159265358979323846;
    switch (Descriptor.Shape)
    {
        case PhysicsShape::Sphere:
        {
            const double Hx = Descriptor.HalfExtentMetres[0];
            const double Hy = Descriptor.HalfExtentMetres[1] > 1.0e-3 ? Descriptor.HalfExtentMetres[1] : Hx;
            const double Hz = Descriptor.HalfExtentMetres[2] > 1.0e-3 ? Descriptor.HalfExtentMetres[2] : Hx;
            if (!(Hx > 1.0e-3)) return nullptr;
            if (std::abs(Hx - Hy) < 1.0e-4 && std::abs(Hx - Hz) < 1.0e-4)
            {
                return new JPH::SphereShape(static_cast<float>(Hx));
            }
            JPH::Array<JPH::Vec3> Points;
            Points.reserve(162);
            Points.push_back(JPH::Vec3(0.0f, 0.0f, static_cast<float>(Hz)));
            Points.push_back(JPH::Vec3(0.0f, 0.0f, static_cast<float>(-Hz)));
            for (int Ring = 1; Ring < 10; ++Ring)
            {
                const double V  = Pi * static_cast<double>(Ring) / 10.0;
                const double Sv = std::sin(V), Cv = std::cos(V);
                for (int Seg = 0; Seg < 18; ++Seg)
                {
                    const double U = 2.0 * Pi * static_cast<double>(Seg) / 18.0;
                    Points.push_back(JPH::Vec3(static_cast<float>(Hx * Sv * std::cos(U)),
                                               static_cast<float>(Hy * Sv * std::sin(U)),
                                               static_cast<float>(Hz * Cv)));
                }
            }
            const float MinAxis = static_cast<float>(std::min({ Hx, Hy, Hz }));
            JPH::ConvexHullShapeSettings Settings(Points, std::min(JPH::cDefaultConvexRadius, 0.25f * MinAxis));
            const JPH::ShapeSettings::ShapeResult Result = Settings.Create();
            return Result.IsValid() ? Result.Get() : nullptr;
        }
        case PhysicsShape::Box:
        {
            const JPH::Vec3 Half(std::max(0.005f, static_cast<float>(Descriptor.HalfExtentMetres[0])),
                                 std::max(0.005f, static_cast<float>(Descriptor.HalfExtentMetres[1])),
                                 std::max(0.005f, static_cast<float>(Descriptor.HalfExtentMetres[2])));
            if (!(Descriptor.HalfExtentMetres[0] > 1.0e-4 && Descriptor.HalfExtentMetres[1] > 1.0e-4 && Descriptor.HalfExtentMetres[2] > 1.0e-4))
                return nullptr;
            // 📝 The convex radius rounds the box's edges; it must stay below half the smallest side.
            return new JPH::BoxShape(Half, std::min(JPH::cDefaultConvexRadius, 0.45f * Half.ReduceMin()));
        }
        case PhysicsShape::Cylinder:
        {
            const double Hx = Descriptor.HalfExtentMetres[0];
            const double Hy = Descriptor.HalfExtentMetres[1];
            const double Hz = std::max(0.005, Descriptor.HalfExtentMetres[2]);
            if (!(Hx > 1.0e-3 && Hy > 1.0e-3)) return nullptr;
            if (std::abs(Hx - Hy) < 1.0e-4 && Hx >= 0.02 && Hz >= 0.02)
            {
                const float Convex = std::min(JPH::cDefaultConvexRadius, static_cast<float>(0.35 * std::min(Hx, Hz)));
                JPH::CylinderShapeSettings CylSettings(static_cast<float>(Hz), static_cast<float>(Hx), Convex);
                const JPH::ShapeSettings::ShapeResult CylResult = CylSettings.Create();
                if (CylResult.IsValid())
                {
                    JPH::RotatedTranslatedShapeSettings ZAligned(
                        JPH::Vec3::sZero(), JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI), CylResult.Get());
                    const JPH::ShapeSettings::ShapeResult RotResult = ZAligned.Create();
                    if (RotResult.IsValid()) return RotResult.Get();
                }
            }
            JPH::Array<JPH::Vec3> Points;
            Points.reserve(48);
            for (int Seg = 0; Seg < 24; ++Seg)
            {
                const double Angle = 2.0 * Pi * static_cast<double>(Seg) / 24.0;
                const float  Px    = static_cast<float>(Hx * std::cos(Angle));
                const float  Py    = static_cast<float>(Hy * std::sin(Angle));
                Points.push_back(JPH::Vec3(Px, Py, static_cast<float>(-Hz)));
                Points.push_back(JPH::Vec3(Px, Py, static_cast<float>(Hz)));
            }
            const float MinAxis = static_cast<float>(std::min({ Hx, Hy, Hz }));
            JPH::ConvexHullShapeSettings Settings(Points, std::min(JPH::cDefaultConvexRadius, 0.25f * MinAxis));
            const JPH::ShapeSettings::ShapeResult Result = Settings.Create();
            return Result.IsValid() ? Result.Get() : nullptr;
        }
        case PhysicsShape::Cone:
        {
            const double Hx = Descriptor.HalfExtentMetres[0];
            const double Hy = Descriptor.HalfExtentMetres[1];
            const double Hz = std::max(0.005, Descriptor.HalfExtentMetres[2]);
            if (!(Hx > 1.0e-3 && Hy > 1.0e-3)) return nullptr;
            JPH::Array<JPH::Vec3> Points;
            Points.reserve(25);
            Points.push_back(JPH::Vec3(0.0f, 0.0f, static_cast<float>(Hz)));
            for (int Seg = 0; Seg < 24; ++Seg)
            {
                const double Angle = 2.0 * Pi * static_cast<double>(Seg) / 24.0;
                Points.push_back(JPH::Vec3(static_cast<float>(Hx * std::cos(Angle)),
                                           static_cast<float>(Hy * std::sin(Angle)),
                                           static_cast<float>(-Hz)));
            }
            const float MinAxis = static_cast<float>(std::min({ Hx, Hy, Hz }));
            JPH::ConvexHullShapeSettings Settings(Points, std::min(JPH::cDefaultConvexRadius, 0.2f * MinAxis));
            const JPH::ShapeSettings::ShapeResult Result = Settings.Create();
            return Result.IsValid() ? Result.Get() : nullptr;
        }
        case PhysicsShape::Torus:
        {
            // 📝 ShapeArchive Torus: outer radius = HalfExtent[0,1], tube_ratio = 0.3 → ring radius = 0.7 * outer, tube = HalfExtent[2].
            const double Hx = Descriptor.HalfExtentMetres[0];
            const double Hy = Descriptor.HalfExtentMetres[1];
            const double Hz = std::max(0.005, Descriptor.HalfExtentMetres[2]);
            if (!(Hx > 1.0e-3 && Hy > 1.0e-3)) return nullptr;
            constexpr int Segments   = 16;
            const double  RingX      = 0.70 * Hx;
            const double  RingY      = 0.70 * Hy;
            const double  TubeX      = 0.30 * Hx;
            const double  TubeY      = 0.30 * Hy;
            const float   Convex     = std::min(JPH::cDefaultConvexRadius, static_cast<float>(0.2f * std::min({ TubeX, TubeY, Hz })));
            JPH::StaticCompoundShapeSettings Compound;
            for (int Seg = 0; Seg < Segments; ++Seg)
            {
                const double U0 = 2.0 * Pi * static_cast<double>(Seg) / static_cast<double>(Segments);
                const double U1 = 2.0 * Pi * static_cast<double>(Seg + 1) / static_cast<double>(Segments);
                JPH::Array<JPH::Vec3> Wedge;
                Wedge.reserve(16);
                for (double U : { U0, U1 })
                {
                    const double Cu = std::cos(U), Su = std::sin(U);
                    for (int RingPt = 0; RingPt < 8; ++RingPt)
                    {
                        const double V  = 2.0 * Pi * static_cast<double>(RingPt) / 8.0;
                        const double Cv = std::cos(V), Sv = std::sin(V);
                        Wedge.push_back(JPH::Vec3(static_cast<float>((RingX + TubeX * Cv) * Cu),
                                                  static_cast<float>((RingY + TubeY * Cv) * Su),
                                                  static_cast<float>(Hz * Sv)));
                    }
                }
                JPH::ConvexHullShapeSettings WedgeSettings(Wedge, Convex);
                const JPH::ShapeSettings::ShapeResult WedgeResult = WedgeSettings.Create();
                if (WedgeResult.IsValid()) Compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), WedgeResult.Get());
            }
            const JPH::ShapeSettings::ShapeResult Result = Compound.Create();
            return Result.IsValid() ? Result.Get() : nullptr;
        }
        case PhysicsShape::Hull:
        {
            if (Descriptor.Points.size() < 4) return nullptr;
            JPH::Array<JPH::Vec3> Points;
            Points.reserve(Descriptor.Points.size());
            for (const auto& Point : Descriptor.Points) Points.push_back(JPH::Vec3(Point[0], Point[1], Point[2]));
            JPH::ConvexHullShapeSettings Settings(Points, JPH::cDefaultConvexRadius);
            const JPH::ShapeSettings::ShapeResult Result = Settings.Create();
            return Result.IsValid() ? Result.Get() : nullptr;
        }
        case PhysicsShape::Triangles:
        {
            if (Descriptor.Motion != PhysicsMotion::Fixed || Descriptor.Points.size() < 3 || Descriptor.Triangles.size() < 3) return nullptr;
            JPH::VertexList Corners;
            Corners.reserve(Descriptor.Points.size());
            for (const auto& Point : Descriptor.Points) Corners.push_back(JPH::Float3(Point[0], Point[1], Point[2]));
            JPH::IndexedTriangleList Faces;
            Faces.reserve(Descriptor.Triangles.size() / 3);
            for (size_t Index = 0; Index + 2 < Descriptor.Triangles.size(); Index += 3)
            {
                const uint32_t A = Descriptor.Triangles[Index], B = Descriptor.Triangles[Index + 1], C = Descriptor.Triangles[Index + 2];
                if (A >= Corners.size() || B >= Corners.size() || C >= Corners.size() || A == B || B == C || A == C) continue;
                Faces.push_back(JPH::IndexedTriangle(A, B, C, 0));
            }
            if (Faces.empty()) return nullptr;
            JPH::MeshShapeSettings          Settings(std::move(Corners), std::move(Faces));
            const JPH::ShapeSettings::ShapeResult Result = Settings.Create();
            return Result.IsValid() ? Result.Get() : nullptr;
        }
    }
    return nullptr;
}

uint32_t JoltAdapter::AddBody(const PhysicsBodyDescriptor& Descriptor)
{
    JPH::RefConst<JPH::Shape> Shape = ConstructShape(Descriptor);
    if (Shape == nullptr) return PhysicsNoBody;
    const bool Moving = Descriptor.Motion == PhysicsMotion::Moving;
    const bool Authored = Moving && Descriptor.MassKilograms > 0.0 && Descriptor.InertiaKgm2[0] > 0.0 && Descriptor.InertiaKgm2[1] > 0.0 &&
                          Descriptor.InertiaKgm2[2] > 0.0;
    if (Authored)
    {
        // 📝 An authored inertia is about the body's origin: the shape's own centre of mass moves onto it.
        const JPH::Vec3 Centre = Shape->GetCenterOfMass();
        if (Centre.LengthSq() > 1.0e-12f)
        {
            JPH::OffsetCenterOfMassShapeSettings Offset(-Centre, Shape);
            const JPH::ShapeSettings::ShapeResult Result = Offset.Create();
            if (!Result.IsValid()) return PhysicsNoBody;
            Shape = Result.Get();
        }
    }
    JPH::BodyCreationSettings Settings(Shape, ToPosition(Descriptor.PositionMetres), ToRotation(Descriptor.Orientation),
                                       Moving ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, Moving ? JoltLayers::Moving : JoltLayers::Fixed);
    Settings.mFriction       = static_cast<float>(Descriptor.Friction);
    Settings.mRestitution    = static_cast<float>(Descriptor.Restitution);
    Settings.mLinearDamping  = static_cast<float>(Descriptor.LinearDamping);
    Settings.mAngularDamping = static_cast<float>(Descriptor.AngularDamping);
    Settings.mAllowSleeping  = Descriptor.Sleeps;
    Settings.mUserData       = Descriptor.Record;
    if (Moving)
    {
        Settings.mLinearVelocity  = ToVector(Descriptor.LinearVelocityMps);
        Settings.mAngularVelocity = ToVector(Descriptor.AngularVelocityRads);
        if (Descriptor.Swept) Settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        if (Authored)
        {
            Settings.mOverrideMassProperties          = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            Settings.mMassPropertiesOverride.mMass    = static_cast<float>(Descriptor.MassKilograms);
            Settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(ToVector(Descriptor.InertiaKgm2));
        }
        else if (Descriptor.MassKilograms > 0.0)
        {
            Settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
            Settings.mMassPropertiesOverride.mMass = static_cast<float>(Descriptor.MassKilograms);
        }
    }
    JPH::BodyInterface& Interface = Solver.GetBodyInterface();
    const JPH::BodyID   Body      = Interface.CreateAndAddBody(Settings, Moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (Body.IsInvalid()) return PhysicsNoBody;
    const uint32_t Index = static_cast<uint32_t>(Bodies.size());
    Bodies.push_back(Body);
    if (Body.GetIndex() < BodyIndices.size()) BodyIndices[Body.GetIndex()] = Index;
    Contacts.ConfigureBody(Body.GetIndex(), Descriptor.Record, Descriptor.ImpactSurface, Descriptor.VehicleChassis);
    return Index;
}

void JoltAdapter::RemoveBody(uint32_t Index)
{
    if (Index >= Bodies.size()) return;
    const JPH::BodyID Body = Bodies[Index];
    if (Body.IsInvalid()) return;
    if (Body.GetIndex() < BodyIndices.size()) BodyIndices[Body.GetIndex()] = PhysicsNoBody;
    Contacts.RemoveBody(Body.GetIndex());
    JPH::BodyInterface& Interface = Solver.GetBodyInterface();
    Interface.RemoveBody(Body);
    Interface.DestroyBody(Body);
    Bodies[Index] = JPH::BodyID();
}

void JoltAdapter::Prepare()
{
    Solver.OptimizeBroadPhase();
}

void JoltAdapter::Step(double Seconds, uint32_t CollisionSteps)
{
    if (!(Seconds > 0.0)) return;
    (void)Solver.Update(static_cast<float>(Seconds), static_cast<int>(std::max(1u, CollisionSteps)), &Backend.AccessTemporary(),
                        &Backend.AccessJobs());
}

PhysicsBodyReading JoltAdapter::ReadBody(uint32_t Index) const
{
    PhysicsBodyReading Reading;
    if (Index >= Bodies.size() || Bodies[Index].IsInvalid()) return Reading;
    const JPH::BodyInterface& Interface = Solver.GetBodyInterfaceNoLock();
    const JPH::BodyID         Body      = Bodies[Index];
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

void JoltAdapter::PlaceBody(uint32_t Index, const PhysicsBodyReading& Pose)
{
    if (Index >= Bodies.size() || Bodies[Index].IsInvalid()) return;
    JPH::BodyInterface& Interface = Solver.GetBodyInterfaceNoLock();
    Interface.SetPositionAndRotation(Bodies[Index], ToPosition(Pose.PositionMetres), ToRotation(Pose.Orientation), JPH::EActivation::Activate);
    Interface.SetLinearAndAngularVelocity(Bodies[Index], ToVector(Pose.LinearVelocityMps), ToVector(Pose.AngularVelocityRads));
}

void JoltAdapter::ApplyForce(uint32_t Index, const std::array<double, 3>& Force, const std::array<double, 3>& Point)
{
    if (Index >= Bodies.size() || Bodies[Index].IsInvalid()) return;
    Solver.GetBodyInterfaceNoLock().AddForce(Bodies[Index], ToVector(Force), ToPosition(Point));
}

void JoltAdapter::ApplyTorque(uint32_t Index, const std::array<double, 3>& Torque)
{
    if (Index >= Bodies.size() || Bodies[Index].IsInvalid()) return;
    Solver.GetBodyInterfaceNoLock().AddTorque(Bodies[Index], ToVector(Torque));
}

bool JoltAdapter::CastRay(const PhysicsRay& Ray, PhysicsRayHit& Hit) const
{
    const JPH::Vec3 Direction = ToVector(Ray.Direction);
    if (!(Ray.LengthMetres > 0.0) || Direction.LengthSq() < 1.0e-12f) return false;
    const JPH::RRayCast Cast{ ToPosition(Ray.OriginMetres), Direction.Normalized() * static_cast<float>(Ray.LengthMetres) };
    JPH::RayCastResult  Result;
    bool                Met = false;
    if (Ray.IgnoredBody < Bodies.size() && !Bodies[Ray.IgnoredBody].IsInvalid())
    {
        const JPH::IgnoreSingleBodyFilter Filter(Bodies[Ray.IgnoredBody]);
        Met = Solver.GetNarrowPhaseQueryNoLock().CastRay(Cast, Result, {}, {}, Filter);
    }
    else
    {
        Met = Solver.GetNarrowPhaseQueryNoLock().CastRay(Cast, Result);
    }
    if (!Met) return false;
    const JPH::RVec3 Point = Cast.GetPointOnRay(Result.mFraction);
    JPH::Vec3        Normal(0.0f, 0.0f, 1.0f);
    {
        JPH::BodyLockRead Lock(Solver.GetBodyLockInterfaceNoLock(), Result.mBodyID);
        if (Lock.Succeeded()) Normal = Lock.GetBody().GetWorldSpaceSurfaceNormal(Result.mSubShapeID2, Point);
    }
    Hit.PointMetres    = { static_cast<double>(Point.GetX()), static_cast<double>(Point.GetY()), static_cast<double>(Point.GetZ()) };
    Hit.Normal         = { Normal.GetX(), Normal.GetY(), Normal.GetZ() };
    Hit.DistanceMetres = static_cast<double>(Result.mFraction) * Ray.LengthMetres;
    const JPH::uint32 JoltIndex = Result.mBodyID.GetIndex();
    Hit.Body = JoltIndex < BodyIndices.size() ? BodyIndices[JoltIndex] : PhysicsNoBody;
    return true;
}

void JoltAdapter::AssignMass(uint32_t Index, double MassKilograms, const std::array<double, 3>& InertiaKgm2)
{
    if (Index >= Bodies.size() || Bodies[Index].IsInvalid() || !(MassKilograms > 0.0)) return;
    JPH::BodyLockWrite Lock(Solver.GetBodyLockInterfaceNoLock(), Bodies[Index]);
    if (!Lock.Succeeded() || !Lock.GetBody().IsDynamic()) return;
    JPH::MotionProperties* Motion = Lock.GetBody().GetMotionProperties();
    Motion->SetInverseMass(static_cast<float>(1.0 / MassKilograms));
    const JPH::Vec3 Inverse(InertiaKgm2[0] > 0.0 ? static_cast<float>(1.0 / InertiaKgm2[0]) : 0.0f, InertiaKgm2[1] > 0.0 ? static_cast<float>(1.0 / InertiaKgm2[1]) : 0.0f,
                            InertiaKgm2[2] > 0.0 ? static_cast<float>(1.0 / InertiaKgm2[2]) : 0.0f);
    Motion->SetInverseInertia(Inverse, JPH::Quat::sIdentity());
}

void JoltAdapter::Dispatch(uint32_t Count, const std::function<void(uint32_t)>& Task)
{
    if (Count == 0) return;
    if (Count == 1 || Backend.QueryWorkers() <= 1)
    {
        for (uint32_t Index = 0; Index < Count; ++Index) Task(Index);
        return;
    }
    // 📝 One job per item on Jolt's pool, gathered by a barrier; the caller's thread joins in while it waits.
    JPH::JobSystem&          Jobs    = Backend.AccessJobs();
    JPH::JobSystem::Barrier* Barrier = Jobs.CreateBarrier();
    for (uint32_t Index = 0; Index < Count; ++Index)
    {
        JPH::JobHandle Handle = Jobs.CreateJob("Frontier dispatch", JPH::Color::sGreen, [&Task, Index] { Task(Index); });
        Barrier->AddJob(Handle);
    }
    Jobs.WaitForJobs(Barrier);
    Jobs.DestroyBarrier(Barrier);
}

size_t JoltAdapter::QueryAwakeCount() const
{
    return Solver.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

} // namespace Frontier
