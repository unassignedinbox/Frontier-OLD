//============================================================================================================================================
//                                                         PLUMEVORTICITY.SLANG
//============================================================================================================================================
// 🧩 Fedkiw-Stam-Jensen vorticity confinement on the coarse toroidal velocity lattice.

#pragma once
#include "IntrinsicParity.slang"
#include "PlumeLayouts.slang"

[[vk::binding(0, 0)]] ConstantBuffer<PlumeConstants> Plume;
[[vk::binding(1, 0)]] Texture3D<float4>              VelocityRead;
[[vk::binding(2, 0)]] Texture3D<float4>              CurlRead;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> FieldWrite;

float3 LoadClampedVelocity(int3 LocalCell)
{
    let Bounds  = int3(Plume.CoarseExtent.xyz);
    let Clamped = clamp(LocalCell, int3(0), Bounds - int3(1));
    let Texel   = WrapLattice(Plume.CoarseOrigin.xyz + Clamped, Bounds);
    return VelocityRead.Load(int4(Texel, 0)).xyz;
}

float4 LoadClampedCurl(int3 LocalCell)
{
    let Bounds  = int3(Plume.CoarseExtent.xyz);
    let Clamped = clamp(LocalCell, int3(0), Bounds - int3(1));
    let Texel   = WrapLattice(Plume.CoarseOrigin.xyz + Clamped, Bounds);
    return CurlRead.Load(int4(Texel, 0));
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ResolveCurl(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let Signed    = int3(Cell);
    let VelocityR = LoadClampedVelocity(Signed + int3(1, 0, 0));
    let VelocityL = LoadClampedVelocity(Signed - int3(1, 0, 0));
    let VelocityF = LoadClampedVelocity(Signed + int3(0, 1, 0));
    let VelocityB = LoadClampedVelocity(Signed - int3(0, 1, 0));
    let VelocityU = LoadClampedVelocity(Signed + int3(0, 0, 1));
    let VelocityD = LoadClampedVelocity(Signed - int3(0, 0, 1));

    let Scale     = 0.5 * Plume.CellMetrics.w;
    let Vorticity = float3((VelocityF.z - VelocityB.z) - (VelocityU.y - VelocityD.y),
                           (VelocityU.x - VelocityD.x) - (VelocityR.z - VelocityL.z),
                           (VelocityR.y - VelocityL.y) - (VelocityF.x - VelocityB.x)) * Scale;
    let OutTexel  = ResolveVelocityTexel(Plume, Cell);
    FieldWrite[OutTexel] = float4(Vorticity, length(Vorticity));
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ConfineVorticity(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let OutTexel = ResolveVelocityTexel(Plume, Cell);
    let Base     = VelocityRead.Load(int4(OutTexel, 0));
    if (Base.w > 0.5)
    {
        FieldWrite[OutTexel] = Base;
        return;
    }

    let Signed   = int3(Cell);
    let MagR     = LoadClampedCurl(Signed + int3(1, 0, 0)).w;
    let MagL     = LoadClampedCurl(Signed - int3(1, 0, 0)).w;
    let MagF     = LoadClampedCurl(Signed + int3(0, 1, 0)).w;
    let MagB     = LoadClampedCurl(Signed - int3(0, 1, 0)).w;
    let MagU     = LoadClampedCurl(Signed + int3(0, 0, 1)).w;
    let MagD     = LoadClampedCurl(Signed - int3(0, 0, 1)).w;

    let Gradient = float3(MagR - MagL, MagF - MagB, MagU - MagD) * (0.5 * Plume.CellMetrics.w);
    let Length   = length(Gradient);
    var Velocity = Base.xyz;
    if (Length > 1.0e-5)
    {
        let Normal = Gradient / Length;
        let Omega  = LoadClampedCurl(Signed).xyz;
        let Force  = Plume.BuoyancyTerms.z * Plume.CellMetrics.z * cross(Normal, Omega);
        Velocity  += clamp(Force, float3(-30.0), float3(30.0)) * Plume.WindowMinimum.w;
    }
    FieldWrite[OutTexel] = float4(Velocity, Base.w);
}
