//============================================================================================================================================
//                                                          PLUMEPRESSURE.SLANG
//============================================================================================================================================
// 🧩 Warm-started Jacobi pressure solve and divergence-free Helmholtz projection with solid Neumann boundaries.

#pragma once
#include "IntrinsicParity.slang"
#include "PlumeLayouts.slang"

[[vk::binding(0, 0)]] ConstantBuffer<PlumeConstants> Plume;
[[vk::binding(1, 0)]] Texture3D<float4>              VelocityRead;
[[vk::binding(2, 0)]] Texture3D<float4>              DivergenceRead;
[[vk::binding(3, 0)]] Texture3D<float4>              PressureRead;
[[vk::binding(4, 0)]] [[vk::image_format("r32f")]]    RWTexture3D<float4> ScalarWrite;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> VelocityWrite;

float4 LoadWindowVelocity(int3 LocalCell)
{
    let Bounds = int3(Plume.CoarseExtent.xyz);
    if (LocalCell.z < 0)
    {
        let FloorTexel = WrapLattice(Plume.CoarseOrigin.xyz + int3(clamp(LocalCell.x, 0, Bounds.x - 1), clamp(LocalCell.y, 0, Bounds.y - 1), 0), Bounds);
        let Sample     = VelocityRead.Load(int4(FloorTexel, 0));
        return float4(Sample.x, Sample.y, 0.0, 1.0);
    }
    if (any(LocalCell < int3(0)) || any(LocalCell >= Bounds))
    {
        return float4(Plume.WindVector.xyz, 0.0);
    }
    let Texel = WrapLattice(Plume.CoarseOrigin.xyz + LocalCell, Bounds);
    return VelocityRead.Load(int4(Texel, 0));
}

float LoadWindowPressure(int3 LocalCell, float CentrePressure)
{
    let Bounds = int3(Plume.CoarseExtent.xyz);
    if (LocalCell.z < 0)
    {
        return CentrePressure;
    }
    if (any(LocalCell < int3(0)) || any(LocalCell >= Bounds))
    {
        return 0.0;
    }
    let Texel = WrapLattice(Plume.CoarseOrigin.xyz + LocalCell, Bounds);
    let Solid = VelocityRead.Load(int4(Texel, 0)).w > 0.5;
    return SelectBranch(PressureRead.Load(int4(Texel, 0)).x, CentrePressure, Solid);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ResolveDivergence(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let OutTexel = ResolveVelocityTexel(Plume, Cell);
    let Centre   = VelocityRead.Load(int4(OutTexel, 0));
    if (Centre.w > 0.5)
    {
        ScalarWrite[OutTexel] = float4(0.0);
        return;
    }

    let Signed = int3(Cell);
    let Right  = LoadWindowVelocity(Signed + int3(1, 0, 0));
    let Left   = LoadWindowVelocity(Signed - int3(1, 0, 0));
    let Front  = LoadWindowVelocity(Signed + int3(0, 1, 0));
    let Back   = LoadWindowVelocity(Signed - int3(0, 1, 0));
    let Up     = LoadWindowVelocity(Signed + int3(0, 0, 1));
    let Down   = LoadWindowVelocity(Signed - int3(0, 0, 1));

    let Divergence = ((Right.x - Left.x) + (Front.y - Back.y) + (Up.z - Down.z)) * (0.5 * Plume.CellMetrics.w);
    ScalarWrite[OutTexel] = float4(Divergence, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void RelaxPressure(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let OutTexel = ResolveVelocityTexel(Plume, Cell);
    let Position = ProjectVelocityWorld(Plume, Cell);
    if (Plume.CoarseOrigin.w != 0 || !ClassifyPreviousVelocity(Plume, Position))
    {
        ScalarWrite[OutTexel] = float4(0.0);
        return;
    }
    if (VelocityRead.Load(int4(OutTexel, 0)).w > 0.5)
    {
        ScalarWrite[OutTexel] = float4(0.0);
        return;
    }

    let Signed         = int3(Cell);
    let CentrePressure = PressureRead.Load(int4(OutTexel, 0)).x;
    let PressureR      = LoadWindowPressure(Signed + int3(1, 0, 0), CentrePressure);
    let PressureL      = LoadWindowPressure(Signed - int3(1, 0, 0), CentrePressure);
    let PressureF      = LoadWindowPressure(Signed + int3(0, 1, 0), CentrePressure);
    let PressureB      = LoadWindowPressure(Signed - int3(0, 1, 0), CentrePressure);
    let PressureU      = LoadWindowPressure(Signed + int3(0, 0, 1), CentrePressure);
    let PressureD      = LoadWindowPressure(Signed - int3(0, 0, 1), CentrePressure);

    let Divergence     = DivergenceRead.Load(int4(OutTexel, 0)).x;
    let CellSquared    = Plume.CellMetrics.z * Plume.CellMetrics.z;
    let NextPressure   = (PressureR + PressureL + PressureF + PressureB + PressureU + PressureD - Divergence * CellSquared) * (1.0 / 6.0);
    ScalarWrite[OutTexel] = float4(NextPressure, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ProjectVelocity(uint3 Cell : SV_DispatchThreadID)
{
    if (any(Cell >= Plume.CoarseExtent.xyz))
    {
        return;
    }
    let OutTexel = ResolveVelocityTexel(Plume, Cell);
    let Centre   = VelocityRead.Load(int4(OutTexel, 0));
    if (Centre.w > 0.5)
    {
        VelocityWrite[OutTexel] = Centre;
        return;
    }

    let Signed         = int3(Cell);
    let CentrePressure = PressureRead.Load(int4(OutTexel, 0)).x;
    let PressureR      = LoadWindowPressure(Signed + int3(1, 0, 0), CentrePressure);
    let PressureL      = LoadWindowPressure(Signed - int3(1, 0, 0), CentrePressure);
    let PressureF      = LoadWindowPressure(Signed + int3(0, 1, 0), CentrePressure);
    let PressureB      = LoadWindowPressure(Signed - int3(0, 1, 0), CentrePressure);
    let PressureU      = LoadWindowPressure(Signed + int3(0, 0, 1), CentrePressure);
    let PressureD      = LoadWindowPressure(Signed - int3(0, 0, 1), CentrePressure);

    let Gradient  = float3(PressureR - PressureL, PressureF - PressureB, PressureU - PressureD) * (0.5 * Plume.CellMetrics.w);
    var Projected = Centre.xyz - Gradient;
    if (Cell.z == 0u && Projected.z < 0.0)
    {
        Projected.z = 0.0;
    }
    VelocityWrite[OutTexel] = float4(Projected, 0.0);
}
