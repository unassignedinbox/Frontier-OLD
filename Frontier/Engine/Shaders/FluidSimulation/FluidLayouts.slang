//============================================================================================================================================
//                                                          FLUIDLAYOUTS.SLANG
//============================================================================================================================================
// 🧩 Eulerian 3D gas & combustion solver layouts, SDF colliders (including tyre ring), blackbody palettes, and 3D curl noise.

#pragma once
#include "IntrinsicParity.slang"

struct FluidSimulationConstants
{
    float4 GridTiming;          // [cell,1/cell,s,s] GridSize, InverseGridSize, DeltaTime, ElapsedTime
    float4 DomainBounds;        // [m,m,m,-] ScaleX, ScaleY, ScaleZ, EnclosedBox
    float4 SourceGeometry;      // [-,u,u,1/s] SourceEnabled, SourceRadius, SourceHeight, SourceRate
    float4 SourcePayload;       // [-,-,K,m/s] SourceFuel, SourceSmoke, SourceTemperature, SourceUpwardVelocity
    float4 SourceMotion;        // [-,-,u,-] SourceSwirl, BrushActive, BrushRadius, BlastActive
    float4 BrushPosition;       // [u,u,u,-] xyz normalized UVW
    float4 BrushVelocity;       // [m/s] xyz injected brush velocity
    float4 BlastGeometry;       // [u,-,K,-] BlastRadius, BlastStrength, BlastTemperature, BlastFuel
    float4 BlastPayload;        // [-,-,-,-] BlastSmoke, BlastLobes, BlastDirectional, MacCormackAdvection
    float4 BlastPosition;       // [u,u,u,-] xyz normalized UVW
    float4 BlastVelocity;       // [m/s] xyz directional blast impulse (colliding waves)
    float4 ObstacleGeometry;    // [-,u,-,-] ObstacleCategory, ObstacleRadius, VelocityDamping, VorticityConfinement
    float4 ObstaclePosition;    // [u,u,u,-] xyz normalized UVW
    float4 ObstacleVelocity;    // [m/s] xyz collider velocity in UVW/s
    float4 BuoyancyTurbulence;  // [-] Buoyancy, SmokeWeight, TurbulenceStrength, TurbulenceScale
    float4 WindCombustion;      // [-] WindX, WindZ, BurnRate, BurnHeat
    float4 ThermalDissipation;  // [-] SootGeneration, CombustionExpansion, CoolingRate, SmokeDissipation
};

struct FluidRaymarchConstants
{
    float4x4 InverseViewProjection;
    float4   CameraPosition;    // [m] xyz eye; w Exposure
    float4   DomainBounds;      // [m,m,m,cell] ScaleX, ScaleY, ScaleZ, GridSize
    float4   SunLight;          // [-] xyz normalized SunDirection; w SunIntensity
    float4   VolumeShading;     // [-] AmbientIntensity, FireIntensity, DensityExtinction, SmokeAlbedo
    float4   ScatteringOptics;  // [-] PhaseAnisotropy, ShadowDensity, InternalScattering, TemperatureScale
    float4   GlowShockwave;     // [-] BloomIntensity, GodRaysIntensity, ShockwaveStrength, ShockwaveRadius
    float4   ShockwaveOrigin;   // [u,u,u,u] xyz UVW centre; w ShockwaveThickness
    float4   ObstacleGeometry;  // [-,u,u,-] ObstacleCategory, ObstacleRadius, VoxelQuantization, SlicePosition
    float4   ObstaclePosition;  // [u,u,u,-] xyz UVW centre
    uint4    RenderControls;    // [-] RaymarchSteps, ShadowSteps, RenderChannel, ColorPalette
    uint4    OverlayControls;   // [-] SliceAxis, ShowFloorGrid, ShowVoxelGridLines, ShowActiveVoxelCells
    uint4    ExportControls;    // [-] FlipbookAlphaExport, ViewportWidth, ViewportHeight, Reserved
};

