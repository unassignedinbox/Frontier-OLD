//============================================================================================================================================
//                                                          PLUMELAYOUTS.SLANG
//============================================================================================================================================
// 🧩 Toroidal world-grid plume constants, source descriptors, coordinate wrapping, and Planckian blackbody radiance.

#pragma once
#include "IntrinsicParity.slang"

// 📝 One moving window, two toroidal lattices over it:
//    — Thermo (Extent.xyz, VoxelSize) carries smoke density, heat, soot fraction, and flame progress;
//    — Velocity (CoarseExtent.xyz, CoarseSize = 2 · VoxelSize) carries air flow, vorticity confinement, and pressure.
//    World cell c lives at texel c mod N, so scrolling the window as the focus moves relabels texels without copying.

struct PlumeConstants
{
    int4   OriginCell;          // [cell] xyz world cell at corner (0,0,0) of Thermo; w SourceCount
    uint4  Extent;              // [cell] xyz Thermo dimensions; w JacobiIterations
    int4   CoarseOrigin;        // [cell] xyz world cell at corner (0,0,0) of Velocity; w ResetFlag
    uint4  CoarseExtent;        // [cell] xyz Velocity dimensions; w StepIndex
    int4   PreviousOrigin;      // [cell] xyz OriginCell before scrolling; w PreviousCoarseValid
    int4   PreviousCoarse;      // [cell] xyz CoarseOrigin before scrolling; w MarchSteps
    float4 CellMetrics;         // [m,1/m,m,1/m] VoxelSize, InverseVoxel, CoarseSize, InverseCoarse
    float4 WindowMinimum;       // [m,m,m,s] xyz Thermo world minimum; w DeltaTime
    float4 WindowMaximum;       // [m,m,m,s] xyz Thermo world maximum; w ElapsedTime
    float4 WindVector;          // [m/s,m/s,m/s,1/s] xyz ambient wind; w VelocityDrag
    float4 BuoyancyTerms;       // [m/s²/K,m/s²,m,1/s] Buoyancy, GravityLoad, Confinement, HeatCooling
    float4 Dissipation;         // [1/s,1/s,m,1/m] SmokeFade, FlameBurnRate, SwirlScale, Extinction
    float4 OpticalTerms;        // [-] AlbedoWhite, AlbedoSoot, PhaseForward, PhaseBackward
    float4 ScatteringTerms;     // [-] PhaseBlend, MultiScatter, ShadowStepScale, DetailStrength
    float4 FlameTerms;          // [cd/m²,K,m,-] FlameRadiance, FlameTemperature, DetailScale, FloorHeight
    uint4  TraceDimensions;     // [px,px,-,-] TraceWidth, TraceHeight, ShadowSteps, QualityGrade
};

struct PlumeSource
{
    float4 PositionRadius;      // [m]    xyz world centre; w 1/e Gaussian radius
    float4 VelocityRate;        // [m/s]  xyz injected flow velocity; w smoke injection rate [1/s]
    float4 ThermalPayload;      // [K/s,1/s,-,1/s] HeatRate, FlameRate, SootFraction, VelocityBlend
};

struct PlumeFireLight
{
    float4 PositionIntensity;   // [m,lm] xyz world seat; w luminous power
    float4 ColourReach;         // [-,m]  rgb linear tint; w cutoff radius
};

int3 WrapLattice(int3 Cell, int3 Dimensions)
{
    return ((Cell % Dimensions) + Dimensions) % Dimensions;
}

int3 ResolveVelocityTexel(PlumeConstants Plume, uint3 LocalCell)
{
    let Dimensions = int3(Plume.CoarseExtent.xyz);
    return WrapLattice(Plume.CoarseOrigin.xyz + int3(LocalCell), Dimensions);
}

int3 ResolveThermoTexel(PlumeConstants Plume, uint3 LocalCell)
{
    let Dimensions = int3(Plume.Extent.xyz);
    return WrapLattice(Plume.OriginCell.xyz + int3(LocalCell), Dimensions);
}

