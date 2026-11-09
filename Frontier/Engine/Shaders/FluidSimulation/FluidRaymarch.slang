//============================================================================================================================================
//                                                         FLUIDRAYMARCH.SLANG
//============================================================================================================================================
// 🧩 Physically-based volumetric gas & blackbody raymarcher, 3D Voxel DDA, cross-section slice, shockwave refraction, and flipbook export.

#pragma once
#include "FluidLayouts.slang"

[[vk::binding(0, 0)]] ConstantBuffer<FluidRaymarchConstants> Raymarch;
[[vk::binding(1, 0)]] Texture3D<float4>                      DensityVolume;
[[vk::binding(2, 0)]] Texture3D<float4>                      VelocityVolume;
[[vk::binding(3, 0)]] Texture3D<float4>                      PressureVolume;
[[vk::binding(4, 0)]] Texture3D<float4>                      VorticityVolume;
[[vk::binding(5, 0)]] SamplerState                           VolumeLinearSampler;
[[vk::binding(6, 0)]] SamplerState                           VolumeNearestSampler;

struct RaymarchVarying
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 Coordinates : LOCATION0;
};

[shader("vertex")]
RaymarchVarying RaymarchVertex(uint VertexIndex : SV_VertexID)
{
    var Out : RaymarchVarying;
    let X           = SelectBranch(-1.0, 3.0, VertexIndex == 1u);
    let Y           = SelectBranch(-1.0, 3.0, VertexIndex == 2u);
    Out.Position    = float4(X, Y, 0.0, 1.0);
    Out.Coordinates = float2(0.5 * X + 0.5, 0.5 * Y + 0.5);
    return Out;
}

float3 ProjectWorldToUvw(float3 World, float3 HalfBox, float3 Scale)
{
    return (World + HalfBox) / Scale;
}

bool ClassifyUvwInside(float3 Uvw)
{
    return all(Uvw >= float3(0.0)) && all(Uvw <= float3(1.0));
}

float2 IntersectFluidBox(float3 Origin, float3 InverseDirection, float3 HalfBox)
{
    let NearPlane = (-HalfBox - Origin) * InverseDirection;
    let FarPlane  = (HalfBox - Origin) * InverseDirection;
    let Smaller   = min(NearPlane, FarPlane);
    let Larger    = max(NearPlane, FarPlane);
    return float2(max(max(Smaller.x, Smaller.y), Smaller.z), min(min(Larger.x, Larger.y), Larger.z));
}

float EvaluateFluidPhase(float Cosine, float ForwardG)
{
    let ForwardSq = ForwardG * ForwardG;
    let Forward   = (1.0 - ForwardSq) / (12.566370614359172 * pow(max(1.0 + ForwardSq - 2.0 * ForwardG * Cosine, 1.0e-3), 1.5));
    let BackG     = -0.25 * ForwardG;
    let BackSq    = BackG * BackG;
    let Backward  = (1.0 - BackSq) / (12.566370614359172 * pow(max(1.0 + BackSq - 2.0 * BackG * Cosine, 1.0e-3), 1.5));
    return lerp(Backward, Forward, 0.78);
}

float3 ToneMapAces(float3 Radiance)
{
    return clamp((Radiance * (2.51 * Radiance + 0.03)) / (Radiance * (2.43 * Radiance + 0.59) + 0.14), float3(0.0), float3(1.0));
}

float3 EvaluateColliderNormal(float3 Uvw, int Category, float3 Centre, float Radius, float3 Scale)
{
    let Step = 0.004;
    return normalize(float3(
        EvaluateColliderSignedDistance(Uvw + float3(Step, 0.0, 0.0), Category, Centre, Radius, Scale) - EvaluateColliderSignedDistance(Uvw - float3(Step, 0.0, 0.0), Category, Centre, Radius, Scale),
        EvaluateColliderSignedDistance(Uvw + float3(0.0, Step, 0.0), Category, Centre, Radius, Scale) - EvaluateColliderSignedDistance(Uvw - float3(0.0, Step, 0.0), Category, Centre, Radius, Scale),
        EvaluateColliderSignedDistance(Uvw + float3(0.0, 0.0, Step), Category, Centre, Radius, Scale) - EvaluateColliderSignedDistance(Uvw - float3(0.0, 0.0, Step), Category, Centre, Radius, Scale)));
}

