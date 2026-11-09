/*==========================================================================================================================================
                                                           GLOBALSDFSCULPT.JS
==========================================================================================================================================*/
// 🧩 Real-time 3D Volumetric SDF Voxel Sculptor:
//    • 4 3D brushes: Clay (Add), Carve (Subtract), Smooth (3D Laplacian), Flatten (locked tangent plane) + Radius, Strength, Falloff.
//    • Dynamic brick allocation into empty air or solid rock + exact local L∞ Chebyshev cone update so rays never clip new sculpts.
//    • Sub-millisecond incremental dirty-brick GPU uploads (gl.texSubImage3D) + instant brick-delta Undo/Redo + stroke persistence.

import {
    SmoothUnion,
    SmoothSubtraction,
    RoundedBoxDistance2D
} from './GlobalSdfField.js';

import {
    BRICK_VOXELS,
    BRICK_VOLUME,
    INDIRECTION_AIR,
    INDIRECTION_SOLID,
    ConvertFloat32BufferToHalf,
    GlobalSlotToCoords,
    SampleLayerVolumeSdf
} from './GlobalSdfBrickAtlas.js';

const YIELD_BUDGET_MS = 12;

// tiny helper that yields back to the browser compositor without starving the dab queue
function yieldToBrowser()
{
    return new Promise(R => (typeof requestAnimationFrame === 'function' ? requestAnimationFrame(() => R()) : setTimeout(R, 0)));
}

//------------------------------------------------------------------------------------------------------------------------
//                                  DYNAMIC BRICK CAPACITY & ALLOCATION DURING SCULPTING
//------------------------------------------------------------------------------------------------------------------------

export function GrowVolumeBrickArrays(Volume, NewCapacity)
{
    if (NewCapacity <= Volume.BrickCapacity) return;
    const NextCap = Math.max(NewCapacity, Volume.BrickCapacity + 512);

    const NextF32   = new Float32Array(NextCap * BRICK_VOLUME);
    const NextF16   = new Uint16Array(NextCap * BRICK_VOLUME);
    const NextCells = new Uint32Array(NextCap);
    const NextSlots = new Int32Array(NextCap).fill(-1);

    NextF32.set(Volume.BricksF32.subarray(0, Volume.AllocatedCount * BRICK_VOLUME));
    NextF16.set(Volume.BricksF16.subarray(0, Volume.AllocatedCount * BRICK_VOLUME));
    NextCells.set(Volume.BrickCells.subarray(0, Volume.AllocatedCount));
    if (Volume.GlobalSlotByBrick)
    {
        NextSlots.set(Volume.GlobalSlotByBrick.subarray(0, Volume.AllocatedCount));
    }

    Volume.BricksF32         = NextF32;
    Volume.BricksF16         = NextF16;
    Volume.BrickCells        = NextCells;
    Volume.GlobalSlotByBrick = NextSlots;
    Volume.BrickCapacity     = NextCap;
}

export function InitializeFreshBrickFromColumnGrid(Volume, IX, IY, IZ, BaseOffset)
{
    const { BoundsMin, Pitch, Stride2D, GridPlanar, GridSurfY, GridSlope, Depth, Band, IsSolidCore, GridDims } = Volume;
    const [GX, GY] = GridDims;
    const CellIdx  = (IZ * GY + IY) * GX + IX;

    if (!GridPlanar || !GridSurfY || !GridSlope)
    {
        const Fill = IsSolidCore[CellIdx] ? -Band : Band;
        Volume.BricksF32.fill(Fill, BaseOffset, BaseOffset + BRICK_VOLUME);
        return;
    }

    const DY      = Pitch[1];
    const BaseGX2 = IX * BRICK_VOXELS + 1;
    const BaseGZ2 = IZ * BRICK_VOXELS + 1;

    for (let LZ = 0; LZ < 10; LZ++)
    {
        const G2Row = (BaseGZ2 + LZ) * Stride2D + BaseGX2;
        for (let LY = 0; LY < 10; LY++)
        {
            const WY        = BoundsMin[1] + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
            const BottomPhi = -WY - Depth;
            const RowOff    = BaseOffset + (LZ * 10 + LY) * 10;
            for (let LX = 0; LX < 10; LX++)
            {
                const C2      = G2Row + LX;
                const Planar  = GridPlanar[C2];
                const TopPhi  = (WY - GridSurfY[C2]) * GridSlope[C2];
                const VertPhi = TopPhi > BottomPhi ? TopPhi : BottomPhi;
                const OutP    = Planar > 0 ? Planar : 0;
                const OutV    = VertPhi > 0 ? VertPhi : 0;
                const Outside = (OutP > 0 && OutV > 0) ? Math.hypot(OutP, OutV) : (OutP + OutV);
                const MaxPV   = Planar > VertPhi ? Planar : VertPhi;
                const Inside  = MaxPV < 0 ? MaxPV : 0;
                const Phi     = Outside + Inside;
                Volume.BricksF32[RowOff + LX] = Phi < -Band ? -Band : (Phi > Band ? Band : Phi);
            }
        }
    }
}

