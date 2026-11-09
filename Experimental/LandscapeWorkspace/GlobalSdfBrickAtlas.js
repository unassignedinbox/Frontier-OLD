/*==========================================================================================================================================
                                                         GLOBALSDFBRICKATLAS.JS
==========================================================================================================================================*/
// 🧩 Full-resolution Sparse-Brick 3D Global SDF Engine (512×512×128, 1024×1024×256, and up — ZERO LOD).
//    • 8×8×8 voxel bricks stored as 10×10×10 texel blocks (1-voxel border on all 6 faces) for seamless hardware GL_LINEAR trilinear SDF.
//    • Single-pass 2D XZ column precomputation + exact narrow-band classification + ryg table-driven bulk float16 conversion.
//    • Exact 26-neighbour 2-pass L∞ Chebyshev empty-space distance transform for zero-overshoot AABB empty-brick skipping.

import {
    ResolveLayerResolution,
    CompileRegion,
    LayerVerticalBounds,
    EvaluateColumnSurfaceHeight,
    VolumetricPrimitiveDistance,
    CombineDistance,
    RoundedBoxDistance2D,
    ApplySculptStrokePoint
} from './GlobalSdfField.js';

export const BRICK_VOXELS   = 8;
export const BRICK_TEXELS   = 10; // 8 interior + 1 border texel on each side
export const BRICK_VOLUME   = 1000;
export const ATLAS_BRICKS_X = 48; // 480 texels (< 512 universal WebGL2 3D texture floor)
export const ATLAS_BRICKS_Y = 48; // 480 texels

export const INDIRECTION_AIR   = 255;
export const INDIRECTION_SOLID = 254;

//------------------------------------------------------------------------------------------------------------------------
//                           RYG TABLE-DRIVEN BULK IEEE-754 FLOAT32 -> FLOAT16 CONVERTER
//------------------------------------------------------------------------------------------------------------------------

const F16BaseTable  = new Uint16Array(512);
const F16ShiftTable = new Uint8Array(512);
(() =>
{
    for (let I = 0; I < 256; I++)
    {
        const E = I - 127;
        if (E < -24)
        {
            F16BaseTable[I]         = 0x0000;
            F16BaseTable[I | 0x100] = 0x8000;
            F16ShiftTable[I]         = 24;
            F16ShiftTable[I | 0x100] = 24;
        }
        else if (E < -14)
        {
            F16BaseTable[I]         = (0x0400 >> (-E - 14));
            F16BaseTable[I | 0x100] = (0x0400 >> (-E - 14)) | 0x8000;
            F16ShiftTable[I]         = -E - 1;
            F16ShiftTable[I | 0x100] = -E - 1;
        }
        else if (E <= 15)
        {
            F16BaseTable[I]         = ((E + 15) << 10);
            F16BaseTable[I | 0x100] = ((E + 15) << 10) | 0x8000;
            F16ShiftTable[I]         = 13;
            F16ShiftTable[I | 0x100] = 13;
        }
        else if (E < 128)
        {
            F16BaseTable[I]         = 0x7c00;
            F16BaseTable[I | 0x100] = 0xfc00;
            F16ShiftTable[I]         = 24;
            F16ShiftTable[I | 0x100] = 24;
        }
        else
        {
            F16BaseTable[I]         = 0x7c00;
            F16BaseTable[I | 0x100] = 0xfc00;
            F16ShiftTable[I]         = 13;
            F16ShiftTable[I | 0x100] = 13;
        }
    }
})();

const SingleF32 = new Float32Array(1);
const SingleU32 = new Uint32Array(SingleF32.buffer);

export function Float32ToHalf(Value)
{
    SingleF32[0] = Value;
    const F = SingleU32[0];
    const Idx = (F >>> 23) & 0x1ff;
    return F16BaseTable[Idx] + ((F & 0x007fffff) >>> F16ShiftTable[Idx]);
}

export function ConvertFloat32BufferToHalf(SrcF32, DstU16, Offset = 0, Count = SrcF32.length)
{
    const SrcU32 = new Uint32Array(SrcF32.buffer, SrcF32.byteOffset + Offset * 4, Count);
    for (let I = 0; I < Count; I++)
    {
        const F   = SrcU32[I];
        const Idx = (F >>> 23) & 0x1ff;
        DstU16[Offset + I] = F16BaseTable[Idx] + ((F & 0x007fffff) >>> F16ShiftTable[Idx]);
    }
}

export function HalfToFloat32(Half)
{
    const Sign = (Half & 0x8000) ? -1 : 1;
    const Exp  = (Half >>> 10) & 0x1f;
    const Frac = Half & 0x03ff;
    if (Exp === 0) return Sign * Math.pow(2, -14) * (Frac / 1024);
    if (Exp === 31) return Frac ? NaN : Sign * Infinity;
    return Sign * Math.pow(2, Exp - 15) * (1 + Frac / 1024);
}

//------------------------------------------------------------------------------------------------------------------------
//                                    EXACT 26-NEIGHBOUR 2-PASS L∞ DISTANCE TRANSFORM
//------------------------------------------------------------------------------------------------------------------------

export function ComputeChebyshevDistance3D(GridX, GridY, GridZ, IsAllocatedMask)
{
    const Total = GridX * GridY * GridZ;
    const Dist  = new Uint8Array(Total);
    const MaxD  = 126;

    for (let I = 0; I < Total; I++) Dist[I] = IsAllocatedMask[I] ? 0 : MaxD;

    const StrideY = GridX;
    const StrideZ = GridX * GridY;

    // Pass 1: Forward scan (+Z, +Y, +X)
    for (let Z = 0; Z < GridZ; Z++)
    {
        const ZMin = Z > 0 ? -1 : 0;
        for (let Y = 0; Y < GridY; Y++)
        {
            const YMin = Y > 0 ? -1 : 0;
            const RowBase = Z * StrideZ + Y * StrideY;
            for (let X = 0; X < GridX; X++)
            {
                const Idx = RowBase + X;
                let Best = Dist[Idx];
                if (Best === 0) continue;
                const XMin = X > 0 ? -1 : 0;
                for (let DZ = ZMin; DZ <= 0; DZ++)
                {
                    const DYMax = DZ === 0 ? 0 : 1;
                    for (let DY = YMin; DY <= DYMax; DY++)
                    {
                        if (Y + DY >= GridY) continue;
                        const DXMax = (DZ === 0 && DY === 0) ? -1 : 1;
                        const NBase = Idx + DZ * StrideZ + DY * StrideY;
                        for (let DX = XMin; DX <= DXMax; DX++)
                        {
                            if (X + DX >= GridX) continue;
                            const N = Dist[NBase + DX] + 1;
                            if (N < Best) Best = N;
                        }
                    }
                }
                Dist[Idx] = Best;
            }
        }
    }

    // Pass 2: Backward scan (-Z, -Y, -X)
    for (let Z = GridZ - 1; Z >= 0; Z--)
    {
        const ZMax = Z + 1 < GridZ ? 1 : 0;
        for (let Y = GridY - 1; Y >= 0; Y--)
        {
            const YMax = Y + 1 < GridY ? 1 : 0;
            const RowBase = Z * StrideZ + Y * StrideY;
            for (let X = GridX - 1; X >= 0; X--)
            {
                const Idx = RowBase + X;
                let Best = Dist[Idx];
                if (Best === 0) continue;
                const XMax = X + 1 < GridX ? 1 : 0;
                for (let DZ = 0; DZ <= ZMax; DZ++)
                {
                    const DYMin = DZ === 0 ? 0 : -1;
                    for (let DY = DYMin; DY <= YMax; DY++)
                    {
                        if (Y + DY < 0) continue;
                        const DXMin = (DZ === 0 && DY === 0) ? 1 : -1;
                        const NBase = Idx + DZ * StrideZ + DY * StrideY;
                        for (let DX = DXMin; DX <= XMax; DX++)
                        {
                            if (X + DX < 0) continue;
                            const N = Dist[NBase + DX] + 1;
                            if (N < Best) Best = N;
                        }
                    }
                }
                Dist[Idx] = Best;
            }
        }
    }

    return Dist;
}

