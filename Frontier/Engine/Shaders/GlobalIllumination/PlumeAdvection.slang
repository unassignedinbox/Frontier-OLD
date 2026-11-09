//============================================================================================================================================
//                                                         PLUMEADVECTION.SLANG
//============================================================================================================================================
// 🧩 Toroidal plume semi-Lagrangian advection, buoyancy, two-octave curl swirl, scene SDF collision, and sun transmittance.

#pragma once
#include "IntrinsicParity.slang"
#include "HostConstants.slang"
#include "RefreshDescriptor.slang"
#include "DistanceMarch.slang"
#include "PlumeLayouts.slang"

[[vk::binding(1, 0)]] ConstantBuffer<PlumeConstants>           Plume;
[[vk::binding(2, 0)]] StructuredBuffer<PlumeSource>            Sources;
[[vk::binding(3, 0)]] Texture3D<float4>                        VelocityRead;
[[vk::binding(4, 0)]] Texture3D<float4>                        ThermoRead;
[[vk::binding(5, 0)]] SamplerState                             LinearRepeat;
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> VelocityWrite;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> ThermoWrite;
[[vk::binding(8, 0)]] [[vk::image_format("r16f")]]    RWTexture3D<float4> TransmittanceWrite;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint>                 Coverage;
[[vk::binding(10, 0)]] StructuredBuffer<OccupantProfile>       Occupants;

groupshared uint3 GroupLow;
groupshared uint3 GroupHigh;
groupshared uint  GroupOccupied;

float HashPlumeCell(int3 Cell)
{
    var Bits = uint(Cell.x) * 374761393u + uint(Cell.y) * 668265263u + uint(Cell.z) * 2147483647u;
    Bits = (Bits ^ (Bits >> 13u)) * 1274126177u;
    return float(Bits ^ (Bits >> 16u)) * (1.0 / 4294967295.0);
}

float EvaluateSmoothValue(float3 Position)
{
    let Base       = int3(floor(Position));
    let Fractional = frac(Position);
    let Weight     = Fractional * Fractional * (3.0 - 2.0 * Fractional);
    let C000       = HashPlumeCell(Base);
    let C100       = HashPlumeCell(Base + int3(1, 0, 0));
    let C010       = HashPlumeCell(Base + int3(0, 1, 0));
    let C110       = HashPlumeCell(Base + int3(1, 1, 0));
    let C001       = HashPlumeCell(Base + int3(0, 0, 1));
    let C101       = HashPlumeCell(Base + int3(1, 0, 1));
    let C011       = HashPlumeCell(Base + int3(0, 1, 1));
    let C111       = HashPlumeCell(Base + int3(1, 1, 1));
    let X00        = lerp(C000, C100, Weight.x);
    let X10        = lerp(C010, C110, Weight.x);
    let X01        = lerp(C001, C101, Weight.x);
    let X11        = lerp(C011, C111, Weight.x);
    let Y0         = lerp(X00, X10, Weight.y);
    let Y1         = lerp(X01, X11, Weight.y);
    return lerp(Y0, Y1, Weight.z) * 2.0 - 1.0;
}

float3 EvaluatePotential(float3 Query)
{
    return float3(EvaluateSmoothValue(Query + float3(17.1, 3.7, 9.2)),
                  EvaluateSmoothValue(Query + float3(5.3, 29.4, 13.8)),
                  EvaluateSmoothValue(Query + float3(41.6, 11.9, 23.5)));
}

