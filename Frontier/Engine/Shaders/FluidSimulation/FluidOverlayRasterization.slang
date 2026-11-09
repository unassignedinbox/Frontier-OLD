//============================================================================================================================================
//                                                   FLUIDOVERLAYRASTERIZATION.SLANG
//============================================================================================================================================
// 🧩 GPU-advected hot ember & ash particle billboards and 2D tiled 3D voxel atlas PiP diagnostic minimap.

#pragma once
#include "FluidLayouts.slang"

[[vk::binding(0, 0)]] ConstantBuffer<FluidEmberConstants> Ember;
[[vk::binding(1, 0)]] Texture3D<float4>                   DensityVolume;
[[vk::binding(2, 0)]] Texture3D<float4>                   VelocityVolume;
[[vk::binding(3, 0)]] Texture3D<float4>                   PressureVolume;
[[vk::binding(4, 0)]] Texture3D<float4>                   VorticityVolume;
[[vk::binding(5, 0)]] SamplerState                        VolumeLinearSampler;
[[vk::binding(6, 0)]] SamplerState                        VolumeNearestSampler;
[[vk::binding(7, 0)]] ConstantBuffer<FluidAtlasConstants> Atlas;

static const float2 BillboardCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
    float2(1.0, -1.0),  float2(1.0, 1.0),  float2(-1.0, 1.0)
};

struct EmberVarying
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 Corner : LOCATION0;
    [[vk::location(1)]] float4 HeatState : LOCATION1; // x heat, y alpha, z ashiness, w active
};

float HashEmber(float Seed)
{
    return frac(sin(Seed * 127.1 + 311.7) * 43758.5453123);
}