//------------------------------------------------------------------------------------------------------------------------
//                                  FAST COMPILED MASK CAPSULE SEGMENTS (AABB REJECT)
//------------------------------------------------------------------------------------------------------------------------

function CompileMaskSegments(Layer)
{
    const Groups = [];
    for (const Stroke of Layer.Strokes || [])
    {
        const Pts = Stroke.Points;
        if (!Pts || !Pts.length) continue;
        const R = Stroke.Radius;
        let MinX = Infinity, MinZ = Infinity, MaxX = -Infinity, MaxZ = -Infinity;
        const Segs = [];
        for (let I = 0; I < Pts.length; I++)
        {
            const A = Pts[I > 0 ? I - 1 : 0];
            const B = Pts[I];
            const DX = B[0] - A[0], DZ = B[1] - A[1];
            const InvL2 = (DX * DX + DZ * DZ) > 1e-12 ? 1.0 / (DX * DX + DZ * DZ) : 0.0;
            Segs.push(A[0], A[1], DX, DZ, InvL2);
            MinX = Math.min(MinX, A[0] - R, B[0] - R);
            MaxX = Math.max(MaxX, A[0] + R, B[0] + R);
            MinZ = Math.min(MinZ, A[1] - R, B[1] - R);
            MaxZ = Math.max(MaxZ, A[1] + R, B[1] + R);
        }
        Groups.push({
            Erase: Boolean(Stroke.Erase),
            Radius: R,
            CX: 0.5 * (MinX + MaxX),
            CZ: 0.5 * (MinZ + MaxZ),
            HX: 0.5 * (MaxX - MinX),
            HZ: 0.5 * (MaxZ - MinZ),
            Segs: new Float64Array(Segs)
        });
    }
    return Groups;
}

function FastMaskDistance(Groups, X, Z)
{
    let Distance = 1e6;
    for (let G = 0; G < Groups.length; G++)
    {
        const Grp = Groups[G];
        // Fast AABB lower-bound cull: skip entire stroke if lower bound cannot improve Distance!
        const QX = Math.abs(X - Grp.CX) - Grp.HX;
        const QZ = Math.abs(Z - Grp.CZ) - Grp.HZ;
        if (!Grp.Erase && (QX > Distance || QZ > Distance)) continue;
        if (Grp.Erase && (QX > 0 || QZ > 0)) continue;

        const Segs = Grp.Segs;
        let Best2  = 1e18;
        for (let S = 0; S < Segs.length; S += 5)
        {
            const AX = Segs[S], AZ = Segs[S + 1];
            const DX = Segs[S + 2], DZ = Segs[S + 3], InvL2 = Segs[S + 4];
            const T  = InvL2 > 0 ? Math.max(0, Math.min(1, ((X - AX) * DX + (Z - AZ) * DZ) * InvL2)) : 0;
            const EX = X - AX - T * DX;
            const EZ = Z - AZ - T * DZ;
            const D2 = EX * EX + EZ * EZ;
            if (D2 < Best2) Best2 = D2;
        }
        const StrokeDist = Math.sqrt(Best2) - Grp.Radius;
        Distance = Grp.Erase ? Math.max(Distance, -StrokeDist) : Math.min(Distance, StrokeDist);
    }
    return Distance;
}

//------------------------------------------------------------------------------------------------------------------------
//                                      PER-LAYER SPARSE-BRICK 3D SDF VOLUME BUILDER
//------------------------------------------------------------------------------------------------------------------------

function ComputeLayerWorldBounds(Layer, Resolution)
{
    let RawMinX =  Infinity, RawMaxX = -Infinity;
    let RawMinY =  Infinity, RawMaxY = -Infinity;
    let RawMinZ =  Infinity, RawMaxZ = -Infinity;

    for (const Region of Layer.Regions)
    {
        const [MinY, MaxY] = LayerVerticalBounds(Layer, Region);
        RawMinX = Math.min(RawMinX, Region.X0);
        RawMaxX = Math.max(RawMaxX, Region.X1);
        RawMinY = Math.min(RawMinY, MinY);
        RawMaxY = Math.max(RawMaxY, MaxY + 64.0);
        RawMinZ = Math.min(RawMinZ, Region.Z0);
        RawMaxZ = Math.max(RawMaxZ, Region.Z1);
    }

    const MarginVoxels = 6;
    const SpanX = Math.max(16, RawMaxX - RawMinX);
    const SpanY = Math.max(16, RawMaxY - RawMinY);
    const SpanZ = Math.max(16, RawMaxZ - RawMinZ);

    const PadX = SpanX * MarginVoxels / Math.max(16, Resolution[0] - 2 * MarginVoxels);
    const PadY = SpanY * MarginVoxels / Math.max(16, Resolution[1] - 2 * MarginVoxels);
    const PadZ = SpanZ * MarginVoxels / Math.max(16, Resolution[2] - 2 * MarginVoxels);

    const BoundsMin = [RawMinX - PadX, RawMinY - PadY, RawMinZ - PadZ];
    const BoundsMax = [RawMaxX + PadX, RawMaxY + PadY, RawMaxZ + PadZ];
    const Pitch = [
        (BoundsMax[0] - BoundsMin[0]) / Resolution[0],
        (BoundsMax[1] - BoundsMin[1]) / Resolution[1],
        (BoundsMax[2] - BoundsMin[2]) / Resolution[2]
    ];
    return { BoundsMin, BoundsMax, Pitch };
}

function BoxOverlapsFeature(BMinX, BMinY, BMinZ, BMaxX, BMaxY, BMaxZ, FeatureAABB, Margin)
{
    return (
        BMaxX >= FeatureAABB[0] - Margin && BMinX <= FeatureAABB[3] + Margin &&
        BMaxY >= FeatureAABB[1] - Margin && BMinY <= FeatureAABB[4] + Margin &&
        BMaxZ >= FeatureAABB[2] - Margin && BMinZ <= FeatureAABB[5] + Margin
    );
}

