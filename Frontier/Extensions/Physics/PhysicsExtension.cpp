/*==========================================================================================================================================
                                                            PHYSICSEXTENSION.CPP
==========================================================================================================================================*/
// 🧩 Level records into bodies, steps written back into records, and the restoration when the simulation stops.

#include "Frontier/Extensions/Physics/PhysicsExtension.h"

#include "Frontier/Engine/Scene/WorldSpace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Frontier
{

namespace
{

constexpr double StepSeconds = 1.0 / 60.0;   // [s] the space's fixed step

std::array<double, 3> ToDouble(const float Values[3])
{
    return { Values[0], Values[1], Values[2] };
}

std::array<double, 3> Conjugated(const std::array<double, 4>& Q, const std::array<double, 3>& Vector)
{
    return RotatePhysicsVector({ -Q[0], -Q[1], -Q[2], Q[3] }, Vector);
}

} // namespace

PhysicsExtension::PhysicsExtension()  = default;
PhysicsExtension::~PhysicsExtension() = default;

size_t PhysicsExtension::Begin(WorldSpace& World, const PhysicsSpaceSettings& Settings)
{
    if (Space) End(World);
    Space     = std::make_unique<PhysicsSpace>(Settings);
    Links.clear();
    Tally     = {};
    Remainder = 0.0;

    // ① the ground: a 4 km slab whose top is z = 0
    PhysicsBodyDescriptor Ground;
    Ground.Shape            = PhysicsShape::Box;
    Ground.HalfExtentMetres = { 2000.0, 2000.0, 1.0 };
    Ground.PositionMetres   = { 0.0, 0.0, -1.0 };
    Ground.Motion           = PhysicsMotion::Fixed;
    Ground.Friction         = 0.8;
    if (Space->AddBody(Ground) != std::numeric_limits<uint32_t>::max()) ++Tally.Fixed;

    // ② Geometry records: Physics on → a moving box; otherwise a single shown copy → a fixed box over its world bounds
    for (const SceneRecordBase* Entry : World.FindAllByType("Geometry"))
    {
        auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Entry->QueryToken()));
        if (Record == nullptr) continue;
        GeometryConfiguration&                     Geometry = Record->AccessConfiguration();
        const GeometryConfiguration::Resolution& Resolved = Geometry.Resolved;
        const std::array<double, 3> Minimum = ToDouble(Resolved.BoundsMinimum), Maximum = ToDouble(Resolved.BoundsMaximum);
        const std::array<double, 3> World3  = { Maximum[0] - Minimum[0], Maximum[1] - Minimum[1], Maximum[2] - Minimum[2] };
        const bool                  Bounded = World3[0] > 1.0e-3 && World3[1] > 1.0e-3 && World3[2] > 1.0e-3;
        if (Resolved.Shown > 1)
        {
            ++Tally.Held;
            continue;
        }
        if (Geometry.Physics)
        {
            const SpatialConfiguration& Spatial = Geometry.Spatial;
            std::array<double, 3> Size = { Resolved.Extent[0] * std::abs(Spatial.Scale[0]), Resolved.Extent[1] * std::abs(Spatial.Scale[1]),
                                           Resolved.Extent[2] * std::abs(Spatial.Scale[2]) };
            if (!(Size[0] > 1.0e-3 && Size[1] > 1.0e-3 && Size[2] > 1.0e-3))
            {
                // 📝 Not resolved against the scene yet (no extent): its world bounds, or nothing to simulate.
                if (!Bounded)
                {
                    ++Tally.Skipped;
                    continue;
                }
                Size = World3;
            }
            const std::array<double, 3> Origin      = ToDouble(Spatial.PositionMetres);
            const std::array<double, 4> Orientation = ResolvePhysicsOrientation(ToDouble(Spatial.OrientationDegrees));
            // 📝 The world bounds' centre is the shape's centre: its offset from the origin, in the object's own axes.
            const std::array<double, 3> Centre = Bounded ? std::array<double, 3>{ 0.5 * (Minimum[0] + Maximum[0]), 0.5 * (Minimum[1] + Maximum[1]),
                                                                                  0.5 * (Minimum[2] + Maximum[2]) }
                                                         : Origin;
            const std::array<double, 3> Offset = Conjugated(Orientation, { Centre[0] - Origin[0], Centre[1] - Origin[1], Centre[2] - Origin[2] });
            PhysicsBodyDescriptor Body;
            Body.Record           = Record->QueryToken().Value;
            Body.Shape            = PhysicsShape::Box;
            Body.HalfExtentMetres = { 0.5 * Size[0], 0.5 * Size[1], 0.5 * Size[2] };
            Body.PositionMetres   = Centre;
            Body.Orientation      = Orientation;
            Body.Motion           = PhysicsMotion::Moving;
            const uint32_t Index  = Space->AddBody(Body);
            if (Index == std::numeric_limits<uint32_t>::max())
            {
                ++Tally.Skipped;
                continue;
            }
            Links.push_back({ Index, Record->QueryToken(), true, Offset, Spatial, Geometry.Dynamic });
            Geometry.Dynamic = true;   // 📝 its cards relight every refresh while it moves
            ++Tally.Moving;
        }
        else if (Geometry.Visible && Resolved.Shown == 1 && Bounded)
        {
            PhysicsBodyDescriptor Collider;
            Collider.Record           = Record->QueryToken().Value;
            Collider.HalfExtentMetres = { 0.5 * World3[0], 0.5 * World3[1], 0.5 * World3[2] };
            Collider.PositionMetres   = { 0.5 * (Minimum[0] + Maximum[0]), 0.5 * (Minimum[1] + Maximum[1]), 0.5 * (Minimum[2] + Maximum[2]) };
            Collider.Motion           = PhysicsMotion::Fixed;
            if (Space->AddBody(Collider) != std::numeric_limits<uint32_t>::max()) ++Tally.Fixed;
        }
        else
        {
            ++Tally.Skipped;
        }
    }

    // ③ RigidBody records: spheres of their volume
    for (const SceneRecordBase* Entry : World.FindAllByType("RigidBody"))
    {
        auto* Record = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Entry->QueryToken()));
        if (Record == nullptr || !Record->QueryPhysics().Enabled) continue;
        const PhysicsConfiguration& Physics = Record->QueryPhysics();
        const double Volume = Physics.Mass.MassKilograms / std::max(1.0e-3f, Physics.Mass.DensityKgPerCubicMetre);
        PhysicsBodyDescriptor Body;
        Body.Record              = Record->QueryToken().Value;
        Body.Shape               = PhysicsShape::Sphere;
        Body.HalfExtentMetres[0] = std::clamp(std::cbrt(3.0 * Volume / (4.0 * 3.14159265358979323846)), 0.05, 50.0);
        Body.PositionMetres      = ToDouble(Record->QuerySpatial().PositionMetres);
        Body.Orientation         = ResolvePhysicsOrientation(ToDouble(Record->QuerySpatial().OrientationDegrees));
        Body.Motion              = Physics.DynamicBody ? PhysicsMotion::Moving : PhysicsMotion::Fixed;
        Body.MassKilograms       = Physics.Mass.MassKilograms;
        Body.Friction            = Physics.DynamicFriction;
        Body.Restitution         = Physics.Restitution;
        Body.LinearDamping       = Physics.LinearDamping;
        Body.LinearVelocityMps   = ToDouble(Physics.LinearVelocityMps);
        Body.AngularVelocityRads = ToDouble(Physics.AngularVelocityRadSec);
        const uint32_t Index     = Space->AddBody(Body);
        if (Index == std::numeric_limits<uint32_t>::max()) continue;
        if (Body.Motion == PhysicsMotion::Moving)
        {
            Links.push_back({ Index, Record->QueryToken(), false, {}, Record->QuerySpatial(), false });
            ++Tally.Moving;
        }
        else ++Tally.Fixed;
    }
    Space->Prepare();
    return Tally.Moving;
}