[shader("vertex")]
EmberVarying EmberVertex(uint VertexIndex : SV_VertexID)
{
    var Out : EmberVarying;
    let ParticleIndex = VertexIndex / 6u;
    let CornerIndex   = VertexIndex % 6u;
    if (ParticleIndex >= Ember.StyleControls.x)
    {
        Out.Position  = float4(0.0, 0.0, 2.0, 1.0);
        Out.Corner    = float2(0.0);
        Out.HeatState = float4(0.0);
        return Out;
    }

    let Id        = float(ParticleIndex);
    let SeedA     = HashEmber(Id * 1.13 + 0.7);
    let SeedB     = HashEmber(Id * 2.71 + 3.1);
    let SeedC     = HashEmber(Id * 4.37 + 7.9);
    let SeedD     = HashEmber(Id * 6.91 + 13.3);

    let IsAsh     = step(1.0 - Ember.MotionControls.w, SeedD);
    let Period    = (1.4 + 2.2 * SeedA) * Ember.MotionControls.z * lerp(1.0, 1.9, IsAsh);
    let Phase     = frac(Ember.DomainTime.w / max(Period, 0.25) + SeedB);
    let Age       = Phase * Period;

    let Angle     = SeedC * 6.2831853 + floor(Ember.DomainTime.w / max(Period, 0.25)) * 2.39996;
    let Radius    = (0.04 + 0.22 * sqrt(SeedA)) * lerp(1.0, 1.85, IsAsh);
    var Uvw       = float3(0.5 + cos(Angle) * Radius, lerp(0.08 + 0.16 * SeedD, 0.18 + 0.45 * SeedD, IsAsh), 0.5 + sin(Angle) * Radius);

    let Scale     = max(Ember.DomainTime.xyz, float3(0.25));
    let Vel0      = VelocityVolume.SampleLevel(VolumeLinearSampler, clamp(Uvw, float3(0.02), float3(0.98)), 0.0).xyz;
    Uvw          += (Vel0 / Scale) * Age * 0.42;
    let Vel1      = VelocityVolume.SampleLevel(VolumeLinearSampler, clamp(Uvw, float3(0.02), float3(0.98)), 0.0).xyz;
    Uvw          += (Vel1 / Scale) * Age * 0.36;

    let RiseRate  = lerp(0.18 + 0.18 * SeedC, -0.04 + 0.09 * sin(Ember.DomainTime.w * 1.7 + Id), IsAsh);
    Uvw.y        += Age * RiseRate;
    Uvw.x        += sin(Age * (3.5 + 3.0 * SeedA) + Id) * lerp(0.035, 0.065, IsAsh) + Ember.MotionControls.x * Age * 0.04;
    Uvw.z        += cos(Age * (3.1 + 2.7 * SeedB) + Id * 1.7) * lerp(0.035, 0.065, IsAsh) + Ember.MotionControls.y * Age * 0.04;

    let OriginDen = DensityVolume.SampleLevel(VolumeLinearSampler, float3(0.5, 0.16, 0.5), 0.0);
    let LocalDen  = DensityVolume.SampleLevel(VolumeLinearSampler, clamp(Uvw, float3(0.02), float3(0.98)), 0.0);
    let Activity  = max(OriginDen.y + OriginDen.x * 0.5, LocalDen.y + LocalDen.x * 0.6);

    let Inside    = all(Uvw >= float3(0.0)) && all(Uvw <= float3(1.0));
    if (!Inside || Activity <= 0.08)
    {
        Out.Position  = float4(0.0, 0.0, 2.0, 1.0);
        Out.Corner    = float2(0.0);
        Out.HeatState = float4(0.0);
        return Out;
    }

    let Heat      = lerp((1.0 - Phase * 0.82) * clamp(0.8 + Activity * 0.55, 0.4, 3.2),
                         max(0.0, (0.35 - Phase * 0.55)) * clamp(OriginDen.y * 0.5, 0.0, 1.4),
                         IsAsh);
    let Alpha     = smoothstep(0.0, 0.08, Phase) * (1.0 - smoothstep(0.68, 1.0, Phase));

    let WorldCent = (Uvw - 0.5) * Scale;
    let Corner    = BillboardCorners[CornerIndex];
    let RadiusM   = Ember.CameraRightSize.w * 0.014 * (0.65 + 0.75 * SeedA) * (1.0 - 0.35 * Phase) * lerp(1.0, 1.35, IsAsh);
    let WorldVert = WorldCent + (Ember.CameraRightSize.xyz * Corner.x + Ember.CameraUpIntensity.xyz * Corner.y) * RadiusM;

    Out.Position  = mul(float4(WorldVert, 1.0), Ember.ViewProjection);
    Out.Corner    = Corner;
    Out.HeatState = float4(Heat, Alpha, IsAsh, 1.0);
    return Out;
}

[shader("fragment")]
float4 EmberFragment(EmberVarying In) : SV_Target0
{
    if (In.HeatState.w < 0.5)
    {
        discard;
    }
    let RadialSq = dot(In.Corner, In.Corner);
    if (RadialSq > 1.0)
    {
        discard;
    }
    let Core      = exp(-RadialSq * 4.5);
    let Halo      = exp(-RadialSq * 1.6) * 0.45;
    let Palette   = int(Ember.StyleControls.y);
    let Exposure  = float(Ember.StyleControls.w) * 0.001;
    let Glow      = EvaluateBlackbodyPalette(In.HeatState.x * 1.35, Palette) * (Core + Halo) * Ember.CameraUpIntensity.w;
    let AshFlake  = float3(0.24, 0.23, 0.25) * smoothstep(1.0, 0.25, RadialSq) * (0.55 + 0.45 * Ember.CameraUpIntensity.w);
    let HdrColour = lerp(Glow, AshFlake + Glow * 0.45, In.HeatState.z) * In.HeatState.y;
    let Mapped    = pow(clamp(HdrColour * Exposure * 0.55, float3(0.0), float3(1.0)), float3(1.0 / 2.2));
    let Opacity   = clamp(In.HeatState.y * lerp((Core + Halo) * 0.9, smoothstep(1.0, 0.2, RadialSq) * 0.72, In.HeatState.z), 0.0, 1.0);
    return float4(Mapped, Opacity);
}