export function BuildLayerSparseSdf(Layer, LayerIndex = 0)
{
    if (!Layer.Visible || !Array.isArray(Layer.Regions) || !Layer.Regions.length)
    {
        return null;
    }

    const StartMs    = performance.now();
    const Resolution = ResolveLayerResolution(Layer);
    const [NX, NY, NZ] = Resolution;
    const GX = NX / BRICK_VOXELS;
    const GY = NY / BRICK_VOXELS;
    const GZ = NZ / BRICK_VOXELS;

    const { BoundsMin, BoundsMax, Pitch } = ComputeLayerWorldBounds(Layer, Resolution);
    const [DX, DY, DZ] = Pitch;
    const BrickWorld   = [DX * BRICK_VOXELS, DY * BRICK_VOXELS, DZ * BRICK_VOXELS];
    const MaxVoxel     = Math.max(DX, DY, DZ);
    const MinVoxel     = Math.min(DX, DY, DZ);
    // Tight 3-voxel narrow-band (covers full trilinear stencil + finite-difference normals + AO)
    const Band         = 3.0 * MaxVoxel;

    const MaskGroups = CompileMaskSegments(Layer);
    const CompiledRegions = Layer.Regions.map(Region => ({
        Region,
        Compiled: CompileRegion(Layer, Region)
    }));

    // 1. Precompute full-resolution 2D XZ Column Grid ONCE ((NX + 4) × (NZ + 4) for border + central differences)
    //    Voxel vx in [-2 .. NX + 1] maps to Grid2D index gx = vx + 2 in [0 .. NX + 3]
    const Stride2D    = NX + 4;
    const Count2D     = Stride2D * (NZ + 4);
    const GridPlanar  = new Float32Array(Count2D);
    const GridSurfY   = new Float32Array(Count2D);
    const GridSlope   = new Float32Array(Count2D);

    const SingleReg = CompiledRegions.length === 1 ? CompiledRegions[0] : null;
    if (SingleReg)
    {
        const { Region, Compiled } = SingleReg;
        const SpanX = Region.X1 - Region.X0, SpanZ = Region.Z1 - Region.Z0;
        const CRad  = Math.min(Region.Edge || 0, SpanX * 0.48, SpanZ * 0.48);
        const HalfX = Math.max(1e-3, SpanX * 0.5 - CRad);
        const HalfZ = Math.max(1e-3, SpanZ * 0.5 - CRad);
        const MidX  = (Region.X0 + Region.X1) * 0.5;
        const MidZ  = (Region.Z0 + Region.Z1) * 0.5;
        const HasHeightSteps = Compiled.HeightSteps.length > 0;

        for (let GZ2 = 0; GZ2 < NZ + 4; GZ2++)
        {
            const WZ  = BoundsMin[2] + (GZ2 - 2 + 0.5) * DZ;
            const QZ  = Math.abs(WZ - MidZ) - HalfZ;
            const Row = GZ2 * Stride2D;
            for (let GX2 = 0; GX2 < Stride2D; GX2++)
            {
                const WX  = BoundsMin[0] + (GX2 - 2 + 0.5) * DX;
                const QX  = Math.abs(WX - MidX) - HalfX;
                const Box = ((QX > 0 && QZ > 0) ? Math.hypot(QX, QZ) : Math.max(QX, 0) + Math.max(QZ, 0)) + Math.min(Math.max(QX, QZ), 0) - CRad;
                const M   = Box > Band + 4.0 ? Box : FastMaskDistance(MaskGroups, WX, WZ);
                const P   = M > Box ? M : Box;
                GridPlanar[Row + GX2] = P;
                GridSurfY[Row + GX2]  = (HasHeightSteps && P <= 0.5) ? EvaluateColumnSurfaceHeight(Compiled, P, WX, WZ) : 0.0;
            }
        }
    }
    else
    {
        for (let GZ2 = 0; GZ2 < NZ + 4; GZ2++)
        {
            const WZ  = BoundsMin[2] + (GZ2 - 2 + 0.5) * DZ;
            const Row = GZ2 * Stride2D;
            for (let GX2 = 0; GX2 < Stride2D; GX2++)
            {
                const WX = BoundsMin[0] + (GX2 - 2 + 0.5) * DX;
                const M  = FastMaskDistance(MaskGroups, WX, WZ);
                let BestP = 1e6, BestH = 0;
                for (let R = 0; R < CompiledRegions.length; R++)
                {
                    const { Region, Compiled } = CompiledRegions[R];
                    const P = Math.max(M, RoundedBoxDistance2D(Region, WX, WZ));
                    if (P < BestP)
                    {
                        BestP = P;
                        BestH = P <= 0.5 ? EvaluateColumnSurfaceHeight(Compiled, P, WX, WZ) : 0.0;
                    }
                }
                GridPlanar[Row + GX2] = BestP;
                GridSurfY[Row + GX2]  = BestH;
            }
        }
    }

    // Central-difference 1 / sqrt(1 + Hx^2 + Hz^2) across interior + 1-texel border
    const Inv2DX = 0.5 / DX;
    const Inv2DZ = 0.5 / DZ;
    for (let GZ2 = 1; GZ2 < NZ + 3; GZ2++)
    {
        const Row = GZ2 * Stride2D;
        for (let GX2 = 1; GX2 < Stride2D - 1; GX2++)
        {
            const C = Row + GX2;
            if (GridPlanar[C] > Band + 2.0)
            {
                GridSlope[C] = 1.0;
                continue;
            }
            const HX = (GridSurfY[C + 1] - GridSurfY[C - 1]) * Inv2DX;
            const HZ = (GridSurfY[C + Stride2D] - GridSurfY[C - Stride2D]) * Inv2DZ;
            GridSlope[C] = 1.0 / Math.sqrt(1.0 + HX * HX + HZ * HZ);
        }
    }

    const BrickRadius = 0.5 * Math.hypot(BrickWorld[0], BrickWorld[1], BrickWorld[2]);

    // Collect 3D volumetric steps & sculpt strokes with tight brick-centre culling
    const AllVolSteps   = [];
    const AllSculptList = [];
    for (const { Region, Compiled } of CompiledRegions)
    {
        for (const Step of Compiled.VolumeSteps)
        {
            AllVolSteps.push({ Region, Mode: Step.Mode, Blend: Step.Blend || 0, Primitives: Step.Primitives });
        }
        for (const Stroke of Region.SculptStrokes || [])
        {
            const R = (Stroke.Radius || 8) * 1.9 + Band + BrickRadius;
            let SX0 = Infinity, SY0 = Infinity, SZ0 = Infinity;
            let SX1 = -Infinity, SY1 = -Infinity, SZ1 = -Infinity;
            for (const Pt of Stroke.Points || [])
            {
                SX0 = Math.min(SX0, Pt[0] - R); SY0 = Math.min(SY0, Pt[1] - R); SZ0 = Math.min(SZ0, Pt[2] - R);
                SX1 = Math.max(SX1, Pt[0] + R); SY1 = Math.max(SY1, Pt[1] + R); SZ1 = Math.max(SZ1, Pt[2] + R);
            }
            if (SX0 <= SX1) AllSculptList.push({ Stroke, AABB: [SX0, SY0, SZ0, SX1, SY1, SZ1] });
        }
    }

    // 2. Exact Narrow-Band Brick Classification using actual 10×10 column extrema!
    const TotalBricks      = GX * GY * GZ;
    const IsAllocated      = new Uint8Array(TotalBricks);
    const IsSolidCore      = new Uint8Array(TotalBricks);
    const BrickIndexByCell = new Int32Array(TotalBricks).fill(-1);
    const ActiveColYLists  = new Array(GX * GZ);
    let AllocatedCount     = 0;

    for (let IZ = 0; IZ < GZ; IZ++)
    {
        const BZ0 = BoundsMin[2] + IZ * BrickWorld[2];
        const BZ1 = BZ0 + BrickWorld[2];

        for (let IX = 0; IX < GX; IX++)
        {
            const BX0 = BoundsMin[0] + IX * BrickWorld[0];
            const BX1 = BX0 + BrickWorld[0];

            // Exact min/max Planar and SurfaceY across all 10×10 texels of this brick column!
            let MinP =  1e6, MaxP = -1e6;
            let MinH =  1e6, MaxH = -1e6;
            const BaseGX2 = IX * BRICK_VOXELS + 1;
            const BaseGZ2 = IZ * BRICK_VOXELS + 1;
            for (let LZ = 0; LZ < 10; LZ++)
            {
                const ROff = (BaseGZ2 + LZ) * Stride2D + BaseGX2;
                for (let LX = 0; LX < 10; LX++)
                {
                    const P = GridPlanar[ROff + LX];
                    const H = GridSurfY[ROff + LX];
                    if (P < MinP) MinP = P;
                    if (P > MaxP) MaxP = P;
                    if (P <= Band)
                    {
                        if (H < MinH) MinH = H;
                        if (H > MaxH) MaxH = H;
                    }
                }
            }

            // Filter volumetric primitives & sculpt strokes whose XZ AABB overlaps this brick column
            let ColVolSteps = null;
            for (let S = 0; S < AllVolSteps.length; S++)
            {
                const Step = AllVolSteps[S];
                const Margin = BrickRadius + Band + Step.Blend;
                const Prims = Step.Primitives.filter(P =>
                    !P.AABB || (BX1 >= P.AABB[0] - Margin && BX0 <= P.AABB[3] + Margin &&
                                BZ1 >= P.AABB[2] - Margin && BZ0 <= P.AABB[5] + Margin)
                );
                if (Prims.length)
                {
                    if (!ColVolSteps) ColVolSteps = [];
                    ColVolSteps.push({ Region: Step.Region, Mode: Step.Mode, Blend: Step.Blend, Primitives: Prims });
                }
            }
            let ColSculpts = null;
            for (let K = 0; K < AllSculptList.length; K++)
            {
                const Entry = AllSculptList[K];
                if (BX1 >= Entry.AABB[0] && BX0 <= Entry.AABB[3] && BZ1 >= Entry.AABB[2] && BZ0 <= Entry.AABB[5])
                {
                    if (!ColSculpts) ColSculpts = [];
                    ColSculpts.push(Entry);
                }
            }

            if (MinP > Band && !ColVolSteps && !ColSculpts) continue;
            if (MinH > MaxH) { MinH = 0; MaxH = 0; }

            const YPad  = Band + DY * 1.25;
            const CX    = 0.5 * (BX0 + BX1);
            const CZ    = 0.5 * (BZ0 + BZ1);
            const YList = [];

            for (let IY = 0; IY < GY; IY++)
            {
                const BrickMinY = BoundsMin[1] + (IY * BRICK_VOXELS - 0.5) * DY;
                const BrickMaxY = BoundsMin[1] + (IY * BRICK_VOXELS + 8.5) * DY;
                const CY        = 0.5 * (BrickMinY + BrickMaxY);
                const CellIdx   = (IZ * GY + IY) * GX + IX;

                const StraddlesCoast  = (MinP <= Band && MaxP >= -Band) && (BrickMaxY >= -Layer.Depth - YPad && BrickMinY <= MaxH + YPad);
                const StraddlesTop    = (MinP <= Band) && (BrickMaxY >= MinH - YPad && BrickMinY <= MaxH + YPad);
                const StraddlesBottom = (MinP <= Band) && (BrickMaxY >= -Layer.Depth - YPad && BrickMinY <= -Layer.Depth + YPad);
                const InsideBaseSolid = (MaxP < -Band && BrickMaxY < MinH - YPad && BrickMinY > -Layer.Depth + YPad);

                let VolSurface = false;
                let VolCarvedAir = false;
                if (ColVolSteps)
                {
                    for (let S = 0; S < ColVolSteps.length; S++)
                    {
                        const Step   = ColVolSteps[S];
                        const Reach  = BrickRadius + Band + Step.Blend * 0.65;
                        for (let P = 0; P < Step.Primitives.length; P++)
                        {
                            const Prim = Step.Primitives[P];
                            if (Prim.AABB && (BrickMaxY < Prim.AABB[1] - Reach || BrickMinY > Prim.AABB[4] + Reach)) continue;
                            const D = VolumetricPrimitiveDistance(Prim, CX, CY, CZ);
                            if (Math.abs(D) <= Reach)
                            {
                                // Subtract only creates surface where base rock exists or overlaps another solid; union creates surface inside region XZ
                                if (Step.Mode !== 'subtract' || MinP <= Reach) VolSurface = true;
                            }
                            else if (Step.Mode === 'subtract' && D < -Reach)
                            {
                                VolCarvedAir = true;
                            }
                        }
                    }
                }
                if (!VolSurface && ColSculpts)
                {
                    for (let K = 0; K < ColSculpts.length; K++)
                    {
                        const { AABB } = ColSculpts[K];
                        if (BrickMaxY >= AABB[1] && BrickMinY <= AABB[4]) { VolSurface = true; break; }
                    }
                }

                if (VolSurface || StraddlesCoast || StraddlesTop || StraddlesBottom)
                {
                    IsAllocated[CellIdx] = 1;
                    YList.push(IY);
                    AllocatedCount++;
                }
                else if (InsideBaseSolid && !VolCarvedAir)
                {
                    IsSolidCore[CellIdx] = 1;
                }
            }

            if (YList.length > 0) ActiveColYLists[IZ * GX + IX] = YList;
        }
    }

    // 3. Exact 26-neighbour 2-pass L∞ Chebyshev empty-space distance transform
    const ChebyshevDist = ComputeChebyshevDistance3D(GX, GY, GZ, IsAllocated);

    // 4. Populate allocated 10×10×10 bricks in Float32, then bulk-convert to IEEE-754 Float16
    const BricksF32  = new Float32Array(AllocatedCount * BRICK_VOLUME);
    const BricksF16  = new Uint16Array(AllocatedCount * BRICK_VOLUME);
    const BrickCells = new Uint32Array(AllocatedCount);

    const HasAnyVolOrSculpt = AllVolSteps.length > 0 || AllSculptList.length > 0 || CompiledRegions.length > 1;
    let Cursor = 0;

    for (let IZ = 0; IZ < GZ; IZ++)
    {
        for (let IX = 0; IX < GX; IX++)
        {
            const YList = ActiveColYLists[IZ * GX + IX];
            if (!YList) continue;

            const BaseGX2 = IX * BRICK_VOXELS + 1;
            const BaseGZ2 = IZ * BRICK_VOXELS + 1;
            const ColX0   = BoundsMin[0] + (IX * BRICK_VOXELS - 1) * DX;
            const ColX1   = BoundsMin[0] + (IX * BRICK_VOXELS + 9) * DX;
            const ColZ0   = BoundsMin[2] + (IZ * BRICK_VOXELS - 1) * DZ;
            const ColZ1   = BoundsMin[2] + (IZ * BRICK_VOXELS + 9) * DZ;
            const CX      = BoundsMin[0] + (IX + 0.5) * BrickWorld[0];
            const CZ      = BoundsMin[2] + (IZ + 0.5) * BrickWorld[2];

            for (let K = 0; K < YList.length; K++)
            {
                const IY        = YList[K];
                const CellIdx   = (IZ * GY + IY) * GX + IX;
                const BrickSlot = Cursor++;
                BrickIndexByCell[CellIdx] = BrickSlot;
                BrickCells[BrickSlot]     = CellIdx;
                const BaseOffset = BrickSlot * BRICK_VOLUME;

                let ActiveVolSteps = null;
                let ActiveSculpts  = null;
                let ForcedInteriorMin = 1e6;
                if (HasAnyVolOrSculpt && SingleReg)
                {
                    const BrickY0 = BoundsMin[1] + (IY * BRICK_VOXELS - 1) * DY;
                    const BrickY1 = BoundsMin[1] + (IY * BRICK_VOXELS + 9) * DY;
                    const CY      = BoundsMin[1] + (IY + 0.5) * BrickWorld[1];
                    for (const Step of SingleReg.Compiled.VolumeSteps)
                    {
                        const Reach = BrickRadius + Band + (Step.Blend || 0) * 0.75 + MaxVoxel;
                        const Prims = [];
                        for (let P = 0; P < Step.Primitives.length; P++)
                        {
                            const Prim = Step.Primitives[P];
                            if (Prim.AABB && !BoxOverlapsFeature(ColX0, BrickY0, ColZ0, ColX1, BrickY1, ColZ1, Prim.AABB, Band + (Step.Blend || 0))) continue;
                            const DCentre = VolumetricPrimitiveDistance(Prim, CX, CY, CZ);
                            if (Math.abs(DCentre) <= Reach)
                            {
                                Prims.push(Prim);
                            }
                            else if (DCentre < -Reach && Step.Mode === 'union')
                            {
                                ForcedInteriorMin = Math.min(ForcedInteriorMin, -Band);
                            }
                            else if (DCentre < -Reach && Step.Mode === 'subtract')
                            {
                                ForcedInteriorMin = Math.max(ForcedInteriorMin === 1e6 ? -1e6 : ForcedInteriorMin, Band);
                            }
                        }
                        if (Prims.length)
                        {
                            if (!ActiveVolSteps) ActiveVolSteps = [];
                            ActiveVolSteps.push({ Mode: Step.Mode, Blend: Step.Blend, Primitives: Prims });
                        }
                    }
                    for (const Stroke of SingleReg.Region.SculptStrokes || [])
                    {
                        const R = (Stroke.Radius || 8) * 1.9 + Band;
                        if ((Stroke.Points || []).some(Pt =>
                            ColX1 >= Pt[0] - R && ColX0 <= Pt[0] + R &&
                            BrickY1 >= Pt[1] - R && BrickY0 <= Pt[1] + R &&
                            ColZ1 >= Pt[2] - R && ColZ0 <= Pt[2] + R))
                        {
                            if (!ActiveSculpts) ActiveSculpts = [];
                            ActiveSculpts.push(Stroke);
                        }
                    }
                }

                if (SingleReg && !ActiveVolSteps && !ActiveSculpts && ForcedInteriorMin === 1e6)
                {
                    // Pure precomputed column path: ~4 arithmetic ops per voxel!
                    for (let LZ = 0; LZ < 10; LZ++)
                    {
                        const G2Row = (BaseGZ2 + LZ) * Stride2D + BaseGX2;
                        for (let LY = 0; LY < 10; LY++)
                        {
                            const WY        = BoundsMin[1] + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
                            const BottomPhi = -WY - Layer.Depth;
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
                                BricksF32[RowOff + LX] = Phi < -Band ? -Band : (Phi > Band ? Band : Phi);
                            }
                        }
                    }
                }
                else
                {
                    const Reg = SingleReg ? SingleReg.Region : CompiledRegions[0].Region;
                    const ClipReg = { X0: Reg.X0, Z0: Reg.Z0, X1: Reg.X1, Z1: Reg.Z1, Edge: 4 };
                    for (let LZ = 0; LZ < 10; LZ++)
                    {
                        const WZ    = BoundsMin[2] + (IZ * BRICK_VOXELS + LZ - 1 + 0.5) * DZ;
                        const G2Row = (BaseGZ2 + LZ) * Stride2D + BaseGX2;
                        for (let LY = 0; LY < 10; LY++)
                        {
                            const WY        = BoundsMin[1] + (IY * BRICK_VOXELS + LY - 1 + 0.5) * DY;
                            const BottomPhi = -WY - Layer.Depth;
                            const RowOff    = BaseOffset + (LZ * 10 + LY) * 10;
                            for (let LX = 0; LX < 10; LX++)
                            {
                                const WX      = BoundsMin[0] + (IX * BRICK_VOXELS + LX - 1 + 0.5) * DX;
                                const C2      = G2Row + LX;
                                const Planar  = GridPlanar[C2];
                                const TopPhi  = (WY - GridSurfY[C2]) * GridSlope[C2];
                                const VertPhi = TopPhi > BottomPhi ? TopPhi : BottomPhi;
                                const OutP    = Planar > 0 ? Planar : 0;
                                const OutV    = VertPhi > 0 ? VertPhi : 0;
                                const Outside = (OutP > 0 && OutV > 0) ? Math.hypot(OutP, OutV) : (OutP + OutV);
                                const Inside  = Math.min(Math.max(Planar, VertPhi), 0);
                                let Phi = Outside + Inside;

                                if (ActiveVolSteps)
                                {
                                    const RegBox = RoundedBoxDistance2D(Reg, WX, WZ);
                                    for (let S = 0; S < ActiveVolSteps.length; S++)
                                    {
                                        const Step = ActiveVolSteps[S];
                                        let Feat = 1e6;
                                        for (let P = 0; P < Step.Primitives.length; P++)
                                        {
                                            const D = VolumetricPrimitiveDistance(Step.Primitives[P], WX, WY, WZ);
                                            if (D < Feat) Feat = D;
                                        }
                                        if (Step.Mode !== 'subtract') Feat = Math.max(Feat, RegBox);
                                        Phi = CombineDistance(Phi, Feat, Step.Mode, Step.Blend);
                                    }
                                }
                                else if (!SingleReg)
                                {
                                    for (const { Region: R2, Compiled: Cmp2 } of CompiledRegions)
                                    {
                                        for (const Step of Cmp2.VolumeSteps)
                                        {
                                            let Feat = 1e6;
                                            for (const Prim of Step.Primitives) Feat = Math.min(Feat, VolumetricPrimitiveDistance(Prim, WX, WY, WZ));
                                            if (Step.Mode !== 'subtract') Feat = Math.max(Feat, RoundedBoxDistance2D(R2, WX, WZ));
                                            Phi = CombineDistance(Phi, Feat, Step.Mode, Step.Blend);
                                        }
                                    }
                                }

                                if (ActiveSculpts)
                                {
                                    const BoundsClip = RoundedBoxDistance2D(ClipReg, WX, WZ) - 2.0;
                                    for (let K2 = 0; K2 < ActiveSculpts.length; K2++)
                                    {
                                        Phi = ApplySculptStrokePoint(Phi, ActiveSculpts[K2], WX, WY, WZ);
                                    }
                                    Phi = Math.max(Phi, BoundsClip);
                                }
                                BricksF32[RowOff + LX] = Phi < -Band ? -Band : (Phi > Band ? Band : Phi);
                            }
                        }
                    }
                }
            }
        }
    }

    ConvertFloat32BufferToHalf(BricksF32, BricksF16);
    const BuildMs = performance.now() - StartMs;

    return {
        LayerIndex,
        Name: Layer.Name,
        Biome: Layer.Biome || 'desert',
        Color: HexToRgb(Layer.Color || '#c8a374'),
        CliffColor: HexToRgb(Layer.CliffColor || '#8d583a'),
        Strata: Number.isFinite(Layer.Strata) ? Layer.Strata : 0.5,
        Roughness: Number.isFinite(Layer.Roughness) ? Layer.Roughness : 0.65,
        Depth: Layer.Depth,
        Resolution,
        GridDims: [GX, GY, GZ],
        BoundsMin,
        BoundsMax,
        Pitch,
        BrickWorld,
        MinVoxel,
        MaxVoxel,
        Band,
        Stride2D,
        GridPlanar,
        GridSurfY,
        GridSlope,
        AllocatedCount,
        BrickCapacity: AllocatedCount,
        IsAllocated,
        IsSolidCore,
        ChebyshevDist,
        BrickIndexByCell,
        BrickCells,
        BricksF32,
        BricksF16,
        BuildMs
    };
}