float TraceColliderHit(float3 Origin, float3 Direction, float StartT, float EndT, float3 HalfBox, float3 Scale, int Category, float3 Centre, float Radius)
{
    if (Category <= 0 || Radius <= 0.001)
    {
        return 1.0e9;
    }
    var Travel = max(0.0, StartT);
    for (var Step = 0; Step < 64; Step++)
    {
        if (Travel > EndT)
        {
            break;
        }
        let Uvw      = ProjectWorldToUvw(Origin + Direction * Travel, HalfBox, Scale);
        let Distance = EvaluateColliderSignedDistance(Uvw, Category, Centre, Radius, Scale);
        if (Distance < 0.0025)
        {
            return Travel;
        }
        Travel += max(Distance * 0.72, 0.005);
    }
    return 1.0e9;
}

float EvaluateLightShadow(float3 WorldPos, float3 SunDir, float3 HalfBox, float3 Scale, uint ShadowSteps, float ExtinctionScale)
{
    if (ShadowSteps == 0u)
    {
        return 1.0;
    }
    let SafeDir = SelectBranch(SunDir, float3(1.0e-5), abs(SunDir) < float3(1.0e-5));
    let Span    = IntersectFluidBox(WorldPos, 1.0 / SafeDir, HalfBox);
    let MaxDist = max(0.0, Span.y);
    if (MaxDist <= 0.001)
    {
        return 1.0;
    }
    let StepLen = min(MaxDist / float(ShadowSteps), 0.14 * max(Scale.x, Scale.y));
    var Tau     = 0.0;
    var Travel  = StepLen * 0.5;
    for (var Step = 0u; Step < ShadowSteps; Step++)
    {
        if (Travel > MaxDist)
        {
            break;
        }
        let Uvw = ProjectWorldToUvw(WorldPos + SunDir * Travel, HalfBox, Scale);
        if (!ClassifyUvwInside(Uvw))
        {
            break;
        }
        let Density = DensityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0);
        Tau        += (Density.x + Density.w * 0.8) * StepLen;
        if (Tau * ExtinctionScale > 6.5)
        {
            return 0.0015;
        }
        Travel += StepLen;
    }
    return exp(-Tau * ExtinctionScale);
}

