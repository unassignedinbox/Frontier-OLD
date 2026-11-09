/*==========================================================================================================================================
                                                             PHYSICSVEHICLE.H
==========================================================================================================================================*/
// 🧩 A vehicle in the PhysicsSpace: Project Drive's solver hosted on a Jolt chassis. The chassis is a moving body with the sprung
//    mass and the authored inertia about the centre of mass (a hull of the ControlVehicle's body), so walls, cones and other
//    bodies meet it; the wheels are the solver's own (its contact fan and soft carcasses ask the space where the road is by ray).
//    Each fixed step the solver reads the chassis, applies strut, tyre and aero forces at their points, and the space integrates.
//    Runs on the physics thread; the view it fills is what the editor draws (chassis, wheels, tyre deformation, telemetry).

#pragma once

#include "Frontier/Extensions/Physics/PhysicsSpace.h"

#include "Frontier/Engine/PhysicalDynamics/Vehicle/VehicleSpecification.h"
#include "Frontier/Engine/Scene/RecordToken.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace Frontier
{

// 📝 Driver controls for one fixed step.
struct PhysicsVehicleInput
{
    float Throttle    = 0.0f;   // [0, 1]
    float Brake       = 0.0f;   // [0, 1]
    float Steer       = 0.0f;   // [−1, 1] +1 turns left
    bool  Handbrake   = false;
    float ClutchPedal = 0.0f;   // [0, 1] 0: automatic clutch servo; 1: manual clutch fully disengaged
};

struct PhysicsVehiclePlacementOptions
{
    bool EquilibriumPlacement    = true;    // [-] fit 4-corner slope plane + static sag + radial tyre deflection
    bool EngageBrakesOnPlacement = false;   // [-] latch service + parking brakes at spawn/reset until throttle > 0.02
};

// 📝 One vehicle as a refresh sees it. Transforms are column-major 4 × 4 (world from local): the body in the ControlVehicle's
//    authored axes (+x nose, +y left, +z up, origin where the model's is), each wheel in its material axes (hub, steer, spin).
struct PhysicsVehicleView
{
    RecordToken                              Record{};
    Vehicle::VehicleTelemetry                Telemetry{};
    std::array<float, 16>                    Body{};
    std::array<std::array<float, 16>, 4>     Wheels{};
    std::array<Vehicle::CarcassField, 4>     Fields{};
    std::array<bool, 4>                      FieldValid{};
    std::array<float, 4>                     ContactAngle{};    // [rad] where the road meets the tyre, material axes (atan2 z, x)
    std::array<float, 4>                     Deflection{};      // [m]
    std::array<float, 4>                     GaugePressure{};   // [Pa]
    std::array<float, 4>                     LateralShift{};    // [m] the analytic profile's patch shear
    PhysicsVehicleInput                      Input{};
};

class PhysicsVehicle
{
  public:
    PhysicsVehicle();
    ~PhysicsVehicle();

    // 📝 Adds the chassis body at Start (ground point under the centre of mass, yaw from OrientationDegrees[2]) and builds the
    //    solver. Returns false (with Error) when the space refuses the body.
    bool Construct(PhysicsSpace& Space, RecordToken Record, const Vehicle::VehicleSpecification& Specification,
                   const Vehicle::VehicleSolverConfiguration& Configuration, const std::array<double, 3>& StartMetres, double StartYawDegrees,
                   std::string* Error = nullptr, PhysicsVehiclePlacementOptions Placement = {});

    void AssignInput(const PhysicsVehicleInput& Input) noexcept { Controls = Input; }
    // 📝 The solver's step: reads the chassis, applies its forces to the body; the space's step follows.
    void Step(double Seconds);
    // 📝 Back to the start, at rest, the solver rebuilt.
    void Reset();
    void ReadView(PhysicsVehicleView& View) const;

    [[nodiscard]] uint32_t                       QueryBody() const noexcept { return Body; }
    [[nodiscard]] RecordToken                    QueryRecord() const noexcept { return Record; }
    [[nodiscard]] const Vehicle::VehicleSolver&  QuerySolver() const noexcept { return *Solver; }
    [[nodiscard]] Vehicle::ChassisKinematics     ReadChassis() const;
    [[nodiscard]] double                         QueryAuthoredOriginBelowCentre() const noexcept { return OriginBelowCentre; }
    [[nodiscard]] bool                           QueryParkedBrakeLatched() const noexcept { return ParkedBrakeLatched; }
    [[nodiscard]] Vehicle::Vec3                  QueryFittedGroundNormal() const noexcept { return FittedGroundNormal; }

  private:
    void BuildSolver(const Vehicle::ChassisKinematics& Start);
    [[nodiscard]] Vehicle::ChassisKinematics ResolveEquilibriumPose(const std::array<double, 3>& StartMetres, double StartYawDegrees,
                                                                    double FallbackCoMHeight, Vehicle::Vec3& OutNormal) const;
    [[nodiscard]] bool QueryGround(const Vehicle::Vec3& Point, Vehicle::Vec3& Surface, Vehicle::Vec3& Normal) const;

    PhysicsSpace*                            Space = nullptr;
    RecordToken                              Record{};
    uint32_t                                 Body = PhysicsNoBody;
    Vehicle::VehicleSolverConfiguration      Configuration{};
    PhysicsVehiclePlacementOptions           PlacementOptions{};
    std::array<double, 3>                    AuthoredStartMetres{};
    double                                   AuthoredStartYawDegrees = 0.0;
    double                                   AuthoredCoMHeight = 0.53;
    std::unique_ptr<Vehicle::VehicleSolver>  Solver;
    Vehicle::ChassisKinematics               Start{};
    Vehicle::Vec3                            FittedGroundNormal{ 0.0f, 0.0f, 1.0f };
    PhysicsVehicleInput                      Controls{};
    bool                                     ParkedBrakeLatched = false;
    double                                   OriginBelowCentre = 0.0;   // [m] the authored model's origin below the centre of mass
    float                                    LastMass = 0.0f;           // [kg] chassis mass the body last carried (fuel burns)
};

} // namespace Frontier
