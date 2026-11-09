//============================================================================================================================================
//                                                           PLUMEMARCH.SLANG
//============================================================================================================================================
// 🧩 Coverage-clipped volumetric plume raymarch with Wrenninge 3-octave multiple scattering, sun shadows, and fire incandescence.

#pragma once
#include "IntrinsicParity.slang"
#include "HostConstants.slang"
#include "RefreshDescriptor.slang"
#include "SkyProjection.slang"
#include "PlumeLayouts.slang"

struct PlumeCascadeReceiver
{
    float4x4 LightViewProjection[3];
    float4   Splits;        // [m] xyz far distance of each cascade, w cascade count
    float4   TexelWorld;    // [m] world size of one shadow texel per cascade
};

[[vk::binding(1, 0)]] ConstantBuffer<PlumeConstants>           Plume;
[[vk::binding(2, 0)]] ConstantBuffer<PlumeFireLight>           FireLight;
[[vk::binding(3, 0)]] Texture3D<float4>                        ThermoTexture;
[[vk::binding(4, 0)]] Texture3D<float4>                        TransmittanceTexture;
[[vk::binding(5, 0)]] Texture3D<float4>                        DetailTexture;
[[vk::binding(6, 0)]] SamplerState                             LinearRepeat;
[[vk::binding(7, 0)]] Texture2D<float>                         SurfaceDepth;
[[vk::binding(8, 0)]] StructuredBuffer<uint>                   Coverage;
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> PlumeTarget;
[[vk::binding(10, 0)]] Texture2DArray<float>                   ShadowDepth;
[[vk::binding(11, 0)]] SamplerComparisonState                  ShadowSampler;
[[vk::binding(12, 0)]] ConstantBuffer<PlumeCascadeReceiver>    Receiver;
[[vk::binding(13, 0)]] Texture2D<float4>                       SkyView;
[[vk::binding(14, 0)]] SamplerState                            SkySampler;

float EvaluateHenyeyGreenstein(float Cosine, float Anisotropy)
{
    let Squared     = Anisotropy * Anisotropy;
    let Denominator = max(1.0 + Squared - 2.0 * Anisotropy * Cosine, 1.0e-4);
    return (1.0 - Squared) / (12.566370614359172 * Denominator * sqrt(Denominator));
}

float EvaluateDualPhase(float Cosine, float ForwardG, float BackwardG, float Blend)
{
    return lerp(EvaluateHenyeyGreenstein(Cosine, BackwardG), EvaluateHenyeyGreenstein(Cosine, ForwardG), Blend);
}

float EvaluateMarchNoise(uint2 Pixel, uint RefreshIndex)
{
    let Seed = float2(Pixel) + float2(float(RefreshIndex & 63u) * 5.588238, float((RefreshIndex >> 3u) & 63u) * 7.258823);
    return frac(52.9829189 * frac(dot(Seed, float2(0.06711056, 0.00583715))));
}

float2 IntersectAxisBox(float3 Origin, float3 InverseDirection, float3 BoxLow, float3 BoxHigh)
{
    let NearPlane = (BoxLow - Origin) * InverseDirection;
    let FarPlane  = (BoxHigh - Origin) * InverseDirection;
    let Smaller   = min(NearPlane, FarPlane);
    let Larger    = max(NearPlane, FarPlane);
    let Entry     = max(max(Smaller.x, Smaller.y), Smaller.z);
    let Exit      = min(min(Larger.x, Larger.y), Larger.z);
    return float2(Entry, Exit);
}

float ResolveOpaqueTravel(uint2 TracePixel, float3 RayDirection)
{
    let ScreenPixel = min(TracePixel * Refresh.TraceScale, uint2(Refresh.ScreenExtent) - uint2(1u));
    let Depth       = SurfaceDepth.Load(int3(int2(ScreenPixel), 0));
    if (Depth <= 0.0)
    {
        return 1.0e6;
    }
    let World = ReconstructWorldPosition(float2(ScreenPixel) + 0.5, Depth);
    return dot(World - Refresh.CameraPosition, RayDirection);
}