float3 ProjectVelocityWorld(PlumeConstants Plume, uint3 LocalCell)
{
    return (float3(Plume.CoarseOrigin.xyz + int3(LocalCell)) + 0.5) * Plume.CellMetrics.z;
}

float3 ProjectThermoWorld(PlumeConstants Plume, uint3 LocalCell)
{
    return (float3(Plume.OriginCell.xyz + int3(LocalCell)) + 0.5) * Plume.CellMetrics.x;
}

float3 ProjectVelocityCoordinates(PlumeConstants Plume, float3 Position)
{
    let Dimensions = float3(Plume.CoarseExtent.xyz);
    return frac(Position / (Dimensions * Plume.CellMetrics.z));
}

float3 ProjectThermoCoordinates(PlumeConstants Plume, float3 Position)
{
    let Dimensions = float3(Plume.Extent.xyz);
    return frac(Position / (Dimensions * Plume.CellMetrics.x));
}

bool ClassifyWindowInterior(PlumeConstants Plume, float3 Position, float Margin)
{
    return all(Position >= Plume.WindowMinimum.xyz + float3(Margin)) &&
           all(Position <= Plume.WindowMaximum.xyz - float3(Margin));
}

bool ClassifyPreviousVelocity(PlumeConstants Plume, float3 Position)
{
    if (Plume.PreviousOrigin.w == 0)
    {
        return false;
    }
    let Low  = float3(Plume.PreviousCoarse.xyz) * Plume.CellMetrics.z;
    let High = Low + float3(Plume.CoarseExtent.xyz) * Plume.CellMetrics.z;
    let Pad  = Plume.CellMetrics.z * 0.5;
    return all(Position >= Low + float3(Pad)) && all(Position <= High - float3(Pad));
}

bool ClassifyPreviousThermo(PlumeConstants Plume, float3 Position)
{
    if (Plume.PreviousOrigin.w == 0)
    {
        return false;
    }
    let Low  = float3(Plume.PreviousOrigin.xyz) * Plume.CellMetrics.x;
    let High = Low + float3(Plume.Extent.xyz) * Plume.CellMetrics.x;
    let Pad  = Plume.CellMetrics.x * 0.5;
    return all(Position >= Low + float3(Pad)) && all(Position <= High - float3(Pad));
}

float ResolveSourceWeight(PlumeSource Source, float3 Position, float CellRadius)
{
    let Radius        = max(Source.PositionRadius.w, CellRadius * 0.65);
    let Offset        = (Position - Source.PositionRadius.xyz) / Radius;
    let DistanceSq    = dot(Offset, Offset);
    if (DistanceSq > 9.0)
    {
        return 0.0;
    }
    return exp(-DistanceSq);
}

// 📝 Kim-Lee analytic Planckian locus fit for hydrocarbon soot incandescence (900 K ember -> 2400 K bright yellow-white).
float3 ResolveBlackbodyColour(float Kelvin)
{
    let Clamped = clamp(Kelvin, 800.0, 4200.0) / 1000.0;
    let Red     = 1.0;
    let Green   = saturate(0.39008158 * log(max(Clamped, 0.9)) + 0.16190357 * Clamped - 0.14120410);
    let Blue    = SelectBranch(0.0, saturate(0.54320679 * log(max(Clamped - 1.0, 0.01)) + 0.19625408 * Clamped - 0.52703704), Clamped > 1.05);
    return float3(Red, Green * Green * (1.25 - 0.25 * Green), Blue * Blue);
}

float3 ResolveBlackbodyRadiance(float Flame, float Heat, PlumeConstants Plume)
{
    let Core = saturate(Flame);
    if (Core <= 1.0e-4)
    {
        return float3(0.0);
    }
    let Kelvin     = lerp(950.0, Plume.FlameTerms.y, pow(Core, 0.55)) + min(Heat * 0.35, 350.0);
    let Brightness = pow(Core, 1.35) * Plume.FlameTerms.x;
    return ResolveBlackbodyColour(Kelvin) * Brightness;
}