export function AllocateBrickInVolume(Volume, PackedState, IX, IY, IZ, DirtyIndirectionBox)
{
    const [GX, GY, GZ] = Volume.GridDims;
    const CellIdx = (IZ * GY + IY) * GX + IX;
    if (Volume.BrickIndexByCell[CellIdx] >= 0) return Volume.BrickIndexByCell[CellIdx];

    if (PackedState && PackedState.NextFreeGlobalSlot >= PackedState.MaxGlobalSlots)
    {
        return -1; // Atlas headroom full; caller falls back to quick repack
    }

    const LocalSlot = Volume.AllocatedCount;
    GrowVolumeBrickArrays(Volume, LocalSlot + 1);
    Volume.AllocatedCount++;

    const BaseOffset = LocalSlot * BRICK_VOLUME;
    InitializeFreshBrickFromColumnGrid(Volume, IX, IY, IZ, BaseOffset);
    ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME);

    Volume.BrickIndexByCell[CellIdx] = LocalSlot;
    Volume.BrickCells[LocalSlot]     = CellIdx;
    Volume.IsAllocated[CellIdx]      = 1;
    Volume.IsSolidCore[CellIdx]      = 0;

    const OldCheb = Volume.ChebyshevDist[CellIdx];
    Volume.ChebyshevDist[CellIdx] = 0;

    // Tighten Chebyshev neighbourhood on the Volume itself (needed for ray-march skipping even without atlas)
    const Reach = Math.min(OldCheb + 1, Math.max(GX, GY, GZ));
    const X0 = Math.max(0, IX - Reach), X1 = Math.min(GX - 1, IX + Reach);
    const Y0 = Math.max(0, IY - Reach), Y1 = Math.min(GY - 1, IY + Reach);
    const Z0 = Math.max(0, IZ - Reach), Z1 = Math.min(GZ - 1, IZ + Reach);
    // pre-tighten volume distances (always)
    for (let Z = Z0; Z <= Z1; Z++)
    {
        const DZ = Math.abs(Z - IZ);
        for (let Y = Y0; Y <= Y1; Y++)
        {
            const DZY = Math.max(DZ, Math.abs(Y - IY));
            const SrcRow = (Z * GY + Y) * GX;
            for (let X = X0; X <= X1; X++)
            {
                if (X === IX && Y === IY && Z === IZ) continue;
                const CIdx = SrcRow + X;
                const DInf = Math.max(DZY, Math.abs(X - IX));
                if (DInf < Volume.ChebyshevDist[CIdx]) Volume.ChebyshevDist[CIdx] = DInf;
            }
        }
    }

    if (PackedState)
    {
        const GlobalSlot = PackedState.NextFreeGlobalSlot++;
        PackedState.TotalBricks++;
        Volume.GlobalSlotByBrick[LocalSlot] = GlobalSlot;

        const [MaxGX, SumGY] = PackedState.IndirectionDims;
        const IndBytes = PackedState.IndirectionBytes;
        const YOff     = Volume.IndirectionYOffset;

        for (let Z = Z0; Z <= Z1; Z++)
        {
            const DZ = Math.abs(Z - IZ);
            for (let Y = Y0; Y <= Y1; Y++)
            {
                const DZY    = Math.max(DZ, Math.abs(Y - IY));
                const SrcRow = (Z * GY + Y) * GX;
                const DstRow = ((Z * SumGY + (YOff + Y)) * MaxGX) * 4;
                for (let X = X0; X <= X1; X++)
                {
                    const CIdx = SrcRow + X;
                    const DInf = Math.max(DZY, Math.abs(X - IX));
                    if (DInf === 0)
                    {
                        const [SX, SY, SZ] = GlobalSlotToCoords(GlobalSlot);
                        const Out = DstRow + X * 4;
                        IndBytes[Out]     = SX;
                        IndBytes[Out + 1] = SY;
                        IndBytes[Out + 2] = SZ;
                        IndBytes[Out + 3] = 128;
                    }
                    else if (Volume.BrickIndexByCell[CIdx] < 0)
                    {
                        const Out = DstRow + X * 4;
                        const D = Volume.ChebyshevDist[CIdx];
                        if (Volume.IsSolidCore[CIdx])
                        {
                            IndBytes[Out]     = INDIRECTION_SOLID;
                            IndBytes[Out + 1] = INDIRECTION_SOLID;
                            IndBytes[Out + 2] = INDIRECTION_SOLID;
                            IndBytes[Out + 3] = Math.max(1, 128 - D);
                        }
                        else
                        {
                            IndBytes[Out]     = INDIRECTION_AIR;
                            IndBytes[Out + 1] = INDIRECTION_AIR;
                            IndBytes[Out + 2] = INDIRECTION_AIR;
                            IndBytes[Out + 3] = Math.min(254, 128 + D);
                        }
                    }
                }
            }
        }

        DirtyIndirectionBox[0] = Math.min(DirtyIndirectionBox[0], X0);
        DirtyIndirectionBox[1] = Math.min(DirtyIndirectionBox[1], YOff + Y0);
        DirtyIndirectionBox[2] = Math.min(DirtyIndirectionBox[2], Z0);
        DirtyIndirectionBox[3] = Math.max(DirtyIndirectionBox[3], X1);
        DirtyIndirectionBox[4] = Math.max(DirtyIndirectionBox[4], YOff + Y1);
        DirtyIndirectionBox[5] = Math.max(DirtyIndirectionBox[5], Z1);
    }

    return LocalSlot;
}

//------------------------------------------------------------------------------------------------------------------------
//                                  LIVE 3D CAPSULE DAB ON DIRTY SPARSE BRICKS
//------------------------------------------------------------------------------------------------------------------------