//------------------------------------------------------------------------------------------------------------------------
//                                        2D TILED 3D VOXEL ATLAS PIP MINIMAP
//------------------------------------------------------------------------------------------------------------------------

struct AtlasVarying
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 PanelUv : LOCATION0;
};

[shader("vertex")]
AtlasVarying AtlasVertex(uint VertexIndex : SV_VertexID)
{
    var Out : AtlasVarying;
    let Corner   = BillboardCorners[VertexIndex % 6u] * 0.5 + 0.5;
    let ClipPos  = lerp(Atlas.PanelRectangle.xy, Atlas.PanelRectangle.zw, Corner);
    Out.Position = float4(ClipPos, 0.0, 1.0);
    Out.PanelUv  = float2(Corner.x, 1.0 - Corner.y);
    return Out;
}

[shader("fragment")]
float4 AtlasFragment(AtlasVarying In) : SV_Target0
{
    let GridSize  = int(Atlas.AtlasControls.x);
    let Field     = int(Atlas.AtlasControls.y);
    let TilesX    = max(int(Atlas.AtlasControls.z), 1);
    let TilesY    = max(int(Atlas.AtlasControls.w), 1);

    let TileCoord = In.PanelUv * float2(float(TilesX), float(TilesY));
    let TileIdx   = int2(floor(TileCoord));
    let LocalUv   = frac(TileCoord);
    let SliceZ    = TileIdx.y * TilesX + TileIdx.x;

    let OuterEdge = min(min(In.PanelUv.x, 1.0 - In.PanelUv.x), min(In.PanelUv.y, 1.0 - In.PanelUv.y));
    if (OuterEdge < 0.012)
    {
        return float4(0.32, 0.44, 0.62, 0.94);
    }
    if (SliceZ >= GridSize)
    {
        return float4(0.03, 0.04, 0.06, 0.88);
    }

    let Uvw       = float3(LocalUv.x, 1.0 - LocalUv.y, (float(SliceZ) + 0.5) / float(GridSize));
    var Colour    = float3(0.04, 0.05, 0.08);

    if (Field == 0)
    {
        let Den = DensityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0);
        Colour += float3(0.55, 0.58, 0.66) * clamp(Den.x * 0.55, 0.0, 1.0);
        Colour += EvaluateBlackbodyPalette(Den.y * 1.15, 1) * 0.65;
    }
    else if (Field == 1)
    {
        let Vel = VelocityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).xyz;
        Colour  = (normalize(Vel + float3(1.0e-4)) * 0.5 + 0.5) * clamp(length(Vel) * 0.22, 0.06, 1.0);
    }
    else if (Field == 2)
    {
        let Pr  = PressureVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0);
        Colour += SelectBranch(float3(0.15, 0.55, 1.0) * min(1.0, -Pr.x * 22.0),
                               float3(1.0, 0.28, 0.12) * min(1.0, Pr.x * 22.0),
                               Pr.x >= 0.0);
        Colour += float3(1.0, 0.65, 0.18) * clamp(Pr.y * 0.85, 0.0, 1.0);
    }
    else
    {
        let Mag = VorticityVolume.SampleLevel(VolumeLinearSampler, Uvw, 0.0).w;
        Colour  = lerp(float3(0.05, 0.03, 0.16), float3(1.0, 0.62, 0.14), clamp(Mag * 0.24, 0.0, 1.0));
    }

    let TileEdge = min(min(LocalUv.x, 1.0 - LocalUv.x), min(LocalUv.y, 1.0 - LocalUv.y));
    let Divider  = 1.0 - smoothstep(0.0, 0.045, TileEdge);
    Colour       = lerp(Colour, float3(0.16, 0.22, 0.32), Divider * 0.65);
    return float4(clamp(Colour, float3(0.0), float3(1.0)), 0.92);
}
