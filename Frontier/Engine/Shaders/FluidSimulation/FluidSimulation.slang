//============================================================================================================================================
//                                                        FLUIDSIMULATION.SLANG
//============================================================================================================================================
// 🧩 Eulerian 3D incompressible Navier-Stokes, MacCormack advection, combustion, vorticity confinement, and Jacobi pressure/irradiance.

#pragma once
#include "FluidLayouts.slang"

[[vk::binding(0, 0)]] ConstantBuffer<FluidSimulationConstants> Simulation;
[[vk::binding(1, 0)]] Texture3D<float4>                        PrimaryRead;
[[vk::binding(2, 0)]] Texture3D<float4>                        SecondaryRead;
[[vk::binding(3, 0)]] Texture3D<float4>                        AuxiliaryRead;
[[vk::binding(4, 0)]] SamplerState                             LinearClamp;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> PrimaryWrite;
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> SecondaryWrite;

int3 ClampFluidTexel(int3 Texel, int GridSize)
{
    return clamp(Texel, int3(0), int3(GridSize - 1));
}

float4 LoadPrimaryClamped(int3 Texel, int GridSize)
{
    return PrimaryRead.Load(int4(ClampFluidTexel(Texel, GridSize), 0));
}

float4 LoadSecondaryClamped(int3 Texel, int GridSize)
{
    return SecondaryRead.Load(int4(ClampFluidTexel(Texel, GridSize), 0));
}

float4 LoadAuxiliaryClamped(int3 Texel, int GridSize)
{
    return AuxiliaryRead.Load(int4(ClampFluidTexel(Texel, GridSize), 0));
}