export function ApplySculptSegmentToVolume(Volume, Layer, PackedState, Segment, StrokeSnapshots = null)
{
    if (!Volume) return { DirtyLocalBricks: [], DirtyIndirectionBox: null, NeedsRepack: false };

    const { A, B, Mode, Radius, Strength, Falloff, PlanePoint, PlaneNormal } = Segment;
    const EffRadius  = Math.max(0.5, Math.min(48, Radius)); // clamp extreme radii — keeps dab < ~10k bricks
    const BlendWidth = EffRadius * (0.18 + 0.45 * Falloff);
    const Reach      = EffRadius + BlendWidth + Volume.MaxVoxel * 2.0;

    const SegMinX = Math.min(A[0], B[0]) - Reach, SegMaxX = Math.max(A[0], B[0]) + Reach;
    const SegMinY = Math.min(A[1], B[1]) - Reach, SegMaxY = Math.max(A[1], B[1]) + Reach;
    const SegMinZ = Math.min(A[2], B[2]) - Reach, SegMaxZ = Math.max(A[2], B[2]) + Reach;

    const [BMinX, BMinY, BMinZ] = Volume.BoundsMin;
    const [DX, DY, DZ]          = Volume.Pitch;
    const [GX, GY, GZ]          = Volume.GridDims;

    // Expand brick range by 1 voxel so shared 10×10×10 border texels stay 100% synchronized across neighbours!
    const IX0 = Math.max(0, Math.floor(((SegMinX - BMinX) / DX - 1) / BRICK_VOXELS));
    const IX1 = Math.min(GX - 1, Math.floor(((SegMaxX - BMinX) / DX + 1) / BRICK_VOXELS));
    const IY0 = Math.max(0, Math.floor(((SegMinY - BMinY) / DY - 1) / BRICK_VOXELS));
    const IY1 = Math.min(GY - 1, Math.floor(((SegMaxY - BMinY) / DY + 1) / BRICK_VOXELS));
    const IZ0 = Math.max(0, Math.floor(((SegMinZ - BMinZ) / DZ - 1) / BRICK_VOXELS));
    const IZ1 = Math.min(GZ - 1, Math.floor(((SegMaxZ - BMinZ) / DZ + 1) / BRICK_VOXELS));

    const DirtyIndirectionBox = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
    const DirtyLocalBricks    = [];
    let NeedsRepack           = false;

    const ABX = B[0] - A[0], ABY = B[1] - A[1], ABZ = B[2] - A[2];
    const ABL2 = ABX * ABX + ABY * ABY + ABZ * ABZ;
    const InvABL2 = ABL2 > 1e-10 ? 1.0 / ABL2 : 0.0;
    const CoreRadius = EffRadius * (0.42 + 0.58 * Strength);
    const Band = Volume.Band;

    // For Smooth mode, sample pre-dab neighbourhood cleanly before writing
    const SmoothScratch = Mode === 'smooth' ? new Float32Array(BRICK_VOLUME) : null;
    const PrimaryReg = Layer.Regions?.[0] ? { ...Layer.Regions[0], Edge: 4 } : null;

    for (let IZ = IZ0; IZ <= IZ1; IZ++)
    {
        for (let IY = IY0; IY <= IY1; IY++)
        {
            for (let IX = IX0; IX <= IX1; IX++)
            {
                // Distance from brick centre to capsule AB
                const CX = BMinX + (IX + 0.5) * BRICK_VOXELS * DX;
                const CY = BMinY + (IY + 0.5) * BRICK_VOXELS * DY;
                const CZ = BMinZ + (IZ + 0.5) * BRICK_VOXELS * DZ;
                const TC = InvABL2 > 0 ? Math.max(0, Math.min(1, ((CX - A[0]) * ABX + (CY - A[1]) * ABY + (CZ - A[2]) * ABZ) * InvABL2)) : 0;
                const DistCentre = Math.hypot(CX - A[0] - TC * ABX, CY - A[1] - TC * ABY, CZ - A[2] - TC * ABZ);
                const BrickDiag  = 0.5 * Math.hypot(10 * DX, 10 * DY, 10 * DZ);
                if (DistCentre > Reach + BrickDiag) continue;

                const CellIdx = (IZ * GY + IY) * GX + IX;
                let LocalSlot = Volume.BrickIndexByCell[CellIdx];
                if (LocalSlot < 0)
                {
                    // Smooth & Carve do not need to allocate pure empty air bricks far outside solid rock
                    if (Mode === 'smooth') continue;
                    if ((Mode === 'carve' || Mode === 'remove') && !Volume.IsSolidCore[CellIdx]) continue;
                    LocalSlot = AllocateBrickInVolume(Volume, PackedState, IX, IY, IZ, DirtyIndirectionBox);
                    if (LocalSlot < 0) { NeedsRepack = true; continue; }
                }

                const BaseOffset = LocalSlot * BRICK_VOLUME;

                // Record pre-stroke brick snapshot once per stroke for instant <0.5 ms Undo!
                if (StrokeSnapshots && !StrokeSnapshots.has(LocalSlot))
                {
                    StrokeSnapshots.set(LocalSlot, Volume.BricksF32.slice(BaseOffset, BaseOffset + BRICK_VOLUME));
                }

                if (Mode === 'smooth')
                {
                    SmoothScratch.set(Volume.BricksF32.subarray(BaseOffset, BaseOffset + BRICK_VOLUME));
                    let Modified = false;
                    for (let LZ = 0; LZ < 10; LZ++)
                    {
                        const WZ = BMinZ + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ;
                        for (let LY = 0; LY < 10; LY++)
                        {
                            const WY     = BMinY + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
                            const RowOff = (LZ * 10 + LY) * 10;
                            for (let LX = 0; LX < 10; LX++)
                            {
                                const WX = BMinX + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX;
                                const T  = InvABL2 > 0 ? Math.max(0, Math.min(1, ((WX - A[0]) * ABX + (WY - A[1]) * ABY + (WZ - A[2]) * ABZ) * InvABL2)) : 0;
                                const D  = Math.hypot(WX - A[0] - T * ABX, WY - A[1] - T * ABY, WZ - A[2] - T * ABZ);
                                if (D >= EffRadius) continue;

                                const U = 1.0 - D / EffRadius;
                                const W = 0.55 * Strength * Math.pow(U * U * (3 - 2 * U), 1.15 - 0.65 * Falloff);

                                // 6-neighbour 3D average (interior uses fast local scratch, border taps SampleLayerVolumeSdf seamlessly!)
                                let Avg;
                                if (LX > 0 && LX < 9 && LY > 0 && LY < 9 && LZ > 0 && LZ < 9)
                                {
                                    const C = RowOff + LX;
                                    Avg = (
                                        SmoothScratch[C - 1] + SmoothScratch[C + 1] +
                                        SmoothScratch[C - 10] + SmoothScratch[C + 10] +
                                        SmoothScratch[C - 100] + SmoothScratch[C + 100]
                                    ) / 6.0;
                                }
                                else
                                {
                                    Avg = (
                                        SampleLayerVolumeSdf(Volume, WX - DX, WY, WZ) + SampleLayerVolumeSdf(Volume, WX + DX, WY, WZ) +
                                        SampleLayerVolumeSdf(Volume, WX, WY - DY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY + DY, WZ) +
                                        SampleLayerVolumeSdf(Volume, WX, WY, WZ - DZ) + SampleLayerVolumeSdf(Volume, WX, WY, WZ + DZ)
                                    ) / 6.0;
                                }
                                const Cur  = SmoothScratch[RowOff + LX];
                                const Next = Cur * (1.0 - W) + Avg * W;
                                Volume.BricksF32[BaseOffset + RowOff + LX] = Math.max(-Band, Math.min(Band, Next));
                                Modified = true;
                            }
                        }
                    }
                    if (Modified)
                    {
                        ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME);
                        DirtyLocalBricks.push(LocalSlot);
                    }
                }
                else
                {
                    let Modified = false;
                    for (let LZ = 0; LZ < 10; LZ++)
                    {
                        const WZ = BMinZ + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ;
                        for (let LY = 0; LY < 10; LY++)
                        {
                            const WY     = BMinY + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
                            const RowOff = (LZ * 10 + LY) * 10;
                            for (let LX = 0; LX < 10; LX++)
                            {
                                const WX = BMinX + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX;
                                const T  = InvABL2 > 0 ? Math.max(0, Math.min(1, ((WX - A[0]) * ABX + (WY - A[1]) * ABY + (WZ - A[2]) * ABZ) * InvABL2)) : 0;
                                const D  = Math.hypot(WX - A[0] - T * ABX, WY - A[1] - T * ABY, WZ - A[2] - T * ABZ);
                                if (D > Reach) continue;

                                const Cur  = Volume.BricksF32[BaseOffset + RowOff + LX];
                                let Next   = Cur;

                                if (Mode === 'flatten' && PlaneNormal && PlanePoint)
                                {
                                    if (D < EffRadius)
                                    {
                                        const U = 1.0 - D / EffRadius;
                                        const W = Strength * Math.pow(U * U * (3 - 2 * U), 1.2 - 0.65 * Falloff);
                                        const PlanePhi = (WX - PlanePoint[0]) * PlaneNormal[0] +
                                                         (WY - PlanePoint[1]) * PlaneNormal[1] +
                                                         (WZ - PlanePoint[2]) * PlaneNormal[2];
                                        Next = Cur * (1.0 - W) + PlanePhi * W;
                                    }
                                }
                                else if (Mode === 'carve' || Mode === 'remove')
                                {
                                    const BrushPhi = D - CoreRadius;
                                    Next = SmoothSubtraction(Cur, BrushPhi, BlendWidth);
                                }
                                else
                                {
                                    // Clay (add) — clip softly to owning region horizontal bounds
                                    const BrushPhi = D - CoreRadius;
                                    Next = SmoothUnion(Cur, BrushPhi, BlendWidth);
                                    if (PrimaryReg && Layer.Regions.length === 1)
                                    {
                                        const Clip = RoundedBoxDistance2D(PrimaryReg, WX, WZ) - 2.0;
                                        if (Clip > Next) Next = Clip;
                                    }
                                }

                                const Clamped = Next < -Band ? -Band : (Next > Band ? Band : Next);
                                if (Clamped !== Cur)
                                {
                                    Volume.BricksF32[BaseOffset + RowOff + LX] = Clamped;
                                    Modified = true;
                                }
                            }
                        }
                    }
                    if (Modified)
                    {
                        ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME);
                        DirtyLocalBricks.push(LocalSlot);
                    }
                }
            }
        }
    }

    const HasDirtyInd = DirtyIndirectionBox[0] <= DirtyIndirectionBox[3];
    return {
        DirtyLocalBricks,
        DirtyIndirectionBox: HasDirtyInd ? DirtyIndirectionBox : null,
        NeedsRepack
    };
}