float3 EvaluateSkyAndFloor(float3 Origin, float3 Direction, float3 HalfBox, float3 Scale, float3 SunDir, float SunIntensity,
                           float AmbientIntensity, uint ShadowSteps, float ExtinctionScale, int Palette, float FireIntensity)
{
    let Horizon = pow(clamp(1.0 - abs(Direction.y), 0.0, 1.0), 3.0);
    var Sky     = lerp(float3(0.010, 0.010, 0.010), float3(0.052, 0.052, 0.052), Horizon) * (0.55 + 0.45 * AmbientIntensity);
    let SunDisc = pow(max(0.0, dot(Direction, SunDir)), 96.0);
    Sky        += float3(1.0, 0.88, 0.70) * SunDisc * 0.32 * SunIntensity;

    let FloorY  = -HalfBox.y - 0.002;
    if (Direction.y < -0.001 && Origin.y > FloorY)
    {
        let HitT = (FloorY - Origin.y) / Direction.y;
        if (HitT > 0.0 && HitT < 28.0)
        {
            let Point    = Origin + Direction * HitT;
            let Distance = length(Point.xz);
            let Fade     = exp(-Distance * 0.16);
            var Ground   = float3(0.020, 0.020, 0.020) * (0.5 + 0.5 * AmbientIntensity);

            if (Raymarch.OverlayControls.y != 0u)
            {
                let GridSize   = Raymarch.DomainBounds.w;
                let VoxelWidth = Scale.x / max(GridSize, 8.0);
                let MinorCoord = abs(frac(Point.xz / max(VoxelWidth * 4.0, 0.08) - 0.5) - 0.5);
                let MajorCoord = abs(frac(Point.xz - 0.5) - 0.5);
                let MinorLine  = 1.0 - smoothstep(0.0, 0.035, min(MinorCoord.x, MinorCoord.y));
                let MajorLine  = 1.0 - smoothstep(0.0, 0.018, min(MajorCoord.x, MajorCoord.y));
                Ground        += float3(0.030, 0.030, 0.030) * MinorLine * Fade;
                Ground        += float3(0.060, 0.060, 0.060) * MajorLine * Fade;
                let AxisX      = (1.0 - smoothstep(0.0, 0.018, abs(Point.z))) * step(abs(Point.x), HalfBox.x * 1.6);
                let AxisZ      = (1.0 - smoothstep(0.0, 0.018, abs(Point.x))) * step(abs(Point.z), HalfBox.z * 1.6);
                Ground        += float3(0.095, 0.095, 0.095) * AxisX * Fade;
                Ground        += float3(0.095, 0.095, 0.095) * AxisZ * Fade;

                let BoxDelta   = abs(Point.xz) - HalfBox.xz;
                let BoxOutline = 1.0 - smoothstep(0.0, 0.018, abs(max(BoxDelta.x, BoxDelta.y)));
                if (max(BoxDelta.x, BoxDelta.y) < 0.03)
                {
                    Ground += float3(0.28, 0.18, 0.07) * BoxOutline * Fade;
                }
            }

            let Shadow = EvaluateLightShadow(Point + float3(0.0, 0.02, 0.0), SunDir, HalfBox, Scale, min(ShadowSteps, 6u), ExtinctionScale);
            Ground    *= (0.32 + 0.68 * Shadow);

            let BaseUvw = ProjectWorldToUvw(float3(Point.x, -HalfBox.y + 0.12 * Scale.y, Point.z), HalfBox, Scale);
            if (all(BaseUvw.xz >= float2(-0.35)) && all(BaseUvw.xz <= float2(1.35)))
            {
                let ClampedUvw = clamp(BaseUvw, float3(0.02), float3(0.98));
                let Irrad      = PressureVolume.SampleLevel(VolumeLinearSampler, ClampedUvw, 0.0).y;
                let EdgeFade   = exp(-length(max(abs(BaseUvw.xz - 0.5) - 0.45, float2(0.0))) * 4.5);
                let BounceCol  = EvaluateBlackbodyPalette(min(Irrad * 1.8, 3.2), Palette);
                Ground        += BounceCol * EdgeFade * 0.28 * FireIntensity;
            }

            let ShockStrength = Raymarch.GlowShockwave.z;
            let ShockRadius   = Raymarch.GlowShockwave.w;
            let ShockThick    = Raymarch.ShockwaveOrigin.w;
            if (ShockStrength > 0.01 && ShockRadius > 0.01)
            {
                let ShockWorld = (Raymarch.ShockwaveOrigin.xyz - 0.5) * Scale;
                let RingDist   = abs(length(Point.xz - ShockWorld.xz) - ShockRadius * max(Scale.x, Scale.z));
                let RingGlow   = exp(-pow(RingDist / max(ShockThick * Scale.x, 0.04), 2.0)) * ShockStrength;
                Ground        += float3(0.55, 0.42, 0.28) * RingGlow * Fade * 0.65;
            }

            return lerp(Sky, Ground, Fade);
        }
    }
    return Sky;
}