export function HexToRgb(Hex)
{
    const Clean = String(Hex || '#b9a481').replace('#', '');
    const Int   = parseInt(Clean.length === 6 ? Clean : 'b9a481', 16);
    return [((Int >> 16) & 255) / 255, ((Int >> 8) & 255) / 255, (Int & 255) / 255];
}

//------------------------------------------------------------------------------------------------------------------------
//                              CPU TRILINEAR SAMPLING & EXACT RAYCAST PICKING ON THE ATLAS
//------------------------------------------------------------------------------------------------------------------------

export function SampleLayerVolumeSdf(Volume, X, Y, Z)
{
    if (!Volume) return 1e6;
    const [BMinX, BMinY, BMinZ] = Volume.BoundsMin;
    const [BMaxX, BMaxY, BMaxZ] = Volume.BoundsMax;

    const OutX = Math.max(BMinX - X, 0, X - BMaxX);
    const OutY = Math.max(BMinY - Y, 0, Y - BMaxY);
    const OutZ = Math.max(BMinZ - Z, 0, Z - BMaxZ);
    if (OutX > 0 || OutY > 0 || OutZ > 0)
    {
        return Math.hypot(OutX, OutY, OutZ) + Volume.MinVoxel;
    }

    const VX = (X - BMinX) / Volume.Pitch[0];
    const VY = (Y - BMinY) / Volume.Pitch[1];
    const VZ = (Z - BMinZ) / Volume.Pitch[2];

    const [GX, GY, GZ] = Volume.GridDims;
    const IX = Math.max(0, Math.min(GX - 1, Math.floor(VX / BRICK_VOXELS)));
    const IY = Math.max(0, Math.min(GY - 1, Math.floor(VY / BRICK_VOXELS)));
    const IZ = Math.max(0, Math.min(GZ - 1, Math.floor(VZ / BRICK_VOXELS)));

    const CellIdx   = (IZ * GY + IY) * GX + IX;
    const BrickSlot = Volume.BrickIndexByCell[CellIdx];
    if (BrickSlot < 0)
    {
        return Volume.IsSolidCore[CellIdx] ? -Volume.Band : Volume.Band;
    }

    const TX = Math.max(0, Math.min(8.9999, VX - IX * BRICK_VOXELS + 0.5));
    const TY = Math.max(0, Math.min(8.9999, VY - IY * BRICK_VOXELS + 0.5));
    const TZ = Math.max(0, Math.min(8.9999, VZ - IZ * BRICK_VOXELS + 0.5));

    const X0 = Math.floor(TX), Y0 = Math.floor(TY), Z0 = Math.floor(TZ);
    const FX = TX - X0,        FY = TY - Y0,        FZ = TZ - Z0;

    const Base = BrickSlot * BRICK_VOLUME;
    const Data = Volume.BricksF32;
    const At = (LX, LY, LZ) => Data[Base + (LZ * 10 + LY) * 10 + LX];

    const C00 = At(X0, Y0, Z0) * (1 - FX) + At(X0 + 1, Y0, Z0) * FX;
    const C10 = At(X0, Y0 + 1, Z0) * (1 - FX) + At(X0 + 1, Y0 + 1, Z0) * FX;
    const C01 = At(X0, Y0, Z0 + 1) * (1 - FX) + At(X0 + 1, Y0, Z0 + 1) * FX;
    const C11 = At(X0, Y0 + 1, Z0 + 1) * (1 - FX) + At(X0 + 1, Y0 + 1, Z0 + 1) * FX;
    const C0  = C00 * (1 - FY) + C10 * FY;
    const C1  = C01 * (1 - FY) + C11 * FY;
    return C0 * (1 - FZ) + C1 * FZ;
}

