//============================================================================================================================================
//                                                            IMPACTSPARK.SLANG
//============================================================================================================================================
// 🧩 Jolt hard contacts emit ballistic filings; this post-AA raster draws their camera-observed motion as hot, depth-tested streaks.

#pragma once
#include "IntrinsicParity.slang"
#include "RefreshDescriptor.slang"
#include "ImpactSparkLayouts.slang"

  [[vk::binding(1, 0)]] ConstantBuffer<ImpactSparkHeader> Impact;
  [[vk::binding(2, 0)]] StructuredBuffer<ImpactSparkRecord> ImpactSparks;

struct ImpactSparkVarying
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 Profile : LOCATION0;             // x across [-1,1], y tail→head [0,1]
    [[vk::location(1)]] nointerpolation uint SparkIndex : LOCATION1;
    [[vk::location(2)]] float3 Radiance : LOCATION2;
    [[vk::location(3)]] float2 Shape : LOCATION3;               // pixel width factor, shutter fraction
};

float2 ImpactSparkCorner(uint Vertex)
{
    if (Vertex == 0u) return float2(0.0, -1.0);
    if (Vertex == 1u) return float2(1.0, -1.0);
    if (Vertex == 2u) return float2(1.0,  1.0);
    if (Vertex == 3u) return float2(0.0, -1.0);
    if (Vertex == 4u) return float2(1.0,  1.0);
    return float2(0.0,  1.0);
}

float2 ImpactSparkScreen(float4 Clip)
{
    let Ndc = Clip.xy / Clip.w;
    return float2(Ndc.x * 0.5 + 0.5, 0.5 - Ndc.y * 0.5) * Refresh.ScreenExtent;
}

[shader("vertex")]
ImpactSparkVarying ImpactSparkVertex(uint Vertex : SV_VulkanVertexID, uint Instance : SV_VulkanInstanceID)
{
    var Varying : ImpactSparkVarying = {};
    Varying.SparkIndex = Instance;
    let Corner = ImpactSparkCorner(Vertex);
    Varying.Profile = float2(Corner.y, Corner.x);
    Varying.Radiance = 0.0;
    Varying.Shape = float2(1.0, 1.0);
    if (Instance >= min(Impact.Header.x, ImpactSparkCapacity))
    {
        Varying.Position = float4(2.0, 2.0, 0.0, 1.0);
        return Varying;
    }

    let Spark = ImpactSparks[Instance];
    let Head = Spark.HeadWidth.xyz;
    let HeadClip = mul(float4(Head, 1.0), Refresh.UnjitteredViewProjection);
    let ViewDepth = -(mul(float4(Head, 1.0), Refresh.View)).z;
    if (HeadClip.w <= 1.0e-4 || ViewDepth <= 0.05)
    {
        Varying.Position = float4(2.0, 2.0, 0.0, 1.0);
        return Varying;
    }

    var TailClip = mul(float4(Spark.TailPixelWidth.xyz, 1.0), Refresh.PreviousUnjitteredViewProjection);
    if (TailClip.w <= 1.0e-4) TailClip = HeadClip;
    let HeadPixels = ImpactSparkScreen(HeadClip);
    var TailPixels = ImpactSparkScreen(TailClip);

    // 📝 Bound the raster footprint: even a near-camera filing stays hair-thin; distant ones keep a subpixel glint.
    let CameraUp = float3(Refresh.View[0].y, Refresh.View[1].y, Refresh.View[2].y);
    let WidthClip = mul(float4(Head + CameraUp * Spark.HeadWidth.w, 1.0), Refresh.UnjitteredViewProjection);
    let Width = WidthClip.w > 1.0e-4
        ? clamp(length(ImpactSparkScreen(WidthClip) - HeadPixels), 0.65, 1.35)
        : 0.65;
    let PixelWidth = clamp(Spark.TailPixelWidth.w, 0.35, 1.0);
    let Shutter = clamp(Spark.RadianceShutter.w, 0.15, 1.0);
    TailPixels = lerp(HeadPixels, TailPixels, Shutter);

    var Along = HeadPixels - TailPixels;
    let Longest = 0.2 * Refresh.ScreenExtent.y;
    let LengthBeforeClamp = length(Along);
    if (LengthBeforeClamp > Longest)
    {
        TailPixels = HeadPixels - Along * (Longest / LengthBeforeClamp);
        Along = HeadPixels - TailPixels;
    }
    let Length = length(Along);
    Along = Length > 1.0e-3 ? Along / Length : float2(1.0, 0.0);
    let Across = float2(-Along.y, Along.x);

    // 📝 Both ends use the head's reversed-Z depth; streaks are short, and the opaque scene still occludes them.
    let BasePixels = Corner.x > 0.5 ? HeadPixels : TailPixels;
    let Offset = Across * Corner.y * Width * PixelWidth * lerp(0.22, 1.0, Corner.x) * 0.5 +
                 Along * (Corner.x - 0.5) * Width * PixelWidth;
    let Pixels = BasePixels + Offset;
    let Ndc = float2(Pixels.x * Refresh.InverseScreenExtent.x * 2.0 - 1.0,
                     1.0 - Pixels.y * Refresh.InverseScreenExtent.y * 2.0);
    Varying.Position = float4(Ndc * HeadClip.w, HeadClip.z, HeadClip.w);

    // 📝 The finite radiance is spread over the streak's screen footprint, as on the VehicleHMI wheel-scrape raster.
    let Spread = Width / max(Length + Width, Width);
    Varying.Radiance = Spark.RadianceShutter.rgb * lerp(0.35, 1.0, Spread) * Refresh.IncandescenceScale;
    Varying.Shape = float2(PixelWidth, Shutter);
    return Varying;
}

[shader("fragment")]
float4 ImpactSparkFragment(ImpactSparkVarying Varying) : SV_Target0
{
    if (Varying.SparkIndex >= min(Impact.Header.x, ImpactSparkCapacity)) discard;
    let Across = pow(max(0.0, 1.0 - Varying.Profile.x * Varying.Profile.x), 2.0);
    let Head = lerp(lerp(0.08, 0.35, Varying.Shape.y), 1.0,
                    pow(Varying.Profile.y, lerp(0.7, 1.8, Varying.Shape.x)));
    return float4(Varying.Radiance * Across * Head, 0.0);
}