struct FluidEmberConstants
{
    float4x4 ViewProjection;
    float4   CameraRightSize;   // [-] xyz CameraRight; w EmberSize
    float4   CameraUpIntensity; // [-] xyz CameraUp; w EmberIntensity
    float4   DomainTime;        // [m,m,m,s] ScaleX, ScaleY, ScaleZ, ElapsedTime
    float4   MotionControls;    // [-] WindX, WindZ, EmberLifetime, EmberAshiness
    uint4    StyleControls;     // [-] EmberCount, ColorPalette, GridSize, ExposureMilli
};

struct FluidAtlasConstants
{
    float4 PanelRectangle;      // [ndc] x0, y0, x1, y1 in clip space
    uint4  AtlasControls;       // [-] GridSize, FieldChannel, TilesX, TilesY
};

float HashScalar3(float3 Seed)
{
    let Shifted = frac(Seed * float3(0.1031, 0.1030, 0.0973));
    let Mixed   = Shifted + dot(Shifted, Shifted.yxz + 33.33);
    return frac((Mixed.x + Mixed.y) * Mixed.z);
}

float EvaluateFluidNoise3(float3 Query)
{
    let Base       = floor(Query);
    let Fractional = frac(Query);
    let Smooth     = Fractional * Fractional * (3.0 - 2.0 * Fractional);
    return lerp(lerp(lerp(HashScalar3(Base + float3(0.0, 0.0, 0.0)), HashScalar3(Base + float3(1.0, 0.0, 0.0)), Smooth.x),
                     lerp(HashScalar3(Base + float3(0.0, 1.0, 0.0)), HashScalar3(Base + float3(1.0, 1.0, 0.0)), Smooth.x), Smooth.y),
                lerp(lerp(HashScalar3(Base + float3(0.0, 0.0, 1.0)), HashScalar3(Base + float3(1.0, 0.0, 1.0)), Smooth.x),
                     lerp(HashScalar3(Base + float3(0.0, 1.0, 1.0)), HashScalar3(Base + float3(1.0, 1.0, 1.0)), Smooth.x), Smooth.y),
                Smooth.z);
}

float3 EvaluateFluidCurl3(float3 Query)
{
    let Step = 0.12;
    let N1   = EvaluateFluidNoise3(Query + float3(0.0, Step, 0.0)) - EvaluateFluidNoise3(Query - float3(0.0, Step, 0.0));
    let N2   = EvaluateFluidNoise3(Query + float3(0.0, 0.0, Step)) - EvaluateFluidNoise3(Query - float3(0.0, 0.0, Step));
    let N3   = EvaluateFluidNoise3(Query + float3(Step, 0.0, 0.0)) - EvaluateFluidNoise3(Query - float3(Step, 0.0, 0.0));
    let M1   = EvaluateFluidNoise3(Query.yzx + float3(31.7, Step, 11.3)) - EvaluateFluidNoise3(Query.yzx + float3(31.7, -Step, 11.3));
    let M2   = EvaluateFluidNoise3(Query.zxy + float3(17.1, 47.2, Step)) - EvaluateFluidNoise3(Query.zxy + float3(17.1, 47.2, -Step));
    return float3(N1 - N2, N2 - N3 + M1 * 0.4, N3 - N1 + M2 * 0.4) / (2.0 * Step);
}

// 📝 Signed distance in world-scaled domain units for all 6 collider types (1:1 with shaders-glsl.js sdObstacle).
//    Category: 0 = None, 1 = Sphere, 2 = Pillar, 3 = Horizontal cylinder, 4 = Deflector slab, 5 = Tyre ring.
float EvaluateColliderSignedDistance(float3 Uvw, int Category, float3 Centre, float Radius, float3 DomainScale)
{
    if (Category <= 0 || Radius <= 0.001)
    {
        return 10.0;
    }
    let Scaled = (Uvw - Centre) * DomainScale;
    if (Category == 1)
    {
        return length(Scaled) - Radius;
    }
    if (Category == 2)
    {
        let W = float2(length(Scaled.xz) - Radius * 0.75, abs(Uvw.y - (Centre.y * 0.5)) * DomainScale.y - Radius * 1.4);
        return min(max(W.x, W.y), 0.0) + length(max(W, float2(0.0)));
    }
    if (Category == 3)
    {
        let W = float2(length(Scaled.yz) - Radius * 0.72, abs(Scaled.x) - Radius * 1.9);
        return min(max(W.x, W.y), 0.0) + length(max(W, float2(0.0)));
    }
    if (Category == 5)
    {
        // Rounded treaded tyre: ring in the X/Y plane, axle along Z (1:1 with shaders-glsl.js sdObstacle).
        return length(float2(length(Scaled.xy) - Radius * 0.76, Scaled.z)) - Radius * 0.24;
    }
    let HalfExtents = float3(Radius * 1.5, Radius * 0.45, Radius * 1.5);
    let Delta       = abs(Scaled) - HalfExtents;
    return length(max(Delta, float3(0.0))) + min(max(Delta.x, max(Delta.y, Delta.z)), 0.0);
}