export function SampleLayerVolumeNormal(Volume, X, Y, Z)
{
    const H  = Volume.MinVoxel * 0.65;
    const K0 = SampleLayerVolumeSdf(Volume, X + H, Y - H, Z - H);
    const K1 = SampleLayerVolumeSdf(Volume, X - H, Y - H, Z + H);
    const K2 = SampleLayerVolumeSdf(Volume, X - H, Y + H, Z - H);
    const K3 = SampleLayerVolumeSdf(Volume, X + H, Y + H, Z + H);
    const NX = K0 - K1 - K2 + K3;
    const NY = -K0 - K1 + K2 + K3;
    const NZ = -K0 + K1 - K2 + K3;
    const L  = Math.hypot(NX, NY, NZ) || 1;
    return [NX / L, NY / L, NZ / L];
}

export function IntersectRayAABB(Origin, Dir, BMin, BMax)
{
    let TMin = 0.0;
    let TMax = 1e7;
    for (let Axis = 0; Axis < 3; Axis++)
    {
        const O = Origin[Axis], D = Dir[Axis];
        if (Math.abs(D) < 1e-9)
        {
            if (O < BMin[Axis] || O > BMax[Axis]) return null;
        }
        else
        {
            const Inv = 1.0 / D;
            let T0 = (BMin[Axis] - O) * Inv;
            let T1 = (BMax[Axis] - O) * Inv;
            if (T0 > T1) { const Tmp = T0; T0 = T1; T1 = Tmp; }
            if (T0 > TMin) TMin = T0;
            if (T1 < TMax) TMax = T1;
            if (TMin > TMax) return null;
        }
    }
    return [Math.max(0.0, TMin), TMax];
}

