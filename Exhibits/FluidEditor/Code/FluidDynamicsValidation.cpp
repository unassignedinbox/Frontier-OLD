/*==========================================================================================================================================
                                                      FLUIDDYNAMICSVALIDATION.CPP
==========================================================================================================================================*/
// 🧩 Deterministic CTest validation for FrontierFluidDynamics, .fluidspace / .gasscene.toml codec parity across all 8 upstream
//    samples, all 18 pyrotechnic presets, PlumeSourceProjection tyre smoke/fire + world FluidPlume projection, explicit tyre
//    no-contact/zero-slip/wheelspin/decay cases, and WorldSpace FluidPlumeRecord TOML persistence.

#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"
#include "Engine/PhysicalDynamics/FluidDynamics/FluidSpaceCodec.h"
#include "Engine/PhysicalDynamics/FluidDynamics/PlumeSourceProjection.h"
#include "Engine/Scene/EngineConfigurations.h"
#include "Engine/Scene/WorldSpace.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{

void Require(bool Condition, const std::string& Message)
{
    if (!Condition)
    {
        throw std::runtime_error(Message);
    }
}

bool ApproxEqual(float Left, float Right, float Epsilon = 1.0e-4f)
{
    return std::fabs(Left - Right) <= Epsilon;
}

} // namespace