// Worker-side no-alloc variant — assumes missing bricks were pre-allocated on main thread, so skips dynamic brick creation entirely.
// Keeps hot loop 100% off main thread.
export function ApplySculptSegmentToVolumeNoAlloc(Volume, Layer, Segment)
{
    if (!Volume) return { DirtyLocalBricks: [], DirtyIndirectionBox: null, NeedsRepack: false };
    const { A, B, Mode, Radius, Strength, Falloff, PlanePoint, PlaneNormal } = Segment;
    const EffRadius  = Math.max(0.5, Math.min(48, Radius));
    const BlendWidth = EffRadius * (0.18 + 0.45 * Falloff);
    const Reach      = EffRadius + BlendWidth + Volume.MaxVoxel * 2.0;
    const SegMinX = Math.min(A[0], B[0]) - Reach, SegMaxX = Math.max(A[0], B[0]) + Reach;
    const SegMinY = Math.min(A[1], B[1]) - Reach, SegMaxY = Math.max(A[1], B[1]) + Reach;
    const SegMinZ = Math.min(A[2], B[2]) - Reach, SegMaxZ = Math.max(A[2], B[2]) + Reach;
    const [BMinX, BMinY, BMinZ] = Volume.BoundsMin;
    const [DX, DY, DZ]          = Volume.Pitch;
    const [GX, GY, GZ]          = Volume.GridDims;
    const IX0 = Math.max(0, Math.floor(((SegMinX - BMinX) / DX - 1) / BRICK_VOXELS));
    const IX1 = Math.min(GX - 1, Math.floor(((SegMaxX - BMinX) / DX + 1) / BRICK_VOXELS));
    const IY0 = Math.max(0, Math.floor(((SegMinY - BMinY) / DY - 1) / BRICK_VOXELS));
    const IY1 = Math.min(GY - 1, Math.floor(((SegMaxY - BMinY) / DY + 1) / BRICK_VOXELS));
    const IZ0 = Math.max(0, Math.floor(((SegMinZ - BMinZ) / DZ - 1) / BRICK_VOXELS));
    const IZ1 = Math.min(GZ - 1, Math.floor(((SegMaxZ - BMinZ) / DZ + 1) / BRICK_VOXELS));
    const DirtyLocalBricks = [];
    const ABX = B[0] - A[0], ABY = B[1] - A[1], ABZ = B[2] - A[2];
    const ABL2 = ABX * ABX + ABY * ABY + ABZ * ABZ;
    const InvABL2 = ABL2 > 1e-10 ? 1.0 / ABL2 : 0.0;
    const CoreRadius = EffRadius * (0.42 + 0.58 * Strength);
    const Band = Volume.Band;
    const SmoothScratch = Mode === 'smooth' ? new Float32Array(BRICK_VOLUME) : null;
    const PrimaryReg = Layer?.Regions?.[0] ? { ...Layer.Regions[0], Edge: 4 } : null;
    for (let IZ = IZ0; IZ <= IZ1; IZ++) for (let IY = IY0; IY <= IY1; IY++) for (let IX = IX0; IX <= IX1; IX++)
    {
        const CX = BMinX + (IX + 0.5) * BRICK_VOXELS * DX;
        const CY = BMinY + (IY + 0.5) * BRICK_VOXELS * DY;
        const CZ = BMinZ + (IZ + 0.5) * BRICK_VOXELS * DZ;
        const TC = InvABL2 > 0 ? Math.max(0, Math.min(1, ((CX - A[0]) * ABX + (CY - A[1]) * ABY + (CZ - A[2]) * ABZ) * InvABL2)) : 0;
        const DistCentre = Math.hypot(CX - A[0] - TC * ABX, CY - A[1] - TC * ABY, CZ - A[2] - TC * ABZ);
        const BrickDiag  = 0.5 * Math.hypot(10 * DX, 10 * DY, 10 * DZ);
        if (DistCentre > Reach + BrickDiag) continue;
        const CellIdx = (IZ * GY + IY) * GX + IX;
        let LocalSlot = Volume.BrickIndexByCell[CellIdx];
        if (LocalSlot < 0) continue;
        const BaseOffset = LocalSlot * BRICK_VOLUME;
        if (Mode === 'smooth')
        {
            SmoothScratch.set(Volume.BricksF32.subarray(BaseOffset, BaseOffset + BRICK_VOLUME));
            let Modified = false;
            for (let LZ = 0; LZ < 10; LZ++) { const WZ = BMinZ + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ; for (let LY = 0; LY < 10; LY++) { const WY = BMinY + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY; const RowOff = (LZ * 10 + LY) * 10; for (let LX = 0; LX < 10; LX++) { const WX = BMinX + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX; const T = InvABL2 > 0 ? Math.max(0, Math.min(1, ((WX - A[0]) * ABX + (WY - A[1]) * ABY + (WZ - A[2]) * ABZ) * InvABL2)) : 0; const D = Math.hypot(WX - A[0] - T * ABX, WY - A[1] - T * ABY, WZ - A[2] - T * ABZ); if (D >= EffRadius) continue; const U = 1.0 - D / EffRadius; const W = 0.55 * Strength * Math.pow(U * U * (3 - 2 * U), 1.15 - 0.65 * Falloff); let Avg; if (LX > 0 && LX < 9 && LY > 0 && LY < 9 && LZ > 0 && LZ < 9) { const C = RowOff + LX; Avg = (SmoothScratch[C - 1] + SmoothScratch[C + 1] + SmoothScratch[C - 10] + SmoothScratch[C + 10] + SmoothScratch[C - 100] + SmoothScratch[C + 100]) / 6.0; } else { Avg = (SampleLayerVolumeSdf(Volume, WX - DX, WY, WZ) + SampleLayerVolumeSdf(Volume, WX + DX, WY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY - DY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY + DY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY, WZ - DZ) + SampleLayerVolumeSdf(Volume, WX, WY, WZ + DZ)) / 6.0; } const Cur = SmoothScratch[RowOff + LX]; const Next = Cur * (1.0 - W) + Avg * W; Volume.BricksF32[BaseOffset + RowOff + LX] = Math.max(-Band, Math.min(Band, Next)); Modified = true; } } } if (Modified) { ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME); DirtyLocalBricks.push(LocalSlot); }
        }
        else
        {
            let Modified = false;
            for (let LZ = 0; LZ < 10; LZ++) { const WZ = BMinZ + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ; for (let LY = 0; LY < 10; LY++) { const WY = BMinY + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY; const RowOff = (LZ * 10 + LY) * 10; for (let LX = 0; LX < 10; LX++) { const WX = BMinX + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX; const T = InvABL2 > 0 ? Math.max(0, Math.min(1, ((WX - A[0]) * ABX + (WY - A[1]) * ABY + (WZ - A[2]) * ABZ) * InvABL2)) : 0; const D = Math.hypot(WX - A[0] - T * ABX, WY - A[1] - T * ABY, WZ - A[2] - T * ABZ); if (D > Reach) continue; const Cur = Volume.BricksF32[BaseOffset + RowOff + LX]; let Next = Cur; if (Mode === 'flatten' && PlaneNormal && PlanePoint) { if (D < EffRadius) { const U = 1.0 - D / EffRadius; const W = Strength * Math.pow(U * U * (3 - 2 * U), 1.2 - 0.65 * Falloff); const PlanePhi = (WX - PlanePoint[0]) * PlaneNormal[0] + (WY - PlanePoint[1]) * PlaneNormal[1] + (WZ - PlanePoint[2]) * PlaneNormal[2]; Next = Cur * (1.0 - W) + PlanePhi * W; } } else if (Mode === 'carve' || Mode === 'remove') { const BrushPhi = D - CoreRadius; Next = SmoothSubtraction(Cur, BrushPhi, BlendWidth); } else { const BrushPhi = D - CoreRadius; Next = SmoothUnion(Cur, BrushPhi, BlendWidth); if (PrimaryReg && Layer.Regions.length === 1) { const Clip = RoundedBoxDistance2D(PrimaryReg, WX, WZ) - 2.0; if (Clip > Next) Next = Clip; } } const Clamped = Next < -Band ? -Band : (Next > Band ? Band : Next); if (Clamped !== Cur) { Volume.BricksF32[BaseOffset + RowOff + LX] = Clamped; Modified = true; } } } } if (Modified) { ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME); DirtyLocalBricks.push(LocalSlot); }
        }
    }
    return { DirtyLocalBricks, DirtyIndirectionBox: null, NeedsRepack: false };
}