float4 RaymarchVoxelCubes(float3 Origin, float3 Direction, float StartT, float EndT, float3 HalfBox, float3 Scale,
                          float3 SunDir, float SunIntensity, float AmbientIntensity, int Palette, float FireIntensity,
                          float TempScale, float ExtinctionBase, float SmokeAlbedo, uint ShadowSteps)
{
    let GridSize  = Raymarch.DomainBounds.w;
    let VoxelSize = Scale / GridSize;
    let EntryPt   = Origin + Direction * (StartT + 1.0e-4);
    let EntryCell = clamp((EntryPt + HalfBox) / VoxelSize, float3(0.0), float3(GridSize - 0.001));
    var Cell      = int3(floor(EntryCell));
    let StepSign  = int3(SelectBranch(float3(-1.0), float3(1.0), Direction >= float3(0.0)));
    let SafeDir   = SelectBranch(Direction, float3(1.0e-6), abs(Direction) < float3(1.0e-6));
    let DeltaT    = abs(VoxelSize / SafeDir);
    let NextPlane = -HalfBox + (float3(Cell) + max(float3(StepSign), float3(0.0))) * VoxelSize;
    var MaxT      = (NextPlane - Origin) / SafeDir;
    var Normal    = float3(0.0, 1.0, 0.0);

    var Radiance  = float3(0.0);
    var Trans     = 1.0;
    var Travel    = StartT;
    let GridLimit = int(GridSize);

    for (var Iter = 0; Iter < 220; Iter++)
    {
        if (any(Cell < int3(0)) || any(Cell >= int3(GridLimit)) || Travel >= EndT || Trans < 0.01)
        {
            break;
        }
        let NextT  = min(min(MaxT.x, MaxT.y), MaxT.z);
        let SegLen = max(0.0, min(NextT, EndT) - Travel);

        let Uvw    = (float3(Cell) + 0.5) / GridSize;
        let Den    = DensityVolume.SampleLevel(VolumeNearestSampler, Uvw, 0.0);
        let Smoke  = Den.x;
        let Temp   = Den.y * TempScale;
        let Soot   = Den.w;

        if (Smoke > 0.06 || Temp > 0.14)
        {
            let FaceShade = 0.58 + 0.42 * max(0.0, dot(Normal, SunDir));
            let WorldCell = -HalfBox + (float3(Cell) + 0.5) * VoxelSize;
            let Shadow    = EvaluateLightShadow(WorldCell, SunDir, HalfBox, Scale, min(ShadowSteps, 6u), ExtinctionBase * 0.42);

            let SootRatio = clamp(Soot / max(Smoke + 0.05, 0.05), 0.0, 1.0);
            let Albedo    = float3(SmokeAlbedo) * lerp(float3(1.0, 0.98, 0.95), float3(0.25, 0.23, 0.22), SootRatio);
            var CellCol   = Albedo * FaceShade * (0.28 * AmbientIntensity + 0.95 * SunIntensity * Shadow);

            if (Raymarch.OverlayControls.z != 0u)
            {
                let HitLocal = frac((Origin + Direction * (Travel + 1.0e-4) + HalfBox) / VoxelSize);
                let Edge     = min(min(min(HitLocal.x, 1.0 - HitLocal.x), min(HitLocal.y, 1.0 - HitLocal.y)), min(HitLocal.z, 1.0 - HitLocal.z));
                CellCol     *= (0.78 + 0.22 * smoothstep(0.0, 0.08, Edge));
            }

            let Flame     = EvaluateBlackbodyPalette(Temp, Palette) * FireIntensity * 1.15;
            let SigmaT    = (Smoke + max(0.0, Temp - 0.15) * 0.35) * ExtinctionBase * 1.25;
            let StepTrans = exp(-SigmaT * SegLen);
            Radiance     += Trans * (CellCol * (1.0 - StepTrans) + Flame * (1.0 - exp(-SegLen * 3.2)) * 0.65);
            Trans        *= StepTrans;
        }

        Travel = NextT;
        if (MaxT.x < MaxT.y)
        {
            if (MaxT.x < MaxT.z) { Cell.x += StepSign.x; MaxT.x += DeltaT.x; Normal = float3(-float(StepSign.x), 0.0, 0.0); }
            else                 { Cell.z += StepSign.z; MaxT.z += DeltaT.z; Normal = float3(0.0, 0.0, -float(StepSign.z)); }
        }
        else
        {
            if (MaxT.y < MaxT.z) { Cell.y += StepSign.y; MaxT.y += DeltaT.y; Normal = float3(0.0, -float(StepSign.y), 0.0); }
            else                 { Cell.z += StepSign.z; MaxT.z += DeltaT.z; Normal = float3(0.0, 0.0, -float(StepSign.z)); }
        }
    }
    return float4(Radiance, Trans);
}