export function RaycastGlobalSdfVolumes(Volumes, Origin, Dir, TargetLayerIndex = -1)
{
    let BestHit = null;

    for (let V = 0; V < Volumes.length; V++)
    {
        const Vol = Volumes[V];
        if (!Vol) continue;
        if (TargetLayerIndex >= 0 && Vol.LayerIndex !== TargetLayerIndex) continue;

        const Span = IntersectRayAABB(Origin, Dir, Vol.BoundsMin, Vol.BoundsMax);
        if (!Span) continue;

        let [T, TExit] = Span;
        if (BestHit && T >= BestHit.Distance) continue;
        TExit = BestHit ? Math.min(TExit, BestHit.Distance) : TExit;

        let PrevT = T;
        const [GX, GY, GZ] = Vol.GridDims;
        const [BWX, BWY, BWZ] = Vol.BrickWorld;
        const [BMinX, BMinY, BMinZ] = Vol.BoundsMin;

        for (let Step = 0; Step < 512 && T <= TExit; Step++)
        {
            const PX = Origin[0] + Dir[0] * T;
            const PY = Origin[1] + Dir[1] * T;
            const PZ = Origin[2] + Dir[2] * T;

            const IX = Math.max(0, Math.min(GX - 1, Math.floor((PX - BMinX) / BWX)));
            const IY = Math.max(0, Math.min(GY - 1, Math.floor((PY - BMinY) / BWY)));
            const IZ = Math.max(0, Math.min(GZ - 1, Math.floor((PZ - BMinZ) / BWZ)));
            const CellIdx = (IZ * GY + IY) * GX + IX;

            if (Vol.BrickIndexByCell[CellIdx] < 0 && !Vol.IsSolidCore[CellIdx])
            {
                const Radius = Math.max(0, Vol.ChebyshevDist[CellIdx] - 1);
                const MinX = BMinX + (IX - Radius) * BWX;
                const MaxX = BMinX + (IX + Radius + 1) * BWX;
                const MinY = BMinY + (IY - Radius) * BWY;
                const MaxY = BMinY + (IY + Radius + 1) * BWY;
                const MinZ = BMinZ + (IZ - Radius) * BWZ;
                const MaxZ = BMinZ + (IZ + Radius + 1) * BWZ;

                const TX = Math.abs(Dir[0]) > 1e-9 ? ((Dir[0] > 0 ? MaxX : MinX) - PX) / Dir[0] : 1e7;
                const TY = Math.abs(Dir[1]) > 1e-9 ? ((Dir[1] > 0 ? MaxY : MinY) - PY) / Dir[1] : 1e7;
                const TZ = Math.abs(Dir[2]) > 1e-9 ? ((Dir[2] > 0 ? MaxZ : MinZ) - PZ) / Dir[2] : 1e7;
                PrevT = T;
                T += Math.max(Vol.MinVoxel * 0.25, Math.min(TX, TY, TZ) + Vol.MinVoxel * 0.05);
                continue;
            }

            const Phi = SampleLayerVolumeSdf(Vol, PX, PY, PZ);
            if (Phi <= 0.0)
            {
                let Lo = PrevT, Hi = T;
                for (let B = 0; B < 5; B++)
                {
                    const Mid  = 0.5 * (Lo + Hi);
                    const MPhi = SampleLayerVolumeSdf(Vol, Origin[0] + Dir[0] * Mid, Origin[1] + Dir[1] * Mid, Origin[2] + Dir[2] * Mid);
                    if (MPhi > 0.0) Lo = Mid; else Hi = Mid;
                }
                const HitT   = 0.5 * (Lo + Hi);
                const HitPos = [Origin[0] + Dir[0] * HitT, Origin[1] + Dir[1] * HitT, Origin[2] + Dir[2] * HitT];
                BestHit = {
                    Distance: HitT,
                    Position: HitPos,
                    Normal: SampleLayerVolumeNormal(Vol, HitPos[0], HitPos[1], HitPos[2]),
                    LayerIndex: Vol.LayerIndex
                };
                break;
            }

            PrevT = T;
            T += Math.max(Phi * 0.88, Vol.MinVoxel * 0.12);
        }
    }

    return BestHit;
}