// 📝 Divergence-free curl of a two-octave smooth vector potential for buoyant plume rolling eddies.
float3 ResolveCurlSwirl(float3 Position, float ElapsedTime)
{
    let Drift = float3(ElapsedTime * 0.28, ElapsedTime * -0.19, ElapsedTime * -0.52);
    let Base  = Position * 0.62 + Drift;
    let Step  = 0.22;
    let Dx    = EvaluatePotential(Base + float3(Step, 0.0, 0.0)) - EvaluatePotential(Base - float3(Step, 0.0, 0.0));
    let Dy    = EvaluatePotential(Base + float3(0.0, Step, 0.0)) - EvaluatePotential(Base - float3(0.0, Step, 0.0));
    let Dz    = EvaluatePotential(Base + float3(0.0, 0.0, Step)) - EvaluatePotential(Base - float3(0.0, 0.0, Step));
    let CurlA = float3(Dy.z - Dz.y, Dz.x - Dx.z, Dx.y - Dy.x) / (2.0 * Step);

    let Fine  = Position * 1.45 + Drift * 1.9;
    let Fx    = EvaluatePotential(Fine + float3(Step, 0.0, 0.0)) - EvaluatePotential(Fine - float3(Step, 0.0, 0.0));
    let Fy    = EvaluatePotential(Fine + float3(0.0, Step, 0.0)) - EvaluatePotential(Fine - float3(0.0, Step, 0.0));
    let Fz    = EvaluatePotential(Fine + float3(0.0, 0.0, Step)) - EvaluatePotential(Fine - float3(0.0, 0.0, Step));
    let CurlB = float3(Fy.z - Fz.y, Fz.x - Fx.z, Fx.y - Fy.x) / (2.0 * Step);
    return CurlA * 0.68 + CurlB * 0.32;
}

// 📝 Evaluates the outward unit gradient of the combined scene + ground signed distance field.
float3 EvaluateSceneGradient(float3 Position, float Step)
{
    let Eval = (float3 P) => min(SampleSceneDistance(P).x, P.z - Plume.FlameTerms.w);
    let Grad = float3(
        Eval(Position + float3(Step, 0.0, 0.0)) - Eval(Position - float3(Step, 0.0, 0.0)),
        Eval(Position + float3(0.0, Step, 0.0)) - Eval(Position - float3(0.0, Step, 0.0)),
        Eval(Position + float3(0.0, 0.0, Step)) - Eval(Position - float3(0.0, 0.0, Step)));
    let Len  = length(Grad);
    return SelectBranch(float3(0.0, 0.0, 1.0), Grad / Len, Len > 1.0e-5);
}