bool ClassifyBoundarySolid(int3 Texel, int GridSize, bool Enclosed)
{
    if (Texel.y < 0)
    {
        return true;
    }
    if (Enclosed && (any(Texel < int3(0)) || any(Texel >= int3(GridSize))))
    {
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------------------------------------------------
//                                            1. SOURCE, BRUSH, BLAST & COLLIDER
//------------------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(4, 4, 4)]
void InjectSources(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel       = int3(Cell);
    let InverseGrid = Simulation.GridTiming.y;
    let DeltaTime   = Simulation.GridTiming.z;
    let ElapsedTime = Simulation.GridTiming.w;
    let Uvw         = (float3(Cell) + 0.5) * InverseGrid;

    var Velocity    = PrimaryRead.Load(int4(Texel, 0));
    var Density     = SecondaryRead.Load(int4(Texel, 0));

    let Scale       = max(Simulation.DomainBounds.xyz, float3(0.25));
    let CellWorld   = InverseGrid * min(Scale.x, min(Scale.y, Scale.z));
    let Category    = int(Simulation.ObstacleGeometry.x + 0.5);
    let SignedDist  = EvaluateColliderSignedDistance(Uvw, Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale);
    let ObsWorldVel = Simulation.ObstacleVelocity.xyz * Scale;
    if (SignedDist < 0.0)
    {
        PrimaryWrite[Texel]   = float4(ObsWorldVel, 1.0);
        SecondaryWrite[Texel] = float4(0.0);
        return;
    }
    Velocity.w = 0.0;
    if (SignedDist < CellWorld * 2.2 && Category > 0)
    {
        let Step   = max(InverseGrid * 0.5, 0.004);
        let Grad   = float3(
            EvaluateColliderSignedDistance(Uvw + float3(Step, 0.0, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale) -
                EvaluateColliderSignedDistance(Uvw - float3(Step, 0.0, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale),
            EvaluateColliderSignedDistance(Uvw + float3(0.0, Step, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale) -
                EvaluateColliderSignedDistance(Uvw - float3(0.0, Step, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale),
            EvaluateColliderSignedDistance(Uvw + float3(0.0, 0.0, Step), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale) -
                EvaluateColliderSignedDistance(Uvw - float3(0.0, 0.0, Step), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale));
        let Normal = normalize(Grad + float3(1.0e-6));
        let Inward = dot(Velocity.xyz - ObsWorldVel, Normal);
        if (Inward < 0.0)
        {
            Velocity.xyz -= Normal * Inward;
        }
        let Skin     = smoothstep(CellWorld * 2.2, 0.0, SignedDist);
        Velocity.xyz = lerp(Velocity.xyz, ObsWorldVel, Skin * 0.45);
    }

    if (Simulation.SourceGeometry.x > 0.5)
    {
        let Centre   = float3(0.5, Simulation.SourceGeometry.z, 0.5);
        let Offset   = Uvw - Centre;
        let Elliptic = float3(Offset.x, Offset.y * 1.45, Offset.z);
        let Distance = length(Elliptic);
        let Radius   = max(Simulation.SourceGeometry.y, 0.015);
        if (Distance < Radius * 1.65)
        {
            let Falloff     = exp(-pow(Distance / Radius, 2.2) * 2.1);
            let NoiseA      = EvaluateFluidNoise3(Uvw * 13.0 + float3(0.0, -ElapsedTime * 3.8, ElapsedTime * 1.3));
            let NoiseB      = EvaluateFluidNoise3(Uvw * 27.0 + float3(ElapsedTime * 2.1, -ElapsedTime * 6.2, -ElapsedTime * 1.7));
            let Modulation  = clamp(0.42 + 0.65 * NoiseA + 0.35 * NoiseB, 0.08, 1.55);
            let Rate        = Simulation.SourceGeometry.w * DeltaTime * Falloff * Modulation;

            Density.z      += Simulation.SourcePayload.x * Rate;
            Density.y       = max(Density.y, Simulation.SourcePayload.z * Falloff * (0.68 + 0.42 * NoiseA));
            Density.y      += Simulation.SourcePayload.z * 0.6 * Rate;
            Density.x      += Simulation.SourcePayload.y * Rate;

            let Tangent     = float3(-Offset.z, 0.0, Offset.x) / max(length(Offset.xz), 0.015);
            let Upward      = Simulation.SourcePayload.w * (0.75 + 0.55 * NoiseB);
            let Swirl       = Tangent * Simulation.SourceMotion.x * (Distance / Radius);
            let RadialPush  = normalize(Offset + float3(0.0, 0.001, 0.0)) * (NoiseA - 0.45) * 1.35;
            Velocity.xyz   += (float3(0.0, Upward, 0.0) + Swirl + RadialPush) * Falloff * min(1.0, DeltaTime * 9.0);
        }
    }

    if (Simulation.SourceMotion.y > 0.5)
    {
        let Offset   = Uvw - Simulation.BrushPosition.xyz;
        let Distance = length(Offset);
        let Radius   = max(Simulation.SourceMotion.z, 0.02);
        if (Distance < Radius * 1.6)
        {
            let Falloff   = exp(-pow(Distance / Radius, 2.0) * 2.2);
            let Variation = 0.65 + 0.6 * EvaluateFluidNoise3(Uvw * 18.0 + ElapsedTime * 4.0);
            let Rate      = DeltaTime * 3.2 * Falloff * Variation;
            Density.z    += 3.4 * Rate;
            Density.y    += 4.2 * Rate;
            Density.x    += 1.6 * Rate;
            Velocity.xyz += (Simulation.BrushVelocity.xyz + float3(0.0, 3.6, 0.0)) * Falloff * min(1.0, DeltaTime * 14.0);
        }
    }

    if (Simulation.SourceMotion.w > 0.5)
    {
        let Offset   = Uvw - Simulation.BlastPosition.xyz;
        let Distance = length(Offset);
        let Radius   = max(Simulation.BlastGeometry.x, 0.025);
        if (Distance < Radius * 2.0)
        {
            let Direction    = normalize(Offset + float3(1.0e-5, 2.0e-5, -1.0e-5));
            let Lobes        = Simulation.BlastPayload.y;
            let LobePattern  = sin(Direction.x * Lobes * 3.1 + ElapsedTime * 2.0) *
                               cos(Direction.y * Lobes * 2.7 - 0.8) *
                               sin(Direction.z * Lobes * 3.5 + 1.4);
            let PerturbedR   = Radius * (1.0 + 0.34 * LobePattern + 0.22 * (EvaluateFluidNoise3(Direction * 7.5 + ElapsedTime) - 0.5));
            let Core         = exp(-pow(Distance / max(PerturbedR, 0.015), 2.0) * 1.85);
            let ShockShell   = exp(-pow((Distance - PerturbedR * 0.78) / max(Radius * 0.28, 0.012), 2.0));

            Density.z       += Simulation.BlastGeometry.w * Core * 0.95;
            Density.y       += Simulation.BlastGeometry.z * (Core * 0.92 + ShockShell * 0.32);
            Density.x       += Simulation.BlastPayload.x * (Core * 0.55 + ShockShell * 0.95);
            Density.w       += Simulation.BlastPayload.x * 0.60 * (Core * 0.4 + ShockShell * 0.9);

            let CurlSpike    = EvaluateFluidCurl3(Uvw * 9.0 + ElapsedTime * 1.7);
            var OutwardSpeed = Simulation.BlastGeometry.y * (0.22 * Core + 0.42 * ShockShell);
            var Impulse      = (Direction * OutwardSpeed + CurlSpike * Simulation.BlastGeometry.y * 0.24 * Core);
            Impulse.y       += Simulation.BlastGeometry.y * 0.48 * Core;
            if (Simulation.BlastPayload.z > 0.5)
            {
                Impulse += Simulation.BlastVelocity.xyz * (0.65 * Core + 0.45 * ShockShell);
            }
            Velocity.xyz    += Impulse;
        }
    }

    Density = clamp(Density, float4(0.0), float4(16.0, 22.0, 14.0, 12.0));
    PrimaryWrite[Texel]   = Velocity;
    SecondaryWrite[Texel] = Density;
}

//------------------------------------------------------------------------------------------------------------------------
//                                         2. MACCORMACK ADVECTION & COMBUSTION
//------------------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(4, 4, 4)]
void AdvectFields(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel       = int3(Cell);
    let InverseGrid = Simulation.GridTiming.y;
    let DeltaTime   = Simulation.GridTiming.z;
    let Uvw         = (float3(Cell) + 0.5) * InverseGrid;

    let CurrentVel  = PrimaryRead.Load(int4(Texel, 0));
    if (CurrentVel.w > 0.5)
    {
        PrimaryWrite[Texel]   = CurrentVel;
        SecondaryWrite[Texel] = float4(0.0);
        return;
    }

    let Scale       = max(Simulation.DomainBounds.xyz, float3(0.25));
    let Category    = int(Simulation.ObstacleGeometry.x + 0.5);
    let UvwVelocity = CurrentVel.xyz / Scale;
    var BackPos     = clamp(Uvw - DeltaTime * UvwVelocity, float3(0.5 * InverseGrid), float3(1.0 - 0.5 * InverseGrid));
    if (Category > 0)
    {
        let BackSigned = EvaluateColliderSignedDistance(BackPos, Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale);
        if (BackSigned < 0.0)
        {
            let Step   = max(InverseGrid * 0.5, 0.004);
            let Grad   = float3(
                EvaluateColliderSignedDistance(BackPos + float3(Step, 0.0, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale) -
                    EvaluateColliderSignedDistance(BackPos - float3(Step, 0.0, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale),
                EvaluateColliderSignedDistance(BackPos + float3(0.0, Step, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale) -
                    EvaluateColliderSignedDistance(BackPos - float3(0.0, Step, 0.0), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale),
                EvaluateColliderSignedDistance(BackPos + float3(0.0, 0.0, Step), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale) -
                    EvaluateColliderSignedDistance(BackPos - float3(0.0, 0.0, Step), Category, Simulation.ObstaclePosition.xyz, Simulation.ObstacleGeometry.y, Scale));
            let Normal = normalize(Grad + float3(1.0e-6));
            BackPos    = clamp(BackPos + (Normal / Scale) * (-BackSigned + 0.25 * InverseGrid * min(Scale.x, min(Scale.y, Scale.z))),
                               float3(0.5 * InverseGrid), float3(1.0 - 0.5 * InverseGrid));
        }
    }

    let ForwardVel  = PrimaryRead.SampleLevel(LinearClamp, BackPos, 0.0);
    var AdvectedVel = ForwardVel.xyz;
    var Density     = SecondaryRead.SampleLevel(LinearClamp, BackPos, 0.0);

    if (Simulation.BlastPayload.w > 0.5)
    {
        let ReturnPos  = clamp(BackPos + DeltaTime * (ForwardVel.xyz / Scale), float3(0.5 * InverseGrid), float3(1.0 - 0.5 * InverseGrid));
        let BackDen    = SecondaryRead.SampleLevel(LinearClamp, ReturnPos, 0.0);
        let OriginDen  = SecondaryRead.Load(int4(Texel, 0));
        let Corrected  = Density + 0.5 * (OriginDen - BackDen);

        let BaseTexel  = int3(floor(BackPos * Simulation.GridTiming.x - 0.5));
        var MinValue   = float4(1.0e9);
        var MaxValue   = float4(-1.0e9);
        for (var Dz = 0; Dz <= 1; Dz++)
        {
            for (var Dy = 0; Dy <= 1; Dy++)
            {
                for (var Dx = 0; Dx <= 1; Dx++)
                {
                    let Tap  = LoadSecondaryClamped(BaseTexel + int3(Dx, Dy, Dz), GridSize);
                    MinValue = min(MinValue, Tap);
                    MaxValue = max(MaxValue, Tap);
                }
            }
        }
        Density = clamp(Corrected, MinValue, MaxValue);

        let BackVel = PrimaryRead.SampleLevel(LinearClamp, ReturnPos, 0.0).xyz;
        var MinVel  = float3(1.0e9);
        var MaxVel  = float3(-1.0e9);
        for (var Vz = 0; Vz <= 1; Vz++)
        {
            for (var Vy = 0; Vy <= 1; Vy++)
            {
                for (var Vx = 0; Vx <= 1; Vx++)
                {
                    let Tap = LoadPrimaryClamped(BaseTexel + int3(Vx, Vy, Vz), GridSize).xyz;
                    MinVel  = min(MinVel, Tap);
                    MaxVel  = max(MaxVel, Tap);
                }
            }
        }
        AdvectedVel = clamp(AdvectedVel + 0.5 * (CurrentVel.xyz - BackVel), MinVel, MaxVel);
    }

    var Smoke       = max(0.0, Density.x);
    var Temperature = max(0.0, Density.y);
    var Fuel        = max(0.0, Density.z);
    var Soot        = max(0.0, Density.w);

    let Ignition    = smoothstep(0.18, 0.65, Temperature);
    let Burned      = min(Fuel, Fuel * Ignition * Simulation.WindCombustion.z * DeltaTime);
    Fuel           -= Burned;
    Temperature    += Burned * Simulation.WindCombustion.w;
    Smoke          += Burned * Simulation.ThermalDissipation.x;
    Soot           += Burned * Simulation.ThermalDissipation.x * 0.65;

    let Radiative   = Simulation.ThermalDissipation.z * (0.35 * Temperature + 0.22 * Temperature * Temperature) * DeltaTime;
    Temperature     = max(0.0, Temperature - Radiative);
    Smoke          *= exp(-Simulation.ThermalDissipation.w * DeltaTime);
    Fuel           *= exp(-0.08 * DeltaTime);
    Soot           *= exp(-Simulation.ThermalDissipation.w * 0.8 * DeltaTime);

    var NextVel     = AdvectedVel * exp(-Simulation.ObstacleGeometry.z * DeltaTime);

    if (Simulation.DomainBounds.w < 0.5)
    {
        let EdgeDist = min(min(Uvw.x, 1.0 - Uvw.x), min(1.0 - Uvw.y, min(Uvw.z, 1.0 - Uvw.z)));
        let Sponge   = smoothstep(0.0, 0.085, EdgeDist);
        Smoke       *= lerp(0.82, 1.0, Sponge);
        Temperature *= lerp(0.86, 1.0, Sponge);
        Fuel        *= lerp(0.82, 1.0, Sponge);
        NextVel     *= lerp(0.88, 1.0, Sponge);
    }

    PrimaryWrite[Texel]   = float4(NextVel, 0.0);
    SecondaryWrite[Texel] = float4(min(Smoke, 16.0), min(Temperature, 24.0), min(Fuel, 16.0), min(Soot, 12.0));
}

//------------------------------------------------------------------------------------------------------------------------
//                                              3. VORTICITY & FORCES
//------------------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(4, 4, 4)]
void EvaluateCurl(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel     = int3(Cell);
    let VelocityR = LoadPrimaryClamped(Texel + int3(1, 0, 0), GridSize).xyz;
    let VelocityL = LoadPrimaryClamped(Texel - int3(1, 0, 0), GridSize).xyz;
    let VelocityU = LoadPrimaryClamped(Texel + int3(0, 1, 0), GridSize).xyz;
    let VelocityD = LoadPrimaryClamped(Texel - int3(0, 1, 0), GridSize).xyz;
    let VelocityF = LoadPrimaryClamped(Texel + int3(0, 0, 1), GridSize).xyz;
    let VelocityB = LoadPrimaryClamped(Texel - int3(0, 0, 1), GridSize).xyz;

    let Omega     = 0.5 * float3((VelocityU.z - VelocityD.z) - (VelocityF.y - VelocityB.y),
                                 (VelocityF.x - VelocityB.x) - (VelocityR.z - VelocityL.z),
                                 (VelocityR.y - VelocityL.y) - (VelocityU.x - VelocityL.x));
    PrimaryWrite[Texel] = float4(Omega, length(Omega));
}

[shader("compute")]
[numthreads(4, 4, 4)]
void IntegrateForces(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel    = int3(Cell);
    let Velocity = PrimaryRead.Load(int4(Texel, 0));
    if (Velocity.w > 0.5)
    {
        PrimaryWrite[Texel] = Velocity;
        return;
    }

    let InverseGrid = Simulation.GridTiming.y;
    let DeltaTime   = Simulation.GridTiming.z;
    let ElapsedTime = Simulation.GridTiming.w;
    let Uvw         = (float3(Cell) + 0.5) * InverseGrid;
    let Density     = SecondaryRead.Load(int4(Texel, 0));

    let MagR        = LoadAuxiliaryClamped(Texel + int3(1, 0, 0), GridSize).w;
    let MagL        = LoadAuxiliaryClamped(Texel - int3(1, 0, 0), GridSize).w;
    let MagU        = LoadAuxiliaryClamped(Texel + int3(0, 1, 0), GridSize).w;
    let MagD        = LoadAuxiliaryClamped(Texel - int3(0, 1, 0), GridSize).w;
    let MagF        = LoadAuxiliaryClamped(Texel + int3(0, 0, 1), GridSize).w;
    let MagB        = LoadAuxiliaryClamped(Texel - int3(0, 0, 1), GridSize).w;
    let Eta         = 0.5 * float3(MagR - MagL, MagU - MagD, MagF - MagB);
    let Normal      = Eta / (length(Eta) + 1.0e-5);
    let Omega       = AuxiliaryRead.Load(int4(Texel, 0)).xyz;
    let VortForce   = Simulation.ObstacleGeometry.w * cross(Normal, Omega);

    let LiftForce   = float3(0.0, Simulation.BuoyancyTurbulence.x * Density.y - Simulation.BuoyancyTurbulence.y * Density.x, 0.0);

    let TempR       = LoadSecondaryClamped(Texel + int3(1, 0, 0), GridSize).y;
    let TempL       = LoadSecondaryClamped(Texel - int3(1, 0, 0), GridSize).y;
    let TempF       = LoadSecondaryClamped(Texel + int3(0, 0, 1), GridSize).y;
    let TempB       = LoadSecondaryClamped(Texel - int3(0, 0, 1), GridSize).y;
    let Baroclinic  = float3(-(TempR - TempL), 0.0, -(TempF - TempB)) * 0.22 * smoothstep(0.2, 2.5, Density.y);

    let Activity    = clamp(Density.x * 0.45 + Density.y * 0.35 + length(Velocity.xyz) * 0.08, 0.0, 1.6);
    let TurbField   = EvaluateFluidCurl3(Uvw * Simulation.BuoyancyTurbulence.w + float3(0.0, -ElapsedTime * 0.85, ElapsedTime * 0.35));
    let TurbForce   = TurbField * Simulation.BuoyancyTurbulence.z * Activity;

    let HeightProf  = smoothstep(0.02, 0.55, Uvw.y);
    let WindForce   = float3(Simulation.WindCombustion.x, 0.0, Simulation.WindCombustion.y) * HeightProf * (0.35 + 0.65 * clamp(Density.x + Density.y * 0.3, 0.0, 1.0));

    let NextVel     = clamp(Velocity.xyz + (VortForce + LiftForce + Baroclinic + TurbForce + WindForce) * DeltaTime, float3(-28.0), float3(28.0));
    PrimaryWrite[Texel] = float4(NextVel, 0.0);
}

//------------------------------------------------------------------------------------------------------------------------
//                                  4. DIVERGENCE, JACOBI PRESSURE/IRRADIANCE & PROJECTION
//------------------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(4, 4, 4)]
void EvaluateDivergence(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel    = int3(Cell);
    let Enclosed = Simulation.DomainBounds.w > 0.5;
    let Centre   = PrimaryRead.Load(int4(Texel, 0));
    if (Centre.w > 0.5)
    {
        PrimaryWrite[Texel] = float4(0.0);
        return;
    }

    let VelR = SelectBranch(LoadPrimaryClamped(Texel + int3(1, 0, 0), GridSize), float4(0.0, 0.0, 0.0, 1.0), ClassifyBoundarySolid(Texel + int3(1, 0, 0), GridSize, Enclosed));
    let VelL = SelectBranch(LoadPrimaryClamped(Texel - int3(1, 0, 0), GridSize), float4(0.0, 0.0, 0.0, 1.0), ClassifyBoundarySolid(Texel - int3(1, 0, 0), GridSize, Enclosed));
    let VelU = SelectBranch(LoadPrimaryClamped(Texel + int3(0, 1, 0), GridSize), float4(0.0, 0.0, 0.0, 1.0), ClassifyBoundarySolid(Texel + int3(0, 1, 0), GridSize, Enclosed));
    let VelD = SelectBranch(LoadPrimaryClamped(Texel - int3(0, 1, 0), GridSize), float4(0.0, 0.0, 0.0, 1.0), ClassifyBoundarySolid(Texel - int3(0, 1, 0), GridSize, Enclosed));
    let VelF = SelectBranch(LoadPrimaryClamped(Texel + int3(0, 0, 1), GridSize), float4(0.0, 0.0, 0.0, 1.0), ClassifyBoundarySolid(Texel + int3(0, 0, 1), GridSize, Enclosed));
    let VelB = SelectBranch(LoadPrimaryClamped(Texel - int3(0, 0, 1), GridSize), float4(0.0, 0.0, 0.0, 1.0), ClassifyBoundarySolid(Texel - int3(0, 0, 1), GridSize, Enclosed));

    let RightX = SelectBranch(VelR.x, 0.0, VelR.w > 0.5);
    let LeftX  = SelectBranch(VelL.x, 0.0, VelL.w > 0.5);
    let UpY    = SelectBranch(VelU.y, 0.0, VelU.w > 0.5);
    let DownY  = SelectBranch(VelD.y, 0.0, VelD.w > 0.5);
    let FrontZ = SelectBranch(VelF.z, 0.0, VelF.w > 0.5);
    let BackZ  = SelectBranch(VelB.z, 0.0, VelB.w > 0.5);

    var Divergence = 0.5 * ((RightX - LeftX) + (UpY - DownY) + (FrontZ - BackZ)) * Simulation.GridTiming.x;
    let Density    = SecondaryRead.Load(int4(Texel, 0));
    let React      = Density.z * smoothstep(0.18, 0.65, Density.y) * Simulation.WindCombustion.z;
    Divergence    -= React * Simulation.ThermalDissipation.y * 0.48;

    let FirePower  = pow(max(0.0, Density.y - 0.32), 1.65) * 0.55;
    let Mask       = (VelR.w > 0.5 ? 1.0 : 0.0) + (VelL.w > 0.5 ? 2.0 : 0.0) + (VelU.w > 0.5 ? 4.0 : 0.0) +
                     (VelD.w > 0.5 ? 8.0 : 0.0) + (VelF.w > 0.5 ? 16.0 : 0.0) + (VelB.w > 0.5 ? 32.0 : 0.0);
    PrimaryWrite[Texel] = float4(Divergence, FirePower, Mask, 0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void RelaxPressure(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel    = int3(Cell);
    let Enclosed = Simulation.DomainBounds.w > 0.5;
    let DivData  = SecondaryRead.Load(int4(Texel, 0));
    let Mask     = int(DivData.z + 0.5);
    let CentreP  = PrimaryRead.Load(int4(Texel, 0)).x;

    let OutR     = !Enclosed && (Texel.x + 1 >= GridSize);
    let OutL     = !Enclosed && (Texel.x - 1 < 0);
    let OutU     = !Enclosed && (Texel.y + 1 >= GridSize);
    let OutF     = !Enclosed && (Texel.z + 1 >= GridSize);
    let OutB     = !Enclosed && (Texel.z - 1 < 0);

    let TapR     = LoadPrimaryClamped(Texel + int3(1, 0, 0), GridSize);
    let TapL     = LoadPrimaryClamped(Texel - int3(1, 0, 0), GridSize);
    let TapU     = LoadPrimaryClamped(Texel + int3(0, 1, 0), GridSize);
    let TapD     = LoadPrimaryClamped(Texel - int3(0, 1, 0), GridSize);
    let TapF     = LoadPrimaryClamped(Texel + int3(0, 0, 1), GridSize);
    let TapB     = LoadPrimaryClamped(Texel - int3(0, 0, 1), GridSize);

    let PressR   = SelectBranch(SelectBranch(TapR.x, CentreP, (Mask & 1) != 0), 0.0, OutR);
    let PressL   = SelectBranch(SelectBranch(TapL.x, CentreP, (Mask & 2) != 0), 0.0, OutL);
    let PressU   = SelectBranch(SelectBranch(TapU.x, CentreP, (Mask & 4) != 0), 0.0, OutU);
    let PressD   = SelectBranch(TapD.x, CentreP, (Mask & 8) != 0);
    let PressF   = SelectBranch(SelectBranch(TapF.x, CentreP, (Mask & 16) != 0), 0.0, OutF);
    let PressB   = SelectBranch(SelectBranch(TapB.x, CentreP, (Mask & 32) != 0), 0.0, OutB);

    let HSquared = Simulation.GridTiming.y * Simulation.GridTiming.y;
    let NextP    = (PressR + PressL + PressU + PressD + PressF + PressB - DivData.x * HSquared) / 6.0;
    let NextPhi  = (TapR.y + TapL.y + TapU.y + TapD.y + TapF.y + TapB.y + DivData.y * 0.42) / 6.32;
    PrimaryWrite[Texel] = float4(NextP, NextPhi, 0.0, 0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ProjectGradient(uint3 Cell : SV_DispatchThreadID)
{
    let GridSize = int(Simulation.GridTiming.x + 0.5);
    if (any(int3(Cell) >= int3(GridSize)))
    {
        return;
    }
    let Texel    = int3(Cell);
    let Enclosed = Simulation.DomainBounds.w > 0.5;
    let Velocity = PrimaryRead.Load(int4(Texel, 0));
    if (Velocity.w > 0.5)
    {
        PrimaryWrite[Texel] = Velocity;
        return;
    }

    let CentreP  = SecondaryRead.Load(int4(Texel, 0)).x;
    let SolidR   = ClassifyBoundarySolid(Texel + int3(1, 0, 0), GridSize, Enclosed) || LoadPrimaryClamped(Texel + int3(1, 0, 0), GridSize).w > 0.5;
    let SolidL   = ClassifyBoundarySolid(Texel - int3(1, 0, 0), GridSize, Enclosed) || LoadPrimaryClamped(Texel - int3(1, 0, 0), GridSize).w > 0.5;
    let SolidU   = ClassifyBoundarySolid(Texel + int3(0, 1, 0), GridSize, Enclosed) || LoadPrimaryClamped(Texel + int3(0, 1, 0), GridSize).w > 0.5;
    let SolidD   = ClassifyBoundarySolid(Texel - int3(0, 1, 0), GridSize, Enclosed) || LoadPrimaryClamped(Texel - int3(0, 1, 0), GridSize).w > 0.5;
    let SolidF   = ClassifyBoundarySolid(Texel + int3(0, 0, 1), GridSize, Enclosed) || LoadPrimaryClamped(Texel + int3(0, 0, 1), GridSize).w > 0.5;
    let SolidB   = ClassifyBoundarySolid(Texel - int3(0, 0, 1), GridSize, Enclosed) || LoadPrimaryClamped(Texel - int3(0, 0, 1), GridSize).w > 0.5;

    let OutR     = !Enclosed && (Texel.x + 1 >= GridSize);
    let OutL     = !Enclosed && (Texel.x - 1 < 0);
    let OutU     = !Enclosed && (Texel.y + 1 >= GridSize);
    let OutF     = !Enclosed && (Texel.z + 1 >= GridSize);
    let OutB     = !Enclosed && (Texel.z - 1 < 0);

    let PressR   = SelectBranch(SelectBranch(LoadSecondaryClamped(Texel + int3(1, 0, 0), GridSize).x, CentreP, SolidR), 0.0, OutR);
    let PressL   = SelectBranch(SelectBranch(LoadSecondaryClamped(Texel - int3(1, 0, 0), GridSize).x, CentreP, SolidL), 0.0, OutL);
    let PressU   = SelectBranch(SelectBranch(LoadSecondaryClamped(Texel + int3(0, 1, 0), GridSize).x, CentreP, SolidU), 0.0, OutU);
    let PressD   = SelectBranch(LoadSecondaryClamped(Texel - int3(0, 1, 0), GridSize).x, CentreP, SolidD);
    let PressF   = SelectBranch(SelectBranch(LoadSecondaryClamped(Texel + int3(0, 0, 1), GridSize).x, CentreP, SolidF), 0.0, OutF);
    let PressB   = SelectBranch(SelectBranch(LoadSecondaryClamped(Texel - int3(0, 0, 1), GridSize).x, CentreP, SolidB), 0.0, OutB);

    let Gradient = 0.5 * float3(PressR - PressL, PressU - PressD, PressF - PressB) * Simulation.GridTiming.x;
    var NextVel  = Velocity.xyz - Gradient;

    if (Texel.y == 0 && NextVel.y < 0.0)
    {
        NextVel.y = 0.0;
    }
    if (Enclosed)
    {
        if ((Texel.x == 0 && NextVel.x < 0.0) || (Texel.x == GridSize - 1 && NextVel.x > 0.0))
        {
            NextVel.x = 0.0;
        }
        if (Texel.y == GridSize - 1 && NextVel.y > 0.0)
        {
            NextVel.y = 0.0;
        }
        if ((Texel.z == 0 && NextVel.z < 0.0) || (Texel.z == GridSize - 1 && NextVel.z > 0.0))
        {
            NextVel.z = 0.0;
        }
    }
    PrimaryWrite[Texel] = float4(NextVel, 0.0);
}