//------------------------------------------------------------------------------------------------------------------------
//                                  PACKING MULTIPLE LAYERS FOR GPU 3D TEXTURES
//------------------------------------------------------------------------------------------------------------------------

export function GlobalSlotToCoords(GlobalSlot)
{
    const PerSlab = ATLAS_BRICKS_X * ATLAS_BRICKS_Y;
    const SZ  = Math.floor(GlobalSlot / PerSlab);
    const Rem = GlobalSlot - SZ * PerSlab;
    const SY  = Math.floor(Rem / ATLAS_BRICKS_X);
    const SX  = Rem - SY * ATLAS_BRICKS_X;
    return [SX, SY, SZ];
}

export function PackGlobalSdfForGpu(LayerVolumes, SculptHeadroomBricks = 3072)
{
    const Active = LayerVolumes.filter(Boolean);
    if (!Active.length)
    {
        return {
            ActiveLayers: [],
            TotalBricks: 0,
            NextFreeGlobalSlot: 0,
            MaxGlobalSlots: ATLAS_BRICKS_X * ATLAS_BRICKS_Y,
            AtlasBrickDims: [ATLAS_BRICKS_X, ATLAS_BRICKS_Y, 1],
            AtlasTexelDims: [ATLAS_BRICKS_X * 10, ATLAS_BRICKS_Y * 10, 10],
            IndirectionDims: [1, 1, 1],
            IndirectionBytes: new Uint8Array([255, 255, 255, 254]),
            AtlasHalfData: new Uint16Array(ATLAS_BRICKS_X * ATLAS_BRICKS_Y * 1000)
        };
    }

    let TotalBricks = 0;
    let MaxGX = 1, SumGY = 0, MaxGZ = 1;
    for (const Vol of Active)
    {
        TotalBricks += Vol.AllocatedCount;
        MaxGX = Math.max(MaxGX, Vol.GridDims[0]);
        SumGY += Vol.GridDims[1];
        MaxGZ = Math.max(MaxGZ, Vol.GridDims[2]);
    }

    const PerSlab      = ATLAS_BRICKS_X * ATLAS_BRICKS_Y; // 2304 bricks per Z-slab
    const NeededBricks = TotalBricks + SculptHeadroomBricks;
    const AtlasBricksZ = Math.max(2, Math.ceil(NeededBricks / PerSlab));
    if (AtlasBricksZ > 48)
    {
        throw Error(`Global SDF brick pool exceeded GPU 3D atlas ceiling (${NeededBricks} bricks > ${48 * PerSlab}).`);
    }

    const AtlasBrickDims  = [ATLAS_BRICKS_X, ATLAS_BRICKS_Y, AtlasBricksZ];
    const AtlasTexelDims  = [ATLAS_BRICKS_X * 10, ATLAS_BRICKS_Y * 10, AtlasBricksZ * 10];
    const IndirectionDims = [MaxGX, SumGY, MaxGZ];

    const IndirectionBytes = new Uint8Array(MaxGX * SumGY * MaxGZ * 4);
    const AtlasHalfData    = new Uint16Array(AtlasTexelDims[0] * AtlasTexelDims[1] * AtlasTexelDims[2]);

    let GlobalBrickSlot = 0;
    let YSliceOffset    = 0;
    const LayerDescriptors = [];

    const TexW     = AtlasTexelDims[0];
    const TexH     = AtlasTexelDims[1];
    const TexSlice = TexW * TexH;

    for (const Vol of Active)
    {
        const [GX, GY, GZ] = Vol.GridDims;
        Vol.IndirectionYOffset = YSliceOffset;
        Vol.GlobalSlotByBrick  = new Int32Array(Vol.BrickCapacity);

        for (let B = 0; B < Vol.AllocatedCount; B++)
        {
            const Slot = GlobalBrickSlot++;
            Vol.GlobalSlotByBrick[B] = Slot;
            const [SX, SY, SZ] = GlobalSlotToCoords(Slot);
            const DstX0   = SX * 10;
            const DstY0   = SY * 10;
            const DstZ0   = SZ * 10;
            const SrcBase = B * BRICK_VOLUME;

            for (let LZ = 0; LZ < 10; LZ++)
            {
                const ZOff = (DstZ0 + LZ) * TexSlice;
                for (let LY = 0; LY < 10; LY++)
                {
                    const DstOff = ZOff + (DstY0 + LY) * TexW + DstX0;
                    const SrcOff = SrcBase + (LZ * 10 + LY) * 10;
                    AtlasHalfData.set(Vol.BricksF16.subarray(SrcOff, SrcOff + 10), DstOff);
                }
            }
        }

        for (let IZ = 0; IZ < GZ; IZ++)
        {
            for (let IY = 0; IY < GY; IY++)
            {
                const IndY   = YSliceOffset + IY;
                const DstRow = ((IZ * SumGY + IndY) * MaxGX) * 4;
                const SrcRow = (IZ * GY + IY) * GX;

                for (let IX = 0; IX < GX; IX++)
                {
                    const CellIdx   = SrcRow + IX;
                    const LocalSlot = Vol.BrickIndexByCell[CellIdx];
                    const Out       = DstRow + IX * 4;

                    if (LocalSlot >= 0)
                    {
                        const [SX, SY, SZ] = GlobalSlotToCoords(Vol.GlobalSlotByBrick[LocalSlot]);
                        IndirectionBytes[Out]     = SX;
                        IndirectionBytes[Out + 1] = SY;
                        IndirectionBytes[Out + 2] = SZ;
                        IndirectionBytes[Out + 3] = 128;
                    }
                    else if (Vol.IsSolidCore[CellIdx])
                    {
                        const D = Math.min(126, Vol.ChebyshevDist[CellIdx]);
                        IndirectionBytes[Out]     = INDIRECTION_SOLID;
                        IndirectionBytes[Out + 1] = INDIRECTION_SOLID;
                        IndirectionBytes[Out + 2] = INDIRECTION_SOLID;
                        IndirectionBytes[Out + 3] = Math.max(1, 128 - D);
                    }
                    else
                    {
                        const D = Math.min(126, Vol.ChebyshevDist[CellIdx]);
                        IndirectionBytes[Out]     = INDIRECTION_AIR;
                        IndirectionBytes[Out + 1] = INDIRECTION_AIR;
                        IndirectionBytes[Out + 2] = INDIRECTION_AIR;
                        IndirectionBytes[Out + 3] = Math.min(254, 128 + D);
                    }
                }
            }
        }

        LayerDescriptors.push({
            LayerIndex: Vol.LayerIndex,
            BoundsMin: Vol.BoundsMin,
            BoundsMax: Vol.BoundsMax,
            Pitch: Vol.Pitch,
            BrickWorld: Vol.BrickWorld,
            GridDims: Vol.GridDims,
            IndirectionYOffset: YSliceOffset,
            Band: Vol.Band,
            MinVoxel: Vol.MinVoxel,
            Color: Vol.Color,
            CliffColor: Vol.CliffColor,
            Strata: Vol.Strata,
            Roughness: Vol.Roughness
        });

        YSliceOffset += GY;
    }

    return {
        ActiveLayers: LayerDescriptors,
        TotalBricks,
        NextFreeGlobalSlot: GlobalBrickSlot,
        MaxGlobalSlots: ATLAS_BRICKS_X * ATLAS_BRICKS_Y * AtlasBricksZ,
        AtlasBrickDims,
        AtlasTexelDims,
        IndirectionDims,
        IndirectionBytes,
        AtlasHalfData
    };
}