size_t PhysicsExtension::Step(WorldSpace& World, double DeltaSeconds)
{
    if (!Space) return 0;
    const auto Start = std::chrono::steady_clock::now();
    Remainder += std::clamp(DeltaSeconds, 0.0, 0.25);
    while (Remainder >= StepSeconds)
    {
        Space->Step(StepSeconds);
        Remainder -= StepSeconds;
    }
    size_t Written = 0;
    for (const Link& Moved : Links)
    {
        const PhysicsBodyReading Reading = Space->ReadBody(Moved.Body);
        const std::array<double, 3> Turned = RotatePhysicsVector(Reading.Orientation, Moved.CentreOffset);
        const std::array<double, 3> Euler  = ResolvePhysicsEulerDegrees(Reading.Orientation);
        SpatialConfiguration*       Spatial = nullptr;
        RevisionCounter*            Revision = nullptr;
        if (Moved.Geometry)
        {
            if (auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Moved.Record)))
            {
                Spatial  = &Record->AccessConfiguration().Spatial;
                Revision = &Record->AccessConfiguration();
            }
        }
        else if (auto* Record = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Moved.Record)))
        {
            Spatial  = &Record->AccessSpatial();
            Revision = &Record->AccessSpatial();
            for (size_t Axis = 0; Axis < 3; ++Axis) Record->AccessPhysics().LinearVelocityMps[Axis] = static_cast<float>(Reading.LinearVelocityMps[Axis]);
        }
        if (Spatial == nullptr) continue;
        for (size_t Axis = 0; Axis < 3; ++Axis)
        {
            Spatial->PositionMetres[Axis]     = static_cast<float>(Reading.PositionMetres[Axis] - Turned[Axis]);
            Spatial->OrientationDegrees[Axis] = static_cast<float>(Euler[Axis]);
        }
        Revision->AdvanceRevision();
        ++Written;
    }
    Tally.StepMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
    return Written;
}

void PhysicsExtension::End(WorldSpace& World)
{
    for (const Link& Moved : Links)
    {
        if (Moved.Geometry)
        {
            if (auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Moved.Record)))
            {
                GeometryConfiguration& Geometry = Record->AccessConfiguration();
                for (size_t Axis = 0; Axis < 3; ++Axis)
                {
                    Geometry.Spatial.PositionMetres[Axis]     = Moved.Saved.PositionMetres[Axis];
                    Geometry.Spatial.OrientationDegrees[Axis] = Moved.Saved.OrientationDegrees[Axis];
                    Geometry.Spatial.Scale[Axis]              = Moved.Saved.Scale[Axis];
                }
                Geometry.Dynamic = Moved.SavedDynamic;
                Geometry.AdvanceRevision();
            }
        }
        else if (auto* Record = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Moved.Record)))
        {
            SpatialConfiguration& Spatial = Record->AccessSpatial();
            for (size_t Axis = 0; Axis < 3; ++Axis)
            {
                Spatial.PositionMetres[Axis]     = Moved.Saved.PositionMetres[Axis];
                Spatial.OrientationDegrees[Axis] = Moved.Saved.OrientationDegrees[Axis];
            }
            Spatial.AdvanceRevision();
        }
    }
    Links.clear();
    Space.reset();
    Remainder = 0.0;
}

} // namespace Frontier
