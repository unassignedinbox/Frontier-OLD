/*==========================================================================================================================================
                                                     FLUIDSCENESPECIFICATION.CPP
==========================================================================================================================================*/
// 🧩 Pure engine gas & combustion scene parameters, 18 presets, 8 parity sample scenes, and orbit camera (Experimental/Fluid 1:1).

#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Frontier::FluidDynamics
{

namespace
{

constexpr std::array<FluidResolutionOption, 7> ResolutionTable = { {
    { 16, "16\xC2\xB3", 4, 4 },
    { 24, "24\xC2\xB3", 6, 4 },
    { 32, "32\xC2\xB3", 8, 4 },
    { 48, "48\xC2\xB3", 8, 6 },
    { 64, "64\xC2\xB3", 8, 8 },
    { 96, "96\xC2\xB3", 12, 8 },
    { 128, "128\xC2\xB3", 16, 8 },
} };

constexpr std::array<const char*, 9> DebugChannelLabels = {
    "Lit volume", "Voxel DDA", "Smoke density", "Temperature", "Fuel",
    "Velocity", "Vorticity", "Pressure / irradiance", "Cross-section"
};

constexpr std::array<const char*, 6> ColorPaletteLabels = {
    "Physical Planckian Blackbody",
    "Cinema Napalm / Rich Hydrocarbon",
    "Thermobaric White-Hot Ordnance",
    "Wildfire / Copper Emerald Flame",
    "Cherenkov Blue Plasma Core",
    "Arcane Void Magenta / Corrupt Pyro"
};

constexpr std::array<const char*, 6> ColliderLabels = {
    "None", "Sphere", "Vertical cylinder", "Horizontal cylinder", "Deflector slab", "Tyre ring"
};

constexpr std::array<const char*, 4> AtlasFieldLabels = {
    "Smoke / temperature", "Velocity", "Pressure / irradiance", "Vorticity"
};

constexpr std::array<FluidPresetDescriptor, 18> PresetTable = { {
    { "ue5_pyro_default", "Pyro plume", "UE5 Niagara Pyro Plume + Blast", "Fireball, soot and rising plume",
      "Balanced hydrocarbon fireball with dynamic expanding bounds, ballistic shrapnel trails, and rolling dark soot.",
      FluidPresetCategory::Fire, true },
    { "oil_well_inferno", "Oil well inferno", "Raging Oil Well Inferno", "Pressurized fuel jet and heavy soot",
      "High-velocity pressurized fuel blowout generating a roaring flame column and dense carbonaceous black smoke.",
      FluidPresetCategory::Fire, false },
    { "shrapnel_airburst", "Shrapnel airburst", "Ordnance Shrapnel Airburst", "Burst with six arcing fragments",
      "High-explosive airburst ejecting 6 burning ballistic shrapnel fragments that arc through the 3D volume leaving smoke streamers.",
      FluidPresetCategory::Blast, true },
    { "megaton_open_bounds", "Wide-bounds blast", "Megaton Wide-Bounds Blast", "Expanded domain for large clouds",
      "Extra-large 3.0x simulation bounds with dynamic surge expansion so massive explosions and salvos have full room to mushroom.",
      FluidPresetCategory::Blast, true },
    { "tactical_ordnance", "Tactical detonation", "Tactical C4 / HE Blast Wave", "Fast pressure wave and dust",
      "High-explosive shockwave detonation with rapid divergence expansion, blinding flash core, and billowing dust/soot cloud.",
      FluidPresetCategory::Blast, true },
    { "obstacle_deflection", "Smoke deflection", "Voxel Collider Flow Deflection", "Plume splitting around a sphere",
      "Demonstrates a 3D solid sphere collider inside the Eulerian grid—fire and smoke split and curl around the obstacle.",
      FluidPresetCategory::Smoke, true },
    { "wildfire_tornado", "Wildfire tornado", "Fire Tornado / Alchemical Vortex", "High-swirl buoyant column",
      "High-swirl fire tornado with copper-emerald blackbody radiation and intense rotational vorticity.",
      FluidPresetCategory::Fire, true },
    { "ashfall_motes", "Ashfall", "Ashfall / Grey Fireflies", "Cooling cloud with grey ash",
      "A cooling afterburn cloud with dense grey ash motes instead of orange sparks.",
      FluidPresetCategory::Smoke, true },
    { "voxelized_stylized", "Voxel fire", "Voxelized Stylized Grid", "Quantized cell-shaded volume",
      "Explicit voxelized resolution look showing discrete 3D grid cells with quantized volumetric shading.",
      FluidPresetCategory::Fire, true },
    { "dust_tornado", "Dust tornado", "Dust Tornado / Debris Vortex", "Cold debris column held by swirl",
      "A cold ground-up vortex with no fuel at all: heavy dust held in a tall swirling column by strong vorticity and almost no buoyancy.",
      FluidPresetCategory::Smoke, false },
    { "tyre_burnout", "Tyre burnout", "Tyre Burnout", "Low hot rubber smoke off a ring",
      "Hot rubber smoke off a spinning tyre: the ring collider sits in the grid, the source is flat on the floor, and the cloud is dragged sideways.",
      FluidPresetCategory::Smoke, false },
    { "ledge_sandfall", "Ledge sandfall", "Sand Falling From A Ledge", "Heavy grains pouring off a slab",
      "Dry sand poured off a slab: the source sits high, smoke weight beats buoyancy so the column falls, and bright grains settle out of the base.",
      FluidPresetCategory::Smoke, false },
    { "settling_dust", "Settling dust", "Settling Dust", "Slow pale cloud that sinks",
      "The quiet aftermath: a slow pale cloud that hangs, drifts on a light crosswind and sinks rather than rises. No fire anywhere in it.",
      FluidPresetCategory::Smoke, false },
    { "small_gust", "Small gust", "Small Gust / Drifting Haze", "Thin haze pushed on a crosswind",
      "A thin sheet of haze pushed across the domain by a steady crosswind and torn apart by fine turbulence.",
      FluidPresetCategory::Smoke, false },
    { "brick_fracture_dust", "Brick fracture", "Fractured Brick Burst", "Cold burst with masonry chips",
      "Masonry breaking: a cold burst with no fuel and a great deal of smoke, throwing heavy chips off a slab.",
      FluidPresetCategory::Blast, true },
    { "lantern_flame", "Lantern flame", "Lantern / Oil Lamp Flame", "Small steady wick in a tight box",
      "One small steady wick in a tight domain. The box is the smallest the editor allows, so a 96 grid spends every voxel on the flame itself.",
      FluidPresetCategory::Fire, false },
    { "camp_fire", "Camp fire", "Camp Fire", "Broad low base with rising sparks",
      "A wood fire in the open: a broad low base, a lazy lean on the breeze, rising sparks and a thin pale smoke rather than a black one.",
      FluidPresetCategory::Fire, false },
    { "low_gpu_performance", "Lightweight plume", "Low-End GPU Fast Mode", "24\xC2\xB3 grid for integrated GPUs",
      "Ultra-lightweight 24\xC2\xB3 voxel grid (13.8K voxels) + 56 raymarch steps for reduced GPU cost.",
      FluidPresetCategory::Smoke, true },
} };

constexpr std::array<FluidSampleDescriptor, 8> SampleTable = { {
    { "hero_detonation", "Hero detonation",
      "Highest lattice, most pressure sweeps, dynamic bounds expanding mid-blast.", "ue5_pyro_default" },
    { "camp_fire_steady", "Camp fire, steady",
      "Continuous wood fire source with rising sparks and pale smoke, no blast.", "camp_fire" },
    { "cold_dust_fall", "Cold dust fall",
      "No fuel and no heat anywhere; everything visible arrives through multiple scattering.", "settling_dust" },
    { "tyre_burnout_ring", "Tyre burnout",
      "Selects the tyre ring collider and flat contact-patch smoke source.", "tyre_burnout" },
    { "deflector_obstacle", "Deflected plume",
      "Plume driven into an obstacle where occupancy changes the silhouette.", "obstacle_deflection" },
    { "fracture_dust_burst", "Fracture dust burst",
      "Cold one-shot masonry burst with no continuous source, aimed at flipbook baking.", "brick_fracture_dust" },
    { "far_cheap_plume", "Distant plume",
      "Lowest legal lattice and pressure sweeps for far-field budget scaling.", "low_gpu_performance" },
    { "open_bounds_megaton", "Open bounds megaton",
      "Largest domain in the parity corpus, exercising optical depth normalization.", "megaton_open_bounds" },
} };

std::array<float, 16> MultiplyColumnMajor(const std::array<float, 16>& A, const std::array<float, 16>& B) noexcept
{
    std::array<float, 16> Out{};
    for (int Col = 0; Col < 4; ++Col)
    {
        for (int Row = 0; Row < 4; ++Row)
        {
            Out[Col * 4 + Row] = A[0 * 4 + Row] * B[Col * 4 + 0] +
                                 A[1 * 4 + Row] * B[Col * 4 + 1] +
                                 A[2 * 4 + Row] * B[Col * 4 + 2] +
                                 A[3 * 4 + Row] * B[Col * 4 + 3];
        }
    }
    return Out;
}

std::array<float, 4> TransformColumnMajor(const std::array<float, 16>& M, const std::array<float, 4>& V) noexcept
{
    return {
        M[0] * V[0] + M[4] * V[1] + M[8] * V[2] + M[12] * V[3],
        M[1] * V[0] + M[5] * V[1] + M[9] * V[2] + M[13] * V[3],
        M[2] * V[0] + M[6] * V[1] + M[10] * V[2] + M[14] * V[3],
        M[3] * V[0] + M[7] * V[1] + M[11] * V[2] + M[15] * V[3],
    };
}

} // namespace

FluidParameters FluidParameters::ConstructUpstreamDefaults() noexcept
{
    FluidParameters Result{};
    Result.GridResolution    = 64;
    Result.RaymarchSteps     = 96;
    Result.RenderScale       = 1.0f;
    Result.ShowAtlasMinimap  = true;
    Result.ShowVoxelGridLines = true;
    Result.ShowBoundingBox   = true;
    Result.ColliderAutoMove  = true;
    return Result;
}

FluidParameters FluidParameters::ConstructInitialParameters() noexcept
{
    FluidParameters Result = ConstructUpstreamDefaults();
    Result.GridResolution     = 48;
    Result.RaymarchSteps      = 72;
    Result.RenderScale        = 0.8f;
    Result.ShowAtlasMinimap   = false;
    Result.ShowVoxelGridLines = false;
    Result.ShowBoundingBox    = false;
    Result.ColliderAutoMove   = false;
    return Result;
}

void FluidParameters::ClampToValidRanges() noexcept
{
    int NearestResolution = 48;
    int BestDelta         = 9999;
    for (const auto& Entry : ResolutionTable)
    {
        const int Delta = std::abs(Entry.Resolution - GridResolution);
        if (Delta < BestDelta)
        {
            BestDelta         = Delta;
            NearestResolution = Entry.Resolution;
        }
    }
    GridResolution       = NearestResolution;
    PressureIterations   = std::clamp(PressureIterations, 4, 60);
    TimeScale            = std::clamp(TimeScale, 0.05f, 2.5f);
    VoxelQuantization    = std::clamp(VoxelQuantization, 0.0f, 1.0f);
    BoundsWidth          = std::clamp(BoundsWidth, 1.0f, 4.0f);
    BoundsHeight         = std::clamp(BoundsHeight, 1.2f, 4.5f);
    DynamicBoundsMax     = std::clamp(DynamicBoundsMax, 1.0f, 2.4f);
    VorticityConfinement = std::clamp(VorticityConfinement, 0.0f, 16.0f);
    Buoyancy             = std::clamp(Buoyancy, 0.0f, 14.0f);
    SmokeWeight          = std::clamp(SmokeWeight, 0.0f, 4.0f);
    BurnRate             = std::clamp(BurnRate, 0.2f, 5.0f);
    BurnHeat             = std::clamp(BurnHeat, 0.5f, 6.0f);
    SootGeneration       = std::clamp(SootGeneration, 0.2f, 4.0f);
    CombustionExpansion  = std::clamp(CombustionExpansion, 0.0f, 10.0f);
    CoolingRate          = std::clamp(CoolingRate, 0.2f, 4.0f);
    SmokeDissipation     = std::clamp(SmokeDissipation, 0.0f, 1.2f);
    VelocityDamping      = std::clamp(VelocityDamping, 0.0f, 0.5f);
    TurbulenceStrength   = std::clamp(TurbulenceStrength, 0.0f, 9.0f);
    TurbulenceScale      = std::clamp(TurbulenceScale, 1.0f, 9.0f);
    WindX                = std::clamp(WindX, -4.0f, 4.0f);
    WindZ                = std::clamp(WindZ, -4.0f, 4.0f);
    SourceRate           = std::clamp(SourceRate, 0.0f, 2.5f);
    SourceRadius         = std::clamp(SourceRadius, 0.03f, 0.24f);
    SourceHeight         = std::clamp(SourceHeight, 0.02f, 0.95f);
    SourceUpwardVelocity = std::clamp(SourceUpwardVelocity, 0.5f, 9.0f);
    SourceSwirl          = std::clamp(SourceSwirl, 0.0f, 8.0f);
    SourceTemperature    = std::clamp(SourceTemperature, 0.2f, 8.0f);
    SourceFuel           = std::clamp(SourceFuel, 0.0f, 7.0f);
    SourceSmoke          = std::clamp(SourceSmoke, 0.0f, 6.0f);
    BlastStrength        = std::clamp(BlastStrength, 2.0f, 20.0f);
    BlastRadius          = std::clamp(BlastRadius, 0.08f, 0.36f);
    BlastTemperature     = std::clamp(BlastTemperature, 2.0f, 10.0f);
    BlastFuel            = std::clamp(BlastFuel, 0.0f, 9.0f);
    BlastSmoke           = std::clamp(BlastSmoke, 0.0f, 7.0f);
    BlastLobes           = std::clamp(BlastLobes, 3, 12);
    RenderChannel        = std::clamp(RenderChannel, 0, 8);
    ColorPalette         = std::clamp(ColorPalette, 0, 5);
    RenderScale          = std::clamp(RenderScale, 0.4f, 1.25f);
    RaymarchSteps        = std::clamp(RaymarchSteps, 32, 180);
    ShadowSteps          = std::clamp(ShadowSteps, 0, 14);
    DensityExtinction    = std::clamp(DensityExtinction, 4.0f, 42.0f);
    SmokeAlbedo          = std::clamp(SmokeAlbedo, 0.04f, 0.85f);
    ShadowDensity        = std::clamp(ShadowDensity, 2.0f, 26.0f);
    FireIntensity        = std::clamp(FireIntensity, 0.5f, 12.0f);
    TemperatureScale     = std::clamp(TemperatureScale, 0.5f, 2.5f);
    InternalScattering   = std::clamp(InternalScattering, 0.0f, 6.0f);
    PhaseAnisotropy      = std::clamp(PhaseAnisotropy, -0.5f, 0.8f);
    AmbientIntensity     = std::clamp(AmbientIntensity, 0.0f, 1.5f);
    SunIntensity         = std::clamp(SunIntensity, 0.0f, 5.0f);
    SunAzimuth           = std::clamp(SunAzimuth, -180.0f, 180.0f);
    SunElevation         = std::clamp(SunElevation, 5.0f, 88.0f);
    Exposure             = std::clamp(Exposure, 0.4f, 2.8f);
    BloomIntensity       = std::clamp(BloomIntensity, 0.0f, 1.5f);
    GodRaysIntensity     = std::clamp(GodRaysIntensity, 0.0f, 1.5f);
    ShockwaveStrength    = std::clamp(ShockwaveStrength, 0.0f, 2.5f);
    SliceAxis            = std::clamp(SliceAxis, 0, 2);
    SlicePosition        = std::clamp(SlicePosition, 0.05f, 0.95f);
    ObstacleCategory     = std::clamp(ObstacleCategory, 0, 5);
    ObstacleX            = std::clamp(ObstacleX, 0.15f, 0.85f);
    ObstacleY            = std::clamp(ObstacleY, 0.12f, 0.85f);
    ObstacleZ            = std::clamp(ObstacleZ, 0.15f, 0.85f);
    ObstacleRadius       = std::clamp(ObstacleRadius, 0.06f, 0.30f);
    ColliderSpeed        = std::clamp(ColliderSpeed, 0.2f, 3.5f);
    EmberCount           = std::clamp(EmberCount, 50, 1200);
    EmberSize            = std::clamp(EmberSize, 0.3f, 2.8f);
    EmberIntensity       = std::clamp(EmberIntensity, 0.2f, 4.0f);
    EmberLifetime        = std::clamp(EmberLifetime, 0.35f, 2.8f);
    EmberAshiness        = std::clamp(EmberAshiness, 0.0f, 1.0f);
    AtlasMinimapField    = std::clamp(AtlasMinimapField, 0, 3);
}

void FluidOrbitCamera::SnapToTarget() noexcept
{
    Theta    = TargetTheta;
    Phi      = TargetPhi;
    Distance = TargetDistance;
    Centre   = TargetCentre;
}

void FluidOrbitCamera::Orbit(float DeltaTheta, float DeltaPhi) noexcept
{
    TargetTheta -= DeltaTheta;
    TargetPhi    = std::clamp(TargetPhi - DeltaPhi, 0.12f, 3.00f);
}

void FluidOrbitCamera::Dolly(float WheelDelta) noexcept
{
    TargetDistance = std::clamp(TargetDistance * (1.0f + WheelDelta * 0.0012f), 1.15f, 12.0f);
}

void FluidOrbitCamera::Pan(float DeltaX, float DeltaY, float ViewportHeight) noexcept
{
    const float Scale       = TargetDistance * 2.0f * std::tan(FieldOfViewY * 0.5f) / std::max(ViewportHeight, 1.0f);
    const std::array<float, 3> Eye = QueryEyePosition();
    std::array<float, 3> Forward   = { Centre[0] - Eye[0], Centre[1] - Eye[1], Centre[2] - Eye[2] };
    const float ForwardLen         = std::max(1.0e-6f, std::sqrt(Forward[0] * Forward[0] + Forward[1] * Forward[1] + Forward[2] * Forward[2]));
    Forward[0] /= ForwardLen;
    Forward[1] /= ForwardLen;
    Forward[2] /= ForwardLen;

    std::array<float, 3> Right = { -Forward[2], 0.0f, Forward[0] };
    const float RightLen       = std::max(1.0e-6f, std::hypot(Right[0], Right[2]));
    Right[0] /= RightLen;
    Right[2] /= RightLen;

    const std::array<float, 3> Up = {
        Right[1] * Forward[2] - Right[2] * Forward[1],
        Right[2] * Forward[0] - Right[0] * Forward[2],
        Right[0] * Forward[1] - Right[1] * Forward[0]
    };

    for (int Axis = 0; Axis < 3; ++Axis)
    {
        TargetCentre[Axis] = std::clamp(TargetCentre[Axis] + (-DeltaX * Right[Axis] + DeltaY * Up[Axis]) * Scale, -2.2f, 2.2f);
    }
}

void FluidOrbitCamera::Reset() noexcept
{
    TargetTheta    = 0.62f;
    TargetPhi      = 1.26f;
    TargetDistance = 4.10f;
    TargetCentre   = { 0.0f, 0.30f, 0.0f };
}

void FluidOrbitCamera::ConfigureViewPreset(std::string_view Preset) noexcept
{
    if (Preset == "front")
    {
        TargetTheta = 0.0f;
        TargetPhi   = 1.48f;
    }
    else if (Preset == "side")
    {
        TargetTheta = 1.57079633f;
        TargetPhi   = 1.48f;
    }
    else if (Preset == "top")
    {
        TargetTheta = 0.0f;
        TargetPhi   = 0.16f;
    }
    else
    {
        Reset();
    }
}

void FluidOrbitCamera::Advance(float DeltaSeconds) noexcept
{
    if (AutoTurntable)
    {
        TargetTheta += DeltaSeconds * TurntableSpeed;
    }
    const float Blend = 1.0f - std::exp(-std::min(DeltaSeconds, 0.1f) * 14.0f);
    Theta    += (TargetTheta - Theta) * Blend;
    Phi      += (TargetPhi - Phi) * Blend;
    Distance += (TargetDistance - Distance) * Blend;
    for (int Axis = 0; Axis < 3; ++Axis)
    {
        Centre[Axis] += (TargetCentre[Axis] - Centre[Axis]) * Blend;
    }
}

std::array<float, 3> FluidOrbitCamera::QueryEyePosition() const noexcept
{
    const float SinPhi = std::sin(Phi);
    return {
        Centre[0] + Distance * SinPhi * std::sin(Theta),
        Centre[1] + Distance * std::cos(Phi),
        Centre[2] + Distance * SinPhi * std::cos(Theta),
    };
}

std::array<float, 16> FluidOrbitCamera::QueryViewMatrix() const noexcept
{
    const std::array<float, 3> Eye = QueryEyePosition();
    float Fx = Centre[0] - Eye[0];
    float Fy = Centre[1] - Eye[1];
    float Fz = Centre[2] - Eye[2];
    const float InvF = 1.0f / std::max(1.0e-6f, std::sqrt(Fx * Fx + Fy * Fy + Fz * Fz));
    Fx *= InvF;
    Fy *= InvF;
    Fz *= InvF;

    float Sx = -Fz;
    float Sy = 0.0f;
    float Sz = Fx;
    const float InvS = 1.0f / std::max(1.0e-6f, std::sqrt(Sx * Sx + Sy * Sy + Sz * Sz));
    Sx *= InvS;
    Sy *= InvS;
    Sz *= InvS;

    const float Ux = Sy * Fz - Sz * Fy;
    const float Uy = Sz * Fx - Sx * Fz;
    const float Uz = Sx * Fy - Sy * Fx;

    return {
        Sx,  Ux,  -Fx, 0.0f,
        Sy,  Uy,  -Fy, 0.0f,
        Sz,  Uz,  -Fz, 0.0f,
        -(Sx * Eye[0] + Sy * Eye[1] + Sz * Eye[2]),
        -(Ux * Eye[0] + Uy * Eye[1] + Uz * Eye[2]),
        (Fx * Eye[0] + Fy * Eye[1] + Fz * Eye[2]),
        1.0f
    };
}

std::array<float, 16> FluidOrbitCamera::QueryProjectionMatrix(float Aspect) const noexcept
{
    const float Focal = 1.0f / std::tan(FieldOfViewY * 0.5f);
    const float Nf    = 1.0f / (NearClip - FarClip);
    return {
        Focal / std::max(Aspect, 0.1f), 0.0f, 0.0f, 0.0f,
        0.0f, Focal, 0.0f, 0.0f,
        0.0f, 0.0f, (FarClip + NearClip) * Nf, -1.0f,
        0.0f, 0.0f, (2.0f * FarClip * NearClip) * Nf, 0.0f
    };
}

std::array<float, 16> FluidOrbitCamera::QueryViewProjectionMatrix(float Aspect) const noexcept
{
    return MultiplyColumnMajor(QueryProjectionMatrix(Aspect), QueryViewMatrix());
}

std::array<float, 16> FluidOrbitCamera::InvertMatrix(const std::array<float, 16>& M) noexcept
{
    std::array<float, 16> Inv{};
    Inv[0]  = M[5] * M[10] * M[15] - M[5] * M[11] * M[14] - M[9] * M[6] * M[15] + M[9] * M[7] * M[14] + M[13] * M[6] * M[11] - M[13] * M[7] * M[10];
    Inv[4]  = -M[4] * M[10] * M[15] + M[4] * M[11] * M[14] + M[8] * M[6] * M[15] - M[8] * M[7] * M[14] - M[12] * M[6] * M[11] + M[12] * M[7] * M[10];
    Inv[8]  = M[4] * M[9] * M[15] - M[4] * M[11] * M[13] - M[8] * M[5] * M[15] + M[8] * M[7] * M[13] + M[12] * M[5] * M[11] - M[12] * M[7] * M[9];
    Inv[12] = -M[4] * M[9] * M[14] + M[4] * M[10] * M[13] + M[8] * M[5] * M[14] - M[8] * M[6] * M[13] - M[12] * M[5] * M[10] + M[12] * M[6] * M[9];
    Inv[1]  = -M[1] * M[10] * M[15] + M[1] * M[11] * M[14] + M[9] * M[2] * M[15] - M[9] * M[3] * M[14] - M[13] * M[2] * M[11] + M[13] * M[3] * M[10];
    Inv[5]  = M[0] * M[10] * M[15] - M[0] * M[11] * M[14] - M[8] * M[2] * M[15] + M[8] * M[3] * M[14] + M[12] * M[2] * M[11] - M[12] * M[3] * M[10];
    Inv[9]  = -M[0] * M[9] * M[15] + M[0] * M[11] * M[13] + M[8] * M[1] * M[15] - M[8] * M[3] * M[13] - M[12] * M[1] * M[11] + M[12] * M[3] * M[9];
    Inv[13] = M[0] * M[9] * M[14] - M[0] * M[10] * M[13] - M[8] * M[1] * M[14] + M[8] * M[2] * M[13] + M[12] * M[1] * M[10] - M[12] * M[2] * M[9];
    Inv[2]  = M[1] * M[6] * M[15] - M[1] * M[7] * M[14] - M[5] * M[2] * M[15] + M[5] * M[3] * M[14] + M[13] * M[2] * M[7] - M[13] * M[3] * M[6];
    Inv[6]  = -M[0] * M[6] * M[15] + M[0] * M[7] * M[14] + M[4] * M[2] * M[15] - M[4] * M[3] * M[14] - M[12] * M[2] * M[7] + M[12] * M[3] * M[6];
    Inv[10] = M[0] * M[5] * M[15] - M[0] * M[7] * M[13] - M[4] * M[1] * M[15] + M[4] * M[3] * M[13] + M[12] * M[1] * M[7] - M[12] * M[3] * M[5];
    Inv[14] = -M[0] * M[5] * M[14] + M[0] * M[6] * M[13] + M[4] * M[1] * M[14] - M[4] * M[2] * M[13] - M[12] * M[1] * M[6] + M[12] * M[2] * M[5];
    Inv[3]  = -M[1] * M[6] * M[11] + M[1] * M[7] * M[10] + M[5] * M[2] * M[11] - M[5] * M[3] * M[10] - M[9] * M[2] * M[7] + M[9] * M[3] * M[6];
    Inv[7]  = M[0] * M[6] * M[11] - M[0] * M[7] * M[10] - M[4] * M[2] * M[11] + M[4] * M[3] * M[10] + M[8] * M[2] * M[7] - M[8] * M[3] * M[6];
    Inv[11] = -M[0] * M[5] * M[11] + M[0] * M[7] * M[9] + M[4] * M[1] * M[11] - M[4] * M[3] * M[9] - M[8] * M[1] * M[7] + M[8] * M[3] * M[5];
    Inv[15] = M[0] * M[5] * M[10] - M[0] * M[6] * M[9] - M[4] * M[1] * M[10] + M[4] * M[2] * M[9] + M[8] * M[1] * M[6] - M[8] * M[2] * M[5];

    const float Det = M[0] * Inv[0] + M[1] * Inv[4] + M[2] * Inv[8] + M[3] * Inv[12];
    if (std::fabs(Det) < 1.0e-12f)
    {
        return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    }
    const float InvDet = 1.0f / Det;
    for (float& Value : Inv) Value *= InvDet;
    return Inv;
}

std::optional<std::array<float, 3>> FluidOrbitCamera::RaycastVolumeUvw(float NormalizedX, float NormalizedY, float Aspect,
                                                                       const std::array<float, 3>& HalfBox) const noexcept
{
    const std::array<float, 16> InvVp = InvertMatrix(QueryViewProjectionMatrix(Aspect));
    const float X                     = NormalizedX * 2.0f - 1.0f;
    const float Y                     = 1.0f - NormalizedY * 2.0f;
    const auto Near4                  = TransformColumnMajor(InvVp, { X, Y, -1.0f, 1.0f });
    const auto Far4                   = TransformColumnMajor(InvVp, { X, Y, 1.0f, 1.0f });
    const std::array<float, 3> Origin = { Near4[0] / Near4[3], Near4[1] / Near4[3], Near4[2] / Near4[3] };
    std::array<float, 3> Direction    = { Far4[0] / Far4[3] - Origin[0], Far4[1] / Far4[3] - Origin[1], Far4[2] / Far4[3] - Origin[2] };
    const float DirLen                = std::max(1.0e-6f, std::sqrt(Direction[0] * Direction[0] + Direction[1] * Direction[1] + Direction[2] * Direction[2]));
    Direction[0] /= DirLen;
    Direction[1] /= DirLen;
    Direction[2] /= DirLen;

    float TMin = -1.0e9f;
    float TMax = 1.0e9f;
    for (int Axis = 0; Axis < 3; ++Axis)
    {
        const float InvD = 1.0f / (std::fabs(Direction[Axis]) < 1.0e-6f ? 1.0e-6f : Direction[Axis]);
        float T0         = (-HalfBox[Axis] - Origin[Axis]) * InvD;
        float T1         = (HalfBox[Axis] - Origin[Axis]) * InvD;
        if (T0 > T1) std::swap(T0, T1);
        TMin = std::max(TMin, T0);
        TMax = std::min(TMax, T1);
    }
    if (TMax < std::max(TMin, 0.0f))
    {
        return std::nullopt;
    }
    const float Enter = std::max(0.0f, TMin);
    const float HitT  = Enter + (TMax - Enter) * 0.45f;
    return std::array<float, 3>{
        std::clamp((Origin[0] + Direction[0] * HitT + HalfBox[0]) / (2.0f * HalfBox[0]), 0.06f, 0.94f),
        std::clamp((Origin[1] + Direction[1] * HitT + HalfBox[1]) / (2.0f * HalfBox[1]), 0.06f, 0.94f),
        std::clamp((Origin[2] + Direction[2] * HitT + HalfBox[2]) / (2.0f * HalfBox[2]), 0.06f, 0.94f),
    };
}

std::vector<std::array<float, 4>> FluidOrbitCamera::ProjectBoundsSegments(const std::array<float, 3>& HalfBox,
                                                                          float ViewportWidth, float ViewportHeight) const noexcept
{
    std::vector<std::array<float, 4>> Segments;
    if (ViewportWidth <= 1.0f || ViewportHeight <= 1.0f)
    {
        return Segments;
    }
    const auto Vp = QueryViewProjectionMatrix(ViewportWidth / ViewportHeight);
    std::array<std::array<float, 4>, 8> Corners{};
    for (int Index = 0; Index < 8; ++Index)
    {
        const float Cx = ((Index & 1) ? 1.0f : -1.0f) * HalfBox[0];
        const float Cy = ((Index & 2) ? 1.0f : -1.0f) * HalfBox[1];
        const float Cz = ((Index & 4) ? 1.0f : -1.0f) * HalfBox[2];
        Corners[Index] = TransformColumnMajor(Vp, { Cx, Cy, Cz, 1.0f });
    }
    static constexpr std::array<std::pair<int, int>, 12> Edges = { {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    } };
    Segments.reserve(12);
    for (const auto& [First, Second] : Edges)
    {
        const auto& A = Corners[First];
        const auto& B = Corners[Second];
        if (A[3] <= 0.05f || B[3] <= 0.05f) continue;
        const float Ax = (A[0] / A[3] * 0.5f + 0.5f) * ViewportWidth;
        const float Ay = (0.5f - A[1] / A[3] * 0.5f) * ViewportHeight;
        const float Bx = (B[0] / B[3] * 0.5f + 0.5f) * ViewportWidth;
        const float By = (0.5f - B[1] / B[3] * 0.5f) * ViewportHeight;
        Segments.push_back({ Ax, Ay, Bx, By });
    }
    return Segments;
}

std::span<const FluidResolutionOption> EnumerateFluidResolutions() noexcept { return ResolutionTable; }

FluidResolutionOption ResolveFluidResolutionOption(int Resolution) noexcept
{
    for (const auto& Option : ResolutionTable)
    {
        if (Option.Resolution == Resolution) return Option;
    }
    return ResolutionTable[3];
}

std::span<const char* const> EnumerateFluidDebugChannels() noexcept { return DebugChannelLabels; }
std::span<const char* const> EnumerateFluidColorPalettes() noexcept { return ColorPaletteLabels; }
std::span<const char* const> EnumerateFluidColliderLabels() noexcept { return ColliderLabels; }
std::span<const char* const> EnumerateFluidAtlasFields() noexcept { return AtlasFieldLabels; }
std::span<const FluidPresetDescriptor> EnumerateFluidPresets() noexcept { return PresetTable; }

const FluidPresetDescriptor* FindFluidPreset(std::string_view Identity) noexcept
{
    for (const auto& Entry : PresetTable)
    {
        if (Identity == Entry.Identity) return &Entry;
    }
    return nullptr;
}

FluidParameters ConstructPresetParameters(std::string_view Identity)
{
    FluidParameters P = FluidParameters::ConstructUpstreamDefaults();
    // Apply InitialParameters base overrides for non-explicit fields just like ConstructPresetParameters in SceneSpecification.js
    P = FluidParameters::ConstructInitialParameters();
    P.ShowEmbers = true;

    if (Identity == "ue5_pyro_default")
    {
        P.GridResolution = 96; P.BlastRadius = 0.13f; P.BoundsWidth = 2.6f; P.BoundsHeight = 2.95f;
        P.DynamicBounds = true; P.DynamicBoundsMax = 1.2f; P.ShrapnelEnabled = true; P.RaymarchSteps = 120;
    }
    else if (Identity == "shrapnel_airburst")
    {
        P.GridResolution = 96; P.BoundsWidth = 3.3f; P.BoundsHeight = 3.45f; P.DynamicBounds = true;
        P.DynamicBoundsMax = 1.2f; P.ShrapnelEnabled = true; P.SourceEnabled = false; P.VorticityConfinement = 8.8f;
        P.Buoyancy = 5.2f; P.SmokeDissipation = 0.12f; P.BlastStrength = 11.5f; P.BlastRadius = 0.14f;
        P.BlastTemperature = 6.2f; P.GodRaysIntensity = 0.65f; P.RaymarchSteps = 96;
    }
    else if (Identity == "megaton_open_bounds")
    {
        P.GridResolution = 128; P.BoundsWidth = 3.9f; P.BoundsHeight = 4.0f; P.DynamicBounds = true;
        P.DynamicBoundsMax = 1.2f; P.ShrapnelEnabled = true; P.RaymarchSteps = 120; P.VorticityConfinement = 9.2f;
        P.Buoyancy = 6.2f; P.SmokeWeight = 1.2f; P.BurnRate = 1.75f; P.BurnHeat = 3.0f; P.SootGeneration = 1.9f;
        P.CombustionExpansion = 5.8f; P.CoolingRate = 1.2f; P.SmokeDissipation = 0.11f; P.TurbulenceStrength = 4.5f;
        P.SourceEnabled = false; P.BlastStrength = 13.5f; P.BlastRadius = 0.13f; P.BlastTemperature = 6.5f;
        P.BlastFuel = 5.4f; P.BlastSmoke = 3.0f; P.DensityExtinction = 20.0f; P.SmokeAlbedo = 0.26f;
        P.FireIntensity = 6.5f; P.InternalScattering = 3.0f;
    }
    else if (Identity == "low_gpu_performance")
    {
        P.GridResolution = 24; P.BoundsWidth = 2.0f; P.BoundsHeight = 2.2f; P.DynamicBounds = true;
        P.PressureIterations = 14; P.RaymarchSteps = 56; P.ShadowSteps = 4; P.RenderScale = 0.85f;
        P.VorticityConfinement = 8.5f; P.Buoyancy = 6.0f;
    }
    else if (Identity == "tactical_ordnance")
    {
        P.GridResolution = 96; P.BoundsWidth = 3.05f; P.BoundsHeight = 3.25f; P.DynamicBounds = true;
        P.DynamicBoundsMax = 1.2f; P.ShrapnelEnabled = true; P.RaymarchSteps = 120; P.ColorPalette = 2;
        P.VorticityConfinement = 9.2f; P.Buoyancy = 4.6f; P.SmokeWeight = 1.3f; P.BurnRate = 2.6f;
        P.BurnHeat = 3.2f; P.SootGeneration = 2.1f; P.CombustionExpansion = 6.5f; P.CoolingRate = 1.75f;
        P.SmokeDissipation = 0.14f; P.TurbulenceStrength = 4.8f; P.SourceEnabled = false; P.BlastStrength = 12.5f;
        P.BlastRadius = 0.15f; P.BlastTemperature = 6.4f; P.BlastFuel = 5.2f; P.BlastSmoke = 3.2f;
        P.DensityExtinction = 22.0f; P.SmokeAlbedo = 0.34f; P.FireIntensity = 6.8f; P.InternalScattering = 3.1f;
    }
    else if (Identity == "oil_well_inferno")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.BoundsWidth = 1.9f; P.BoundsHeight = 2.4f;
        P.DynamicBounds = true; P.ColorPalette = 1; P.VorticityConfinement = 8.8f; P.Buoyancy = 7.2f;
        P.SmokeWeight = 1.4f; P.BurnRate = 1.6f; P.BurnHeat = 3.1f; P.SootGeneration = 2.2f;
        P.CombustionExpansion = 3.4f; P.CoolingRate = 1.15f; P.SmokeDissipation = 0.22f; P.TurbulenceStrength = 4.2f;
        P.WindX = 0.65f; P.SourceEnabled = true; P.SourceRate = 1.45f; P.SourceRadius = 0.11f;
        P.SourceUpwardVelocity = 5.4f; P.SourceSwirl = 1.8f; P.SourceTemperature = 4.2f; P.SourceFuel = 3.6f;
        P.SourceSmoke = 1.9f; P.DensityExtinction = 24.0f; P.SmokeAlbedo = 0.12f; P.FireIntensity = 6.0f;
        P.InternalScattering = 2.8f;
    }
    else if (Identity == "obstacle_deflection")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.BoundsWidth = 1.85f; P.BoundsHeight = 2.1f;
        P.VorticityConfinement = 8.4f; P.Buoyancy = 6.4f; P.BurnRate = 1.75f; P.CombustionExpansion = 3.8f;
        P.CoolingRate = 1.2f; P.SmokeDissipation = 0.18f; P.SourceEnabled = true; P.SourceRate = 1.25f;
        P.SourceRadius = 0.11f; P.SourceUpwardVelocity = 4.6f; P.ObstacleCategory = 1;
        P.ObstacleX = 0.5f; P.ObstacleY = 0.42f; P.ObstacleZ = 0.5f; P.ObstacleRadius = 0.16f;
    }
    else if (Identity == "wildfire_tornado")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.BoundsWidth = 1.9f; P.BoundsHeight = 2.3f;
        P.ColorPalette = 3; P.VorticityConfinement = 9.5f; P.Buoyancy = 6.8f; P.BurnRate = 1.7f;
        P.CombustionExpansion = 3.5f; P.CoolingRate = 1.25f; P.SmokeDissipation = 0.2f; P.TurbulenceStrength = 3.8f;
        P.SourceEnabled = true; P.SourceRate = 1.3f; P.SourceRadius = 0.12f; P.SourceUpwardVelocity = 4.2f;
        P.SourceSwirl = 5.5f; P.SourceTemperature = 3.9f; P.SourceFuel = 3.2f;
    }
    else if (Identity == "ashfall_motes")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.SourceEnabled = false; P.BlastStrength = 7.0f;
        P.BlastRadius = 0.2f; P.BlastTemperature = 2.4f; P.BlastFuel = 1.2f; P.BlastSmoke = 3.8f;
        P.ColorPalette = 1; P.SmokeDissipation = 0.08f; P.ShowEmbers = true; P.EmberCount = 900;
        P.EmberSize = 1.25f; P.EmberIntensity = 0.95f; P.EmberLifetime = 1.65f; P.EmberAshiness = 1.0f;
    }
    else if (Identity == "voxelized_stylized")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.VoxelQuantization = 0.88f; P.RenderChannel = 1;
        P.VorticityConfinement = 8.0f; P.ShowActiveVoxelCells = true;
    }
    else if (Identity == "dust_tornado")
    {
        P.GridResolution = 96; P.RaymarchSteps = 96; P.BoundsWidth = 2.2f; P.BoundsHeight = 3.6f;
        P.DynamicBounds = false; P.ColorPalette = 1; P.VorticityConfinement = 11.5f; P.Buoyancy = 1.3f;
        P.SmokeWeight = 1.9f; P.BurnRate = 0.2f; P.BurnHeat = 0.5f; P.SootGeneration = 0.2f;
        P.CombustionExpansion = 0.0f; P.CoolingRate = 3.2f; P.SmokeDissipation = 0.05f; P.VelocityDamping = 0.03f;
        P.TurbulenceStrength = 2.6f; P.TurbulenceScale = 2.4f; P.WindX = 0.12f; P.SourceEnabled = true;
        P.SourceRate = 1.6f; P.SourceRadius = 0.09f; P.SourceHeight = 0.04f; P.SourceUpwardVelocity = 2.6f;
        P.SourceSwirl = 7.6f; P.SourceTemperature = 0.3f; P.SourceFuel = 0.0f; P.SourceSmoke = 3.4f;
        P.BlastFuel = 0.0f; P.BlastTemperature = 2.0f; P.BlastSmoke = 5.5f; P.DensityExtinction = 15.0f;
        P.SmokeAlbedo = 0.62f; P.ShadowDensity = 7.0f; P.FireIntensity = 0.5f; P.InternalScattering = 1.2f;
        P.PhaseAnisotropy = 0.1f; P.ShowEmbers = true; P.EmberCount = 700; P.EmberSize = 0.6f;
        P.EmberIntensity = 0.35f; P.EmberLifetime = 2.4f; P.EmberAshiness = 1.0f;
    }
    else if (Identity == "tyre_burnout")
    {
        P.GridResolution = 96; P.RaymarchSteps = 96; P.BoundsWidth = 2.6f; P.BoundsHeight = 1.7f;
        P.DynamicBounds = false; P.ColorPalette = 1; P.VorticityConfinement = 6.4f; P.Buoyancy = 2.3f;
        P.SmokeWeight = 1.25f; P.BurnRate = 0.4f; P.BurnHeat = 0.8f; P.SootGeneration = 3.4f;
        P.CombustionExpansion = 0.4f; P.CoolingRate = 2.6f; P.SmokeDissipation = 0.07f; P.VelocityDamping = 0.05f;
        P.TurbulenceStrength = 3.4f; P.TurbulenceScale = 4.2f; P.WindX = 1.15f; P.WindZ = -0.35f;
        P.SourceEnabled = true; P.SourceRate = 1.9f; P.SourceRadius = 0.13f; P.SourceHeight = 0.02f;
        P.SourceUpwardVelocity = 1.4f; P.SourceSwirl = 3.2f; P.SourceTemperature = 0.9f; P.SourceFuel = 0.2f;
        P.SourceSmoke = 4.2f; P.BlastFuel = 0.0f; P.BlastTemperature = 2.0f; P.BlastSmoke = 5.5f;
        P.DensityExtinction = 27.0f; P.SmokeAlbedo = 0.17f; P.ShadowDensity = 14.0f; P.FireIntensity = 0.8f;
        P.InternalScattering = 0.9f; P.ObstacleCategory = 5; P.ObstacleX = 0.5f; P.ObstacleY = 0.26f;
        P.ObstacleZ = 0.5f; P.ObstacleRadius = 0.17f; P.ColliderAutoMove = false; P.ShowEmbers = false;
    }
    else if (Identity == "ledge_sandfall")
    {
        P.GridResolution = 96; P.RaymarchSteps = 96; P.BoundsWidth = 1.9f; P.BoundsHeight = 2.8f;
        P.DynamicBounds = false; P.ColorPalette = 1; P.VorticityConfinement = 3.6f; P.Buoyancy = 0.2f;
        P.SmokeWeight = 3.2f; P.BurnRate = 0.2f; P.BurnHeat = 0.5f; P.SootGeneration = 0.2f;
        P.CombustionExpansion = 0.0f; P.CoolingRate = 3.5f; P.SmokeDissipation = 0.06f; P.VelocityDamping = 0.02f;
        P.TurbulenceStrength = 1.4f; P.TurbulenceScale = 5.5f; P.WindX = 0.18f; P.SourceEnabled = true;
        P.SourceRate = 1.35f; P.SourceRadius = 0.07f; P.SourceHeight = 0.92f; P.SourceUpwardVelocity = 0.5f;
        P.SourceSwirl = 0.4f; P.SourceTemperature = 0.2f; P.SourceFuel = 0.0f; P.SourceSmoke = 3.8f;
        P.BlastFuel = 0.0f; P.BlastTemperature = 2.0f; P.BlastSmoke = 5.5f; P.DensityExtinction = 13.0f;
        P.SmokeAlbedo = 0.74f; P.ShadowDensity = 6.0f; P.FireIntensity = 0.5f; P.InternalScattering = 1.6f;
        P.PhaseAnisotropy = 0.22f; P.ObstacleCategory = 4; P.ObstacleX = 0.5f; P.ObstacleY = 0.62f;
        P.ObstacleZ = 0.5f; P.ObstacleRadius = 0.2f; P.ColliderAutoMove = false; P.ShowEmbers = true;
        P.EmberCount = 850; P.EmberSize = 0.5f; P.EmberIntensity = 0.3f; P.EmberLifetime = 1.3f; P.EmberAshiness = 1.0f;
    }
    else if (Identity == "settling_dust")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.BoundsWidth = 2.3f; P.BoundsHeight = 1.8f;
        P.DynamicBounds = false; P.ColorPalette = 1; P.VorticityConfinement = 2.8f; P.Buoyancy = 0.6f;
        P.SmokeWeight = 2.4f; P.BurnRate = 0.2f; P.BurnHeat = 0.5f; P.SootGeneration = 0.2f;
        P.CombustionExpansion = 0.0f; P.CoolingRate = 3.5f; P.SmokeDissipation = 0.04f; P.VelocityDamping = 0.12f;
        P.TurbulenceStrength = 1.2f; P.TurbulenceScale = 2.8f; P.WindX = 0.38f; P.SourceEnabled = true;
        P.SourceRate = 0.6f; P.SourceRadius = 0.18f; P.SourceHeight = 0.1f; P.SourceUpwardVelocity = 0.8f;
        P.SourceSwirl = 0.6f; P.SourceTemperature = 0.2f; P.SourceFuel = 0.0f; P.SourceSmoke = 2.4f;
        P.BlastFuel = 0.0f; P.BlastTemperature = 2.0f; P.BlastSmoke = 5.5f; P.DensityExtinction = 11.0f;
        P.SmokeAlbedo = 0.68f; P.ShadowDensity = 5.0f; P.FireIntensity = 0.5f; P.InternalScattering = 1.8f;
        P.PhaseAnisotropy = 0.18f; P.ShowEmbers = false;
    }
    else if (Identity == "small_gust")
    {
        P.GridResolution = 48; P.BoundsWidth = 2.5f; P.BoundsHeight = 1.5f; P.DynamicBounds = false;
        P.ColorPalette = 1; P.VorticityConfinement = 4.2f; P.Buoyancy = 1.1f; P.SmokeWeight = 0.35f;
        P.BurnRate = 0.2f; P.BurnHeat = 0.5f; P.SootGeneration = 0.2f; P.CombustionExpansion = 0.0f;
        P.CoolingRate = 3.0f; P.SmokeDissipation = 0.42f; P.VelocityDamping = 0.04f; P.TurbulenceStrength = 2.4f;
        P.TurbulenceScale = 6.8f; P.WindX = 1.75f; P.WindZ = 0.55f; P.SourceEnabled = true; P.SourceRate = 0.45f;
        P.SourceRadius = 0.12f; P.SourceHeight = 0.22f; P.SourceUpwardVelocity = 0.9f; P.SourceSwirl = 0.9f;
        P.SourceTemperature = 0.2f; P.SourceFuel = 0.0f; P.SourceSmoke = 1.1f; P.RaymarchSteps = 64;
        P.BlastFuel = 0.0f; P.BlastTemperature = 2.0f; P.BlastSmoke = 5.5f; P.DensityExtinction = 7.0f;
        P.SmokeAlbedo = 0.6f; P.ShadowDensity = 4.0f; P.FireIntensity = 0.5f; P.InternalScattering = 1.4f;
        P.ShowEmbers = false;
    }
    else if (Identity == "brick_fracture_dust")
    {
        P.GridResolution = 96; P.BoundsWidth = 2.4f; P.BoundsHeight = 2.2f; P.DynamicBounds = true;
        P.DynamicBoundsMax = 1.2f; P.ShrapnelEnabled = true; P.ColorPalette = 1; P.VorticityConfinement = 6.8f;
        P.Buoyancy = 0.9f; P.SmokeWeight = 2.1f; P.BurnRate = 0.2f; P.BurnHeat = 0.5f; P.SootGeneration = 0.2f;
        P.CombustionExpansion = 0.2f; P.CoolingRate = 3.4f; P.SmokeDissipation = 0.07f; P.TurbulenceStrength = 4.4f;
        P.TurbulenceScale = 3.2f; P.SourceEnabled = false; P.SourceFuel = 0.0f; P.SourceTemperature = 0.2f;
        P.BlastStrength = 8.5f; P.BlastRadius = 0.11f; P.BlastTemperature = 2.0f; P.BlastFuel = 0.0f;
        P.BlastSmoke = 6.5f; P.BlastLobes = 9; P.RaymarchSteps = 110; P.DensityExtinction = 26.0f;
        P.SmokeAlbedo = 0.56f; P.ShadowDensity = 12.0f; P.FireIntensity = 0.5f; P.InternalScattering = 1.1f;
        P.ObstacleCategory = 4; P.ObstacleX = 0.5f; P.ObstacleY = 0.3f; P.ObstacleZ = 0.5f;
        P.ObstacleRadius = 0.22f; P.ColliderAutoMove = false; P.ShowEmbers = true; P.EmberCount = 820;
        P.EmberSize = 1.45f; P.EmberIntensity = 0.4f; P.EmberLifetime = 1.1f; P.EmberAshiness = 1.0f;
    }
    else if (Identity == "lantern_flame")
    {
        P.GridResolution = 96; P.BoundsWidth = 1.0f; P.BoundsHeight = 1.3f; P.DynamicBounds = false;
        P.ColorPalette = 0; P.PressureIterations = 26; P.VorticityConfinement = 3.4f; P.Buoyancy = 3.8f;
        P.SmokeWeight = 0.6f; P.BurnRate = 1.15f; P.BurnHeat = 2.2f; P.SootGeneration = 0.3f;
        P.CombustionExpansion = 1.6f; P.CoolingRate = 1.9f; P.SmokeDissipation = 0.82f; P.VelocityDamping = 0.1f;
        P.TurbulenceStrength = 0.85f; P.TurbulenceScale = 7.5f; P.WindX = 0.06f; P.SourceEnabled = true;
        P.SourceRate = 0.4f; P.SourceRadius = 0.05f; P.SourceHeight = 0.02f; P.SourceUpwardVelocity = 1.2f;
        P.SourceSwirl = 0.5f; P.SourceTemperature = 3.2f; P.SourceFuel = 2.4f; P.SourceSmoke = 0.3f;
        P.RaymarchSteps = 128; P.DensityExtinction = 12.0f; P.SmokeAlbedo = 0.3f; P.FireIntensity = 4.2f;
        P.TemperatureScale = 1.05f; P.InternalScattering = 2.2f; P.AmbientIntensity = 0.18f;
        P.SunIntensity = 0.9f; P.Exposure = 1.05f; P.BloomIntensity = 0.7f; P.GodRaysIntensity = 0.25f;
        P.ShowEmbers = false;
    }
    else if (Identity == "camp_fire")
    {
        P.GridResolution = 64; P.RaymarchSteps = 96; P.BoundsWidth = 1.6f; P.BoundsHeight = 2.2f;
        P.DynamicBounds = false; P.ColorPalette = 0; P.VorticityConfinement = 7.4f; P.Buoyancy = 5.2f;
        P.SmokeWeight = 0.85f; P.BurnRate = 1.5f; P.BurnHeat = 2.4f; P.SootGeneration = 1.05f;
        P.CombustionExpansion = 2.8f; P.CoolingRate = 1.55f; P.SmokeDissipation = 0.34f; P.VelocityDamping = 0.06f;
        P.TurbulenceStrength = 3.0f; P.TurbulenceScale = 4.6f; P.WindX = 0.32f; P.SourceEnabled = true;
        P.SourceRate = 0.95f; P.SourceRadius = 0.15f; P.SourceHeight = 0.05f; P.SourceUpwardVelocity = 2.5f;
        P.SourceSwirl = 1.7f; P.SourceTemperature = 3.1f; P.SourceFuel = 2.3f; P.SourceSmoke = 0.95f;
        P.DensityExtinction = 14.0f; P.SmokeAlbedo = 0.38f; P.FireIntensity = 4.8f; P.InternalScattering = 2.4f;
        P.AmbientIntensity = 0.22f; P.SunIntensity = 1.2f; P.ShowEmbers = true; P.EmberCount = 430;
        P.EmberSize = 0.8f; P.EmberIntensity = 1.6f; P.EmberLifetime = 1.9f; P.EmberAshiness = 0.25f;
    }
    else
    {
        throw std::runtime_error("Unknown fluid preset: " + std::string(Identity));
    }

    P.ClampToValidRanges();
    return P;
}

