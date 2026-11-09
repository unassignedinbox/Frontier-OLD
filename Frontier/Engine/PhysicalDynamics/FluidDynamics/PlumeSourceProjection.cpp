/*==========================================================================================================================================
                                                     PLUMESOURCEPROJECTION.CPP
==========================================================================================================================================*/
// 🧩 Projects live vehicle wheel slip/thermal telemetry and authored FluidPlume records into world-grid PlumeSource & FireLight sets.

#include "Engine/PhysicalDynamics/FluidDynamics/PlumeSourceProjection.h"

#include <algorithm>
#include <cmath>

namespace Frontier::FluidDynamics
{

namespace
{

std::array<float, 3> ResolvePaletteTint(int Palette) noexcept
{
    switch (Palette)
    {
        case 2:  return { 1.0f, 0.78f, 0.48f };
        case 3:  return { 0.28f, 1.0f, 0.56f };
        case 4:  return { 0.52f, 0.44f, 1.0f };
        case 5:  return { 0.85f, 1.0f, 0.28f };
        default: return { 1.0f, 0.46f, 0.14f };
    }
}

} // namespace

void PlumeSourceProjector::Reset() noexcept
{
    VehicleStates_.clear();
    RecordStates_.clear();
    ActiveBursts_.clear();
}

void PlumeSourceProjector::BeginStep(float DeltaSeconds) noexcept
{
    const float Step = std::clamp(DeltaSeconds, 0.0f, 0.25f);
    for (auto Iter = ActiveBursts_.begin(); Iter != ActiveBursts_.end();)
    {
        if (Iter->StartDelay > 0.0f)
        {
            Iter->StartDelay -= Step;
            ++Iter;
            continue;
        }
        Iter->Age += Step;
        if (Iter->Age >= Iter->Duration)
        {
            Iter = ActiveBursts_.erase(Iter);
        }
        else
        {
            ++Iter;
        }
    }
}

void PlumeSourceProjector::QueueBurst(const PlumeTransientBurst& Burst)
{
    if (ActiveBursts_.size() < 64u)
    {
        ActiveBursts_.push_back(Burst);
    }
}

void PlumeSourceProjector::ProjectVehicle(uint64_t VehicleKey, const Vehicle::VehicleTelemetry& Telemetry, float ThrottleInput,
                                          const PlumeSettings& Settings, float DeltaSeconds, PlumeProjectionOutput& Output)
{
    if (!Settings.Enabled) return;

    PlumeVehicleState& State = VehicleStates_[VehicleKey];
    const float Dt           = std::clamp(DeltaSeconds, 1.0e-3f, 0.1f);
    State.AfterfireCooldown  = std::max(0.0f, State.AfterfireCooldown - Dt);

    const auto& ChassisPos = Telemetry.Chassis.Position;
    const auto& Orient     = Telemetry.Chassis.Orientation;
    const auto  Forward    = Orient.Rotate({ 1.0f, 0.0f, 0.0f });
    const auto  Left       = Orient.Rotate({ 0.0f, 1.0f, 0.0f });
    const auto  Up         = Orient.Rotate({ 0.0f, 0.0f, 1.0f });

    if (!Output.HasFocus)
    {
        Output.FocusPosition = { ChassisPos.x, ChassisPos.y, ChassisPos.z };
        Output.HasFocus      = true;
    }

    for (size_t Corner = 0; Corner < 4u; ++Corner)
    {
        const auto& Wheel   = Telemetry.Wheels[Corner];
        const auto& Thermal = Wheel.Thermal;
        const float Side    = (Corner == 0u || Corner == 2u) ? 1.0f : -1.0f; // FL/RL = +Left, FR/RR = -Left

        float TargetSlip = 0.0f;
        if (Wheel.InContact && Wheel.VerticalLoad > 120.0f)
        {
            const float SlipRatio = std::fabs(Wheel.SlipRatio);
            const float ClampedAngle = std::clamp(Wheel.SlipAngleRad, -1.35f, 1.35f);
            const float SlipTangent = std::fabs(std::tan(ClampedAngle));
            const float SlipSpeed = std::max(std::sqrt(Wheel.SlipVelocity.x * Wheel.SlipVelocity.x +
                                                       Wheel.SlipVelocity.y * Wheel.SlipVelocity.y +
                                                       Wheel.SlipVelocity.z * Wheel.SlipVelocity.z),
                                             std::fabs(Wheel.SpinSlipSpeed));
            const float ContactSpeed = std::max(2.0f, std::sqrt(Wheel.ContactVelocity.x * Wheel.ContactVelocity.x +
                                                                 Wheel.ContactVelocity.y * Wheel.ContactVelocity.y +
                                                                 Wheel.ContactVelocity.z * Wheel.ContactVelocity.z));
            // 📝 Kinematic slip catches wheelspin/drift; dissipated contact power catches skids when a low-speed or
            //    relaxed slip telemetry value would otherwise suppress the visible contact-patch smoke.
            const float PowerSlip = std::clamp(Wheel.SlidingPower_W /
                                                std::max(1.0f, Wheel.VerticalLoad * ContactSpeed) * 2.0f,
                                                0.0f, 1.0f);
            const float KinematicSlip = std::max(std::max(SlipRatio, SlipTangent), SlipSpeed * 0.045f);
            const float Metric = std::max(KinematicSlip, PowerSlip);
            const float Onset = std::clamp(Settings.SlipOnset, 0.04f, 0.5f);
            const float Full = std::max(Settings.SlipFull, Onset + 0.08f);
            const float RawSlip = std::clamp((Metric - Onset) / (Full - Onset), 0.0f, 1.0f);
            // Warm rubber increases smoke density, while the floor keeps a cold tyre's visible skid from vanishing.
            const float SurfaceTemp = static_cast<float>(Thermal.SurfaceTemperature);
            const float WarmRubber = std::clamp((SurfaceTemp - 305.0f) / 85.0f, 0.18f, 1.0f);
            TargetSlip = RawSlip * (0.28f + 0.72f * WarmRubber);
        }

        const float Rate      = (TargetSlip > State.SmoothedSlip[Corner]) ? 9.5f : 2.8f;
        State.SmoothedSlip[Corner] += (TargetSlip - State.SmoothedSlip[Corner]) * (1.0f - std::exp(-Rate * Dt));
        const float Intensity = State.SmoothedSlip[Corner];

        // 1. Contact-patch white rubber smoke flung rearward off the spinning tread
        if (Intensity > 0.025f && Output.Sources.size() < MaximumSources)
        {
            const float RimLinear    = 0.34f * Wheel.WheelOmega;
            const float GroundTravel = Wheel.ContactVelocity.x * Forward.x + Wheel.ContactVelocity.y * Forward.y +
                                       Wheel.ContactVelocity.z * Forward.z;
            const float SlipFling    = std::clamp(GroundTravel - RimLinear, -16.0f, 16.0f);
            const float FlingX    = Forward.x * SlipFling * 0.42f + Left.x * Side * (0.55f + Intensity * 0.9f);
            const float FlingY    = Forward.y * SlipFling * 0.42f + Left.y * Side * (0.55f + Intensity * 0.9f);
            const float FlingZ    = 0.55f + Intensity * 1.15f;

            PlumeSourceEntry PatchSource{};
            // 📝 Offset to the outside of the sidewall and just above the patch. Emitting at ContactPoint itself
            //    places the source inside the wheel SDF, where the transport pass correctly crushes its density.
            PatchSource.PositionRadius = {
                Wheel.ContactPoint.x - Forward.x * 0.14f + Left.x * Side * 0.18f + Up.x * 0.08f,
                Wheel.ContactPoint.y - Forward.y * 0.14f + Left.y * Side * 0.18f + Up.y * 0.08f,
                Wheel.ContactPoint.z - Forward.z * 0.14f + Left.z * Side * 0.18f + Up.z * 0.08f,
                0.22f + Intensity * 0.14f
            };
            PatchSource.VelocityRate = {
                Telemetry.Chassis.LinearVelocity.x * 0.25f + FlingX,
                Telemetry.Chassis.LinearVelocity.y * 0.25f + FlingY,
                FlingZ,
                (1.8f + Intensity * 7.4f) * Settings.SmokeScale
            };
            PatchSource.ThermalPayload = {
                (18.0f + Intensity * 65.0f),
                0.0f,
                Thermal.Ignited ? 0.52f : 0.06f,
                5.5f + Intensity * 4.5f
            };
            Output.Sources.push_back(PatchSource);
        }

        // 2. Wheel-arch secondary smoke shedding
        if (Intensity > 0.35f && Output.Sources.size() < MaximumSources)
        {
            PlumeSourceEntry ArchSource{};
            ArchSource.PositionRadius = {
                Wheel.HubPosition.x + Left.x * Side * 0.18f,
                Wheel.HubPosition.y + Left.y * Side * 0.18f,
                Wheel.HubPosition.z + Up.z * 0.16f,
                0.26f
            };
            ArchSource.VelocityRate = {
                Telemetry.Chassis.LinearVelocity.x * 0.35f + Left.x * Side * 1.1f,
                Telemetry.Chassis.LinearVelocity.y * 0.35f + Left.y * Side * 1.1f,
                0.85f + Intensity * 0.9f,
                (0.9f + Intensity * 3.1f) * Settings.SmokeScale
            };
            ArchSource.ThermalPayload = {
                25.0f + Intensity * 40.0f,
                0.0f,
                Thermal.Ignited ? 0.65f : 0.08f,
                3.8f
            };
            Output.Sources.push_back(ArchSource);
        }

        // 3. Burning-rubber ignition: two flame seats (sidewall + trailing tread) + carbonaceous black soot + GI FireLight!
        if (Thermal.Ignited && Thermal.CombustionPower > 0.0)
        {
            const float PowerNorm = std::clamp(static_cast<float>(Thermal.CombustionPower) / 45000.0f, 0.2f, 1.5f);
            for (int Seat = 0; Seat < 2 && Output.Sources.size() < MaximumSources; ++Seat)
            {
                const float BackOffset = (Seat == 0) ? -0.06f : -0.24f;
                const float SideOffset = (Seat == 0) ? 0.16f : 0.06f;
                PlumeSourceEntry FireSeat{};
                FireSeat.PositionRadius = {
                    Wheel.HubPosition.x + Forward.x * BackOffset + Left.x * Side * SideOffset,
                    Wheel.HubPosition.y + Forward.y * BackOffset + Left.y * Side * SideOffset,
                    Wheel.ContactPoint.z + 0.18f + 0.08f * static_cast<float>(Seat),
                    0.20f + 0.06f * PowerNorm
                };
                FireSeat.VelocityRate = {
                    Settings.WindVector[0] * 0.35f + Left.x * Side * 0.45f,
                    Settings.WindVector[1] * 0.35f + Left.y * Side * 0.45f,
                    1.65f + PowerNorm * 1.85f,
                    (2.4f + PowerNorm * 4.6f) * Settings.FireScale
                };
                FireSeat.ThermalPayload = {
                    (320.0f + PowerNorm * 440.0f) * Settings.FireScale,
                    (2.1f + PowerNorm * 2.8f) * Settings.FireScale,
                    0.94f,
                    6.5f
                };
                Output.Sources.push_back(FireSeat);
            }

            PlumeFireLightEntry TyreFire{};
            TyreFire.Position = {
                Wheel.HubPosition.x + Left.x * Side * 0.22f,
                Wheel.HubPosition.y + Left.y * Side * 0.22f,
                Wheel.ContactPoint.z + 0.32f
            };
            TyreFire.Intensity = (34.0f + PowerNorm * 92.0f) * Settings.FireScale;
            TyreFire.Colour    = { 1.0f, 0.44f, 0.12f };
            TyreFire.Reach     = 16.0f;
            Output.FireLights.push_back(TyreFire);
        }
    }

    // 4. Exhaust overrun afterfire pop on throttle lift at high RPM
    const float Speed = std::hypot(Telemetry.Chassis.LinearVelocity.x, Telemetry.Chassis.LinearVelocity.y);
    if (State.PreviousThrottle > 0.65f && ThrottleInput < 0.08f && Telemetry.EngineRPM > 4600.0f &&
        Speed > 5.0f && State.AfterfireCooldown <= 0.0f)
    {
        PlumeTransientBurst Pop{};
        Pop.Position      = { ChassisPos.x - Forward.x * 2.15f + Left.x * 0.42f,
                              ChassisPos.y - Forward.y * 2.15f + Left.y * 0.42f,
                              ChassisPos.z + 0.28f };
        Pop.Velocity      = { Telemetry.Chassis.LinearVelocity.x - Forward.x * 9.5f,
                              Telemetry.Chassis.LinearVelocity.y - Forward.y * 9.5f,
                              0.45f };
        Pop.Radius        = 0.20f;
        Pop.SmokeRate     = 3.2f;
        Pop.HeatRate      = 680.0f;
        Pop.FlameRate     = 3.8f;
        Pop.Soot          = 0.45f;
        Pop.VelocityBlend = 12.0f;
        Pop.Duration      = 0.14f;
        Pop.LightPower    = 42.0f;
        QueueBurst(Pop);
        State.AfterfireCooldown = 0.65f;
    }
    State.PreviousThrottle = ThrottleInput;
}

void PlumeSourceProjector::ProjectFluidPlume(uint64_t RecordKey, const std::array<float, 3>& WorldPosition, float DomainScale,
                                             const FluidParameters& Parameters, bool TriggerOnFirstSight,
                                             uint32_t BurstGeneration, FluidBurstPattern Pattern,
                                             float ElapsedSeconds, PlumeProjectionOutput& Output)
{
    const float Scale = std::clamp(DomainScale, 0.25f, 8.0f);
    if (!Output.HasFocus)
    {
        Output.FocusPosition = WorldPosition;
        Output.HasFocus      = true;
    }

    PlumeRecordState& State = RecordStates_[RecordKey];
    const bool FireBurst    = (!State.Initialized && TriggerOnFirstSight) ||
                              (State.Initialized && BurstGeneration != State.LastBurstGeneration);
    State.Initialized         = true;
    State.LastBurstGeneration = BurstGeneration;

    if (FireBurst)
    {
        const float BaseRadius = std::max(0.24f, Parameters.BlastRadius * Parameters.BoundsWidth * Scale * 1.35f);
        const bool  HasFire    = Parameters.BlastFuel > 0.05f || Parameters.BlastTemperature > 2.5f;
        const float SootFrac   = HasFire ? 0.78f : 0.16f;
        auto SpawnOne = [&](std::array<float, 3> Offset, std::array<float, 3> Impulse, float Delay, float RadiusMul) {
            PlumeTransientBurst Burst{};
            Burst.Position      = { WorldPosition[0] + Offset[0], WorldPosition[1] + Offset[1], WorldPosition[2] + Offset[2] };
            Burst.Velocity      = Impulse;
            Burst.Radius        = BaseRadius * RadiusMul;
            Burst.SmokeRate     = (4.5f + Parameters.BlastSmoke * 2.6f);
            Burst.HeatRate      = HasFire ? (380.0f + Parameters.BlastTemperature * 125.0f) : 45.0f;
            Burst.FlameRate     = HasFire ? (1.8f + Parameters.BlastFuel * 0.85f) : 0.0f;
            Burst.Soot          = SootFrac;
            Burst.VelocityBlend = 11.0f;
            Burst.Duration      = 0.34f;
            Burst.StartDelay    = Delay;
            Burst.LightPower    = HasFire ? (85.0f + Parameters.BlastStrength * 14.0f) : 0.0f;
            QueueBurst(Burst);
        };

        if (Pattern == FluidBurstPattern::ClusterSalvo)
        {
            const std::array<std::array<float, 3>, 4> Offsets = { {
                { -0.45f * Scale, -0.42f * Scale, 0.28f * Scale },
                {  0.48f * Scale, -0.38f * Scale, 0.32f * Scale },
                { -0.40f * Scale,  0.46f * Scale, 0.36f * Scale },
                {  0.44f * Scale,  0.42f * Scale, 0.40f * Scale }
            } };
            for (size_t Idx = 0; Idx < 4u; ++Idx)
            {
                SpawnOne(Offsets[Idx], { Offsets[Idx][0] * 2.2f, Offsets[Idx][1] * 2.2f, Parameters.BlastStrength * 0.55f },
                         static_cast<float>(Idx) * 0.16f, 0.88f);
            }
        }
        else if (Pattern == FluidBurstPattern::CollidingWaves)
        {
            SpawnOne({ -0.65f * Scale, 0.0f, 0.45f * Scale }, {  9.5f, 0.0f, 2.2f }, 0.0f, 0.85f);
            SpawnOne({  0.65f * Scale, 0.0f, 0.45f * Scale }, { -9.5f, 0.0f, 2.2f }, 0.03f, 0.85f);
        }
        else if (Pattern == FluidBurstPattern::MushroomCloud)
        {
            SpawnOne({ 0.0f, 0.0f, 0.25f * Scale }, { 0.0f, 0.0f, Parameters.BlastStrength * 0.72f }, 0.0f, 1.18f);
            SpawnOne({ 0.0f, 0.0f, 0.78f * Scale }, { 0.0f, 0.0f, Parameters.BlastStrength * 0.62f }, 0.15f, 0.96f);
        }
        else
        {
            SpawnOne({ 0.0f, 0.0f, 0.35f * Scale }, { 0.0f, 0.0f, Parameters.BlastStrength * 0.58f }, 0.0f, 1.0f);
            if (Pattern == FluidBurstPattern::Shrapnel || Parameters.ShrapnelEnabled)
            {
                for (int Frag = 0; Frag < 4; ++Frag)
                {
                    const float Angle = static_cast<float>(Frag) * 1.5707963f + 0.38f;
                    SpawnOne({ std::cos(Angle) * 0.32f * Scale, std::sin(Angle) * 0.32f * Scale, 0.48f * Scale },
                             { std::cos(Angle) * 6.8f, std::sin(Angle) * 6.8f, 5.4f }, 0.06f, 0.55f);
                }
            }
        }
    }

    if (Parameters.SourceEnabled && Output.Sources.size() < MaximumSources)
    {
        const float SourceZ   = WorldPosition[2] + Parameters.SourceHeight * Parameters.BoundsHeight * Scale;
        const float Radius    = std::max(0.18f, Parameters.SourceRadius * Parameters.BoundsWidth * Scale * 1.4f);
        const bool  HasFlame  = Parameters.SourceFuel > 0.05f && Parameters.SourceTemperature > 1.0f;
        const float SwirlAng  = ElapsedSeconds * std::max(1.2f, Parameters.SourceSwirl);
        const float SwirlPush = Parameters.SourceSwirl * 0.55f;

        PlumeSourceEntry Continuous{};
        Continuous.PositionRadius = { WorldPosition[0], WorldPosition[1], SourceZ, Radius };
        Continuous.VelocityRate   = {
            Parameters.WindX + std::cos(SwirlAng) * SwirlPush,
            Parameters.WindZ + std::sin(SwirlAng) * SwirlPush,
            Parameters.SourceUpwardVelocity * 0.72f,
            std::max(0.4f, Parameters.SourceSmoke * Parameters.SourceRate * 2.1f)
        };
        Continuous.ThermalPayload = {
            Parameters.SourceTemperature * Parameters.SourceRate * 135.0f,
            HasFlame ? (Parameters.SourceFuel * Parameters.SourceRate * 0.95f) : 0.0f,
            HasFlame ? std::clamp(Parameters.SootGeneration * 0.42f, 0.18f, 0.92f) : 0.12f,
            5.8f
        };
        Output.Sources.push_back(Continuous);

        if (HasFlame)
        {
            PlumeFireLightEntry PlumeLight{};
            PlumeLight.Position  = { WorldPosition[0], WorldPosition[1], SourceZ + 0.45f * Scale };
            PlumeLight.Intensity = (28.0f + Parameters.FireIntensity * 14.5f * Parameters.SourceRate) * Scale;
            PlumeLight.Colour    = ResolvePaletteTint(Parameters.ColorPalette);
            PlumeLight.Reach     = std::clamp(14.0f * Scale, 10.0f, 32.0f);
            Output.FireLights.push_back(PlumeLight);
        }
    }
}

void PlumeSourceProjector::FinalizeStep(PlumeProjectionOutput& Output) const noexcept
{
    for (const auto& Burst : ActiveBursts_)
    {
        if (Burst.StartDelay > 0.0f) continue;
        const float Progress = std::clamp(Burst.Age / std::max(Burst.Duration, 0.02f), 0.0f, 1.0f);
        const float Envelope = 1.0f - Progress * Progress;
        if (Output.Sources.size() < MaximumSources)
        {
            PlumeSourceEntry Impulse{};
            Impulse.PositionRadius = {
                Burst.Position[0] + Burst.Velocity[0] * Burst.Age * 0.28f,
                Burst.Position[1] + Burst.Velocity[1] * Burst.Age * 0.28f,
                Burst.Position[2] + Burst.Velocity[2] * Burst.Age * 0.28f,
                Burst.Radius * (0.85f + 0.65f * Progress)
            };
            Impulse.VelocityRate = {
                Burst.Velocity[0] * Envelope,
                Burst.Velocity[1] * Envelope,
                Burst.Velocity[2] * Envelope,
                Burst.SmokeRate * Envelope
            };
            Impulse.ThermalPayload = {
                Burst.HeatRate * Envelope,
                Burst.FlameRate * Envelope,
                Burst.Soot,
                Burst.VelocityBlend * Envelope
            };
            Output.Sources.push_back(Impulse);
        }
        if (Burst.LightPower > 0.0f)
        {
            PlumeFireLightEntry Flash{};
            Flash.Position  = { Burst.Position[0], Burst.Position[1], Burst.Position[2] + 0.4f };
            Flash.Intensity = Burst.LightPower * Envelope;
            Flash.Colour    = { 1.0f, 0.56f, 0.20f };
            Flash.Reach     = 22.0f;
            Output.FireLights.push_back(Flash);
        }
    }

    // Power-weighted primary FireLight for RefreshExternalInputs::FireBlock (DirectRadiance + CardDirect + RayIntersection)
    double WeightSum = 0.0;
    double PosX      = 0.0;
    double PosY      = 0.0;
    double PosZ      = 0.0;
    double ColR      = 0.0;
    double ColG      = 0.0;
    double ColB      = 0.0;
    float  MaxReach  = 14.0f;
    for (const auto& Light : Output.FireLights)
    {
        if (Light.Intensity <= 0.0f) continue;
        const double W = static_cast<double>(Light.Intensity);
        WeightSum += W;
        PosX      += W * Light.Position[0];
        PosY      += W * Light.Position[1];
        PosZ      += W * Light.Position[2];
        ColR      += W * Light.Colour[0];
        ColG      += W * Light.Colour[1];
        ColB      += W * Light.Colour[2];
        MaxReach   = std::max(MaxReach, Light.Reach);
    }
    if (WeightSum > 1.0e-3)
    {
        Output.PrimaryFireLight.Position  = { static_cast<float>(PosX / WeightSum),
                                              static_cast<float>(PosY / WeightSum),
                                              static_cast<float>(PosZ / WeightSum) };
        Output.PrimaryFireLight.Intensity = static_cast<float>(std::min(WeightSum, 480.0));
        Output.PrimaryFireLight.Colour    = { static_cast<float>(ColR / WeightSum),
                                              static_cast<float>(ColG / WeightSum),
                                              static_cast<float>(ColB / WeightSum) };
        Output.PrimaryFireLight.Reach     = MaxReach;
    }
    else
    {
        Output.PrimaryFireLight = {};
    }
}

} // namespace Frontier::FluidDynamics
