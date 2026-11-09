/*==========================================================================================================================================
                                                     GLOBALSDFSCULPTWORKER.JS
==========================================================================================================================================*/
// Unified build + sculpt worker — off-main-thread for both heavy paths.
// Keeps a live copy of each built volume so pointermove never blocks the UI.
// Main thread only handles allocation bookkeeping + texSubImage3D uploads.

import { BuildLayerSparseSdf } from './GlobalSdfBrickAtlas.js';
import {
    ApplySculptSegmentToVolumeNoAlloc,
    GrowVolumeBrickArrays,
    InitializeFreshBrickFromColumnGrid,
    AllocateBrickInVolume
} from './GlobalSdfSculpt.js';
import { BRICK_VOLUME, ConvertFloat32BufferToHalf } from './GlobalSdfBrickAtlas.js';

const StoredVolumes = new Map(); // LayerId -> Volume   (fallback to LayerIndex key if no Id)

function keyFor(LayerId, LayerIndex) { return LayerId || `__idx_${LayerIndex}`; }

self.onmessage = async (Event) =>
{
    const Payload = Event.data;
    if (!Payload) return;

    if (Payload.Type === 'build-layer')
    {
        const { RequestId, Layer, LayerIndex, LayerId } = Payload;
        try
        {
            const Volume = BuildLayerSparseSdf(Layer, LayerIndex ?? 0);
            if (!Volume) { self.postMessage({ Type: 'layer-built', RequestId, LayerIndex: LayerIndex ?? 0, LayerId, Volume: null }); return; }
            const Key = keyFor(LayerId || Layer?.Id, LayerIndex ?? 0);
            // Keep original in worker; send clone to main
            StoredVolumes.set(Key, Volume);
            // Also store by index for sculpt lookup if Id changes after reorder
            if (LayerId) StoredVolumes.set(`__idx_${LayerIndex ?? 0}`, Volume);

            const VolumeForMain = {
                ...Volume,
                IsAllocated: new Uint8Array(Volume.IsAllocated),
                IsSolidCore: new Uint8Array(Volume.IsSolidCore),
                ChebyshevDist: new Uint8Array(Volume.ChebyshevDist),
                BrickIndexByCell: new Int32Array(Volume.BrickIndexByCell),
                BrickCells: new Uint32Array(Volume.BrickCells),
                BricksF32: new Float32Array(Volume.BricksF32),
                BricksF16: new Uint16Array(Volume.BricksF16),
                GridPlanar: new Float32Array(Volume.GridPlanar),
                GridSurfY: new Float32Array(Volume.GridSurfY),
                GridSlope: new Float32Array(Volume.GridSlope),
                GlobalSlotByBrick: Volume.GlobalSlotByBrick ? new Int32Array(Volume.GlobalSlotByBrick) : null
            };
            const Transfer = [
                VolumeForMain.IsAllocated.buffer,
                VolumeForMain.IsSolidCore.buffer,
                VolumeForMain.ChebyshevDist.buffer,
                VolumeForMain.BrickIndexByCell.buffer,
                VolumeForMain.BrickCells.buffer,
                VolumeForMain.BricksF32.buffer,
                VolumeForMain.BricksF16.buffer,
                VolumeForMain.GridPlanar.buffer,
                VolumeForMain.GridSurfY.buffer,
                VolumeForMain.GridSlope.buffer
            ];
            if (VolumeForMain.GlobalSlotByBrick) Transfer.push(VolumeForMain.GlobalSlotByBrick.buffer);
            self.postMessage({ Type: 'layer-built', RequestId, LayerIndex: LayerIndex ?? 0, LayerId, Volume: VolumeForMain }, Transfer);
        }
        catch (Err)
        {
            self.postMessage({ Type: 'layer-error', RequestId, LayerIndex: LayerIndex ?? 0, LayerId: Payload.LayerId, Message: Err instanceof Error ? Err.message : String(Err) });
        }
        return;
    }

    if (Payload.Type === 'sculpt-dab')
    {
        const { RequestId, LayerId, LayerIndex, Segment, LayerSnapshot, NewBricks } = Payload;
        const Key = keyFor(LayerId, LayerIndex ?? 0);
        let Vol = StoredVolumes.get(Key);
        if (!Vol) Vol = StoredVolumes.get(`__idx_${LayerIndex ?? 0}`);
        if (!Vol)
        {
            self.postMessage({ Type: 'sculpt-done', RequestId, LayerId, LayerIndex, Error: 'Volume not in worker' });
            return;
        }
        try
        {
            // 1) Apply any pre-allocated bricks that main thread just created
            if (NewBricks && NewBricks.length)
            {
                for (const NB of NewBricks)
                {
                    const { IX, IY, IZ, CellIdx, LocalSlot, GlobalSlot } = NB;
                    // Skip if already allocated (stale)
                    if (Vol.BrickIndexByCell[CellIdx] >= 0) continue;
                    // Grow if needed (mirror main's GrowVolumeBrickArrays)
                    if (LocalSlot >= Vol.BrickCapacity) GrowVolumeBrickArrays(Vol, LocalSlot + 1);
                    // Ensure AllocatedCount consistency: LocalSlot should equal current count if allocations are ordered
                    // but we allow gaps by bumping count
                    if (Vol.AllocatedCount <= LocalSlot) Vol.AllocatedCount = LocalSlot + 1;
                    const BaseOffset = LocalSlot * BRICK_VOLUME;
                    InitializeFreshBrickFromColumnGrid(Vol, IX, IY, IZ, BaseOffset);
                    ConvertFloat32BufferToHalf(Vol.BricksF32, Vol.BricksF16, BaseOffset, BRICK_VOLUME);
                    Vol.BrickIndexByCell[CellIdx] = LocalSlot;
                    Vol.BrickCells[LocalSlot] = CellIdx;
                    Vol.IsAllocated[CellIdx] = 1;
                    Vol.IsSolidCore[CellIdx] = 0;
                    // ChebyshevDist handling: set centre to 0 and tighten neighbours (no indirection)
                    const OldCheb = Vol.ChebyshevDist[CellIdx];
                    Vol.ChebyshevDist[CellIdx] = 0;
                    const [GX, GY, GZ] = Vol.GridDims;
                    const Reach = Math.min(OldCheb + 1, Math.max(GX, GY, GZ));
                    const X0 = Math.max(0, IX - Reach), X1 = Math.min(GX - 1, IX + Reach);
                    const Y0 = Math.max(0, IY - Reach), Y1 = Math.min(GY - 1, IY + Reach);
                    const Z0 = Math.max(0, IZ - Reach), Z1 = Math.min(GZ - 1, IZ + Reach);
                    for (let Z = Z0; Z <= Z1; Z++) { const DZ = Math.abs(Z - IZ); for (let Y = Y0; Y <= Y1; Y++) { const DZY = Math.max(DZ, Math.abs(Y - IY)); const SrcRow = (Z * GY + Y) * GX; for (let X = X0; X <= X1; X++) { if (X===IX&&Y===IY&&Z===IZ) continue; const CIdx = SrcRow + X; const DInf = Math.max(DZY, Math.abs(X-IX)); if (DInf < Vol.ChebyshevDist[CIdx]) Vol.ChebyshevDist[CIdx]=DInf; } } }
                    if (!Vol.GlobalSlotByBrick) Vol.GlobalSlotByBrick = new Int32Array(Vol.BrickCapacity).fill(-1);
                    else if (Vol.GlobalSlotByBrick.length <= LocalSlot) { const Next = new Int32Array(Math.max(Vol.BrickCapacity, LocalSlot+1)).fill(-1); Next.set(Vol.GlobalSlotByBrick); Vol.GlobalSlotByBrick = Next; }
                    Vol.GlobalSlotByBrick[LocalSlot] = GlobalSlot;
                }
            }

            const Layer = LayerSnapshot || { Regions: Vol?.Regions || [{ X0:-1e6,Z0:-1e6,X1:1e6,Z1:1e6, Edge:4 }] };

            // 2) Run no-alloc sculpt (pure voxel math)
            const Res = ApplySculptSegmentToVolumeNoAlloc(Vol, Layer, Segment);

            const DirtyPayloads = [];
            const Transfer = [];
            for (const Slot of Res.DirtyLocalBricks)
            {
                const Base = Slot * BRICK_VOLUME;
                const SliceF16 = Vol.BricksF16.slice(Base, Base + BRICK_VOLUME);
                const SliceF32 = Vol.BricksF32.slice(Base, Base + BRICK_VOLUME);
                DirtyPayloads.push({ LocalSlot: Slot, GlobalSlot: Vol.GlobalSlotByBrick?.[Slot] ?? -1, CellIdx: Vol.BrickCells[Slot], DataF16: SliceF16, DataF32: SliceF32 });
                Transfer.push(SliceF16.buffer, SliceF32.buffer);
            }
            self.postMessage({ Type: 'sculpt-done', RequestId, LayerId, LayerIndex, DirtyPayloads, NeedsRepack: false }, Transfer);
        }
        catch (Err)
        {
            self.postMessage({ Type: 'sculpt-done', RequestId, LayerId, LayerIndex, Error: Err instanceof Error ? Err.message : String(Err) });
        }
        return;
    }

    if (Payload.Type === 'sync-volume-slots')
    {
        const { LayerId, LayerIndex, GlobalSlotByBrick, BrickCapacity } = Payload;
        const Key = keyFor(LayerId, LayerIndex ?? 0);
        let Vol = StoredVolumes.get(Key);
        if (!Vol) Vol = StoredVolumes.get(`__idx_${LayerIndex ?? 0}`);
        if (!Vol) return;
        if (GlobalSlotByBrick) {
            Vol.GlobalSlotByBrick = new Int32Array(GlobalSlotByBrick);
            Vol.BrickCapacity = BrickCapacity ?? Vol.BrickCapacity;
        }
        if (Payload.IndirectionYOffset !== undefined) Vol.IndirectionYOffset = Payload.IndirectionYOffset;
    }

    if (Payload.Type === 'sync-all-volumes')
    {
        StoredVolumes.clear();
    }

    if (Payload.Type === 'update-bricks')
    {
        const { LayerId, LayerIndex, Updates } = Payload;
        const Key = keyFor(LayerId, LayerIndex ?? 0);
        let Vol = StoredVolumes.get(Key);
        if (!Vol) Vol = StoredVolumes.get(`__idx_${LayerIndex ?? 0}`);
        if (!Vol || !Updates) return;
        for (const U of Updates)
        {
            const Base = U.LocalSlot * BRICK_VOLUME;
            if (Base + BRICK_VOLUME > Vol.BricksF32.length) continue;
            if (U.DataF32) Vol.BricksF32.set(new Float32Array(U.DataF32), Base);
            if (U.DataF16) Vol.BricksF16.set(new Uint16Array(U.DataF16), Base);
        }
    }
};