std::span<const FluidSampleDescriptor> EnumerateSampleFluidScenes() noexcept { return SampleTable; }

FluidSceneDocument ConstructSampleFluidScene(std::string_view Identity)
{
    FluidSceneDocument Scene{};
    auto ApplyFraming = [&](float Theta, float Phi, float Distance, std::array<float, 3> Centre) {
        Scene.Camera.TargetTheta    = Theta;
        Scene.Camera.TargetPhi      = Phi;
        Scene.Camera.TargetDistance = Distance;
        Scene.Camera.TargetCentre   = Centre;
        Scene.Camera.SnapToTarget();
    };

    if (Identity == "hero_detonation")
    {
        Scene.Name                        = "Hero detonation";
        Scene.Parameters                  = ConstructPresetParameters("ue5_pyro_default");
        Scene.Parameters.GridResolution   = 128;
        Scene.Parameters.PressureIterations = 26;
        Scene.Names                       = { "Blast domain", "Charge", "Ground plate", "Key light" };
        ApplyFraming(0.72f, 1.32f, 3.05f, { 0.0f, 0.18f, 0.0f });
    }
    else if (Identity == "camp_fire_steady")
    {
        Scene.Name       = "Camp fire, steady";
        Scene.Parameters = ConstructPresetParameters("camp_fire");
        Scene.Names      = { "Fire domain", "Log fire", "Stone ring", "Moonlight" };
        ApplyFraming(1.15f, 1.28f, 1.75f, { 0.0f, 0.05f, 0.0f });
    }
    else if (Identity == "cold_dust_fall")
    {
        Scene.Name       = "Cold dust fall";
        Scene.Parameters = ConstructPresetParameters("settling_dust");
        Scene.Names      = { "Dust domain", "Ledge spill", "Rubble", "Overcast key" };
        ApplyFraming(2.30f, 1.52f, 2.45f, { 0.0f, -0.05f, 0.0f });
    }
    else if (Identity == "tyre_burnout_ring")
    {
        Scene.Name       = "Tyre burnout";
        Scene.Parameters = ConstructPresetParameters("tyre_burnout");
        Scene.Names      = { "Contact patch", "Slip", "Rear tyre", "Pit light" };
        ApplyFraming(2.30f, 1.52f, 2.45f, { 0.0f, -0.05f, 0.0f });
    }
    else if (Identity == "deflector_obstacle")
    {
        Scene.Name       = "Deflected plume";
        Scene.Parameters = ConstructPresetParameters("obstacle_deflection");
        Scene.Names      = { "Vent domain", "Floor vent", "Deflector slab", "Key light" };
        ApplyFraming(0.72f, 1.32f, 3.05f, { 0.0f, 0.18f, 0.0f });
    }
    else if (Identity == "fracture_dust_burst")
    {
        Scene.Name       = "Fracture dust burst";
        Scene.Parameters = ConstructPresetParameters("brick_fracture_dust");
        Scene.Names      = { "Impact domain", "Break point", "Masonry", "Key light" };
        ApplyFraming(1.15f, 1.28f, 1.75f, { 0.0f, 0.05f, 0.0f });
    }
    else if (Identity == "far_cheap_plume")
    {
        Scene.Name                          = "Distant plume";
        Scene.Parameters                    = ConstructPresetParameters("low_gpu_performance");
        Scene.Parameters.GridResolution     = 32;
        Scene.Parameters.PressureIterations = 14;
        Scene.Names                         = { "Far domain", "Distant fire", "None", "Key light" };
        ApplyFraming(-0.95f, 0.92f, 4.30f, { 0.0f, 0.35f, 0.0f });
    }
    else if (Identity == "open_bounds_megaton")
    {
        Scene.Name       = "Open bounds megaton";
        Scene.Parameters = ConstructPresetParameters("megaton_open_bounds");
        Scene.Names      = { "Open domain", "Device", "Terrain", "Key light" };
        ApplyFraming(-0.95f, 0.92f, 4.30f, { 0.0f, 0.35f, 0.0f });
    }
    else
    {
        throw std::runtime_error("Unknown sample fluid scene: " + std::string(Identity));
    }

    Scene.Parameters.ClampToValidRanges();
    return Scene;
}

std::array<float, 3> ComputeAnimatedColliderPosition(const FluidParameters& Parameters, float ElapsedSeconds) noexcept
{
    if (!Parameters.ColliderAutoMove || Parameters.ObstacleCategory <= 0)
    {
        return { Parameters.ObstacleX, Parameters.ObstacleY, Parameters.ObstacleZ };
    }
    const float Phase = ElapsedSeconds * Parameters.ColliderSpeed;
    return {
        std::clamp(Parameters.ObstacleX + 0.19f * std::sin(Phase), 0.14f, 0.86f),
        std::clamp(Parameters.ObstacleY + 0.06f * std::sin(Phase * 0.7f), 0.12f, 0.88f),
        std::clamp(Parameters.ObstacleZ + 0.14f * std::cos(Phase * 0.85f), 0.14f, 0.86f),
    };
}

} // namespace Frontier::FluidDynamics
