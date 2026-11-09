/*==========================================================================================================================================
                                                      PLUMESOURCEPROJECTION.H
==========================================================================================================================================*/
// 🧩 Projects live vehicle wheel slip/thermal telemetry and authored FluidPlume records into world-grid PlumeSource & FireLight sets.

#pragma once

#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"
#include "Engine/PhysicalDynamics/Vehicle/VehicleSolver.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Frontier::FluidDynamics
{

struct PlumeSettings
{
    bool                 Enabled              = true;
    int                  QualityGrade         = 2;     // [-] 0 Low, 1 Medium, 2 High, 3 Ultra
    float                SmokeScale           = 1.0f;  // [-]
    float                FireScale            = 1.0f;  // [-]
    float                SlipOnset            = 0.14f; // [-]
    float                SlipFull             = 0.72f; // [-]
    float                FadeTime             = 6.5f;  // [s]
    float                Buoyancy             = 0.055f;// [m/s²/K]
    float                GravityLoad          = 0.16f; // [m/s²]
    float                VorticityConfinement = 2.8f;  // [-]
    float                SwirlScale           = 1.6f;  // [m]
    float                VelocityDrag         = 0.55f; // [1/s]
    float                HeatCooling          = 0.85f; // [1/s]
    float                FlameBurnRate        = 2.6f;  // [1/s]
    float                Extinction           = 3.4f;  // [1/m]
    float                AlbedoWhite          = 0.90f; // [-]
    float                AlbedoSoot           = 0.14f; // [-]
    float                PhaseForward         = 0.52f; // [-]
    float                PhaseBackward        = -0.18f;// [-]
    float                PhaseBlend           = 0.76f; // [-]
    float                MultipleScattering   = 0.62f; // [-]
    float                DetailStrength       = 0.35f; // [-]
    float                DetailScale          = 0.55f; // [1/m]
    float                FlameRadiance        = 18.0f; // [cd/m²]
    float                FlameTemperature     = 1850.0f;// [K]
    float                FloorHeight          = -0.5f; // [m]
    std::array<float, 3> WindVector           = { 0.8f, 0.3f, 0.0f }; // [m/s]
};

// 📝 GPU-aligned 48-byte source record matching PlumeSource in PlumeLayouts.slang.
struct PlumeSourceEntry
{
    std::array<float, 4> PositionRadius = { 0.0f, 0.0f, 0.0f, 0.25f }; // [m] xyz world centre; w 1/e radius
    std::array<float, 4> VelocityRate   = { 0.0f, 0.0f, 0.0f, 0.0f };  // [m/s] xyz flow; w smoke injection [1/s]
    std::array<float, 4> ThermalPayload = { 0.0f, 0.0f, 0.0f, 0.0f };  // [K/s,1/s,-,1/s] Heat, Flame, Soot, VelBlend
};

struct PlumeFireLightEntry
{
    std::array<float, 3> Position  = { 0.0f, 0.0f, 0.0f };
    float                Intensity = 0.0f;
    std::array<float, 3> Colour    = { 1.0f, 0.46f, 0.14f };
    float                Reach     = 14.0f;
};

struct PlumeTransientBurst
{
    std::array<float, 3> Position      = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> Velocity      = { 0.0f, 0.0f, 2.0f };
    float                Radius        = 0.45f;
    float                SmokeRate     = 8.0f;
    float                HeatRate      = 540.0f;
    float                FlameRate     = 3.2f;
    float                Soot          = 0.65f;
    float                VelocityBlend = 9.0f;
    float                Age           = 0.0f;
    float                Duration      = 0.28f;
    float                StartDelay    = 0.0f;
    float                LightPower    = 0.0f;
};

struct PlumeVehicleState
{
    std::array<float, 4> SmoothedSlip      = { 0.0f, 0.0f, 0.0f, 0.0f };
    float                PreviousThrottle  = 0.0f;
    float                AfterfireCooldown = 0.0f;
};

struct PlumeRecordState
{
    uint32_t LastBurstGeneration = 0;
    bool     Initialized         = false;
};

struct PlumeProjectionOutput
{
    std::vector<PlumeSourceEntry>    Sources{};
    std::vector<PlumeFireLightEntry> FireLights{};
    PlumeFireLightEntry              PrimaryFireLight{};
    std::array<float, 3>             FocusPosition = { 0.0f, 0.0f, 0.0f };
    bool                             HasFocus      = false;
};

class PlumeSourceProjector
{
  public:
    static constexpr size_t MaximumSources = 48u;

    void Reset() noexcept;

    void BeginStep(float DeltaSeconds) noexcept;

    void ProjectVehicle(uint64_t VehicleKey, const Vehicle::VehicleTelemetry& Telemetry, float ThrottleInput,
                        const PlumeSettings& Settings, float DeltaSeconds, PlumeProjectionOutput& Output);

    void ProjectFluidPlume(uint64_t RecordKey, const std::array<float, 3>& WorldPosition, float DomainScale,
                           const FluidParameters& Parameters, bool TriggerOnFirstSight, uint32_t BurstGeneration,
                           FluidBurstPattern Pattern, float ElapsedSeconds, PlumeProjectionOutput& Output);

    void QueueBurst(const PlumeTransientBurst& Burst);

    void FinalizeStep(PlumeProjectionOutput& Output) const noexcept;

  private:
    std::unordered_map<uint64_t, PlumeVehicleState> VehicleStates_{};
    std::unordered_map<uint64_t, PlumeRecordState>  RecordStates_{};
    std::vector<PlumeTransientBurst>                ActiveBursts_{};
};

} // namespace Frontier::FluidDynamics