// 📝 All 6 combustion blackbody palettes (1:1 with shaders-wgsl.js evaluateBlackbodyPalette).
float3 EvaluateBlackbodyPalette(float Temperature, int Palette)
{
    let Core = clamp(max(0.0, Temperature) * 0.28, 0.0, 1.6);
    if (Core <= 0.01)
    {
        return float3(0.0);
    }

    if (Palette == 0)
    {
        var Colour = lerp(float3(0.0), float3(0.55, 0.03, 0.005), smoothstep(0.0, 0.22, Core));
        Colour     = lerp(Colour, float3(1.00, 0.24, 0.01), smoothstep(0.18, 0.52, Core));
        Colour     = lerp(Colour, float3(1.00, 0.68, 0.10), smoothstep(0.48, 0.88, Core));
        Colour     = lerp(Colour, float3(1.00, 0.96, 0.78), smoothstep(0.82, 1.35, Core));
        return Colour * (Core * Core * 1.65);
    }
    if (Palette == 1)
    {
        var Colour = lerp(float3(0.0), float3(0.42, 0.018, 0.003), smoothstep(0.0, 0.24, Core));
        Colour     = lerp(Colour, float3(1.00, 0.18, 0.008), smoothstep(0.20, 0.54, Core));
        Colour     = lerp(Colour, float3(1.00, 0.58, 0.06), smoothstep(0.50, 0.92, Core));
        Colour     = lerp(Colour, float3(1.00, 0.92, 0.65), smoothstep(0.86, 1.40, Core));
        return Colour * (Core * Core * 1.55);
    }
    if (Palette == 2)
    {
        let Colour = float3(smoothstep(0.01, 0.22, Core),
                            smoothstep(0.10, 0.55, Core),
                            smoothstep(0.28, 0.95, Core) * 1.15);
        return Colour * (Core * Core * 2.1);
    }
    if (Palette == 3)
    {
        var Colour = lerp(float3(0.0), float3(0.02, 0.38, 0.08), smoothstep(0.0, 0.25, Core));
        Colour     = lerp(Colour, float3(0.14, 0.95, 0.28), smoothstep(0.22, 0.58, Core));
        Colour     = lerp(Colour, float3(0.72, 1.00, 0.45), smoothstep(0.55, 1.25, Core));
        return Colour * (Core * Core * 1.7);
    }
    if (Palette == 4)
    {
        var Colour = lerp(float3(0.0), float3(0.04, 0.12, 0.65), smoothstep(0.0, 0.25, Core));
        Colour     = lerp(Colour, float3(0.08, 0.58, 1.00), smoothstep(0.22, 0.58, Core));
        Colour     = lerp(Colour, float3(0.55, 0.95, 1.00), smoothstep(0.55, 1.25, Core));
        return Colour * (Core * Core * 1.85);
    }
    var Colour = lerp(float3(0.0), float3(0.32, 0.02, 0.48), smoothstep(0.0, 0.25, Core));
    Colour     = lerp(Colour, float3(0.88, 0.08, 0.65), smoothstep(0.22, 0.58, Core));
    Colour     = lerp(Colour, float3(1.00, 0.55, 0.92), smoothstep(0.55, 1.25, Core));
    return Colour * (Core * Core * 1.75);
}