float ResolveVolumeCascadeShadow(float3 Position)
{
    let ViewPosition = mul(float4(Position, 1.0), Refresh.View);
    let ViewDistance = -ViewPosition.z;
    let Count        = ToUnsigned(Receiver.Splits.w);
    if (Count == 0u || ViewDistance > Receiver.Splits[Count - 1u])
    {
        return 1.0;
    }
    var Cascade = 0u;
    while (Cascade + 1u < Count && ViewDistance > Receiver.Splits[Cascade])
    {
        Cascade++;
    }
    let Clip        = mul(float4(Position, 1.0), Receiver.LightViewProjection[Cascade]);
    let Ndc         = Clip.xyz / max(Clip.w, 1.0e-5);
    let Coordinates = float2(Ndc.x * 0.5 + 0.5, 0.5 - Ndc.y * 0.5);
    if (any(Coordinates < float2(0.0)) || any(Coordinates > float2(1.0)) || Ndc.z > 1.0)
    {
        return 1.0;
    }
    return ShadowDepth.SampleCmpLevelZero(ShadowSampler, float3(Coordinates, float(Cascade)), Ndc.z);
}

float3 ResolveFireGlow(float3 Position)
{
    let Intensity = FireLight.PositionIntensity.w;
    if (Intensity <= 0.0)
    {
        return float3(0.0);
    }
    let Offset     = FireLight.PositionIntensity.xyz - Position;
    let DistanceSq = dot(Offset, Offset);
    let Reach      = max(FireLight.ColourReach.w, 0.5);
    let ReachSq    = Reach * Reach;
    if (DistanceSq >= ReachSq)
    {
        return float3(0.0);
    }
    let Window  = 1.0 - DistanceSq / ReachSq;
    let Falloff = (Window * Window) / (DistanceSq + 0.35);
    return FireLight.ColourReach.rgb * (Intensity * Falloff * (1.0 / 12.566370614359172));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void MarchPlume(uint3 Pixel : SV_DispatchThreadID)
{
    let TraceExtent = Plume.TraceDimensions.xy;
    if (any(Pixel.xy >= TraceExtent))
    {
        return;
    }

    if (Coverage[6] == 0u)
    {
        PlumeTarget[Pixel.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    let VoxelSize   = Plume.CellMetrics.x;
    let WindowLow   = Plume.WindowMinimum.xyz;
    let LowCells    = float3(float(Coverage[0]), float(Coverage[1]), float(Coverage[2]));
    let HighCells   = float3(float(Coverage[3]), float(Coverage[4]), float(Coverage[5]));
    let Pad         = float3(VoxelSize * 1.5);
    let BoxLow      = max(WindowLow + LowCells * VoxelSize - Pad, WindowLow);
    let BoxHigh     = min(WindowLow + HighCells * VoxelSize + Pad, Plume.WindowMaximum.xyz);
    if (any(BoxHigh <= BoxLow))
    {
        PlumeTarget[Pixel.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    let ScreenPixel   = (float2(Pixel.xy) + 0.5) * float(Refresh.TraceScale);
    let FarPoint      = ReconstructWorldPosition(ScreenPixel, 1.0e-4);
    let RayOrigin     = Refresh.CameraPosition;
    let RayDirection  = normalize(FarPoint - RayOrigin);
    let SafeDirection = SelectBranch(RayDirection, float3(1.0e-6) * SelectBranch(float3(-1.0), float3(1.0), RayDirection >= float3(0.0)),
                                     abs(RayDirection) < float3(1.0e-6));
    let Span          = IntersectAxisBox(RayOrigin, 1.0 / SafeDirection, BoxLow, BoxHigh);
    if (Span.y <= max(Span.x, 0.0))
    {
        PlumeTarget[Pixel.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    let OpaqueTravel = ResolveOpaqueTravel(Pixel.xy, RayDirection);
    let StartTravel  = max(Span.x, 0.02);
    let EndTravel    = min(Span.y, OpaqueTravel);
    if (EndTravel <= StartTravel)
    {
        PlumeTarget[Pixel.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    let MaxSteps       = uint(max(Plume.PreviousCoarse.w, 8));
    let TotalSpan      = EndTravel - StartTravel;
    let BaseStep       = max(VoxelSize * 0.72, TotalSpan / float(MaxSteps));
    let Jitter         = EvaluateMarchNoise(Pixel.xy, Refresh.RefreshIndex);
    var Travel         = StartTravel + Jitter * BaseStep;

    let SunDirection   = normalize(Refresh.SunDirection);
    let Cosine         = dot(RayDirection, SunDirection);
    let ForwardG       = Plume.OpticalTerms.z;
    let BackwardG      = Plume.OpticalTerms.w;
    let PhaseBlend     = Plume.ScatteringTerms.x;
    let MultiScatter   = Plume.ScatteringTerms.y;
    let ExtinctionRate = Plume.Dissipation.w;
    let DetailAmount   = Plume.ScatteringTerms.w;
    let DetailScale    = Plume.FlameTerms.z;
    let ElapsedTime    = Plume.WindowMaximum.w;

    let ZenithSky      = SkyView.SampleLevel(SkySampler, ProjectSkyCoordinates(float3(0.0, 0.0, 1.0)), 0.0).rgb * Refresh.SkyScale + Refresh.NightSkyRadiance;
    let HorizonSky     = SkyView.SampleLevel(SkySampler, ProjectSkyCoordinates(normalize(float3(RayDirection.x, RayDirection.y, 0.18))), 0.0).rgb * Refresh.SkyScale + Refresh.NightSkyRadiance;
    let AmbientLight   = ZenithSky * 0.62 + HorizonSky * 0.38;

    var Transmittance  = 1.0;
    var Radiance       = float3(0.0);

    for (var Step = 0u; Step < MaxSteps; Step++)
    {
        if (Travel >= EndTravel || Transmittance < 0.008)
        {
            break;
        }
        let Position  = RayOrigin + RayDirection * Travel;
        let Thermo    = ThermoTexture.SampleLevel(LinearRepeat, ProjectThermoCoordinates(Plume, Position), 0.0);
        var Density   = Thermo.x;
        let Heat      = Thermo.y;
        let Soot      = Thermo.z;
        let Flame     = Thermo.w;

        var StepSize  = BaseStep;
        if (Density < 0.0025 && Flame < 0.002)
        {
            Travel += BaseStep * 1.75;
            continue;
        }

        if (DetailAmount > 0.0)
        {
            let Drift  = float3(Plume.WindVector.x, Plume.WindVector.y, 0.8) * (ElapsedTime * 0.35);
            let Detail = DetailTexture.SampleLevel(LinearRepeat, (Position - Drift) * DetailScale, 0.0);
            let Erode  = lerp(Detail.x, Detail.y, 0.45);
            let Edge   = saturate(1.0 - Density * 1.35);
            Density    = max(Density - Edge * DetailAmount * (1.0 - Erode) * 0.55, 0.0);
            Density   *= lerp(1.0, 0.72 + 0.56 * Detail.z, DetailAmount * 0.65);
        }

        if (Density > 0.001 || Flame > 0.001)
        {
            let SigmaT           = Density * ExtinctionRate;
            let Albedo           = lerp(Plume.OpticalTerms.x, Plume.OpticalTerms.y, Soot);
            let SelfShadow       = max(TransmittanceTexture.SampleLevel(LinearRepeat, ProjectVelocityCoordinates(Plume, Position), 0.0).x, 0.0);
            let SceneShadow      = SelectBranch(1.0, ResolveVolumeCascadeShadow(Position), FeatureEnabled(Refresh, FeatureDirectShadow));
            let SunIlluminance   = Refresh.SunIlluminance * SceneShadow;

            let OpticalEstimate  = -log(max(SelfShadow, 1.0e-4));
            var SunScattered     = float3(0.0);
            var OctaveExtinction = 1.0;
            var OctaveAlbedo     = 1.0;
            var OctaveG          = 1.0;
            for (var Octave = 0u; Octave < 3u; Octave++)
            {
                let Phase         = EvaluateDualPhase(Cosine, ForwardG * OctaveG, BackwardG * OctaveG, PhaseBlend);
                let ShadowTerm    = exp(-OpticalEstimate * OctaveExtinction);
                SunScattered     += SunIlluminance * (OctaveAlbedo * Phase * ShadowTerm);
                OctaveAlbedo     *= Albedo * MultiScatter;
                OctaveExtinction *= 0.48;
                OctaveG          *= 0.55;
            }

            let RoofDistance     = max(BoxHigh.z - Position.z, 0.0);
            let SkyVisibility    = exp(-Density * ExtinctionRate * min(RoofDistance, 2.2) * 0.35);
            let SkyScattered     = AmbientLight * (SkyVisibility * (1.0 / 12.566370614359172) * (1.0 + MultiScatter * Albedo * 0.65));
            let FireScattered    = ResolveFireGlow(Position) * (1.0 / 12.566370614359172) * (1.0 + Albedo * 0.8);

            let InScatter        = (SunScattered + SkyScattered + FireScattered) * (SigmaT * Albedo);
            let FlameRadiance    = ResolveBlackbodyRadiance(Flame, Heat, Plume);

            let StepTrans        = exp(-SigmaT * StepSize);
            let Integral         = SelectBranch(StepSize, (1.0 - StepTrans) / SigmaT, SigmaT > 1.0e-4);
            Radiance            += Transmittance * (InScatter + FlameRadiance * max(SigmaT * 0.35, 1.0)) * Integral;
            Transmittance       *= StepTrans;
        }
        Travel += StepSize;
    }

    PlumeTarget[Pixel.xy] = float4(Radiance, Transmittance);
}