export function ExportLayerSparseArchive(Volume)
{
    const Header = {
        Format: 'FrontierGlobalSDF/1',
        Name: Volume.Name,
        Biome: Volume.Biome,
        Resolution: Volume.Resolution,
        BrickVoxels: BRICK_VOXELS,
        BrickTexels: BRICK_TEXELS,
        BoundsMin: Volume.BoundsMin,
        BoundsMax: Volume.BoundsMax,
        Pitch: Volume.Pitch,
        Band: Volume.Band,
        AllocatedBricks: Volume.AllocatedCount
    };
    const HeaderBytes = new TextEncoder().encode(JSON.stringify(Header) + '\n');
    const CellBytes   = new Uint8Array(Volume.BrickCells.buffer, Volume.BrickCells.byteOffset, Volume.AllocatedCount * 4);
    const HalfBytes   = new Uint8Array(Volume.BricksF16.buffer, Volume.BricksF16.byteOffset, Volume.AllocatedCount * BRICK_VOLUME * 2);

    const Combined = new Uint8Array(HeaderBytes.byteLength + CellBytes.byteLength + HalfBytes.byteLength);
    Combined.set(HeaderBytes, 0);
    Combined.set(CellBytes, HeaderBytes.byteLength);
    Combined.set(HalfBytes, HeaderBytes.byteLength + CellBytes.byteLength);
    return { Header, Bytes: Combined };
}
