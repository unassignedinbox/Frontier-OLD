/*==========================================================================================================================================
                                                      FLUIDSCENESPECIFICATION.H
==========================================================================================================================================*/
// 🧩 Pure engine gas & combustion scene parameters, 18 presets, 8 parity sample scenes, and orbit camera (Experimental/Fluid 1:1).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Frontier::FluidDynamics
{

enum class FluidInteractionTool : uint8_t
{
    Orbit    = 0,
    Brush    = 1,
    Detonate = 2
};

enum class FluidBurstPattern : uint8_t
{
    SingleBurst    = 0,
    Shrapnel       = 1,
    ClusterSalvo   = 2,
    CollidingWaves = 3,
    MushroomCloud  = 4
};

enum class FluidPresetCategory : uint8_t
{
    All   = 0,
    Fire  = 1,
    Smoke = 2,
    Blast = 3
};

struct FluidParameters
{
    int                  GridResolution       = 48;    // [cell]
    int                  PressureIterations   = 22;    // [-]
    float                TimeScale            = 1.0f;  // [-]
    bool                 Paused               = false;
    bool                 MacCormackAdvection  = true;
    bool                 EnclosedBox          = false;
    float                VoxelQuantization    = 0.0f;  // [-]
    bool                 AutoGpuGovernor      = false;
    float                BoundsWidth          = 1.85f; // [m]
    float                BoundsHeight         = 2.10f; // [m]
    bool                 DynamicBounds        = true;
    float                DynamicBoundsMax     = 1.55f; // [-]
    FluidInteractionTool InteractionTool      = FluidInteractionTool::Orbit;
    bool                 AutoTurntable        = false;

    float                VorticityConfinement = 7.8f;  // [-]
    float                Buoyancy             = 5.8f;  // [m/s²]
    float                SmokeWeight          = 1.15f; // [m/s²]
    float                BurnRate             = 1.85f; // [1/s]
    float                BurnHeat             = 2.6f;  // [K]
    float                SootGeneration       = 1.55f; // [-]
    float                CombustionExpansion  = 4.4f;  // [-]
    float                CoolingRate          = 1.35f; // [1/s]
    float                SmokeDissipation     = 0.18f; // [1/s]
    float                VelocityDamping      = 0.07f; // [1/s]
    float                TurbulenceStrength   = 3.5f;  // [-]
    float                TurbulenceScale      = 3.8f;  // [-]
    float                WindX                = 0.22f; // [m/s]
    float                WindZ                = 0.0f;  // [m/s]

    bool                 SourceEnabled        = true;
    float                SourceRate           = 1.0f;  // [1/s]
    float                SourceRadius         = 0.10f; // [-]
    float                SourceHeight         = 0.07f; // [-]
    float                SourceUpwardVelocity = 3.2f;  // [m/s]
    float                SourceSwirl          = 1.2f;  // [m/s]
    float                SourceTemperature    = 3.5f;  // [-]
    float                SourceFuel           = 2.8f;  // [-]
    float                SourceSmoke          = 1.4f;  // [-]

    float                BlastStrength        = 9.8f;  // [m/s]
    float                BlastRadius          = 0.18f; // [-]
    float                BlastTemperature     = 5.4f;  // [-]
    float                BlastFuel            = 4.6f;  // [-]
    float                BlastSmoke           = 2.6f;  // [-]
    int                  BlastLobes           = 6;     // [-]
    bool                 ShrapnelEnabled      = true;

    int                  RenderChannel        = 0;     // [-]
    int                  ColorPalette         = 0;     // [-]
    float                RenderScale          = 0.8f;  // [-]
    int                  RaymarchSteps        = 72;    // [-]
    int                  ShadowSteps          = 6;     // [-]
    float                DensityExtinction    = 18.5f; // [1/m]
    float                SmokeAlbedo          = 0.22f; // [-]
    float                ShadowDensity        = 11.5f; // [-]
    float                FireIntensity        = 5.6f;  // [-]
    float                TemperatureScale     = 1.15f; // [-]
    float                InternalScattering   = 2.5f;  // [-]
    float                PhaseAnisotropy      = 0.38f; // [-]
    float                AmbientIntensity     = 0.35f; // [-]
    float                SunIntensity         = 2.2f;  // [-]
    float                SunAzimuth           = 42.0f; // [°]
    float                SunElevation         = 52.0f; // [°]
    float                Exposure             = 1.25f; // [-]
    float                BloomIntensity       = 0.55f; // [-]
    float                GodRaysIntensity     = 0.45f; // [-]
    float                ShockwaveStrength    = 1.0f;  // [-]
    int                  SliceAxis            = 2;     // [-] 0 = X, 1 = Y, 2 = Z
    float                SlicePosition        = 0.5f;  // [-]

    int                  ObstacleCategory     = 0;     // [-] 0..5
    float                ObstacleX            = 0.5f;  // [-]
    float                ObstacleY            = 0.26f; // [-]
    float                ObstacleZ            = 0.5f;  // [-]
    float                ObstacleRadius       = 0.16f; // [-]
    bool                 ColliderAutoMove     = false;
    float                ColliderSpeed        = 1.25f; // [rad/s]

    bool                 ShowBoundingBox      = false;
    bool                 ShowVoxelGridLines   = false;
    bool                 ShowActiveVoxelCells = false;
    bool                 ShowFloorGrid        = true;
    bool                 ShowEmbers           = true;
    int                  EmberCount           = 550;   // [-]
    float                EmberSize            = 1.0f;  // [-]
    float                EmberIntensity       = 1.4f;  // [-]
    float                EmberLifetime        = 1.0f;  // [-]
    float                EmberAshiness        = 0.0f;  // [-]
    bool                 ShowAtlasMinimap     = false;
    int                  AtlasMinimapField    = 0;     // [-]

    static FluidParameters ConstructUpstreamDefaults() noexcept;
    static FluidParameters ConstructInitialParameters() noexcept;
    void                   ClampToValidRanges() noexcept;
};

struct FluidObjectNames
{
    std::string Domain   = "Gas domain";
    std::string Source   = "Fire emitter";
    std::string Collider = "Sphere collider";
    std::string Sun      = "Directional light";
};

class FluidOrbitCamera
{
  public:
    float                Theta          = 0.62f;                     // [rad]
    float                Phi            = 1.26f;                     // [rad]
    float                Distance       = 3.35f;                     // [m]
    std::array<float, 3> Centre         = { 0.0f, 0.22f, 0.0f };     // [m]

    float                TargetTheta    = 0.62f;                     // [rad]
    float                TargetPhi      = 1.26f;                     // [rad]
    float                TargetDistance = 4.10f;                     // [m]
    std::array<float, 3> TargetCentre   = { 0.0f, 0.30f, 0.0f };     // [m]

    float                FieldOfViewY   = 0.80285146f;               // [rad] 46°
    float                NearClip       = 0.08f;                     // [m]
    float                FarClip        = 60.0f;                     // [m]
    bool                 AutoTurntable  = false;
    float                TurntableSpeed = 0.22f;                     // [rad/s]

    void                                         SnapToTarget() noexcept;
    void                                         Orbit(float DeltaTheta, float DeltaPhi) noexcept;
    void                                         Dolly(float WheelDelta) noexcept;
    void                                         Pan(float DeltaX, float DeltaY, float ViewportHeight) noexcept;
    void                                         Reset() noexcept;
    void                                         ConfigureViewPreset(std::string_view Preset) noexcept;
    void                                         Advance(float DeltaSeconds) noexcept;

    [[nodiscard]] std::array<float, 3>           QueryEyePosition() const noexcept;
    [[nodiscard]] std::array<float, 16>          QueryViewMatrix() const noexcept;
    [[nodiscard]] std::array<float, 16>          QueryProjectionMatrix(float Aspect) const noexcept;
    [[nodiscard]] std::array<float, 16>          QueryViewProjectionMatrix(float Aspect) const noexcept;
    [[nodiscard]] static std::array<float, 16>   InvertMatrix(const std::array<float, 16>& Matrix) noexcept;
    [[nodiscard]] std::optional<std::array<float, 3>> RaycastVolumeUvw(float NormalizedX, float NormalizedY, float Aspect,
                                                                       const std::array<float, 3>& HalfBox = { 1.0f, 1.0f, 1.0f }) const noexcept;
    [[nodiscard]] std::vector<std::array<float, 4>>   ProjectBoundsSegments(const std::array<float, 3>& HalfBox,
                                                                            float ViewportWidth, float ViewportHeight) const noexcept;
};

struct FluidSceneDocument
{
    std::string      Format     = "frontier-fluid-scene";
    int              Version    = 1;
    std::string      Name       = "Untitled scene";
    FluidParameters  Parameters = FluidParameters::ConstructInitialParameters();
    FluidObjectNames Names{};
    FluidOrbitCamera Camera{};
};

struct FluidPresetDescriptor
{
    const char*         Identity               = "";
    const char*         Title                  = "";
    const char*         UpstreamName           = "";
    const char*         Summary                = "";
    const char*         Description            = "";
    FluidPresetCategory Category               = FluidPresetCategory::Fire;
    bool                TriggerExplosionOnLoad = false;
};

struct FluidSampleDescriptor
{
    const char* Identity   = "";
    const char* Name       = "";
    const char* Covers     = "";
    const char* PresetKey  = "";
};

struct FluidResolutionOption
{
    int         Resolution = 48;
    const char* Label      = "48\xC2\xB3";
    int         TilesX     = 8;
    int         TilesY     = 6;
};

[[nodiscard]] std::span<const FluidResolutionOption> EnumerateFluidResolutions() noexcept;
[[nodiscard]] FluidResolutionOption                  ResolveFluidResolutionOption(int Resolution) noexcept;
[[nodiscard]] std::span<const char* const>           EnumerateFluidDebugChannels() noexcept;
[[nodiscard]] std::span<const char* const>           EnumerateFluidColorPalettes() noexcept;
[[nodiscard]] std::span<const char* const>           EnumerateFluidColliderLabels() noexcept;
[[nodiscard]] std::span<const char* const>           EnumerateFluidAtlasFields() noexcept;
[[nodiscard]] std::span<const FluidPresetDescriptor> EnumerateFluidPresets() noexcept;
[[nodiscard]] const FluidPresetDescriptor*           FindFluidPreset(std::string_view Identity) noexcept;
[[nodiscard]] FluidParameters                        ConstructPresetParameters(std::string_view Identity);

[[nodiscard]] std::span<const FluidSampleDescriptor> EnumerateSampleFluidScenes() noexcept;
[[nodiscard]] FluidSceneDocument                     ConstructSampleFluidScene(std::string_view Identity);
[[nodiscard]] std::array<float, 3>                   ComputeAnimatedColliderPosition(const FluidParameters& Parameters, float ElapsedSeconds) noexcept;

} // namespace Frontier::FluidDynamics