// Async cooperative variant — yields every ~12 ms so pointermove/RAF never freeze.
// Used by the UI thread; the pure sync version above stays for tests/workers.
export async function ApplySculptSegmentToVolumeAsync(Volume, Layer, PackedState, Segment, StrokeSnapshots = null, onBatch = null)
{
    if (!Volume) return { DirtyLocalBricks: [], DirtyIndirectionBox: null, NeedsRepack: false };

    const { A, B, Mode, Radius, Strength, Falloff, PlanePoint, PlaneNormal } = Segment;
    const EffRadius  = Math.max(0.5, Math.min(48, Radius));
    const BlendWidth = EffRadius * (0.18 + 0.45 * Falloff);
    const Reach      = EffRadius + BlendWidth + Volume.MaxVoxel * 2.0;

    const SegMinX = Math.min(A[0], B[0]) - Reach, SegMaxX = Math.max(A[0], B[0]) + Reach;
    const SegMinY = Math.min(A[1], B[1]) - Reach, SegMaxY = Math.max(A[1], B[1]) + Reach;
    const SegMinZ = Math.min(A[2], B[2]) - Reach, SegMaxZ = Math.max(A[2], B[2]) + Reach;

    const [BMinX, BMinY, BMinZ] = Volume.BoundsMin;
    const [DX, DY, DZ]          = Volume.Pitch;
    const [GX, GY, GZ]          = Volume.GridDims;

    const IX0 = Math.max(0, Math.floor(((SegMinX - BMinX) / DX - 1) / BRICK_VOXELS));
    const IX1 = Math.min(GX - 1, Math.floor(((SegMaxX - BMinX) / DX + 1) / BRICK_VOXELS));
    const IY0 = Math.max(0, Math.floor(((SegMinY - BMinY) / DY - 1) / BRICK_VOXELS));
    const IY1 = Math.min(GY - 1, Math.floor(((SegMaxY - BMinY) / DY + 1) / BRICK_VOXELS));
    const IZ0 = Math.max(0, Math.floor(((SegMinZ - BMinZ) / DZ - 1) / BRICK_VOXELS));
    const IZ1 = Math.min(GZ - 1, Math.floor(((SegMaxZ - BMinZ) / DZ + 1) / BRICK_VOXELS));

    const DirtyIndirectionBox = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
    const DirtyLocalBricks    = [];
    let NeedsRepack           = false;

    const ABX = B[0] - A[0], ABY = B[1] - A[1], ABZ = B[2] - A[2];
    const ABL2 = ABX * ABX + ABY * ABY + ABZ * ABZ;
    const InvABL2 = ABL2 > 1e-10 ? 1.0 / ABL2 : 0.0;
    const CoreRadius = EffRadius * (0.42 + 0.58 * Strength);
    const Band = Volume.Band;

    const SmoothScratch = Mode === 'smooth' ? new Float32Array(BRICK_VOLUME) : null;
    const PrimaryReg = Layer.Regions?.[0] ? { ...Layer.Regions[0], Edge: 4 } : null;

    let batchBricks = 0;
    let budgetStart = performance.now();

    const maybeYield = async () =>
    {
        // push incremental GPU upload so the user sees partial result within ~1 frame
        if (onBatch && DirtyLocalBricks.length && batchBricks >= 16)
        {
            const batch = DirtyLocalBricks.splice(0, DirtyLocalBricks.length);
            const box = DirtyIndirectionBox[0] <= DirtyIndirectionBox[3] ? [...DirtyIndirectionBox] : null;
            DirtyIndirectionBox[0] = Infinity; DirtyIndirectionBox[1] = Infinity; DirtyIndirectionBox[2] = Infinity;
            DirtyIndirectionBox[3] = -Infinity; DirtyIndirectionBox[4] = -Infinity; DirtyIndirectionBox[5] = -Infinity;
            await onBatch(batch, box);
            batchBricks = 0;
        }
        if (performance.now() - budgetStart > YIELD_BUDGET_MS)
        {
            await yieldToBrowser();
            budgetStart = performance.now();
        }
    };

    for (let IZ = IZ0; IZ <= IZ1; IZ++)
    {
        for (let IY = IY0; IY <= IY1; IY++)
        {
            for (let IX = IX0; IX <= IX1; IX++)
            {
                const CX = BMinX + (IX + 0.5) * BRICK_VOXELS * DX;
                const CY = BMinY + (IY + 0.5) * BRICK_VOXELS * DY;
                const CZ = BMinZ + (IZ + 0.5) * BRICK_VOXELS * DZ;
                const TC = InvABL2 > 0 ? Math.max(0, Math.min(1, ((CX - A[0]) * ABX + (CY - A[1]) * ABY + (CZ - A[2]) * ABZ) * InvABL2)) : 0;
                const DistCentre = Math.hypot(CX - A[0] - TC * ABX, CY - A[1] - TC * ABY, CZ - A[2] - TC * ABZ);
                const BrickDiag  = 0.5 * Math.hypot(10 * DX, 10 * DY, 10 * DZ);
                if (DistCentre > Reach + BrickDiag) continue;

                const CellIdx = (IZ * GY + IY) * GX + IX;
                let LocalSlot = Volume.BrickIndexByCell[CellIdx];
                if (LocalSlot < 0)
                {
                    if (Mode === 'smooth') continue;
                    if ((Mode === 'carve' || Mode === 'remove') && !Volume.IsSolidCore[CellIdx]) continue;
                    LocalSlot = AllocateBrickInVolume(Volume, PackedState, IX, IY, IZ, DirtyIndirectionBox);
                    if (LocalSlot < 0) { NeedsRepack = true; continue; }
                }

                const BaseOffset = LocalSlot * BRICK_VOLUME;
                if (StrokeSnapshots && !StrokeSnapshots.has(LocalSlot))
                {
                    StrokeSnapshots.set(LocalSlot, Volume.BricksF32.slice(BaseOffset, BaseOffset + BRICK_VOLUME));
                }

                if (Mode === 'smooth')
                {
                    SmoothScratch.set(Volume.BricksF32.subarray(BaseOffset, BaseOffset + BRICK_VOLUME));
                    let Modified = false;
                    for (let LZ = 0; LZ < 10; LZ++)
                    {
                        const WZ = BMinZ + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ;
                        for (let LY = 0; LY < 10; LY++)
                        {
                            const WY     = BMinY + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
                            const RowOff = (LZ * 10 + LY) * 10;
                            for (let LX = 0; LX < 10; LX++)
                            {
                                const WX = BMinX + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX;
                                const T  = InvABL2 > 0 ? Math.max(0, Math.min(1, ((WX - A[0]) * ABX + (WY - A[1]) * ABY + (WZ - A[2]) * ABZ) * InvABL2)) : 0;
                                const D  = Math.hypot(WX - A[0] - T * ABX, WY - A[1] - T * ABY, WZ - A[2] - T * ABZ);
                                if (D >= EffRadius) continue;
                                const U = 1.0 - D / EffRadius;
                                const W = 0.55 * Strength * Math.pow(U * U * (3 - 2 * U), 1.15 - 0.65 * Falloff);
                                let Avg;
                                if (LX > 0 && LX < 9 && LY > 0 && LY < 9 && LZ > 0 && LZ < 9)
                                {
                                    const C = RowOff + LX;
                                    Avg = (SmoothScratch[C - 1] + SmoothScratch[C + 1] + SmoothScratch[C - 10] + SmoothScratch[C + 10] + SmoothScratch[C - 100] + SmoothScratch[C + 100]) / 6.0;
                                }
                                else
                                {
                                    Avg = (SampleLayerVolumeSdf(Volume, WX - DX, WY, WZ) + SampleLayerVolumeSdf(Volume, WX + DX, WY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY - DY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY + DY, WZ) + SampleLayerVolumeSdf(Volume, WX, WY, WZ - DZ) + SampleLayerVolumeSdf(Volume, WX, WY, WZ + DZ)) / 6.0;
                                }
                                const Cur  = SmoothScratch[RowOff + LX];
                                const Next = Cur * (1.0 - W) + Avg * W;
                                Volume.BricksF32[BaseOffset + RowOff + LX] = Math.max(-Band, Math.min(Band, Next));
                                Modified = true;
                            }
                        }
                    }
                    if (Modified)
                    {
                        ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME);
                        DirtyLocalBricks.push(LocalSlot); batchBricks++;
                    }
                }
                else
                {
                    let Modified = false;
                    for (let LZ = 0; LZ < 10; LZ++)
                    {
                        const WZ = BMinZ + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ;
                        for (let LY = 0; LY < 10; LY++)
                        {
                            const WY     = BMinY + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
                            const RowOff = (LZ * 10 + LY) * 10;
                            for (let LX = 0; LX < 10; LX++)
                            {
                                const WX = BMinX + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX;
                                const T  = InvABL2 > 0 ? Math.max(0, Math.min(1, ((WX - A[0]) * ABX + (WY - A[1]) * ABY + (WZ - A[2]) * ABZ) * InvABL2)) : 0;
                                const D  = Math.hypot(WX - A[0] - T * ABX, WY - A[1] - T * ABY, WZ - A[2] - T * ABZ);
                                if (D > Reach) continue;
                                const Cur  = Volume.BricksF32[BaseOffset + RowOff + LX];
                                let Next   = Cur;
                                if (Mode === 'flatten' && PlaneNormal && PlanePoint)
                                {
                                    if (D < EffRadius)
                                    {
                                        const U = 1.0 - D / EffRadius;
                                        const W = Strength * Math.pow(U * U * (3 - 2 * U), 1.2 - 0.65 * Falloff);
                                        const PlanePhi = (WX - PlanePoint[0]) * PlaneNormal[0] + (WY - PlanePoint[1]) * PlaneNormal[1] + (WZ - PlanePoint[2]) * PlaneNormal[2];
                                        Next = Cur * (1.0 - W) + PlanePhi * W;
                                    }
                                }
                                else if (Mode === 'carve' || Mode === 'remove')
                                {
                                    const BrushPhi = D - CoreRadius;
                                    Next = SmoothSubtraction(Cur, BrushPhi, BlendWidth);
                                }
                                else
                                {
                                    const BrushPhi = D - CoreRadius;
                                    Next = SmoothUnion(Cur, BrushPhi, BlendWidth);
                                    if (PrimaryReg && Layer.Regions.length === 1)
                                    {
                                        const Clip = RoundedBoxDistance2D(PrimaryReg, WX, WZ) - 2.0;
                                        if (Clip > Next) Next = Clip;
                                    }
                                }
                                const Clamped = Next < -Band ? -Band : (Next > Band ? Band : Next);
                                if (Clamped !== Cur)
                                {
                                    Volume.BricksF32[BaseOffset + RowOff + LX] = Clamped;
                                    Modified = true;
                                }
                            }
                        }
                    }
                    if (Modified)
                    {
                        ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, BaseOffset, BRICK_VOLUME);
                        DirtyLocalBricks.push(LocalSlot); batchBricks++;
                    }
                }

                if (batchBricks >= 16 || performance.now() - budgetStart > YIELD_BUDGET_MS) await maybeYield();
            }
            await maybeYield();
        }
        await maybeYield();
    }

    const HasDirtyInd = DirtyIndirectionBox[0] <= DirtyIndirectionBox[3];
    return {
        DirtyLocalBricks,
        DirtyIndirectionBox: HasDirtyInd ? DirtyIndirectionBox : null,
        NeedsRepack
    };
}

export function CaptureBrickSnapshotsAfterStroke(Volume, PreStrokeMap)
{
    const Entries = [];
    for (const [LocalSlot, BeforeF32] of PreStrokeMap.entries())
    {
        const Base = LocalSlot * BRICK_VOLUME;
        const AfterF32 = Volume.BricksF32.slice(Base, Base + BRICK_VOLUME);
        Entries.push({ LocalSlot, BeforeF32, AfterF32 });
    }
    return Entries;
}

export function ApplyBrickSnapshotDelta(Volume, Entries, UseAfter = false)
{
    const DirtySlots = [];
    for (const Entry of Entries)
    {
        const Src  = UseAfter ? Entry.AfterF32 : Entry.BeforeF32;
        const Base = Entry.LocalSlot * BRICK_VOLUME;
        if (Base + BRICK_VOLUME > Volume.BricksF32.length) continue;
        Volume.BricksF32.set(Src, Base);
        ConvertFloat32BufferToHalf(Volume.BricksF32, Volume.BricksF16, Base, BRICK_VOLUME);
        DirtySlots.push(Entry.LocalSlot);
    }
    return DirtySlots;
}