[shader("fragment")]
float4 RaymarchFragment(RaymarchVarying In) : SV_Target0
{
    let Ndc       = In.Coordinates * 2.0 - 1.0;
    let NearWorld = mul(float4(Ndc, -1.0, 1.0), Raymarch.InverseViewProjection);
    let FarWorld  = mul(float4(Ndc, 1.0, 1.0), Raymarch.InverseViewProjection);
    let Origin    = Raymarch.CameraPosition.xyz;
    var Direction = normalize(FarWorld.xyz / FarWorld.w - NearWorld.xyz / NearWorld.w);

    let Scale            = Raymarch.DomainBounds.xyz;
    let HalfBox          = Scale * 0.5;
    let GridSize         = Raymarch.DomainBounds.w;
    let SunDir           = normalize(Raymarch.SunLight.xyz);
    let SunIntensity     = Raymarch.SunLight.w;
    let AmbientIntensity = Raymarch.VolumeShading.x;
    let FireIntensity    = Raymarch.VolumeShading.y;
    let DensityExtinct   = Raymarch.VolumeShading.z;
    let SmokeAlbedo      = Raymarch.VolumeShading.w;
    let PhaseAniso       = Raymarch.ScatteringOptics.x;
    let ShadowDensity    = Raymarch.ScatteringOptics.y;
    let InternalScatter  = Raymarch.ScatteringOptics.z;
    let TempScale        = Raymarch.ScatteringOptics.w;
    let BloomIntensity   = Raymarch.GlowShockwave.x;
    let GodRaysIntensity = Raymarch.GlowShockwave.y;
    let ShockStrength    = Raymarch.GlowShockwave.z;
    let ShockRadius      = Raymarch.GlowShockwave.w;
    let ShockThick       = Raymarch.ShockwaveOrigin.w;
    let ObstacleCat      = int(Raymarch.ObstacleGeometry.x + 0.5);
    let ObstacleRadius   = Raymarch.ObstacleGeometry.y;
    let VoxelQuant       = Raymarch.ObstacleGeometry.z;
    let SlicePos         = Raymarch.ObstacleGeometry.w;
    let RaymarchSteps    = Raymarch.RenderControls.x;
    let ShadowSteps      = Raymarch.RenderControls.y;
    let RenderChannel    = int(Raymarch.RenderControls.z);
    let Palette          = int(Raymarch.RenderControls.w);
    let SliceAxis        = int(Raymarch.OverlayControls.x);
    let ExportFlipbook   = Raymarch.ExportControls.x != 0u;

    let OpticalScale     = 1.9 / max(max(Scale.x, Scale.z), 0.5);
    let ExtinctionNorm   = DensityExtinct * OpticalScale;
    let ShadowExtinction = ExtinctionNorm * ShadowDensity * 0.42;

    if (ShockStrength > 0.005 && ShockRadius > 0.01)
    {
        let ShockWorld   = (Raymarch.ShockwaveOrigin.xyz - 0.5) * Scale;
        let ToCentre     = ShockWorld - Origin;
        let Along        = dot(ToCentre, Direction);
        if (Along > 0.0)
        {
            let Closest      = Origin + Direction * Along - ShockWorld;
            let RadialDist   = length(Closest);
            let WorldRadius  = ShockRadius * max(Scale.x, Scale.z) * 0.85;
            let ShellBand    = max(ShockThick * Scale.x, 0.035);
            let ShellProfile = (RadialDist - WorldRadius) / ShellBand;
            let Deflection   = -ShellProfile * exp(-ShellProfile * ShellProfile) * ShockStrength * 0.065;
            let RadialDir    = normalize(Closest + float3(1.0e-5));
            Direction        = normalize(Direction + RadialDir * Deflection);
        }
    }

    let SafeDir    = SelectBranch(Direction, float3(1.0e-6), abs(Direction) < float3(1.0e-6));
    let Span       = IntersectFluidBox(Origin, 1.0 / SafeDir, HalfBox);
    var Background = SelectBranch(EvaluateSkyAndFloor(Origin, Direction, HalfBox, Scale, SunDir, SunIntensity, AmbientIntensity, ShadowSteps, ShadowExtinction, Palette, FireIntensity),
                                  float3(0.0),
                                  ExportFlipbook);

    if (Span.x > Span.y || Span.y < 0.0)
    {
        if (ExportFlipbook)
        {
            return float4(0.0);
        }
        let Mapped = pow(ToneMapAces(Background * Raymarch.CameraPosition.w), float3(1.0 / 2.2));
        return float4(Mapped, 1.0);
    }

    let StartT = max(0.0, Span.x);
    var EndT   = Span.y;

    let ObstacleHitT = SelectBranch(TraceColliderHit(Origin, Direction, StartT, EndT, HalfBox, Scale, ObstacleCat, Raymarch.ObstaclePosition.xyz, ObstacleRadius),
                                    1.0e9,
                                    ExportFlipbook);
    if (ObstacleHitT < EndT)
    {
        let HitWorld  = Origin + Direction * ObstacleHitT;
        let HitUvw    = ProjectWorldToUvw(HitWorld, HalfBox, Scale);
        let Normal    = EvaluateColliderNormal(HitUvw, ObstacleCat, Raymarch.ObstaclePosition.xyz, ObstacleRadius, Scale);
        let Diffuse   = max(0.0, dot(Normal, SunDir));
        let Shadow    = EvaluateLightShadow(HitWorld + Normal * 0.03, SunDir, HalfBox, Scale, min(ShadowSteps, 6u), ShadowExtinction);
        let HalfVec   = normalize(SunDir - Direction);
        let Specular  = pow(max(0.0, dot(Normal, HalfVec)), 28.0) * Shadow;
        let Irrad     = PressureVolume.SampleLevel(VolumeLinearSampler, clamp(HitUvw + Normal * 0.06, float3(0.02), float3(0.98)), 0.0).y;
        let FireGlow  = EvaluateBlackbodyPalette(Irrad * 2.1, Palette) * FireIntensity * 0.45;
        let BaseTint  = SelectBranch(float3(0.14, 0.16, 0.20), float3(0.06, 0.062, 0.068), ObstacleCat == 5);
        Background    = BaseTint * (0.28 * AmbientIntensity + 0.85 * Diffuse * SunIntensity * Shadow) + float3(0.22) * Specular + FireGlow;
        EndT          = ObstacleHitT;
    }

    // Channel 8: 2D Cross-Section Diagnostic Slice
    if (RenderChannel == 8)
    {
        let PlaneWorld = -HalfBox[SliceAxis] + SlicePos * Scale[SliceAxis];
        let Denom      = Direction[SliceAxis];
        var FinalSlice = Background;
        if (abs(Denom) > 1.0e-5)
        {
            let PlaneT = (PlaneWorld - Origin[SliceAxis]) / Denom;
            if (PlaneT >= StartT && PlaneT <= EndT)
            {
                let SliceUvw = ProjectWorldToUvw(Origin + Direction * PlaneT, HalfBox, Scale);
                if (ClassifyUvwInside(SliceUvw))
                {
                    let Den      = DensityVolume.SampleLevel(VolumeLinearSampler, SliceUvw, 0.0);
                    let Vel      = VelocityVolume.SampleLevel(VolumeLinearSampler, SliceUvw, 0.0);
                    let CellEdge = abs(frac(SliceUvw * GridSize) - 0.5);
                    let GridMask = 1.0 - smoothstep(0.42, 0.5, max(CellEdge.x, max(CellEdge.y, CellEdge.z)));
                    var SliceCol = float3(0.03, 0.04, 0.06);
                    SliceCol    += float3(0.55) * clamp(Den.x * 0.5, 0.0, 1.0);
                    SliceCol    += EvaluateBlackbodyPalette(Den.y * TempScale, Palette) * 0.65;
                    SliceCol    += float3(0.1, 0.55, 0.95) * clamp(length(Vel.xyz) * 0.06, 0.0, 0.8);
                    SliceCol     = lerp(SliceCol * 1.25, SliceCol, GridMask);
                    FinalSlice   = lerp(Background, SliceCol, 0.92);
                }
            }
        }
        let MappedSlice = pow(ToneMapAces(FinalSlice * Raymarch.CameraPosition.w), float3(1.0 / 2.2));
        return float4(MappedSlice, 1.0);
    }

    // Channel 1: Explicit 3D Voxel DDA Cube Raymarcher
    if (RenderChannel == 1)
    {
        let VoxelRes  = RaymarchVoxelCubes(Origin, Direction, StartT, EndT, HalfBox, Scale, SunDir, SunIntensity, AmbientIntensity,
                                           Palette, FireIntensity, TempScale, ExtinctionNorm, SmokeAlbedo, ShadowSteps);
        if (ExportFlipbook)
        {
            let Coverage = clamp(1.0 - VoxelRes.w, 0.0, 1.0);
            if (Coverage < 0.004) return float4(0.0);
            let Straight = pow(ToneMapAces((VoxelRes.xyz / Coverage) * Raymarch.CameraPosition.w), float3(1.0 / 2.2));
            return float4(Straight, Coverage);
        }
        let Composited = VoxelRes.xyz + Background * VoxelRes.w;
        return float4(pow(ToneMapAces(Composited * Raymarch.CameraPosition.w), float3(1.0 / 2.2)), 1.0);
    }

    let DiagLen   = length(Scale);
    let StepLen   = DiagLen / float(max(RaymarchSteps, 16u));
    let Jitter    = frac(52.9829189 * frac(dot(In.Position.xy, float2(0.06711056, 0.00583715))));
    var Travel    = StartT + Jitter * StepLen;

    let CosTheta  = dot(Direction, SunDir);
    let PhaseVal  = EvaluateFluidPhase(CosTheta, PhaseAniso);
    let SunAlign  = pow(max(0.0, CosTheta), 6.0);

    var Radiance  = float3(0.0);
    var Trans     = 1.0;
    var BloomAcc  = float3(0.0);
    var ShaftAcc  = 0.0;
    var DebugWire = float3(0.0);

    for (var Step = 0u; Step < 256u; Step++)
    {
        if (Step >= RaymarchSteps || Travel >= EndT || Trans < 0.006)
        {
            break;
        }
        let WorldPos = Origin + Direction * Travel;
        var Uvw      = ProjectWorldToUvw(WorldPos, HalfBox, Scale);
        if (VoxelQuant > 0.01)
        {
            let QuantUvw = (floor(Uvw * GridSize) + 0.5) / GridSize;
            Uvw          = lerp(Uvw, QuantUvw, VoxelQuant);
        }

        if (RenderChannel >= 2 && RenderChannel <= 7)
        {
            var FieldCol = float3(0.0);
            var FieldDen = 0.0;
            if (RenderChannel == 2)
            {
                let Val  = DensityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).x;
                FieldDen = Val * 0.9;
                FieldCol = float3(0.85, 0.88, 0.94) * clamp(Val * 0.5, 0.0, 1.5);
            }
            else if (RenderChannel == 3)
            {
                let Val  = DensityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).y;
                FieldDen = Val * 0.7;
                FieldCol = EvaluateBlackbodyPalette(Val * TempScale, 0) * 0.8;
            }
            else if (RenderChannel == 4)
            {
                let Val  = DensityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).z;
                FieldDen = Val * 0.9;
                FieldCol = lerp(float3(0.05, 0.25, 0.65), float3(0.2, 0.95, 1.0), clamp(Val * 0.5, 0.0, 1.0));
            }
            else if (RenderChannel == 5)
            {
                let Vel  = VelocityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).xyz;
                let Mag  = length(Vel);
                FieldDen = clamp(Mag * 0.14, 0.0, 2.5);
                FieldCol = (normalize(Vel + float3(1.0e-4)) * 0.5 + 0.5) * clamp(Mag * 0.22, 0.0, 1.8);
            }
            else if (RenderChannel == 6)
            {
                let Mag  = VorticityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).w;
                FieldDen = clamp(Mag * 0.28, 0.0, 2.5);
                FieldCol = lerp(float3(0.08, 0.02, 0.28), float3(1.0, 0.55, 0.12), clamp(Mag * 0.22, 0.0, 1.0)) * 1.4;
            }
            else
            {
                let Press = PressureVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).x;
                FieldDen  = clamp(abs(Press) * 18.0, 0.0, 2.5);
                FieldCol  = SelectBranch(float3(0.15, 0.55, 1.0) * min(1.8, -Press * 25.0),
                                         float3(1.0, 0.28, 0.12) * min(1.8, Press * 25.0),
                                         Press >= 0.0);
            }
            if (FieldDen > 0.01)
            {
                let StepTr = exp(-FieldDen * 4.5 * OpticalScale * StepLen);
                Radiance  += Trans * FieldCol * (1.0 - StepTr);
                Trans     *= StepTr;
            }
            Travel += StepLen;
            continue;
        }

        let Den   = DensityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0);
        let Smoke = Den.x;
        let Temp  = Den.y * TempScale;
        let Soot  = Den.w;

        if (GodRaysIntensity > 0.01 && (Step & 1u) == 0u)
        {
            let EdgeFade    = smoothstep(0.49, 0.20, max(abs(Uvw.x - 0.5), abs(Uvw.z - 0.5))) * smoothstep(0.99, 0.78, Uvw.y);
            let ShaftShadow = EvaluateLightShadow(WorldPos, SunDir, HalfBox, Scale, 4u, ShadowExtinction);
            ShaftAcc       += (1.0 - ShaftShadow) * EdgeFade * StepLen * OpticalScale * Trans * 1.35;
        }

        if (Smoke < 0.008 && Temp < 0.04)
        {
            Travel += StepLen * 1.65;
            continue;
        }

        if (Raymarch.OverlayControls.w != 0u && (Smoke > 0.08 || Temp > 0.2))
        {
            let CellFrac = abs(frac(Uvw * GridSize) - 0.5);
            let MaxAxis  = max(CellFrac.x, max(CellFrac.y, CellFrac.z));
            let MidAxis  = CellFrac.x + CellFrac.y + CellFrac.z - MaxAxis - min(CellFrac.x, min(CellFrac.y, CellFrac.z));
            let Wire     = smoothstep(0.45, 0.495, MaxAxis) * smoothstep(0.45, 0.495, MidAxis);
            DebugWire   += Trans * float3(0.18, 0.72, 1.0) * Wire * 0.18;
        }

        let SootRatio   = clamp(Soot / max(Smoke + 0.08, 0.08), 0.0, 1.0);
        let SigmaS      = Smoke * ExtinctionNorm * SmokeAlbedo;
        let SigmaA      = Smoke * ExtinctionNorm * (1.0 - SmokeAlbedo + 0.55 * SootRatio) + Temp * 0.65 * OpticalScale;
        let SigmaT      = max(SigmaS + SigmaA, 1.0e-3);

        let LightTrans  = EvaluateLightShadow(WorldPos, SunDir, HalfBox, Scale, ShadowSteps, ShadowExtinction);
        let Powder      = 1.0 - 0.50 * exp(-Smoke * ExtinctionNorm * 0.55);
        let SunScatter  = float3(1.0, 0.93, 0.82) * SunIntensity * LightTrans * (PhaseVal * 2.8 + 0.38 * Powder);
        let AmbGradient = 0.40 + 0.65 * Uvw.y;
        let SkyScatter  = lerp(float3(0.18, 0.18, 0.18), float3(0.38, 0.38, 0.38), AmbGradient) * AmbientIntensity * 0.55;

        let IrradField  = PressureVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).y;
        let FireScatter = EvaluateBlackbodyPalette(max(0.75, IrradField * 1.35), Palette) * IrradField * InternalScatter * (0.45 + 0.55 * exp(-Smoke * 0.9));

        let SootTint    = lerp(float3(0.96, 0.94, 0.92), float3(0.22, 0.20, 0.19), SootRatio);
        let InScatter   = (SunScatter + SkyScatter) * SootTint + FireScatter;

        let SelfShadow  = exp(-Smoke * (0.28 + 0.42 * SootRatio));
        let FlameRad    = EvaluateBlackbodyPalette(Temp, Palette) * FireIntensity * SelfShadow;
        BloomAcc       += Trans * FlameRad * StepLen * OpticalScale * 0.55;

        let StepTrans   = exp(-SigmaT * StepLen);
        let Integral    = (1.0 - StepTrans) / SigmaT;
        let ReactBoost  = 1.0 + clamp(Den.z * 0.25, 0.0, 0.45);
        let SourceTerm  = SigmaS * InScatter + FlameRad * ReactBoost * OpticalScale;
        Radiance       += Trans * SourceTerm * Integral;
        Trans          *= StepTrans;

        if (Raymarch.OverlayControls.z != 0u)
        {
            let CellEdge = abs(frac(Uvw * GridSize) - 0.5);
            let EdgeLine = smoothstep(0.47, 0.5, max(CellEdge.x, max(CellEdge.y, CellEdge.z)));
            Radiance    += Trans * float3(0.08, 0.12, 0.18) * EdgeLine * StepLen * 0.7;
        }
        Travel += StepLen;
    }

    let ShaftGlow = float3(1.0, 0.88, 0.68) * ShaftAcc * GodRaysIntensity * (0.12 + 0.55 * SunAlign) * SunIntensity;
    if (ExportFlipbook)
    {
        let PreMult  = Radiance + BloomAcc * BloomIntensity * 0.85;
        let Coverage = clamp(1.0 - Trans, 0.0, 1.0);
        let GlowPeak = max(PreMult.x, max(PreMult.y, PreMult.z));
        let Alpha    = clamp(max(Coverage, clamp(GlowPeak * 0.35, 0.0, 1.0) * 0.85), 0.0, 1.0);
        if (Alpha < 0.004)
        {
            return float4(0.0);
        }
        let Unpremult = PreMult / max(Coverage, 0.08);
        let Tonemapped = pow(ToneMapAces(Unpremult * Raymarch.CameraPosition.w), float3(1.0 / 2.2));
        return float4(Tonemapped, Alpha);
    }

    let TotalHdr = Radiance + Background * Trans + BloomAcc * BloomIntensity * 0.85 + ShaftGlow + DebugWire;
    let Mapped   = pow(ToneMapAces(TotalHdr * Raymarch.CameraPosition.w), float3(1.0 / 2.2));
    return float4(Mapped, 1.0);
}