// 📝 Queries the live 4-cascade scene signed distance field and reconstructs rigid-body velocity from the nearest occupant.
float4 ResolveObstacleState(float3 Position, float DeltaTime)
{
    let GroundDistance = Position.z - Plume.FlameTerms.w;
    let Sample         = SampleSceneDistance(Position);
    let SignedDistance = min(Sample.x, GroundDistance);
    var BodyVelocity   = float3(0.0);
    let Level          = uint(Sample.y + 0.5);
    if (Level < Distance.ClipCount && Sample.x < GroundDistance)
    {
        uint OccupantCount  = 0u;
        uint OccupantStride = 0u;
        Occupants.GetDimensions(OccupantCount, OccupantStride);
        let Identity = ResolveClipIdentity(Level, Position);
        if (Identity != GroundIdentity && Identity != SkyIdentity && Identity < OccupantCount)
        {
            let Occupant      = Occupants[Identity];
            let LocalPoint    = mul(float4(Position, 1.0), Occupant.WorldToLocal);
            let PreviousWorld = (mul(LocalPoint, Occupant.PreviousLocalToWorld)).xyz;
            let RawVelocity   = (Position - PreviousWorld) / max(DeltaTime, 1.0e-3);
            if (dot(RawVelocity, RawVelocity) < 3600.0)
            {
                BodyVelocity = RawVelocity;
            }
        }
    }
    return float4(BodyVelocity, SignedDistance);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void AdvectVelocity(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let OutTexel = ResolveVelocityTexel(Plume, Cell);
    if (Plume.CoarseOrigin.w != 0)
    {
        VelocityWrite[OutTexel] = float4(0.0);
        return;
    }

    let DeltaTime   = Plume.WindowMinimum.w;
    let ElapsedTime = Plume.WindowMaximum.w;
    let Position    = ProjectVelocityWorld(Plume, Cell);
    let Obstacle    = ResolveObstacleState(Position, DeltaTime);
    let CoarseSize  = Plume.CellMetrics.z;
    if (Obstacle.w <= 0.0)
    {
        VelocityWrite[OutTexel] = float4(Obstacle.xyz, 1.0);
        return;
    }
    if (!ClassifyPreviousVelocity(Plume, Position))
    {
        VelocityWrite[OutTexel] = float4(Plume.WindVector.xyz, 0.0);
        return;
    }

    let Current   = VelocityRead.SampleLevel(LinearRepeat, ProjectVelocityCoordinates(Plume, Position), 0.0).xyz;
    let Midpoint  = Position - 0.5 * DeltaTime * Current;
    let MidFlow   = SelectBranch(Plume.WindVector.xyz,
                                 VelocityRead.SampleLevel(LinearRepeat, ProjectVelocityCoordinates(Plume, Midpoint), 0.0).xyz,
                                 ClassifyPreviousVelocity(Plume, Midpoint));
    let Backtrace = Position - DeltaTime * MidFlow;
    var Advected  = SelectBranch(Plume.WindVector.xyz,
                                 VelocityRead.SampleLevel(LinearRepeat, ProjectVelocityCoordinates(Plume, Backtrace), 0.0).xyz,
                                 ClassifyPreviousVelocity(Plume, Backtrace));

    let Drag     = exp(-Plume.WindVector.w * DeltaTime);
    Advected     = Plume.WindVector.xyz + (Advected - Plume.WindVector.xyz) * Drag;

    let Thermo   = SelectBranch(float4(0.0),
                                ThermoRead.SampleLevel(LinearRepeat, ProjectThermoCoordinates(Plume, Position), 0.0),
                                ClassifyPreviousThermo(Plume, Position));
    let Density  = Thermo.x;
    let Heat     = Thermo.y;
    let Flame    = Thermo.w;

    let Lift     = Plume.BuoyancyTerms.x * (Heat + Flame * 260.0) - Plume.BuoyancyTerms.y * Density;
    Advected.z  += Lift * DeltaTime;

    let Activity = saturate(Density * 1.4 + Heat * 0.008 + Flame * 1.8);
    if (Activity > 1.0e-3 && Plume.Dissipation.z > 0.0)
    {
        Advected += ResolveCurlSwirl(Position, ElapsedTime) * (Plume.Dissipation.z * Activity * DeltaTime);
    }

    let Count = Plume.OriginCell.w;
    for (var Index = 0; Index < Count; Index++)
    {
        let Source = Sources[uint(Index)];
        let Weight = ResolveSourceWeight(Source, Position, CoarseSize);
        if (Weight > 1.0e-4)
        {
            let Blend = 1.0 - exp(-Source.ThermalPayload.w * Weight * DeltaTime);
            Advected  = lerp(Advected, Source.VelocityRate.xyz, Blend);
        }
    }

    if (Obstacle.w < CoarseSize * 0.85)
    {
        let Normal = EvaluateSceneGradient(Position, CoarseSize * 0.35);
        let Inward = dot(Advected - Obstacle.xyz, Normal);
        if (Inward < 0.0)
        {
            Advected -= Normal * Inward;
        }
        let Skin = saturate(1.0 - Obstacle.w / (CoarseSize * 0.85));
        Advected = lerp(Advected, Obstacle.xyz, Skin * 0.35);
    }

    let Speed = length(Advected);
    if (Speed > 42.0)
    {
        Advected *= 42.0 / Speed;
    }
    VelocityWrite[OutTexel] = float4(Advected, 0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void AdvectThermo(uint3 Cell : SV_DispatchThreadID, uint LocalIndex : SV_GroupIndex)
{
    if (LocalIndex == 0u)
    {
        GroupLow      = uint3(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
        GroupHigh     = uint3(0u, 0u, 0u);
        GroupOccupied = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    let Inside   = all(Cell < Plume.Extent.xyz);
    let OutTexel = ResolveThermoTexel(Plume, min(Cell, Plume.Extent.xyz - uint3(1u)));
    var Density  = 0.0;
    var Heat     = 0.0;
    var Soot     = 0.0;
    var Flame    = 0.0;

    if (Inside && Plume.CoarseOrigin.w == 0)
    {
        let DeltaTime = Plume.WindowMinimum.w;
        let Position  = ProjectThermoWorld(Plume, Cell);
        let VoxelSize = Plume.CellMetrics.x;
        let Ground    = Position.z - Plume.FlameTerms.w;
        let SignedDst = min(SampleSceneDistance(Position).x, Ground);

        if (SignedDst > -VoxelSize * 0.25)
        {
            var Flow      = SelectBranch(Plume.WindVector.xyz,
                                         VelocityRead.SampleLevel(LinearRepeat, ProjectVelocityCoordinates(Plume, Position), 0.0).xyz,
                                         ClassifyPreviousVelocity(Plume, Position));
            let LocalNow  = SelectBranch(float4(0.0),
                                         ThermoRead.SampleLevel(LinearRepeat, ProjectThermoCoordinates(Plume, Position), 0.0),
                                         ClassifyPreviousThermo(Plume, Position));
            let SubgridUp = 0.0085 * LocalNow.y + 2.4 * LocalNow.w;
            Flow.z       += SubgridUp;

            let Midpoint  = Position - 0.5 * DeltaTime * Flow;
            var MidFlow   = SelectBranch(Plume.WindVector.xyz,
                                         VelocityRead.SampleLevel(LinearRepeat, ProjectVelocityCoordinates(Plume, Midpoint), 0.0).xyz,
                                         ClassifyPreviousVelocity(Plume, Midpoint));
            MidFlow.z    += SubgridUp;
            var Backtrace = Position - DeltaTime * MidFlow;
            Backtrace.z   = max(Backtrace.z, Plume.FlameTerms.w + VoxelSize * 0.35);

            let BackSigned = min(SampleSceneDistance(Backtrace).x, Backtrace.z - Plume.FlameTerms.w);
            if (BackSigned < 0.0)
            {
                let Normal = EvaluateSceneGradient(Backtrace, VoxelSize * 0.5);
                Backtrace += Normal * (-BackSigned + VoxelSize * 0.15);
                Backtrace.z = max(Backtrace.z, Plume.FlameTerms.w + VoxelSize * 0.35);
            }

            if (ClassifyPreviousThermo(Plume, Backtrace))
            {
                let Sampled = ThermoRead.SampleLevel(LinearRepeat, ProjectThermoCoordinates(Plume, Backtrace), 0.0);
                Density     = max(Sampled.x, 0.0);
                Heat        = max(Sampled.y, 0.0);
                Soot        = clamp(Sampled.z, 0.0, 1.0);
                Flame       = max(Sampled.w, 0.0);
            }

            Density *= exp(-Plume.Dissipation.x * DeltaTime);
            let RadiativeLoss = 1.0 + 0.0022 * Heat;
            Heat    *= exp(-Plume.BuoyancyTerms.w * RadiativeLoss * DeltaTime);

            if (Flame > 1.0e-4)
            {
                let Burned   = Flame * (1.0 - exp(-Plume.Dissipation.y * DeltaTime));
                Flame       -= Burned;
                Heat        += Burned * 520.0;
                let SootGain = Burned * 0.62;
                let NewMass  = Density + SootGain;
                if (NewMass > 1.0e-5)
                {
                    Soot = clamp((Density * Soot + SootGain * 0.94) / NewMass, 0.0, 1.0);
                }
                Density = NewMass;
            }

            let Count = Plume.OriginCell.w;
            for (var Index = 0; Index < Count; Index++)
            {
                let Source = Sources[uint(Index)];
                let Weight = ResolveSourceWeight(Source, Position, VoxelSize);
                if (Weight > 1.0e-4)
                {
                    let AddedSmoke  = max(Source.VelocityRate.w, 0.0) * Weight * DeltaTime;
                    let AddedHeat   = max(Source.ThermalPayload.x, 0.0) * Weight * DeltaTime;
                    let AddedFlame  = max(Source.ThermalPayload.y, 0.0) * Weight * DeltaTime;
                    let SourceSoot  = clamp(Source.ThermalPayload.z, 0.0, 1.0);
                    let Combined    = Density + AddedSmoke;
                    if (Combined > 1.0e-5)
                    {
                        Soot = clamp((Density * Soot + AddedSmoke * SourceSoot) / Combined, 0.0, 1.0);
                    }
                    Density = min(Combined, 8.0);
                    Heat    = min(Heat + AddedHeat, 1800.0);
                    Flame   = min(Flame + AddedFlame, 3.0);
                }
            }

            let Margin = VoxelSize * 3.0;
            let Wall   = min(min(Position.x - Plume.WindowMinimum.x, Plume.WindowMaximum.x - Position.x),
                             min(Position.y - Plume.WindowMinimum.y, Plume.WindowMaximum.y - Position.y));
            let Top    = Plume.WindowMaximum.z - Position.z;
            let Sponge = saturate(min(Wall, Top) / Margin);
            Density   *= Sponge;
            Flame     *= Sponge;

            if (SignedDst < 0.0)
            {
                let Crush = saturate(1.0 + SignedDst / (VoxelSize * 0.5));
                Density  *= Crush;
                Flame    *= Crush;
            }
        }

        ThermoWrite[OutTexel] = float4(Density, Heat, Soot, Flame);
        if (Density > 0.003 || Flame > 0.002)
        {
            var Discard = 0u;
            InterlockedMin(GroupLow.x, Cell.x, Discard);
            InterlockedMin(GroupLow.y, Cell.y, Discard);
            InterlockedMin(GroupLow.z, Cell.z, Discard);
            InterlockedMax(GroupHigh.x, Cell.x + 1u, Discard);
            InterlockedMax(GroupHigh.y, Cell.y + 1u, Discard);
            InterlockedMax(GroupHigh.z, Cell.z + 1u, Discard);
            InterlockedOr(GroupOccupied, 1u, Discard);
        }
    }
    else if (Inside)
    {
        ThermoWrite[OutTexel] = float4(0.0);
    }

    GroupMemoryBarrierWithGroupSync();
    if (LocalIndex == 0u && GroupOccupied != 0u)
    {
        var Discard = 0u;
        InterlockedMin(Coverage[0], GroupLow.x, Discard);
        InterlockedMin(Coverage[1], GroupLow.y, Discard);
        InterlockedMin(Coverage[2], GroupLow.z, Discard);
        InterlockedMax(Coverage[3], GroupHigh.x, Discard);
        InterlockedMax(Coverage[4], GroupHigh.y, Discard);
        InterlockedMax(Coverage[5], GroupHigh.z, Discard);
        InterlockedAdd(Coverage[6], 1u, Discard);
    }
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ResolveSunTransmittance(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let OutTexel = ResolveVelocityTexel(Plume, Cell);
    if (Coverage[6] == 0u)
    {
        TransmittanceWrite[OutTexel] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }

    let Position     = ProjectVelocityWorld(Plume, Cell);
    let SunDirection = normalize(Refresh.SunDirection);
    let StepLength   = Plume.CellMetrics.z * Plume.ScatteringTerms.z;
    let StepCount    = Plume.TraceDimensions.z;
    let Extinction   = Plume.Dissipation.w;

    var OpticalDepth = 0.0;
    var Cursor       = Position + SunDirection * (StepLength * 0.5);
    for (var Step = 0u; Step < StepCount; Step++)
    {
        if (!ClassifyWindowInterior(Plume, Cursor, 0.0))
        {
            break;
        }
        let Sampled   = ThermoRead.SampleLevel(LinearRepeat, ProjectThermoCoordinates(Plume, Cursor), 0.0);
        OpticalDepth += Sampled.x * Extinction * StepLength;
        if (OpticalDepth > 8.0)
        {
            break;
        }
        Cursor += SunDirection * StepLength;
    }
    TransmittanceWrite[OutTexel] = float4(exp(-OpticalDepth), 0.0, 0.0, 0.0);
}
