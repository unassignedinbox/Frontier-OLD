/*==========================================================================================================================================
                                                        IMPACTSPARKFIXTURE.H
==========================================================================================================================================*/
// 🧩 Deterministic replay fixture: real Jolt chassis boxes collide with metal and concrete, then their events feed spark validation/capture.

#pragma once

#include "Frontier/Engine/Interface/SceneViewportPresentation.h"
#include "Frontier/Extensions/Physics/PhysicsSpace.h"

#include <array>
#include <optional>
#include <vector>

namespace FrontierExhibit
{

inline std::optional<Frontier::PhysicsImpactEvent> StrikeImpact(Frontier::PhysicsImpactSurface Surface, uint64_t VehicleRecord,
                                                                 uint64_t SurfaceRecord, double X, double Y, double ContactHeight)
{
    using namespace Frontier;
    PhysicsSpaceSettings Settings;
    Settings.MaximumBodies = 8;
    Settings.Workers = 1;
    PhysicsSpace Space(Settings);

    PhysicsBodyDescriptor HardSurface;
    HardSurface.Record = SurfaceRecord;
    HardSurface.ImpactSurface = Surface;
    HardSurface.Shape = PhysicsShape::Box;
    HardSurface.HalfExtentMetres = {2.0, 2.0, 0.5};
    HardSurface.PositionMetres = {X, Y, ContactHeight - 0.5};
    HardSurface.Motion = PhysicsMotion::Fixed;
    HardSurface.Friction = 0.8;
    if (Space.AddBody(HardSurface) == PhysicsNoBody) return std::nullopt;

    PhysicsBodyDescriptor Chassis;
    Chassis.Record = VehicleRecord;
    Chassis.VehicleChassis = true;
    Chassis.Shape = PhysicsShape::Box;
    Chassis.HalfExtentMetres = {0.5, 0.5, 0.5};
    Chassis.PositionMetres = {X, Y, ContactHeight + 0.75};
    // 📝 Strong lateral scrape velocity makes the camera-observed streak direction unambiguous while the vertical component supplies impact speed.
    Chassis.LinearVelocityMps = {Surface == PhysicsImpactSurface::Metal ? -6.0 : -8.0,
                                 Surface == PhysicsImpactSurface::Metal ? 8.0 : -6.0, -16.0};
    Chassis.Motion = PhysicsMotion::Moving;
    Chassis.MassKilograms = 1200.0;
    Chassis.Swept = true;
    Chassis.Sleeps = false;
    if (Space.AddBody(Chassis) == PhysicsNoBody) return std::nullopt;

    Space.Prepare();
    for (uint32_t Step = 0; Step < 240u; ++Step)
    {
        Space.Step(1.0 / 240.0, 2u);
        std::vector<PhysicsImpactEvent> Events = Space.DrainImpactEvents();
        if (!Events.empty()) return Events.front();
    }
    return std::nullopt;
}

inline std::vector<Frontier::SceneImpactSpark> ConstructImpactReplay()
{
    using namespace Frontier;
    const auto Metal = StrikeImpact(PhysicsImpactSurface::Metal, 9001u, 7001u, -44.0, -0.55, 2.3786);
    const auto Concrete = StrikeImpact(PhysicsImpactSurface::Concrete, 9002u, 7002u, -44.0, 0.55, 2.3786);
    std::vector<SceneImpactSpark> Impacts;
    for (const auto* Event : {Metal ? &*Metal : nullptr, Concrete ? &*Concrete : nullptr})
    {
        if (Event == nullptr) continue;
        SceneImpactSpark Spark;
        Spark.Surface = Event->Surface == PhysicsImpactSurface::Concrete ? SceneImpactSurface::Concrete : SceneImpactSurface::Metal;
        Spark.PointMetres = Event->PointMetres;
        Spark.Normal = Event->Normal;
        Spark.RelativeVelocityMps = Event->RelativeVelocityMps;
        Spark.ClosingSpeedMps = Event->ClosingSpeedMps;
        Spark.EventSeed = Event->EventSeed;
        Impacts.push_back(Spark);
    }
    return Impacts;
}

} // namespace FrontierExhibit
