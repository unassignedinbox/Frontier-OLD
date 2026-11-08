/*==========================================================================================================================================
                                                  VEHICLEIMPACTEVENTVALIDATION.CPP
==========================================================================================================================================*/
// 🧩 Jolt vehicle-chassis contacts publish bounded events only for hard metal/concrete impacts above the closing-speed threshold.

#include "Frontier/Extensions/Physics/PhysicsSpace.h"

#include <cmath>
#include <iostream>
#include <optional>

namespace
{

using namespace Frontier;

bool Check(bool Passed, const char* Label)
{
    std::cout << (Passed ? "PASS " : "FAIL ") << Label << '\n';
    return Passed;
}

std::optional<PhysicsImpactEvent> Strike(PhysicsImpactSurface Surface, double DownwardSpeed, bool VehicleChassis = true,
                                         PhysicsImpactSurface VehicleMaterial = PhysicsImpactSurface::Other)
{
    PhysicsSpaceSettings Settings;
    Settings.MaximumBodies = 16;
    Settings.Workers = 1;
    PhysicsSpace Space(Settings);

    PhysicsBodyDescriptor HardSurface;
    HardSurface.Record = 77u;
    HardSurface.ImpactSurface = Surface;
    HardSurface.Shape = PhysicsShape::Box;
    HardSurface.HalfExtentMetres = {10.0, 10.0, 0.5};
    HardSurface.PositionMetres = {0.0, 0.0, -0.5};
    HardSurface.Motion = PhysicsMotion::Fixed;
    HardSurface.Friction = 0.8;
    if (Space.AddBody(HardSurface) == PhysicsNoBody) return std::nullopt;

    PhysicsBodyDescriptor Chassis;
    Chassis.Record = 42u;
    Chassis.ImpactSurface = VehicleMaterial;
    Chassis.VehicleChassis = VehicleChassis;
    Chassis.Shape = PhysicsShape::Box;
    Chassis.HalfExtentMetres = {0.5, 0.5, 0.5};
    Chassis.PositionMetres = {0.0, 0.0, 1.4};
    Chassis.LinearVelocityMps = {0.0, 0.0, -DownwardSpeed};
    Chassis.Motion = PhysicsMotion::Moving;
    Chassis.MassKilograms = 1200.0;
    Chassis.Sleeps = false;
    if (Space.AddBody(Chassis) == PhysicsNoBody) return std::nullopt;
    Space.Prepare();

    for (uint32_t Step = 0; Step < 360u; ++Step)
    {
        Space.Step(1.0 / 240.0, 2u);
        const std::vector<PhysicsImpactEvent> Events = Space.DrainImpactEvents();
        if (!Events.empty()) return Events.front();
    }
    return std::nullopt;
}

} // namespace

int main()
{
    using namespace Frontier;
    bool Passed = true;

    const auto Metal = Strike(PhysicsImpactSurface::Metal, 6.0);
    Passed &= Check(Metal.has_value() && Metal->VehicleRecord == 42u && Metal->SurfaceRecord == 77u &&
                        Metal->Surface == PhysicsImpactSurface::Metal && Metal->ClosingSpeedMps >= 4.5f && Metal->Normal[2] > 0.7 &&
                        Metal->RelativeVelocityMps[2] < -4.5 && Metal->EventSeed != 0,
                    "fast chassis-to-metal contact carries point velocity, outward normal, IDs and a stable particle seed");

    const auto Concrete = Strike(PhysicsImpactSurface::Concrete, 6.0);
    Passed &= Check(Concrete.has_value() && Concrete->Surface == PhysicsImpactSurface::Concrete && Concrete->Normal[2] > 0.7,
                    "fast chassis-to-concrete contact is classified distinctly");

    Passed &= Check(!Strike(PhysicsImpactSurface::Metal, 1.0).has_value(), "sub-threshold contact does not publish a spark event");
    Passed &= Check(!Strike(PhysicsImpactSurface::Other, 6.0).has_value(), "ordinary surfaces do not publish hard-impact spark events");
    Passed &= Check(!Strike(PhysicsImpactSurface::Metal, 6.0, false).has_value(), "non-vehicle rigid bodies do not publish vehicle spark events");
    Passed &= Check(!Strike(PhysicsImpactSurface::Other, 6.0, true, PhysicsImpactSurface::Metal).has_value(),
                    "chassis material does not make an ordinary contact surface spark-worthy");

    return Passed ? 0 : 1;
}