int main(int ArgumentCount, char** Arguments)
{
    using namespace Frontier;
    try
    {
        Require(ArgumentCount >= 3, "usage: FluidDynamicsValidation <Experimental/Fluid/Samples> <Projects/Everest/Content/FluidSpaces>");
        const std::filesystem::path SamplesDir     = std::filesystem::absolute(Arguments[1]);
        const std::filesystem::path FluidSpacesDir = std::filesystem::absolute(Arguments[2]);

        // 1. Verify all 18 presets and all 8 sample constructors
        const auto Presets = FluidDynamics::EnumerateFluidPresets();
        Require(Presets.size() == 18u, "Expected 18 fluid presets, got " + std::to_string(Presets.size()));
        for (const auto& Entry : Presets)
        {
            Require(FluidDynamics::FindFluidPreset(Entry.Identity) != nullptr,
                    std::string("FindFluidPreset failed for ") + Entry.Identity);
            const auto P = FluidDynamics::ConstructPresetParameters(Entry.Identity);
            Require(P.GridResolution >= 16 && P.GridResolution <= 128, "Preset GridResolution out of bounds");
            Require(P.RaymarchSteps >= 16 && P.RaymarchSteps <= 256, "Preset RaymarchSteps out of bounds");
        }

        const auto Samples = FluidDynamics::EnumerateSampleFluidScenes();
        Require(Samples.size() == 8u, "Expected 8 fluid samples, got " + std::to_string(Samples.size()));
        for (const auto& Sample : Samples)
        {
            const auto Doc = FluidDynamics::ConstructSampleFluidScene(Sample.Identity);
            Require(!Doc.Name.empty(), std::string("Sample scene name empty for ") + Sample.Identity);
        }

        // 2. Lossless round-trip across all 8 Experimental/Fluid/Samples/*.gasscene.toml AND Projects/Everest/Content/FluidSpaces/*.fluidspace
        for (const auto& Dir : { SamplesDir, FluidSpacesDir })
        {
            int Count = 0;
            for (const auto& Item : std::filesystem::directory_iterator(Dir))
            {
                if (!Item.is_regular_file())
                {
                    continue;
                }
                const auto Name = Item.path().filename().string();
                if (!Name.ends_with(".gasscene.toml") && !Name.ends_with(".fluidspace"))
                {
                    continue;
                }

                FluidDynamics::FluidSceneDocument First{};
                std::string                       Err;
                Require(FluidDynamics::LoadFluidSpaceFile(Item.path(), First, &Err),
                        "LoadFluidSpaceFile failed on " + Name + ": " + Err);

                const std::string Serialized = FluidDynamics::WriteFluidSpaceToml(First);
                FluidDynamics::FluidSceneDocument Second{};
                Require(FluidDynamics::ReadFluidSpaceToml(Serialized, Second, &Err),
                        "ReadFluidSpaceToml failed on " + Name + ": " + Err);

                Require(First.Name == Second.Name, "Document name mismatch on " + Name);
                Require(First.Parameters.GridResolution == Second.Parameters.GridResolution, "GridResolution mismatch on " + Name);
                Require(ApproxEqual(First.Parameters.Buoyancy, Second.Parameters.Buoyancy), "Buoyancy mismatch on " + Name);
                Require(ApproxEqual(First.Parameters.VorticityConfinement, Second.Parameters.VorticityConfinement),
                        "VorticityConfinement mismatch on " + Name);
                Require(ApproxEqual(First.Parameters.BurnRate, Second.Parameters.BurnRate), "BurnRate mismatch on " + Name);
                Require(ApproxEqual(First.Parameters.FireIntensity, Second.Parameters.FireIntensity), "FireIntensity mismatch on " + Name);
                Require(First.Parameters.ObstacleCategory == Second.Parameters.ObstacleCategory, "ObstacleCategory mismatch on " + Name);
                Require(ApproxEqual(First.Camera.TargetDistance, Second.Camera.TargetDistance), "Camera TargetDistance mismatch on " + Name);
                ++Count;
            }
            Require(Count == 8, "Expected 8 fluid scene files in " + Dir.string() + ", found " + std::to_string(Count));
        }

        // 3. Verify FluidOrbitCamera projection/inverse & raycast hit inside [-1, 1]^3
        {
            FluidDynamics::FluidOrbitCamera Cam{};
            Cam.Reset();
            Cam.SnapToTarget();
            const auto Hit = Cam.RaycastVolumeUvw(0.5f, 0.5f, 16.0f / 9.0f, { 1.0f, 1.0f, 1.0f });
            Require(Hit.has_value(), "Center camera ray should intersect [-1, 1]^3 fluid volume");
            Require(Hit->at(0) >= 0.0f && Hit->at(0) <= 1.0f, "Raycast U out of [0, 1]");
        }

        // 4. Verify PlumeSourceProjector: tyre burnout smoke -> hot tyre fire + world FluidPlume burst & FireLight
        {
            Vehicle::VehicleTelemetry Telemetry{};
            Telemetry.Chassis.Position                 = { -38.0f, 0.0f, 0.5f };
            Telemetry.SpeedMetresPerSecond             = 6.5f;
            Telemetry.EngineRPM                        = 6500.0f;
            Telemetry.Wheels[2].InContact              = true;
            Telemetry.Wheels[2].ContactPoint           = { -39.3f, 0.78f, 0.02f };
            Telemetry.Wheels[2].SlipRatio              = 0.85f;
            Telemetry.Wheels[2].SlipAngleRad           = 0.25f;
            Telemetry.Wheels[2].VerticalLoad           = 4200.0f;
            Telemetry.Wheels[2].Thermal.SurfaceTemperature = 435.0f; // Overheated burning tyre!

            FluidDynamics::PlumeSourceProjector Projector;
            FluidDynamics::PlumeSettings        Settings{};
            FluidDynamics::PlumeProjectionOutput Output{};

            Projector.BeginStep(0.033f);
            Projector.ProjectVehicle(1u, Telemetry, 1.0f, Settings, 0.033f, Output);

            const auto HeroParams = FluidDynamics::ConstructPresetParameters("tactical_ordnance");
            Projector.ProjectFluidPlume(77u, { -28.0f, 3.1f, 0.15f }, 1.65f, HeroParams, true, 1u,
                                        FluidDynamics::FluidBurstPattern::MushroomCloud, 0.033f, Output);
            Projector.FinalizeStep(Output);

            Require(!Output.Sources.empty(), "Expected active PlumeSourceEntry items from burnout wheel + FluidPlume");
            Require(!Output.FireLights.empty(), "Expected active ReSTIR FireLights from hot tyre + combustion FluidPlume");
            Require(Output.PrimaryFireLight.Intensity > 0.0f, "Expected non-zero PrimaryFireLight intensity");
        }

        // 5. A moderate non-burning lateral skid must emit smoke at the loaded tyre contact patch.
        {
            Vehicle::VehicleTelemetry Telemetry{};
            Telemetry.Chassis.Position = { 2.0f, 0.0f, 0.45f };
            Telemetry.Chassis.LinearVelocity = { 8.0f, 0.0f, 0.0f };
            auto& Wheel = Telemetry.Wheels[0];
            Wheel.InContact = true;
            Wheel.VerticalLoad = 3000.0f;
            Wheel.ContactPoint = { 2.0f, 0.8f, 0.04f };
            Wheel.HubPosition = { 2.0f, 0.8f, 0.36f };
            Wheel.ContactVelocity = { 8.0f, 0.0f, 0.0f };
            Wheel.SlipAngleRad = 0.12f;
            Wheel.SlidingPower_W = 5000.0f;
            Wheel.Thermal.SurfaceTemperature = 340.0f;

            FluidDynamics::PlumeSourceProjector Projector;
            FluidDynamics::PlumeSettings Settings{};
            FluidDynamics::PlumeProjectionOutput Output{};
            Projector.BeginStep(1.0f / 30.0f);
            Projector.ProjectVehicle(42u, Telemetry, 0.0f, Settings, 1.0f / 30.0f, Output);
            Require(!Output.Sources.empty(), "Expected visible tyre smoke from moderate lateral sliding power");
            const auto& ContactSmoke = Output.Sources.front();
            Require(std::fabs(ContactSmoke.PositionRadius[0] - (Wheel.ContactPoint.x - 0.14f)) < 1.0e-4f &&
                    std::fabs(ContactSmoke.PositionRadius[1] - (Wheel.ContactPoint.y + 0.18f)) < 1.0e-4f &&
                    std::fabs(ContactSmoke.PositionRadius[2] - (Wheel.ContactPoint.z + 0.08f)) < 1.0e-4f,
                    "Expected tyre smoke to be emitted at the wheel's contact patch");
            Require(ContactSmoke.VelocityRate[3] > 0.0f && ContactSmoke.ThermalPayload[1] == 0.0f,
                    "Expected non-burning skid smoke instead of a flame source");
        }

        // 6. Zero slip and missing contact must not create tyre smoke, even if an unweighted wheel reports large slip values.
        {
            FluidDynamics::PlumeSourceProjector Projector;
            FluidDynamics::PlumeSettings Settings{};
            Vehicle::VehicleTelemetry Telemetry{};
            auto& Wheel = Telemetry.Wheels[0];
            Wheel.SlipRatio = 0.9f;
            Wheel.SlipAngleRad = 0.55f;
            Wheel.SpinSlipSpeed = 12.0f;
            Wheel.SlidingPower_W = 12000.0f;
            Wheel.InContact = false;
            Wheel.VerticalLoad = 2500.0f;

            FluidDynamics::PlumeProjectionOutput NoContact{};
            Projector.BeginStep(1.0f / 30.0f);
            Projector.ProjectVehicle(1001u, Telemetry, 0.0f, Settings, 1.0f / 30.0f, NoContact);
            Require(NoContact.Sources.empty() && NoContact.FireLights.empty(),
                    "a wheel without contact must not emit smoke or fire despite high vertical load and extreme slip telemetry");

            Wheel.InContact = true;
            Wheel.VerticalLoad = 0.0f;
            FluidDynamics::PlumeProjectionOutput NoLoad{};
            Projector.BeginStep(1.0f / 30.0f);
            Projector.ProjectVehicle(1002u, Telemetry, 0.0f, Settings, 1.0f / 30.0f, NoLoad);
            Require(NoLoad.Sources.empty() && NoLoad.FireLights.empty(),
                    "a wheel without vertical load must not emit smoke or fire despite contact and extreme slip telemetry");

            Wheel.VerticalLoad = 2500.0f;
            Wheel.SlipRatio = 0.0f;
            Wheel.SlipAngleRad = 0.0f;
            Wheel.SlipVelocity = {};
            Wheel.SpinSlipSpeed = 0.0f;
            Wheel.SlidingPower_W = 0.0f;
            Wheel.ContactVelocity = {};
            FluidDynamics::PlumeProjectionOutput ZeroSlip{};
            Projector.BeginStep(1.0f / 30.0f);
            Projector.ProjectVehicle(1003u, Telemetry, 0.0f, Settings, 1.0f / 30.0f, ZeroSlip);
            Require(ZeroSlip.Sources.empty() && ZeroSlip.FireLights.empty(),
                    "a loaded wheel with zero longitudinal/lateral slip and zero dissipated power must not emit tyre effects");
        }

        // 7. Longitudinal wheelspin emits contact-patch smoke, which fades out once the wheel returns to zero slip.
        {
            constexpr float Delta = 1.0f / 30.0f;
            FluidDynamics::PlumeSourceProjector Projector;
            FluidDynamics::PlumeSettings Settings{};
            Vehicle::VehicleTelemetry Telemetry{};
            Telemetry.Chassis.Position = { 0.0f, 0.0f, 0.5f };
            Telemetry.Chassis.LinearVelocity = { 6.0f, 0.0f, 0.0f };
            auto& Wheel = Telemetry.Wheels[0];
            Wheel.InContact = true;
            Wheel.VerticalLoad = 3200.0f;
            Wheel.ContactPoint = { 0.0f, 0.8f, 0.03f };
            Wheel.HubPosition = { 0.0f, 0.8f, 0.36f };
            Wheel.ContactVelocity = { 6.0f, 0.0f, 0.0f };
            Wheel.WheelOmega = 24.0f;
            Wheel.SlipRatio = 0.82f;
            Wheel.Thermal.SurfaceTemperature = 320.0;

            FluidDynamics::PlumeProjectionOutput Burnout{};
            Projector.BeginStep(Delta);
            Projector.ProjectVehicle(2001u, Telemetry, 0.0f, Settings, Delta, Burnout);
            Require(!Burnout.Sources.empty() && Burnout.Sources.front().VelocityRate[3] > 0.0f &&
                        Burnout.Sources.front().ThermalPayload[1] == 0.0f && Burnout.FireLights.empty(),
                    "loaded longitudinal wheelspin must emit non-burning contact-patch smoke");
            const float InitialRate = Burnout.Sources.front().VelocityRate[3];

            Wheel.SlipRatio = 0.0f;
            Wheel.SlipAngleRad = 0.0f;
            Wheel.SpinSlipSpeed = 0.0f;
            Wheel.SlidingPower_W = 0.0f;
            Wheel.SlipVelocity = {};
            bool  ObservedDecay = false;
            bool  ReachedNoSmoke = false;
            float FirstDecayRate = InitialRate;
            for (int Step = 0; Step < 90; ++Step)
            {
                FluidDynamics::PlumeProjectionOutput Fade{};
                Projector.BeginStep(Delta);
                Projector.ProjectVehicle(2001u, Telemetry, 0.0f, Settings, Delta, Fade);
                if (Step == 0 && !Fade.Sources.empty())
                {
                    FirstDecayRate = Fade.Sources.front().VelocityRate[3];
                    ObservedDecay = FirstDecayRate < InitialRate;
                }
                if (Fade.Sources.empty())
                {
                    ReachedNoSmoke = true;
                    break;
                }
            }
            Require(ObservedDecay, "tyre smoke rate should decrease immediately after longitudinal slip ends");
            Require(ReachedNoSmoke, "tyre smoke should decay below its emission threshold after sustained zero slip");
        }

        // 8. Verify WorldSpace FluidPlumeRecord creation & TOML round-trip persistence
        {
            WorldSpace        Original;
            const RecordToken Token   = Original.ConstructRecord("FluidPlume", "Rig Alpha");
            auto*             Created = dynamic_cast<FluidPlumeRecord*>(Original.FindRecord(Token));
            Require(Created != nullptr, "WorldSpace::ConstructRecord(\"FluidPlume\") failed");
            Created->AccessConfiguration().PositionMetres[0]   = -25.5f;
            Created->AccessConfiguration().PositionMetres[1]   = 3.4f;
            Created->AccessConfiguration().PositionMetres[2]   = 0.20f;
            Created->AccessConfiguration().DomainScale         = 1.75f;
            Created->AccessConfiguration().PresetIdentity      = "megaton_open_bounds";
            Created->AccessConfiguration().SpaceFile           = "hero_detonation.fluidspace";
            Created->AccessConfiguration().BurstPattern        = 4u;
            Created->AccessConfiguration().BurstGeneration     = 3u;
            Created->AccessConfiguration().BlackbodyCoreKelvin = 2450.0f;

            const auto  TempLevel = std::filesystem::temp_directory_path() / "frontier_fluid_plume_roundtrip.levelspace";
            std::string Diag;
            Require(Original.SaveLevelSpace(TempLevel, &Diag), "WorldSpace::SaveLevelSpace failed: " + Diag);

            WorldSpace Reloaded;
            Require(Reloaded.LoadLevelSpace(TempLevel, &Diag), "WorldSpace::LoadLevelSpace failed: " + Diag);
            std::filesystem::remove(TempLevel);

            const auto* Restored = dynamic_cast<const FluidPlumeRecord*>(Reloaded.FindFirstByType("FluidPlume"));
            Require(Restored != nullptr, "Reloaded WorldSpace missing FluidPlumeRecord");
            const auto& C = Restored->QueryConfiguration();
            Require(ApproxEqual(C.PositionMetres[0], -25.5f) && ApproxEqual(C.PositionMetres[1], 3.4f),
                    "FluidPlumeRecord PositionMetres did not round-trip");
            Require(ApproxEqual(C.DomainScale, 1.75f), "FluidPlumeRecord DomainScale did not round-trip");
            Require(C.PresetIdentity == "megaton_open_bounds", "FluidPlumeRecord PresetIdentity did not round-trip");
            Require(C.SpaceFile == "hero_detonation.fluidspace", "FluidPlumeRecord SpaceFile did not round-trip");
            Require(C.BurstPattern == 4u, "FluidPlumeRecord BurstPattern did not round-trip");
            Require(ApproxEqual(C.BlackbodyCoreKelvin, 2450.0f), "FluidPlumeRecord BlackbodyCoreKelvin did not round-trip");
        }

        std::printf("[FluidDynamicsValidation] All 18 presets, 8 sample codecs, PlumeSourceProjection, and WorldSpace checks PASSED.\n");
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::fprintf(stderr, "[FluidDynamicsValidation] FAILED: %s\n", Error.what());
        return 1;
    }
}
