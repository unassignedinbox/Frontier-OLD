/*==========================================================================================================================================
                                                             WORKSPACE.JS
==========================================================================================================================================*/
// 🧩 Author-Time Global 3D SDF Landscape Editor Controller:
//    • Zero LOD: independent per-layer 3D SDF resolutions starting at 512×512×128 and scaling up (e.g. 1024×1024×256 Desert + 512×512×128 Rocky).
//    • Concurrent off-thread Web Worker SDF builders + shared R16F hardware-trilinear sparse brick atlas.
//    • Unified ray-space 24-wave Ocean + Global SDF rendering (zero clipping, zero depth/order fighting).
//    • Real-time 60 FPS 3D volumetric sculpting (Clay, Carve, Smooth, Flatten) with sub-millisecond brick-delta Undo/Redo.

import {
    BiomePresets,
    ResolveLayerResolution,
    ApplyLayerResolution,
    CreateLayer,
    CreateDefaultWorld,
    CreateAppliedShowcaseWorld,
    ValidateDocument,
    InvalidateLandforms,
    MaskDistance,
    EnsureLayerId
} from './GlobalSdfField.js';

import {
    BuildLayerSparseSdf,
    PackGlobalSdfForGpu,
    RaycastGlobalSdfVolumes,
    ExportLayerSparseArchive,
    BRICK_VOXELS,
    BRICK_VOLUME
} from './GlobalSdfBrickAtlas.js';

import {
    ApplySculptSegmentToVolume,
    ApplySculptSegmentToVolumeAsync,
    ApplySculptSegmentToVolumeNoAlloc,
    CaptureBrickSnapshotsAfterStroke,
    ApplyBrickSnapshotDelta,
    AllocateBrickInVolume
} from './GlobalSdfSculpt.js';

import {
    GlobalSdfRenderer,
    ComputeCameraBasis,
    UnprojectScreenRay
} from './GlobalSdfRenderer.js';

import { InitializeCameraNavigation } from './CameraNavigation.js';
import { InitializeLayerStackPanel } from './LayerStackPanel.js';
import { InitializeGlyphs, StackGlyph } from './StackGlyphs.js';

//------------------------------------------------------------------------------------------------------------------------
//                                      DOM HELPERS & APPLICATION STATE
//------------------------------------------------------------------------------------------------------------------------

const Find    = (Sel) => document.querySelector(Sel);
const FindAll = (Sel) => [...document.querySelectorAll(Sel)];

function SetSliderProgress(Input)
{
    if (!Input) return;
    const Min = Number(Input.min) || 0;
    const Max = Number(Input.max) || 1;
    const Val = Number(Input.value) || 0;
    const Pct = Max > Min ? ((Val - Min) / (Max - Min)) * 100 : 0;
    Input.style.setProperty('--progress', `${Math.max(0, Math.min(100, Pct))}%`);
}

InitializeGlyphs(document);

const PreviewCanvas = Find('#Preview');
const PaintCanvas   = Find('#Paint');
const ViewportEl    = Find('#Viewport');

let Layers         = CreateDefaultWorld();
let ActiveIndex    = 0;
const SelectedSet  = new Set([0]);
let WorkspaceMode  = 'landscape'; // 'landscape' | 'mask'
let InspectorTab   = 'shape';     // 'shape' | 'sculpt' | 'ocean'
let ActiveTool     = 'orbit';     // 'orbit' | 'pan' | 'clay' | 'carve' | 'smooth' | 'flatten'

const OceanState = {
    Enabled: true,
    WaveAmplitude: 0.65,
    SunElevationDeg: 36,
    SunAzimuthDeg: 44
};

const SculptSettings = {
    Mode: 'clay',
    Radius: 18,
    Strength: 0.55,
    Falloff: 0.60
};

const BrushHover = {
    Active: false,
    LayerIndex: -1,
    Center: null,
    Normal: [0, 1, 0]
};

const Camera = {
    Target: [-65, 38, -35],
    Yaw: -0.44,
    Pitch: 0.42,
    Distance: 940
};

const PaintView = {
    CenterX: -50,
    CenterZ: -40,
    MetresPerPixel: 1.85,
    BrushRadius: 85,
    Erase: false
};

// Sparse 3D SDF state per layer + packed WebGL2 state
let Volumes     = new Array(Layers.length).fill(null);
let PackedState = null;

// Undo / Redo history (supports both full-document edits AND sub-ms 3D brick-delta sculpt strokes!)
const UndoStack = [];
const RedoStack = [];
const MAX_HISTORY = 40;

//------------------------------------------------------------------------------------------------------------------------
//                                  CONCURRENT WEB WORKER POOL FOR PER-LAYER 3D SDF BUILDS
//------------------------------------------------------------------------------------------------------------------------

const WORKER_COUNT = Math.max(1, Math.min(4, (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) ? Math.min(3, navigator.hardwareConcurrency) : 3));
const WorkerPool   = [];
const BuildQueue   = [];
const LayerEpoch   = new Map();
let NextRequestId  = 1;
let ActiveBuilds   = 0;
let WorldResetEpoch = 0;

// LayerId -> Worker Slot that holds its live volume copy (for sculpt routing)
const LayerOwner = new Map();
const SculptPendingByReq = new Map(); // RequestId -> { LayerId, LayerIndex }

function InitWorkerPool()
{
    if (typeof Worker === 'undefined') return;
    for (let I = 0; I < WORKER_COUNT; I++)
    {
        try
        {
            const W = new Worker(new URL('./GlobalSdfSculptWorker.js', import.meta.url), { type: 'module' });
            const Slot = { Worker: W, Busy: false, Index: I };
            W.onmessage = (Ev) => HandleWorkerMessage(Slot, Ev.data);
            W.onerror   = (Err) =>
            {
                Slot.Busy = false;
                ActiveBuilds = Math.max(0, ActiveBuilds - 1);
                UpdateBuildBadge();
                ReportStatus(`Worker warning: ${Err.message || 'SDF worker error'}`);
                PumpBuildQueue();
                PumpSculptQueue();
            };
            WorkerPool.push(Slot);
        }
        catch
        {
            break;
        }
    }
}

function UpdateBuildBadge()
{
    const Badge = Find('#BuildStatusBadge');
    if (!Badge) return;
    const Pending = ActiveBuilds + BuildQueue.length;
    Badge.hidden = Pending <= 0;
    if (Pending > 0)
    {
        Badge.textContent = `BUILDING GLOBAL SDF (${Pending} LAYER${Pending > 1 ? 'S' : ''})…`;
    }
}

function PumpBuildQueue()
{
    UpdateBuildBadge();
    if (!BuildQueue.length) return;

    if (!WorkerPool.length)
    {
        // Synchronous fallback if Workers unavailable
        const Job = BuildQueue.shift();
        if (!Job || Job.WorldEpoch !== WorldResetEpoch || LayerEpoch.get(Job.LayerId) !== Job.Epoch) return;
        const Vol = BuildLayerSparseSdf(Job.LayerClone, Job.LayerIndex);
        CommitBuiltVolume(Job.LayerIndex, Job.LayerId, Job.Epoch, Job.WorldEpoch, Vol);
        PumpBuildQueue();
        return;
    }

    for (const Slot of WorkerPool)
    {
        if (Slot.Busy || !BuildQueue.length) continue;
        const Job = BuildQueue.shift();
        if (Job.WorldEpoch !== WorldResetEpoch || LayerEpoch.get(Job.LayerId) !== Job.Epoch) continue;

        Slot.Busy = true;
        ActiveBuilds++;
        UpdateBuildBadge();
        Slot.Worker.postMessage({
            Type: 'build-layer',
            RequestId: Job.RequestId,
            LayerIndex: Job.LayerIndex,
            LayerId: Job.LayerId,
            Layer: Job.LayerClone
        });
    }
}

function HandleWorkerMessage(Slot, Data)
{
    // Sculpt completion uses separate busy accounting
    if (Data && Data.Type === 'sculpt-done')
    {
        Slot.Busy = false;
        const Pending = SculptPendingByReq.get(Data.RequestId);
        SculptPendingByReq.delete(Data.RequestId);
        if (!Pending)
        {
            PumpSculptQueue();
            return;
        }
        if (Data.Error)
        {
            Slot.Busy = false;
            SculptInFlight = false;
            ReportStatus(`Sculpt error: ${Data.Error}`);
            // fallback to sync CPU for this dab
            const FallbackJob = Pending.Job;
            if (FallbackJob)
            {
                try {
                    const Vol = Volumes[FallbackJob.LayerIndex];
                    const Lay = Layers[FallbackJob.LayerIndex];
                    const R = ApplySculptSegmentToVolume(Vol, Lay, PackedState, FallbackJob.Segment, FallbackJob.PreSnapshots);
                    if (R.DirtyLocalBricks.length) Renderer?.UploadDirtyBricks(Vol, R.DirtyLocalBricks, R.DirtyIndirectionBox);
                } catch {}
            }
            PumpSculptQueue();
            return;
        }
        // Apply dirty payloads to main Volume
        const TargetIdx = Layers.findIndex(L => L.Id === Pending.LayerId);
        const VolIdx = TargetIdx >= 0 ? TargetIdx : Pending.LayerIndex;
        const Vol = Volumes[VolIdx];
        if (Vol && Data.DirtyPayloads)
        {
            const DirtySlots = [];
            const PreSnapshots = Pending.Job?.PreSnapshots;
            for (const P of Data.DirtyPayloads)
            {
                const SlotId = P.LocalSlot;
                // capture undo snapshot before overwrite
                if (PreSnapshots && !PreSnapshots.has(SlotId))
                {
                    const Base = SlotId * BRICK_VOLUME;
                    if (Base + BRICK_VOLUME <= Vol.BricksF32.length)
                        PreSnapshots.set(SlotId, Vol.BricksF32.slice(Base, Base + BRICK_VOLUME));
                }
                const Base = SlotId * BRICK_VOLUME;
                // ensure capacity (should already be grown on main during allocation)
                if (Base + BRICK_VOLUME > Vol.BricksF32.length)
                {
                    // grow to match worker (should not happen often)
                    const Need = SlotId + 1;
                    if (Need > Vol.BrickCapacity)
                    {
                        const NextCap = Math.max(Need, Vol.BrickCapacity + 512);
                        const NextF32 = new Float32Array(NextCap * BRICK_VOLUME);
                        const NextF16 = new Uint16Array(NextCap * BRICK_VOLUME);
                        const NextCells = new Uint32Array(NextCap);
                        const NextSlots = new Int32Array(NextCap).fill(-1);
                        NextF32.set(Vol.BricksF32.subarray(0, Vol.AllocatedCount * BRICK_VOLUME));
                        NextF16.set(Vol.BricksF16.subarray(0, Vol.AllocatedCount * BRICK_VOLUME));
                        NextCells.set(Vol.BrickCells.subarray(0, Vol.AllocatedCount));
                        if (Vol.GlobalSlotByBrick) NextSlots.set(Vol.GlobalSlotByBrick.subarray(0, Vol.AllocatedCount));
                        Vol.BricksF32 = NextF32; Vol.BricksF16 = NextF16; Vol.BrickCells = NextCells; Vol.GlobalSlotByBrick = NextSlots; Vol.BrickCapacity = NextCap;
                    }
                    if (Vol.AllocatedCount <= SlotId) Vol.AllocatedCount = SlotId + 1;
                }
                Vol.BricksF32.set(P.DataF32, Base);
                Vol.BricksF16.set(P.DataF16, Base);
                DirtySlots.push(SlotId);
            }
            if (DirtySlots.length) Renderer?.UploadDirtyBricks(Vol, DirtySlots, null);
        }
        Slot.Busy = false;
        SculptInFlight = false;
        PumpSculptQueue();
        return;
    }

    Slot.Busy = false;
    ActiveBuilds = Math.max(0, ActiveBuilds - 1);

    if (Data && Data.Type === 'layer-built')
    {
        const JobMeta = PendingByReq.get(Data.RequestId);
        PendingByReq.delete(Data.RequestId);
        if (JobMeta)
        {
            // remember which worker owns this layer for sculpt routing
            LayerOwner.set(JobMeta.LayerId, Slot);
            CommitBuiltVolume(JobMeta.LayerIndex, JobMeta.LayerId, JobMeta.Epoch, JobMeta.WorldEpoch, Data.Volume);
        }
    }
    else if (Data && Data.Type === 'layer-error')
    {
        PendingByReq.delete(Data.RequestId);
        ReportStatus(`SDF build failed: ${Data.Message}`);
    }

    PumpBuildQueue();
    // sculpt queue may have been waiting for a worker to become free
    PumpSculptQueue();
}

const PendingByReq = new Map();

function ScheduleLayerBuild(LayerIndex)
{
    const Layer = Layers[LayerIndex];
    if (!Layer) return;
    const LayerId = EnsureLayerId(Layer, LayerIndex);
    const Epoch   = (LayerEpoch.get(LayerId) || 0) + 1;
    LayerEpoch.set(LayerId, Epoch);

    // Remove any queued older job for the same layer
    for (let I = BuildQueue.length - 1; I >= 0; I--)
    {
        if (BuildQueue[I].LayerId === LayerId) BuildQueue.splice(I, 1);
    }

    const RequestId = NextRequestId++;
    const Job = {
        RequestId,
        LayerIndex,
        LayerId,
        Epoch,
        WorldEpoch: WorldResetEpoch,
        LayerClone: structuredClone(Layer)
    };
    PendingByReq.set(RequestId, Job);
    BuildQueue.push(Job);
    PumpBuildQueue();
}

function ScheduleAllLayersBuild(ResetVolumes = false)
{
    if (ResetVolumes)
    {
        WorldResetEpoch++;
        BuildQueue.length = 0;
        PendingByReq.clear();
        SculptQueue.length = 0;
        SculptPendingByReq.clear();
        LayerOwner.clear();
        Volumes = new Array(Layers.length).fill(null);
        // tell workers to drop old volumes
        for (const Slot of WorkerPool) { try { Slot.Worker.postMessage({ Type: 'sync-all-volumes' }); } catch {} }
    }
    else
    {
        Volumes.length = Layers.length;
    }
    for (let I = 0; I < Layers.length; I++)
    {
        EnsureLayerId(Layers[I], I);
        ScheduleLayerBuild(I);
    }
}

function RepackAndUploadAll()
{
    // Keep LayerIndex synchronized with current Layers array ordering
    const ReadyVolumes = [];
    for (let I = 0; I < Layers.length; I++)
    {
        if (Volumes[I])
        {
            Volumes[I].LayerIndex = I;
            ReadyVolumes.push(Volumes[I]);
        }
    }
    PackedState = PackGlobalSdfForGpu(ReadyVolumes, 3072);
    if (Renderer)
    {
        Renderer.UploadPackedState(PackedState, Layers);
    }
    RefreshDiagnosticsUI();
    SyncWorkersAfterRepack();
}

function SyncWorkersAfterRepack()
{
    if (!WorkerPool.length || !PackedState) return;
    for (let I = 0; I < Volumes.length; I++)
    {
        const Vol = Volumes[I];
        if (!Vol) continue;
        const Layer = Layers[I];
        if (!Layer?.Id) continue;
        const Owner = LayerOwner.get(Layer.Id);
        if (!Owner) continue;
        try {
            const Copy = new Int32Array(Vol.GlobalSlotByBrick);
            Owner.Worker.postMessage({ Type: 'sync-volume-slots', LayerId: Layer.Id, LayerIndex: I, GlobalSlotByBrick: Copy, BrickCapacity: Vol.BrickCapacity, IndirectionYOffset: Vol.IndirectionYOffset }, [Copy.buffer]);
        } catch {}
    }
}

//------------------------------------------------------------------------------------------------------------------------
//                               OFF-THREAD SCULPT: ALLOCATION + WORKER QUEUE
//------------------------------------------------------------------------------------------------------------------------

let SculptQueue = [];
let SculptInFlight = false;

function allocateMissingBricksForSegment(Volume, PackedStateLocal, Segment)
{
    if (!Volume || !PackedStateLocal) return { newBricks: [], dirtyBox: null, needsRepack: false };
    const { A, B, Mode, Radius, Falloff } = Segment;
    const EffRadius = Math.max(0.5, Math.min(48, Radius));
    const BlendWidth = EffRadius * (0.18 + 0.45 * (Falloff ?? 0.6));
    const Reach = EffRadius + BlendWidth + Volume.MaxVoxel * 2.0;
    const [BMinX, BMinY, BMinZ] = Volume.BoundsMin;
    const [DX, DY, DZ] = Volume.Pitch;
    const [GX, GY, GZ] = Volume.GridDims;
    const SegMinX = Math.min(A[0], B[0]) - Reach, SegMaxX = Math.max(A[0], B[0]) + Reach;
    const SegMinY = Math.min(A[1], B[1]) - Reach, SegMaxY = Math.max(A[1], B[1]) + Reach;
    const SegMinZ = Math.min(A[2], B[2]) - Reach, SegMaxZ = Math.max(A[2], B[2]) + Reach;
    const IX0 = Math.max(0, Math.floor(((SegMinX - BMinX) / DX - 1) / BRICK_VOXELS));
    const IX1 = Math.min(GX - 1, Math.floor(((SegMaxX - BMinX) / DX + 1) / BRICK_VOXELS));
    const IY0 = Math.max(0, Math.floor(((SegMinY - BMinY) / DY - 1) / BRICK_VOXELS));
    const IY1 = Math.min(GY - 1, Math.floor(((SegMaxY - BMinY) / DY + 1) / BRICK_VOXELS));
    const IZ0 = Math.max(0, Math.floor(((SegMinZ - BMinZ) / DZ - 1) / BRICK_VOXELS));
    const IZ1 = Math.min(GZ - 1, Math.floor(((SegMaxZ - BMinZ) / DZ + 1) / BRICK_VOXELS));
    const ABX = B[0]-A[0], ABY=B[1]-A[1], ABZ=B[2]-A[2];
    const ABL2 = ABX*ABX+ABY*ABY+ABZ*ABZ;
    const InvABL2 = ABL2>1e-10?1/ABL2:0;
    const DirtyBox = [Infinity,Infinity,Infinity,-Infinity,-Infinity,-Infinity];
    const newBricks = [];
    let needsRepack = false;
    for (let IZ=IZ0; IZ<=IZ1; IZ++) for (let IY=IY0; IY<=IY1; IY++) for (let IX=IX0; IX<=IX1; IX++)
    {
        const CX = BMinX + (IX + 0.5) * BRICK_VOXELS * DX;
        const CY = BMinY + (IY + 0.5) * BRICK_VOXELS * DY;
        const CZ = BMinZ + (IZ + 0.5) * BRICK_VOXELS * DZ;
        const TC = InvABL2>0 ? Math.max(0, Math.min(1, ((CX-A[0])*ABX + (CY-A[1])*ABY + (CZ-A[2])*ABZ) * InvABL2)) : 0;
        const DistCentre = Math.hypot(CX - A[0] - TC*ABX, CY - A[1] - TC*ABY, CZ - A[2] - TC*ABZ);
        const BrickDiag = 0.5 * Math.hypot(10*DX,10*DY,10*DZ);
        if (DistCentre > Reach + BrickDiag) continue;
        const CellIdx=(IZ*GY+IY)*GX+IX;
        if (Volume.BrickIndexByCell[CellIdx] >=0) continue;
        if (Mode==='smooth') continue;
        if ((Mode==='carve'||Mode==='remove') && !Volume.IsSolidCore[CellIdx]) continue;
        const LocalSlot = AllocateBrickInVolume(Volume, PackedStateLocal, IX, IY, IZ, DirtyBox);
        if (LocalSlot <0) { needsRepack = true; continue; }
        newBricks.push({ IX, IY, IZ, CellIdx, LocalSlot, GlobalSlot: Volume.GlobalSlotByBrick[LocalSlot] });
    }
    const hasBox = DirtyBox[0] <= DirtyBox[3];
    return { newBricks, dirtyBox: hasBox ? DirtyBox : null, needsRepack };
}

function PumpSculptQueue()
{
    if (SculptInFlight) return;
    if (!SculptQueue.length) return;
    const Job = SculptQueue[0];
    const Layer = Layers[Job.LayerIndex];
    if (!Layer) { SculptQueue.shift(); PumpSculptQueue(); return; }
    const LayerId = Layer.Id;
    let Owner = LayerOwner.get(LayerId);
    if (!Owner) {
        Owner = WorkerPool.find(S => !S.Busy) || WorkerPool[0];
        if (!Owner) return;
    }
    if (Owner.Busy) return;
    SculptQueue.shift();
    SculptInFlight = true;
    Owner.Busy = true;
    const RequestId = NextRequestId++;
    SculptPendingByReq.set(RequestId, { LayerId, LayerIndex: Job.LayerIndex, Job });
    const LayerSnap = { Regions: Layer.Regions ? Layer.Regions.map(R=>({ X0:R.X0,Z0:R.Z0,X1:R.X1,Z1:R.Z1, Edge:R.Edge })) : [] };
    if (!WorkerPool.length)
    {
        Owner.Busy = false; SculptInFlight=false;
        try {
            const Vol = Volumes[Job.LayerIndex];
            const R = ApplySculptSegmentToVolume(Vol, Layer, PackedState, Job.Segment, Job.PreSnapshots);
            if (R.DirtyLocalBricks.length) Renderer?.UploadDirtyBricks(Vol, R.DirtyLocalBricks, R.DirtyIndirectionBox);
            if (R.NeedsRepack) { RepackAndUploadAll(); }
        } catch {}
        PumpSculptQueue();
        return;
    }
    Owner.Worker.postMessage({ Type: 'sculpt-dab', RequestId, LayerId, LayerIndex: Job.LayerIndex, Segment: Job.Segment, LayerSnapshot: LayerSnap, NewBricks: Job.NewBricks });
}

function enqueueSculptDab(Vol, Layer, Segment, PreSnapshots)
{
    if (!Vol || !Layer || !PackedState) return;
    if (!WorkerPool.length)
    {
        const { DirtyLocalBricks, DirtyIndirectionBox, NeedsRepack } = ApplySculptSegmentToVolume(Vol, Layer, PackedState, Segment, PreSnapshots);
        if (NeedsRepack) RepackAndUploadAll(); else if (DirtyLocalBricks.length) Renderer?.UploadDirtyBricks(Vol, DirtyLocalBricks, DirtyIndirectionBox);
        return;
    }
    const { newBricks, dirtyBox, needsRepack } = allocateMissingBricksForSegment(Vol, PackedState, Segment);
    if (needsRepack)
    {
        RepackAndUploadAll();
        const retry = allocateMissingBricksForSegment(Vol, PackedState, Segment);
        if (retry.dirtyBox) Renderer?.UploadDirtyBricks(Vol, [], retry.dirtyBox);
        if (retry.newBricks.length) newBricks.push(...retry.newBricks);
        else if (dirtyBox) Renderer?.UploadDirtyBricks(Vol, [], dirtyBox);
        const LayerIndex = Layers.indexOf(Layer);
        SculptQueue.push({ LayerIndex, NewBricks: newBricks, Segment, PreSnapshots });
        PumpSculptQueue();
        return;
    }
    if (dirtyBox) Renderer?.UploadDirtyBricks(Vol, [], dirtyBox);
    const LayerIndex = Layers.indexOf(Layer);
    SculptQueue.push({ LayerIndex, NewBricks: newBricks, Segment, PreSnapshots });
    PumpSculptQueue();
}

function CommitBuiltVolume(LayerIndex, LayerId, Epoch, JobWorldEpoch, Volume)
{
    if (JobWorldEpoch !== WorldResetEpoch) return;
    const CurrentIdx = Layers.findIndex((L) => L.Id === LayerId);
    if (CurrentIdx < 0) return;
    if (LayerEpoch.get(LayerId) !== Epoch) return;

    Volume.LayerIndex = CurrentIdx;
    Volumes[CurrentIdx] = Volume;
    RepackAndUploadAll();

    const [RX, RY, RZ] = Volume.Resolution;
    ReportStatus(`Built "${Layers[CurrentIdx].Name}" 3D SDF (${RX}×${RZ}×${RY}, ${Volume.AllocatedCount.toLocaleString()} bricks in ${Volume.BuildMs.toFixed(0)} ms).`);
}

//------------------------------------------------------------------------------------------------------------------------
//                                  RENDERER & SUN DIRECTION INITIALIZATION
//------------------------------------------------------------------------------------------------------------------------

let Renderer = null;
try
{
    Renderer = new GlobalSdfRenderer(PreviewCanvas);
}
catch (Err)
{
    ReportStatus(Err.message);
}

function ComputeSunDirection()
{
    const Elev = (OceanState.SunElevationDeg * Math.PI) / 180;
    const Azim = (OceanState.SunAzimuthDeg * Math.PI) / 180;
    return [
        Math.cos(Elev) * Math.sin(Azim),
        Math.sin(Elev),
        Math.cos(Elev) * Math.cos(Azim)
    ];
}

function ReportStatus(Message)
{
    const El = Find('#StatusMessage');
    if (El) El.textContent = Message;
}

//------------------------------------------------------------------------------------------------------------------------
//                                  DOCUMENT HISTORY (FULL STATE & 3D SCULPT DELTAS)
//------------------------------------------------------------------------------------------------------------------------

function SyncHistoryButtons()
{
    Find('#Undo').disabled = UndoStack.length === 0;
    Find('#Redo').disabled = RedoStack.length === 0;
}

function PushDocumentSnapshot()
{
    UndoStack.push({
        Kind: 'document',
        Layers: structuredClone(Layers),
        ActiveIndex,
        StackSelection: StackPanel ? StackPanel.RetrieveSelection() : null
    });
    if (UndoStack.length > MAX_HISTORY) UndoStack.shift();
    RedoStack.length = 0;
    SyncHistoryButtons();
}

function PushSculptDeltaEntry(LayerIndex, StrokeClone, BrickSnapshots)
{
    UndoStack.push({
        Kind: 'sculpt-delta',
        LayerId: Layers[LayerIndex]?.Id,
        StrokeClone: structuredClone(StrokeClone),
        BrickSnapshots
    });
    if (UndoStack.length > MAX_HISTORY) UndoStack.shift();
    RedoStack.length = 0;
    SyncHistoryButtons();
}

function PerformUndo()
{
    const Entry = UndoStack.pop();
    if (!Entry) return;

    if (Entry.Kind === 'sculpt-delta')
    {
        const Idx = Layers.findIndex((L) => L.Id === Entry.LayerId);
        const Vol = Idx >= 0 ? Volumes[Idx] : null;
        if (Idx >= 0 && Vol && Layers[Idx].Regions[0]?.SculptStrokes?.length)
        {
            Layers[Idx].Regions[0].SculptStrokes.pop();
            const Dirty = ApplyBrickSnapshotDelta(Vol, Entry.BrickSnapshots, false);
            Renderer?.UploadDirtyBricks(Vol, Dirty, null);
            { const Owner = LayerOwner.get(Entry.LayerId); if (Owner && Dirty.length) { const Updates = Dirty.map(Slot=>{ const B=Slot*BRICK_VOLUME; return { LocalSlot: Slot, DataF32: Vol.BricksF32.slice(B,B+BRICK_VOLUME), DataF16: Vol.BricksF16.slice(B,B+BRICK_VOLUME) }; }); try { Owner.Worker.postMessage({ Type:'update-bricks', LayerId: Entry.LayerId, LayerIndex: Idx, Updates }); } catch {} } }
            RedoStack.push(Entry);
            SyncHistoryButtons();
            RefreshUI();
            ReportStatus(`Undid 3D sculpt stroke on "${Layers[Idx].Name}".`);
            return;
        }
    }

    RedoStack.push({
        Kind: 'document',
        Layers: structuredClone(Layers),
        ActiveIndex,
        StackSelection: StackPanel ? StackPanel.RetrieveSelection() : null
    });
    RestoreDocumentSnapshot(Entry);
    SyncHistoryButtons();
    ReportStatus('Undid landscape change.');
}

function PerformRedo()
{
    const Entry = RedoStack.pop();
    if (!Entry) return;

    if (Entry.Kind === 'sculpt-delta')
    {
        const Idx = Layers.findIndex((L) => L.Id === Entry.LayerId);
        const Vol = Idx >= 0 ? Volumes[Idx] : null;
        if (Idx >= 0 && Vol && Layers[Idx].Regions[0])
        {
            Layers[Idx].Regions[0].SculptStrokes.push(structuredClone(Entry.StrokeClone));
            const Dirty = ApplyBrickSnapshotDelta(Vol, Entry.BrickSnapshots, true);
            Renderer?.UploadDirtyBricks(Vol, Dirty, null);
            { const Owner = LayerOwner.get(Entry.LayerId); if (Owner && Dirty.length) { const Updates = Dirty.map(Slot=>{ const B=Slot*BRICK_VOLUME; return { LocalSlot: Slot, DataF32: Vol.BricksF32.slice(B,B+BRICK_VOLUME), DataF16: Vol.BricksF16.slice(B,B+BRICK_VOLUME) }; }); try { Owner.Worker.postMessage({ Type:'update-bricks', LayerId: Entry.LayerId, LayerIndex: Idx, Updates }); } catch {} } }
            UndoStack.push(Entry);
            SyncHistoryButtons();
            RefreshUI();
            ReportStatus(`Redid 3D sculpt stroke on "${Layers[Idx].Name}".`);
            return;
        }
    }

    UndoStack.push({
        Kind: 'document',
        Layers: structuredClone(Layers),
        ActiveIndex,
        StackSelection: StackPanel ? StackPanel.RetrieveSelection() : null
    });
    RestoreDocumentSnapshot(Entry);
    SyncHistoryButtons();
    ReportStatus('Redid landscape change.');
}

function RestoreDocumentSnapshot(Snapshot)
{
    const PrevById = new Map(Layers.map((L, I) => [L.Id, { JSON: JSON.stringify(L), Vol: Volumes[I] }]));
    Layers = structuredClone(Snapshot.Layers);
    ActiveIndex = Math.max(0, Math.min(Layers.length - 1, Snapshot.ActiveIndex ?? 0));
    SelectedSet.clear();
    SelectedSet.add(ActiveIndex);
    if (Snapshot.StackSelection && StackPanel) StackPanel.RestoreSelection(Snapshot.StackSelection);

    const NextVolumes = new Array(Layers.length).fill(null);
    let ReusedAll = true;
    for (let I = 0; I < Layers.length; I++)
    {
        EnsureLayerId(Layers[I], I);
        const Prev = PrevById.get(Layers[I].Id);
        if (Prev && Prev.Vol && Prev.JSON === JSON.stringify(Layers[I]))
        {
            NextVolumes[I] = Prev.Vol;
        }
        else
        {
            ReusedAll = false;
        }
    }
    Volumes = NextVolumes;
    RepackAndUploadAll();

    if (!ReusedAll)
    {
        for (let I = 0; I < Layers.length; I++)
        {
            if (!Volumes[I]) ScheduleLayerBuild(I);
        }
    }
    RefreshUI();
}

//------------------------------------------------------------------------------------------------------------------------
//                              CAMERA NAVIGATION & LAYER STACK PANEL INTEGRATION
//------------------------------------------------------------------------------------------------------------------------

const CameraNav = InitializeCameraNavigation({
    Canvas: PreviewCanvas,
    Camera,
    RetrieveWorkspace: () => WorkspaceMode
});

const StackPanel = InitializeLayerStackPanel({
    RetrieveLayers: () => Layers,
    ReplaceLayers: (Next) => { Layers = Next; },
    RetrieveActive: () => ActiveIndex,
    RetrieveSelected: () => SelectedSet,
    RetrieveWorkspace: () => WorkspaceMode,
    RetrieveInspector: () => InspectorTab,
    ActivateInspector: (Tab) => SetInspectorTab(Tab),
    Remember: () => PushDocumentSnapshot(),
    Changed: (GeometryChanged) =>
    {
        InvalidateLandforms();
        if (GeometryChanged)
        {
            if (SelectedSet.size > 1)
            {
                for (const Idx of SelectedSet) ScheduleLayerBuild(Idx);
            }
            else
            {
                ScheduleLayerBuild(ActiveIndex);
            }
        }
        else
        {
            Renderer?.SyncLayerUniforms(Layers);
        }
        RefreshUI();
    },
    Refresh: () => RefreshUI(),
    Report: (Msg) => ReportStatus(Msg)
});

//------------------------------------------------------------------------------------------------------------------------
//                                  OUTLINER, THUMBNAILS & INSPECTOR UI REFRESH
//------------------------------------------------------------------------------------------------------------------------

function DrawMaskThumbnail(ThumbCanvas, Layer)
{
    const Ctx = ThumbCanvas.getContext('2d');
    const W = ThumbCanvas.width  = 60;
    const H = ThumbCanvas.height = 56;
    Ctx.clearRect(0, 0, W, H);

    const Reg = Layer.Regions[0];
    if (!Reg) return;
    const Span = Math.max(Reg.X1 - Reg.X0, Reg.Z1 - Reg.Z0, 120) * 1.15;
    const CX = 0.5 * (Reg.X0 + Reg.X1);
    const CZ = 0.5 * (Reg.Z0 + Reg.Z1);

    Ctx.fillStyle = Layer.Color || '#c8a374';
    for (let PY = 0; PY < H; PY += 2)
    {
        const WZ = CZ + ((PY + 1) / H - 0.5) * Span;
        for (let PX = 0; PX < W; PX += 2)
        {
            const WX = CX + ((PX + 1) / W - 0.5) * Span;
            if (MaskDistance(Layer, WX, WZ) <= 0)
            {
                Ctx.fillRect(PX, PY, 2, 2);
            }
        }
    }
}

function RenderLayerDirectory()
{
    const Container = Find('#MaskRows');
    Container.replaceChildren();
    Find('#LayerCount').textContent = String(Layers.length);

    Layers.forEach((Layer, Index) =>
    {
        const [RX, RY, RZ] = ResolveLayerResolution(Layer);
        const Row = document.createElement('div');
        Row.className = `mask-row ${Index === ActiveIndex ? 'selected' : ''} ${SelectedSet.has(Index) ? 'batch-selected' : ''} ${!Layer.Visible ? 'muted' : ''}`;
        Row.setAttribute('role', 'option');
        Row.setAttribute('aria-selected', String(Index === ActiveIndex));

        if (SelectedSet.size > 1 && SelectedSet.has(Index))
        {
            const Dot = document.createElement('span');
            Dot.className = 'batch-mark';
            Row.append(Dot);
        }

        const ThumbWrap = document.createElement('div');
        ThumbWrap.className = 'mask-thumbnail';
        const ThumbCanvas = document.createElement('canvas');
        DrawMaskThumbnail(ThumbCanvas, Layer);
        ThumbWrap.append(ThumbCanvas);

        const Copy = document.createElement('div');
        Copy.className = 'mask-copy';
        const Title = document.createElement('strong');
        Title.textContent = Layer.Name;
        const Sub = document.createElement('small');
        Sub.textContent = `${RX}×${RZ}×${RY} SDF · ${(Layer.Landforms || []).length} ops`;
        Copy.append(Title, Sub);

        const EyeBtn = document.createElement('button');
        EyeBtn.type = 'button';
        EyeBtn.className = 'icon-button';
        EyeBtn.title = Layer.Visible ? 'Hide SDF layer' : 'Show SDF layer';
        EyeBtn.append(StackGlyph(Layer.Visible ? 'eye' : 'eye-off'));
        EyeBtn.onclick = (Ev) =>
        {
            Ev.stopPropagation();
            Layer.Visible = !Layer.Visible;
            Renderer?.SyncLayerUniforms(Layers);
            RefreshUI();
        };

        Row.onclick = (Ev) =>
        {
            if (Ev.shiftKey)
            {
                if (SelectedSet.has(Index) && SelectedSet.size > 1) SelectedSet.delete(Index);
                else SelectedSet.add(Index);
                ActiveIndex = Index;
            }
            else
            {
                ActiveIndex = Index;
                SelectedSet.clear();
                SelectedSet.add(Index);
            }
            if (InspectorTab === 'ocean') InspectorTab = 'shape';
            RefreshUI();
        };

        Row.append(ThumbWrap, Copy, EyeBtn);
        Container.append(Row);
    });
}

function SyncLayerSettingsCard()
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;

    const [RX, RY, RZ] = ResolveLayerResolution(Layer);
    Find('#HeaderActiveLayerName').textContent = Layer.Name;
    Find('#LayerVoxelSummaryPill').textContent = `${RX}×${RZ}×${RY}`;

    const XZSlider = Find('#LayerResXZSlider');
    const YSlider  = Find('#LayerResYSlider');
    XZSlider.value = String(RX);
    YSlider.value  = String(RY);
    Find('#LayerResXZOut').textContent = `${RX} × ${RZ}`;
    Find('#LayerResYOut').textContent  = `${RY}`;
    SetSliderProgress(XZSlider);
    SetSliderProgress(YSlider);

    Find('#LayerBiomeSelect').value  = Layer.Biome || 'desert';
    Find('#LayerSurfaceColor').value = Layer.Color || '#c8a374';
    Find('#LayerCliffColor').value   = Layer.CliffColor || '#8d583a';

    const StrataSlider = Find('#LayerStrataSlider');
    StrataSlider.value = String(Layer.Strata ?? 0.5);
    Find('#LayerStrataOut').textContent = Number(Layer.Strata ?? 0.5).toFixed(2);
    SetSliderProgress(StrataSlider);

    const RoughSlider = Find('#LayerRoughnessSlider');
    RoughSlider.value = String(Layer.Roughness ?? 0.65);
    Find('#LayerRoughnessOut').textContent = Number(Layer.Roughness ?? 0.65).toFixed(2);
    SetSliderProgress(RoughSlider);

    const DepthSlider = Find('#LayerDepthSlider');
    DepthSlider.value = String(Layer.Depth ?? 16);
    Find('#LayerDepthOut').textContent = `${Math.round(Layer.Depth ?? 16)} m`;
    SetSliderProgress(DepthSlider);

    const SculptCount = (Layer.Regions[0]?.SculptStrokes || []).length;
    Find('#SculptStrokeCountPill').textContent = `${SculptCount} stroke${SculptCount === 1 ? '' : 's'}`;
}

function RefreshDiagnosticsUI()
{
    const Layer = Layers[ActiveIndex];
    const Vol   = Volumes[ActiveIndex];

    if (Vol)
    {
        const [PX, PY, PZ] = Vol.Pitch;
        const MemMB = ((Vol.AllocatedCount * 2000) / (1024 * 1024)).toFixed(1);
        Find('#LayerPitchReadout').textContent  = `Voxel pitch: ${PX.toFixed(2)} × ${PY.toFixed(2)} × ${PZ.toFixed(2)} m`;
        Find('#LayerBricksReadout').textContent = `Narrow-band bricks: ${Vol.AllocatedCount.toLocaleString()} (${MemMB} MB R16F) · Zero LOD`;
    }
    else if (Layer)
    {
        const [RX, RY, RZ] = ResolveLayerResolution(Layer);
        Find('#LayerPitchReadout').textContent  = `Building ${RX}×${RZ}×${RY} 3D SDF lattice…`;
        Find('#LayerBricksReadout').textContent = `Off-thread Web Worker active · Zero LOD`;
    }

    const ResLabels = Layers.map((L) =>
    {
        const [X, Y, Z] = ResolveLayerResolution(L);
        return `${X}×${Z}×${Y}`;
    });
    Find('#GeometryQuality').textContent = `${ResLabels.join(' + ')} · No LOD`;

    if (PackedState)
    {
        const [AX, AY, AZ] = PackedState.AtlasTexelDims;
        const TotalMB = (PackedState.AtlasHalfData.byteLength / (1024 * 1024)).toFixed(1);
        const Summary = `Atlas ${AX}×${AY}×${AZ} R16F (${PackedState.TotalBricks.toLocaleString()} bricks · ${TotalMB} MB)`;
        Find('#GlobalBrickSummary').textContent = Summary;
        Find('#RenderReadout').textContent      = Summary;
    }
}

function SetInspectorTab(Tab)
{
    InspectorTab = Tab;
    FindAll('[data-inspector-tab]').forEach((Btn) =>
    {
        Btn.classList.toggle('active', Btn.dataset.inspectorTab === Tab);
    });

    const IsShape  = Tab === 'shape';
    const IsSculpt = Tab === 'sculpt';
    const IsOcean  = Tab === 'ocean';

    Find('#LayerSdfSettingsCard').hidden = !IsShape;
    Find('#OperationInspector').hidden   = !IsShape;
    Find('#SculptInspector').hidden      = !IsSculpt;
    Find('#OceanInspector').hidden       = !IsOcean;
}

function SyncSculptControlsUI()
{
    const IsSculptTool = ['clay', 'carve', 'smooth', 'flatten'].includes(ActiveTool);

    FindAll('[data-sculpt-tool]').forEach((Btn) =>
    {
        Btn.classList.toggle('active', Btn.dataset.sculptTool === ActiveTool);
    });
    FindAll('[data-sculpt-pill]').forEach((Btn) =>
    {
        Btn.classList.toggle('active', Btn.dataset.sculptPill === SculptSettings.Mode);
    });
    FindAll('[data-inspector-sculpt]').forEach((Btn) =>
    {
        Btn.classList.toggle('active', Btn.dataset.inspectorSculpt === SculptSettings.Mode);
    });

    Find('#OrbitCamera').classList.toggle('active', ActiveTool === 'orbit');
    Find('#PanCamera').classList.toggle('active',   ActiveTool === 'pan');
    Find('#SculptHud').hidden = !(WorkspaceMode === 'landscape' && IsSculptTool);

    const ModeTitles = {
        clay: '3D Sculpt · Clay (+)',
        carve: '3D Sculpt · Carve (−)',
        smooth: '3D Sculpt · Smooth',
        flatten: '3D Sculpt · Flatten'
    };
    Find('#SculptHudTitle').textContent = ModeTitles[SculptSettings.Mode] || '3D Sculpt';
    Find('#ActiveToolReadout').textContent = IsSculptTool
        ? `${ModeTitles[SculptSettings.Mode]} · Radius ${SculptSettings.Radius} m · LMB paint on 3D SDF`
        : ActiveTool === 'pan'
            ? 'Camera Pan · Drag to pan target · Wheel zoom'
            : 'Camera Orbit · Drag to orbit · RMB orbit · Wheel zoom';

    for (const [SliderId, OutId, Val, Suffix] of [
        ['#HudBrushRadius',    '#HudBrushRadiusOut',    SculptSettings.Radius,   ' m'],
        ['#InspBrushRadius',   '#InspBrushRadiusOut',   SculptSettings.Radius,   ' m'],
        ['#HudBrushStrength',  '#HudBrushStrengthOut',  SculptSettings.Strength.toFixed(2), ''],
        ['#InspBrushStrength', '#InspBrushStrengthOut', SculptSettings.Strength.toFixed(2), ''],
        ['#HudBrushFalloff',   '#HudBrushFalloffOut',   SculptSettings.Falloff.toFixed(2),  ''],
        ['#InspBrushFalloff',  '#InspBrushFalloffOut',  SculptSettings.Falloff.toFixed(2),  '']
    ])
    {
        const S = Find(SliderId);
        const O = Find(OutId);
        if (S) { S.value = String(Val); SetSliderProgress(S); }
        if (O) { O.textContent = `${Val}${Suffix}`; }
    }

    PreviewCanvas.style.cursor = IsSculptTool ? 'crosshair' : ActiveTool === 'pan' ? 'grab' : 'default';
}

function SetWorkspaceMode(Mode)
{
    WorkspaceMode = Mode;
    const IsMask = Mode === 'mask';

    Find('#MaskWorkspaceTab').classList.toggle('active', IsMask);
    Find('#LandscapeWorkspaceTab').classList.toggle('active', !IsMask);
    PaintCanvas.hidden   = !IsMask;
    PreviewCanvas.hidden = IsMask;
    Find('#MaskInstructions').hidden  = !IsMask;
    Find('#TerrainStackPanel').hidden = IsMask;
    Find('#LandscapeTools').hidden    = IsMask;

    Find('#ViewTitle').textContent = IsMask ? '2D Vector Shoreline Mask' : '3D Global Distance Field';
    Find('#ViewSubtitle').textContent = IsMask
        ? 'Resolution-independent world-metre shoreline strokes'
        : 'Hardware Trilinear R16F Sparse Bricks · Analytic Ray-Ocean';

    if (IsMask) DrawMaskPaintCanvas();
    RefreshUI();
}

function RefreshUI()
{
    RenderLayerDirectory();
    StackPanel.Render();
    SyncLayerSettingsCard();
    SetInspectorTab(InspectorTab);
    SyncSculptControlsUI();
    RefreshDiagnosticsUI();
    if (WorkspaceMode === 'mask') DrawMaskPaintCanvas();
}

//------------------------------------------------------------------------------------------------------------------------
//                              PER-LAYER RESOLUTION, BIOME & OCEAN INSPECTOR BINDINGS
//------------------------------------------------------------------------------------------------------------------------

FindAll('[data-inspector-tab]').forEach((Btn) =>
{
    Btn.onclick = () => SetInspectorTab(Btn.dataset.inspectorTab);
});

Find('#EnvironmentButton').onclick = () => SetInspectorTab('ocean');
Find('#MaskWorkspaceTab').onclick  = () => SetWorkspaceMode('mask');
Find('#LandscapeWorkspaceTab').onclick = () => SetWorkspaceMode('landscape');
Find('#SwitchToMaskEdit').onclick  = () => SetWorkspaceMode('mask');

// Independent Per-Layer Horizontal (X/Z) and Vertical (Y) SDF Resolution Sliders (starting at 512×512×128 and going up!)
Find('#LayerResXZSlider').oninput = (Ev) =>
{
    const V = Number(Ev.target.value);
    Find('#LayerResXZOut').textContent = `${V} × ${V}`;
    SetSliderProgress(Ev.target);
};
Find('#LayerResXZSlider').onchange = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    const [, CurY] = ResolveLayerResolution(Layer);
    const NextXZ = Number(Ev.target.value);
    PushDocumentSnapshot();
    const [RX, RY, RZ] = ApplyLayerResolution(Layer, NextXZ, CurY, NextXZ);
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
    ReportStatus(`Rebuilding "${Layer.Name}" SDF at ${RX}×${RZ}×${RY}…`);
};

Find('#LayerResYSlider').oninput = (Ev) =>
{
    Find('#LayerResYOut').textContent = Ev.target.value;
    SetSliderProgress(Ev.target);
};
Find('#LayerResYSlider').onchange = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    const [CurX, , CurZ] = ResolveLayerResolution(Layer);
    const NextY = Number(Ev.target.value);
    PushDocumentSnapshot();
    const [RX, RY, RZ] = ApplyLayerResolution(Layer, CurX, NextY, CurZ);
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
    ReportStatus(`Rebuilding "${Layer.Name}" SDF at ${RX}×${RZ}×${RY}…`);
};

// Per-Layer Biome Preset & Material Appearance (instant uniform sync — zero SDF rebuild needed!)
Find('#LayerBiomeSelect').onchange = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    const Preset = BiomePresets[Ev.target.value];
    if (!Layer || !Preset) return;
    PushDocumentSnapshot();
    Layer.Biome      = Ev.target.value;
    Layer.Color      = Preset.Color;
    Layer.CliffColor = Preset.CliffColor;
    Layer.Strata     = Preset.Strata;
    Layer.Roughness  = Preset.Roughness;
    Renderer?.SyncLayerUniforms(Layers);
    RefreshUI();
    ReportStatus(`Applied ${Preset.Label} material preset to "${Layer.Name}".`);
};

Find('#LayerSurfaceColor').oninput = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    Layer.Color = Ev.target.value;
    Renderer?.SyncLayerUniforms(Layers);
    RenderLayerDirectory();
};

Find('#LayerCliffColor').oninput = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    Layer.CliffColor = Ev.target.value;
    Renderer?.SyncLayerUniforms(Layers);
};

Find('#LayerStrataSlider').oninput = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    Layer.Strata = Number(Ev.target.value);
    Find('#LayerStrataOut').textContent = Layer.Strata.toFixed(2);
    SetSliderProgress(Ev.target);
    Renderer?.SyncLayerUniforms(Layers);
};

Find('#LayerRoughnessSlider').oninput = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    Layer.Roughness = Number(Ev.target.value);
    Find('#LayerRoughnessOut').textContent = Layer.Roughness.toFixed(2);
    SetSliderProgress(Ev.target);
    Renderer?.SyncLayerUniforms(Layers);
};

Find('#LayerDepthSlider').oninput = (Ev) =>
{
    Find('#LayerDepthOut').textContent = `${Ev.target.value} m`;
    SetSliderProgress(Ev.target);
};
Find('#LayerDepthSlider').onchange = (Ev) =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    PushDocumentSnapshot();
    Layer.Depth = Number(Ev.target.value);
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
};

// Ocean & Atmosphere Controls
Find('#OceanEnabledToggle').onchange = (Ev) =>
{
    OceanState.Enabled = Ev.target.checked;
    Find('#OceanSummary').textContent = OceanState.Enabled ? 'Ocean: 24-wave analytic ray space' : 'Ocean: Disabled';
};
Find('#OceanAmplitudeSlider').oninput = (Ev) =>
{
    OceanState.WaveAmplitude = Number(Ev.target.value);
    const Label = OceanState.WaveAmplitude.toFixed(2);
    Find('#OceanAmplitudeOut').textContent = Label;
    Find('#OceanWaveMetric').textContent   = Label;
    SetSliderProgress(Ev.target);
};
Find('#SunElevationSlider').oninput = (Ev) =>
{
    OceanState.SunElevationDeg = Number(Ev.target.value);
    Find('#SunElevationOut').textContent = `${OceanState.SunElevationDeg}°`;
    SetSliderProgress(Ev.target);
};
Find('#SunAzimuthSlider').oninput = (Ev) =>
{
    OceanState.SunAzimuthDeg = Number(Ev.target.value);
    Find('#SunAzimuthOut').textContent = `${OceanState.SunAzimuthDeg}°`;
    SetSliderProgress(Ev.target);
};
for (const Sel of ['#OceanAmplitudeSlider', '#SunElevationSlider', '#SunAzimuthSlider', '#MaskBrushRadius'])
{
    SetSliderProgress(Find(Sel));
}

//------------------------------------------------------------------------------------------------------------------------
//                              LAYER ADD / DUPLICATE / DELETE / PRESET WORLDS / IO
//------------------------------------------------------------------------------------------------------------------------

Find('#AddLayer').onclick = () =>
{
    if (Layers.length >= 16) { ReportStatus('Maximum 16 SDF layers reached.'); return; }
    PushDocumentSnapshot();
    const Index = Layers.length;
    const Layer = CreateLayer(Index);
    const OffsetX = ((Index % 3) - 1) * 340;
    const OffsetZ = (Math.floor(Index / 3) - 0.5) * 320;
    Layer.Strokes = [{ Points: [[OffsetX - 60, OffsetZ - 20], [OffsetX + 60, OffsetZ + 25]], Radius: 110, Erase: false }];
    Layer.Regions = [{ X0: OffsetX - 220, Z0: OffsetZ - 180, X1: OffsetX + 220, Z1: OffsetZ + 185, Edge: 42, SculptStrokes: [] }];
    Layer.Landforms = [CreateDefaultWorld()[1].Landforms[0]];
    Layers.push(Layer);
    Volumes.push(null);
    ActiveIndex = Layers.length - 1;
    SelectedSet.clear();
    SelectedSet.add(ActiveIndex);
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
    FrameLayer(ActiveIndex);
    ReportStatus(`Added "${Layer.Name}" at 512×512×128 SDF resolution.`);
};

Find('#DuplicateLayer').onclick = () =>
{
    if (Layers.length >= 16 || !Layers[ActiveIndex]) return;
    PushDocumentSnapshot();
    const Copy = structuredClone(Layers[ActiveIndex]);
    delete Copy.Id;
    EnsureLayerId(Copy, Layers.length);
    Copy.Name = `${Copy.Name} copy`;
    for (const S of Copy.Strokes) S.Points = S.Points.map(([X, Z]) => [X + 180, Z + 140]);
    for (const R of Copy.Regions) { R.X0 += 180; R.X1 += 180; R.Z0 += 140; R.Z1 += 140; }
    Layers.push(Copy);
    Volumes.push(null);
    ActiveIndex = Layers.length - 1;
    SelectedSet.clear();
    SelectedSet.add(ActiveIndex);
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
    ReportStatus(`Duplicated layer as "${Copy.Name}".`);
};

Find('#DeleteLayer').onclick = () =>
{
    if (Layers.length <= 1) { ReportStatus('At least one SDF layer is required.'); return; }
    PushDocumentSnapshot();
    const RemovedName = Layers[ActiveIndex].Name;
    Layers.splice(ActiveIndex, 1);
    Volumes.splice(ActiveIndex, 1);
    ActiveIndex = Math.max(0, Math.min(Layers.length - 1, ActiveIndex));
    SelectedSet.clear();
    SelectedSet.add(ActiveIndex);
    RepackAndUploadAll();
    RefreshUI();
    ReportStatus(`Deleted layer "${RemovedName}".`);
};

Find('#LoadDefaultWorld').onclick = () =>
{
    PushDocumentSnapshot();
    Layers = CreateDefaultWorld();
    ActiveIndex = 0;
    SelectedSet.clear();
    SelectedSet.add(0);
    StackPanel.ResetSelection();
    ScheduleAllLayersBuild(true);
    RepackAndUploadAll();
    FrameWorld();
    RefreshUI();
    ReportStatus('Loaded Default World (1024×1024×256 Desert + two 512×512×128 coastal layers).');
};

Find('#LoadShowcaseWorld').onclick = () =>
{
    PushDocumentSnapshot();
    Layers = CreateAppliedShowcaseWorld();
    ActiveIndex = 0;
    SelectedSet.clear();
    SelectedSet.add(0);
    StackPanel.ResetSelection();
    ScheduleAllLayersBuild(true);
    RepackAndUploadAll();
    Camera.Target   = [0, 48, 0];
    Camera.Distance = 1480;
    Camera.Yaw      = -0.38;
    Camera.Pitch    = 0.44;
    RefreshUI();
    ReportStatus('Loaded 5-Layer Showcase World.');
};

Find('#Undo').onclick = PerformUndo;
Find('#Redo').onclick = PerformRedo;

Find('#Export').onclick = () =>
{
    const Doc  = { Format: 'FrontierLandscape/2', Ocean: OceanState, Layers };
    const BlobObj = new Blob([JSON.stringify(Doc, null, 2)], { type: 'application/json' });
    const Link = document.createElement('a');
    Link.href = URL.createObjectURL(BlobObj);
    Link.download = 'frontier-global-sdf-world.json';
    Link.click();
    setTimeout(() => URL.revokeObjectURL(Link.href), 2000);
    ReportStatus('Saved FrontierLandscape/2 JSON document.');
};

Find('#Import').onclick = () => Find('#ImportFile').click();
Find('#ImportFile').onchange = async (Ev) =>
{
    const FileObj = Ev.target.files?.[0];
    Ev.target.value = '';
    if (!FileObj) return;
    try
    {
        const Parsed = JSON.parse(await FileObj.text());
        ValidateDocument(Parsed);
        PushDocumentSnapshot();
        Layers = Parsed.Layers;
        if (Parsed.Ocean) Object.assign(OceanState, Parsed.Ocean);
        ActiveIndex = 0;
        SelectedSet.clear();
        SelectedSet.add(0);
        StackPanel.ResetSelection();
        ScheduleAllLayersBuild(true);
        RepackAndUploadAll();
        FrameWorld();
        RefreshUI();
        ReportStatus(`Loaded "${FileObj.name}" (${Layers.length} SDF layers).`);
    }
    catch (Err)
    {
        ReportStatus(`Import failed: ${Err.message}`);
    }
};

Find('#ExportGsdf').onclick = () =>
{
    const Vol = Volumes[ActiveIndex];
    if (!Vol) { ReportStatus('Active layer SDF is still building — wait a moment before exporting.'); return; }
    const { Header, Bytes } = ExportLayerSparseArchive(Vol);
    const BlobObj = new Blob([Bytes], { type: 'application/octet-stream' });
    const Link = document.createElement('a');
    Link.href = URL.createObjectURL(BlobObj);
    const SafeName = (Layers[ActiveIndex]?.Name || 'layer').toLowerCase().replace(/[^a-z0-9]+/g, '-');
    Link.download = `${SafeName}-${Header.Resolution[0]}x${Header.Resolution[2]}x${Header.Resolution[1]}.gsdf`;
    Link.click();
    setTimeout(() => URL.revokeObjectURL(Link.href), 2000);
    ReportStatus(`Exported sparse 3D SDF archive (${Header.AllocatedBricks.toLocaleString()} R16F bricks).`);
};

//------------------------------------------------------------------------------------------------------------------------
//                              CAMERA FRAMING, ORBIT / PAN & REAL-TIME 3D SDF SCULPTING
//------------------------------------------------------------------------------------------------------------------------

function FrameLayer(Index = ActiveIndex)
{
    const Layer = Layers[Index];
    const Reg   = Layer?.Regions?.[0];
    if (!Reg) return;
    const CX = 0.5 * (Reg.X0 + Reg.X1);
    const CZ = 0.5 * (Reg.Z0 + Reg.Z1);
    const Span = Math.max(Reg.X1 - Reg.X0, Reg.Z1 - Reg.Z0, 220);
    Camera.Target   = [CX, 35, CZ];
    Camera.Distance = Span * 1.12;
    PaintView.CenterX = CX;
    PaintView.CenterZ = CZ;
    PaintView.MetresPerPixel = Math.max(0.4, Span / 420);
    if (WorkspaceMode === 'mask') DrawMaskPaintCanvas();
}

function FrameWorld()
{
    let MinX = Infinity, MaxX = -Infinity, MinZ = Infinity, MaxZ = -Infinity;
    for (const L of Layers)
    {
        for (const R of L.Regions)
        {
            MinX = Math.min(MinX, R.X0); MaxX = Math.max(MaxX, R.X1);
            MinZ = Math.min(MinZ, R.Z0); MaxZ = Math.max(MaxZ, R.Z1);
        }
    }
    if (!Number.isFinite(MinX)) return;
    Camera.Target   = [0.5 * (MinX + MaxX), 38, 0.5 * (MinZ + MaxZ)];
    Camera.Distance = Math.max(MaxX - MinX, MaxZ - MinZ, 400) * 0.92;
}

Find('#Fit').onclick        = () => FrameLayer(ActiveIndex);
Find('#FrameWorld').onclick = () => FrameWorld();

function SelectSculptTool(Mode)
{
    SculptSettings.Mode = Mode;
    ActiveTool = Mode;
    if (WorkspaceMode !== 'landscape') SetWorkspaceMode('landscape');
    SyncSculptControlsUI();
}

Find('#OrbitCamera').addEventListener('click', () => { ActiveTool = 'orbit'; BrushHover.Active = false; SyncSculptControlsUI(); });
Find('#PanCamera').addEventListener('click',   () => { ActiveTool = 'pan';   BrushHover.Active = false; SyncSculptControlsUI(); });
Find('#ExitSculptHud').onclick = () => { ActiveTool = 'orbit'; BrushHover.Active = false; SyncSculptControlsUI(); };
Find('#ActivateSculptBtn').onclick = () => SelectSculptTool(SculptSettings.Mode);

FindAll('[data-sculpt-tool]').forEach((Btn) =>
{
    Btn.onclick = () => SelectSculptTool(Btn.dataset.sculptTool);
});
FindAll('[data-sculpt-pill], [data-inspector-sculpt]').forEach((Btn) =>
{
    Btn.onclick = () => SelectSculptTool(Btn.dataset.sculptPill || Btn.dataset.inspectorSculpt);
});

for (const Id of ['#HudBrushRadius', '#InspBrushRadius'])
{
    Find(Id).oninput = (Ev) => { SculptSettings.Radius = Number(Ev.target.value); SyncSculptControlsUI(); };
}
for (const Id of ['#HudBrushStrength', '#InspBrushStrength'])
{
    Find(Id).oninput = (Ev) => { SculptSettings.Strength = Number(Ev.target.value); SyncSculptControlsUI(); };
}
for (const Id of ['#HudBrushFalloff', '#InspBrushFalloff'])
{
    Find(Id).oninput = (Ev) => { SculptSettings.Falloff = Number(Ev.target.value); SyncSculptControlsUI(); };
}

Find('#ClearLayerSculpts').onclick = () =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer?.Regions?.[0]?.SculptStrokes?.length) return;
    PushDocumentSnapshot();
    for (const R of Layer.Regions) R.SculptStrokes = [];
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
    ReportStatus(`Cleared 3D sculpt strokes on "${Layer.Name}".`);
};

// Raycast helper for 3D SDF Sculpting (hits active/visible SDF volume, with sea-level plane fallback inside bounds for Clay)
function PickSculptPoint(ClientX, ClientY, LockedLayerIndex = -1)
{
    const Rect = PreviewCanvas.getBoundingClientRect();
    const Ray  = UnprojectScreenRay(Camera, ClientX, ClientY, Rect);
    const CandidateVolumes = Volumes.filter((V, I) => V && Layers[I]?.Visible !== false);

    // Prefer locked layer during an active drag, otherwise raycast all visible layers (or active layer)
    let Hit = null;
    if (LockedLayerIndex >= 0 && Volumes[LockedLayerIndex])
    {
        Hit = RaycastGlobalSdfVolumes([Volumes[LockedLayerIndex]], Ray.Origin, Ray.Dir, LockedLayerIndex);
    }
    else
    {
        Hit = RaycastGlobalSdfVolumes(CandidateVolumes, Ray.Origin, Ray.Dir, -1);
    }

    if (Hit) return Hit;

    // Fallback to y = 0 plane inside target layer's horizontal AABB so Clay can raise new sandbars/arches out of shallow water
    const TargetIdx = LockedLayerIndex >= 0 ? LockedLayerIndex : ActiveIndex;
    const TargetVol = Volumes[TargetIdx];
    if (TargetVol && Math.abs(Ray.Dir[1]) > 1e-4)
    {
        const T = -Ray.Origin[1] / Ray.Dir[1];
        if (T > 0 && T < 25000)
        {
            const PX = Ray.Origin[0] + Ray.Dir[0] * T;
            const PZ = Ray.Origin[2] + Ray.Dir[2] * T;
            if (PX >= TargetVol.BoundsMin[0] && PX <= TargetVol.BoundsMax[0] &&
                PZ >= TargetVol.BoundsMin[2] && PZ <= TargetVol.BoundsMax[2])
            {
                return {
                    LayerIndex: TargetIdx,
                    Distance: T,
                    Position: [PX, 0, PZ],
                    Normal: [0, 1, 0]
                };
            }
        }
    }
    return null;
}

// 3D Viewport Pointer Interaction (Camera Orbit / Pan + Real-Time 3D Volumetric SDF Sculpting)
// Note: Sculpt queue + off-thread worker definitions are above (allocateMissingBricksForSegment etc.)
let PointerGesture = null;

PreviewCanvas.addEventListener('contextmenu', (Ev) => Ev.preventDefault());

PreviewCanvas.addEventListener('pointermove', (Ev) =>
{
    const IsSculptTool = ['clay', 'carve', 'smooth', 'flatten'].includes(ActiveTool);

    if (!PointerGesture)
    {
        if (IsSculptTool)
        {
            const Hit = PickSculptPoint(Ev.clientX, Ev.clientY, -1);
            BrushHover.Active     = Boolean(Hit);
            BrushHover.Center     = Hit ? Hit.Position : null;
            BrushHover.Normal     = Hit ? Hit.Normal   : [0, 1, 0];
            BrushHover.LayerIndex = Hit ? Hit.LayerIndex : ActiveIndex;
        }
        else
        {
            BrushHover.Active = false;
        }
        return;
    }

    const DX = Ev.clientX - PointerGesture.LastX;
    const DY = Ev.clientY - PointerGesture.LastY;
    PointerGesture.LastX = Ev.clientX;
    PointerGesture.LastY = Ev.clientY;

    if (PointerGesture.Type === 'orbit')
    {
        Camera.Yaw   -= DX * 0.0052;
        Camera.Pitch  = Math.max(-1.42, Math.min(1.48, Camera.Pitch + DY * 0.0052));
        return;
    }

    if (PointerGesture.Type === 'pan')
    {
        const Basis = ComputeCameraBasis(Camera);
        const Scale = Camera.Distance * 0.0014;
        for (let A = 0; A < 3; A++)
        {
            Camera.Target[A] += (-Basis.Right[A] * DX + Basis.Up[A] * DY) * Scale;
        }
        return;
    }

    if (PointerGesture.Type === 'sculpt')
    {
        const Hit = PickSculptPoint(Ev.clientX, Ev.clientY, PointerGesture.LayerIndex);
        if (!Hit) return;

        BrushHover.Active = true;
        BrushHover.Center = Hit.Position;
        BrushHover.Normal = Hit.Normal;

        const Prev = PointerGesture.LastPoint;
        const Dist = Math.hypot(Hit.Position[0] - Prev[0], Hit.Position[1] - Prev[1], Hit.Position[2] - Prev[2]);
        const StepThreshold = Math.max(1.2, SculptSettings.Radius * 0.18);
        if (Dist < StepThreshold) return;

        const Vol   = Volumes[PointerGesture.LayerIndex];
        const Layer = Layers[PointerGesture.LayerIndex];
        if (!Vol || !Layer || !PackedState) return;

        PointerGesture.LastPoint = Hit.Position;
        PointerGesture.StrokeRecord.Points.push([...Hit.Position]);

        enqueueSculptDab(Vol, Layer, {
            A: Prev,
            B: Hit.Position,
            Mode: PointerGesture.EffectiveMode,
            Radius: SculptSettings.Radius,
            Strength: SculptSettings.Strength,
            Falloff: SculptSettings.Falloff,
            PlanePoint: PointerGesture.PlanePoint,
            PlaneNormal: PointerGesture.PlaneNormal
        }, PointerGesture.PreSnapshots);
    }
});

PreviewCanvas.addEventListener('pointerdown', (Ev) =>
{
    PreviewCanvas.setPointerCapture(Ev.pointerId);
    const IsSculptTool = ['clay', 'carve', 'smooth', 'flatten'].includes(ActiveTool);

    // Middle-drag or Shift+Right-drag -> Pan; Right-drag or Alt+Left-drag -> Orbit
    if (Ev.button === 1 || (Ev.button === 2 && Ev.shiftKey) || (ActiveTool === 'pan' && Ev.button === 0 && !IsSculptTool))
    {
        PointerGesture = { Type: 'pan', PointerId: Ev.pointerId, LastX: Ev.clientX, LastY: Ev.clientY };
        return;
    }
    if (Ev.button === 2 || Ev.altKey || !IsSculptTool)
    {
        PointerGesture = { Type: 'orbit', PointerId: Ev.pointerId, LastX: Ev.clientX, LastY: Ev.clientY };
        return;
    }

    // Left-click in 3D Sculpt mode -> Real-time 3D Volumetric SDF Stroke
    const Hit = PickSculptPoint(Ev.clientX, Ev.clientY, -1);
    if (!Hit)
    {
        // Clicking open sky/water outside SDF bounds orbits smoothly instead of doing nothing
        PointerGesture = { Type: 'orbit', PointerId: Ev.pointerId, LastX: Ev.clientX, LastY: Ev.clientY };
        return;
    }

    const TargetIdx = Hit.LayerIndex;
    if (TargetIdx !== ActiveIndex)
    {
        ActiveIndex = TargetIdx;
        SelectedSet.clear();
        SelectedSet.add(TargetIdx);
    }

    let EffectiveMode = SculptSettings.Mode;
    if (Ev.shiftKey)
    {
        if (EffectiveMode === 'clay') EffectiveMode = 'carve';
        else if (EffectiveMode === 'carve') EffectiveMode = 'clay';
    }

    const PreSnapshots = new Map();
    const Vol          = Volumes[TargetIdx];
    const Layer        = Layers[TargetIdx];
    const StrokeRecord = {
        Mode: EffectiveMode,
        Radius: SculptSettings.Radius,
        Strength: SculptSettings.Strength,
        Falloff: SculptSettings.Falloff,
        PlaneNormal: [...Hit.Normal],
        Points: [[...Hit.Position]]
    };

    // first dab — enqueue async so pointerdown never blocks (streaming GPU upload within one frame)
    enqueueSculptDab(Vol, Layer, {
        A: Hit.Position,
        B: Hit.Position,
        Mode: EffectiveMode,
        Radius: SculptSettings.Radius,
        Strength: SculptSettings.Strength,
        Falloff: SculptSettings.Falloff,
        PlanePoint: Hit.Position,
        PlaneNormal: Hit.Normal
    }, PreSnapshots);

    PointerGesture = {
        Type: 'sculpt',
        PointerId: Ev.pointerId,
        LayerIndex: TargetIdx,
        EffectiveMode,
        LastX: Ev.clientX,
        LastY: Ev.clientY,
        LastPoint: Hit.Position,
        PlanePoint: [...Hit.Position],
        PlaneNormal: [...Hit.Normal],
        PreSnapshots,
        StrokeRecord
    };
});

function FinishPointerGesture(Ev)
{
    if (!PointerGesture || (Ev && Ev.pointerId !== PointerGesture.PointerId)) return;
    if (PointerGesture.Type === 'sculpt')
    {
        const Idx = PointerGesture.LayerIndex;
        const CapturedGesture = PointerGesture;
        PointerGesture = null;
        // Wait for any in-flight worker dabs to complete before committing history
        const tryFinalize = () =>
        {
            if (SculptQueue.length || SculptInFlight)
            {
                setTimeout(tryFinalize, 16);
                return;
            }
            const Layer = Layers[Idx];
            const Vol   = Volumes[Idx];
            if (Layer?.Regions?.[0] && Vol)
            {
                Layer.Regions[0].SculptStrokes ||= [];
                Layer.Regions[0].SculptStrokes.push(CapturedGesture.StrokeRecord);
                const Snapshots = CaptureBrickSnapshotsAfterStroke(Vol, CapturedGesture.PreSnapshots);
                if (Snapshots.length > 0)
                {
                    PushSculptDeltaEntry(Idx, CapturedGesture.StrokeRecord, Snapshots);
                }
                RefreshDiagnosticsUI();
                SyncLayerSettingsCard();
            }
        };
        tryFinalize();
        return;
    }
    PointerGesture = null;
}

PreviewCanvas.addEventListener('pointerup', FinishPointerGesture);
PreviewCanvas.addEventListener('pointercancel', FinishPointerGesture);
PreviewCanvas.addEventListener('pointerleave', () => { if (!PointerGesture) BrushHover.Active = false; });

PreviewCanvas.addEventListener('wheel', (Ev) =>
{
    Ev.preventDefault();
    const Factor = Math.exp(Ev.deltaY * 0.0011);
    Camera.Distance = Math.max(8, Math.min(18000, Camera.Distance * Factor));
    const ApproxScale = Math.max(5, Math.round((Camera.Distance * 0.11) / 5) * 5);
    Find('#ScaleReadout').textContent = `${ApproxScale} m world scale`;
}, { passive: false });

//------------------------------------------------------------------------------------------------------------------------
//                              2D VECTOR SHORELINE MASK PAINT CANVAS (WORKSPACE 1)
//------------------------------------------------------------------------------------------------------------------------

let MaskStrokeGesture = null;

Find('#MaskBrushRadius').oninput = (Ev) =>
{
    PaintView.BrushRadius = Number(Ev.target.value);
    Find('#MaskBrushReadout').textContent = `${PaintView.BrushRadius} m`;
    SetSliderProgress(Ev.target);
};
Find('#MaskErase').onchange = (Ev) => { PaintView.Erase = Ev.target.checked; };

function RecomputeRegionBoundsFromStrokes(Layer)
{
    let MinX = Infinity, MaxX = -Infinity, MinZ = Infinity, MaxZ = -Infinity;
    for (const S of Layer.Strokes)
    {
        if (S.Erase) continue;
        for (const [X, Z] of S.Points)
        {
            MinX = Math.min(MinX, X - S.Radius - 24);
            MaxX = Math.max(MaxX, X + S.Radius + 24);
            MinZ = Math.min(MinZ, Z - S.Radius - 24);
            MaxZ = Math.max(MaxZ, Z + S.Radius + 24);
        }
    }
    if (!Number.isFinite(MinX)) return;
    if (!Layer.Regions.length)
    {
        Layer.Regions.push({ X0: MinX, Z0: MinZ, X1: MaxX, Z1: MaxZ, Edge: 48, SculptStrokes: [] });
    }
    else
    {
        Layer.Regions[0].X0 = Math.round(MinX);
        Layer.Regions[0].Z0 = Math.round(MinZ);
        Layer.Regions[0].X1 = Math.round(MaxX);
        Layer.Regions[0].Z1 = Math.round(MaxZ);
    }
}

Find('#FitRegions').onclick = () =>
{
    const Layer = Layers[ActiveIndex];
    if (!Layer) return;
    PushDocumentSnapshot();
    RecomputeRegionBoundsFromStrokes(Layer);
    ScheduleLayerBuild(ActiveIndex);
    RefreshUI();
    ReportStatus(`Updated "${Layer.Name}" 3D SDF bounds from painted shoreline strokes.`);
};

function ScreenToWorld2D(ClientX, ClientY)
{
    const Rect = PaintCanvas.getBoundingClientRect();
    const SX = ClientX - Rect.left - Rect.width * 0.5;
    const SZ = ClientY - Rect.top  - Rect.height * 0.5;
    return [
        PaintView.CenterX + SX * PaintView.MetresPerPixel,
        PaintView.CenterZ + SZ * PaintView.MetresPerPixel
    ];
}

function DrawMaskPaintCanvas()
{
    if (WorkspaceMode !== 'mask') return;
    const DPR = Math.min(window.devicePixelRatio || 1, 2);
    const W   = Math.max(1, Math.round(PaintCanvas.clientWidth * DPR));
    const H   = Math.max(1, Math.round(PaintCanvas.clientHeight * DPR));
    if (PaintCanvas.width !== W || PaintCanvas.height !== H)
    {
        PaintCanvas.width  = W;
        PaintCanvas.height = H;
    }

    const Ctx = PaintCanvas.getContext('2d');
    Ctx.save();
    Ctx.scale(DPR, DPR);
    const CW = PaintCanvas.clientWidth;
    const CH = PaintCanvas.clientHeight;

    Ctx.fillStyle = '#14191b';
    Ctx.fillRect(0, 0, CW, CH);

    const WorldToScreen = (WX, WZ) => [
        CW * 0.5 + (WX - PaintView.CenterX) / PaintView.MetresPerPixel,
        CH * 0.5 + (WZ - PaintView.CenterZ) / PaintView.MetresPerPixel
    ];

    // World 100 m engineering grid
    const StepM = PaintView.MetresPerPixel > 3 ? 250 : 100;
    const StartX = Math.floor((PaintView.CenterX - CW * 0.5 * PaintView.MetresPerPixel) / StepM) * StepM;
    const EndX   = PaintView.CenterX + CW * 0.5 * PaintView.MetresPerPixel;
    const StartZ = Math.floor((PaintView.CenterZ - CH * 0.5 * PaintView.MetresPerPixel) / StepM) * StepM;
    const EndZ   = PaintView.CenterZ + CH * 0.5 * PaintView.MetresPerPixel;

    Ctx.strokeStyle = '#232c2f';
    Ctx.lineWidth   = 1;
    Ctx.beginPath();
    for (let X = StartX; X <= EndX; X += StepM)
    {
        const [SX] = WorldToScreen(X, 0);
        Ctx.moveTo(SX, 0); Ctx.lineTo(SX, CH);
    }
    for (let Z = StartZ; Z <= EndZ; Z += StepM)
    {
        const [, SZ] = WorldToScreen(0, Z);
        Ctx.moveTo(0, SZ); Ctx.lineTo(CW, SZ);
    }
    Ctx.stroke();

    // Render all visible layers' strokes & region AABBs
    Layers.forEach((Layer, Idx) =>
    {
        if (!Layer.Visible) return;
        const IsAct = Idx === ActiveIndex;
        Ctx.globalAlpha = IsAct ? 0.85 : 0.30;

        for (const S of Layer.Strokes)
        {
            if (!S.Points.length) continue;
            Ctx.strokeStyle = S.Erase ? '#14191b' : (Layer.Color || '#c8a374');
            Ctx.lineCap     = 'round';
            Ctx.lineJoin    = 'round';
            Ctx.lineWidth   = (S.Radius * 2) / PaintView.MetresPerPixel;
            Ctx.beginPath();
            S.Points.forEach(([PX, PZ], PIdx) =>
            {
                const [SX, SZ] = WorldToScreen(PX, PZ);
                if (PIdx === 0) Ctx.moveTo(SX, SZ);
                else Ctx.lineTo(SX, SZ);
            });
            if (S.Points.length === 1)
            {
                const [SX, SZ] = WorldToScreen(S.Points[0][0], S.Points[0][1]);
                Ctx.lineTo(SX + 0.01, SZ);
            }
            Ctx.stroke();
        }

        const Reg = Layer.Regions[0];
        if (Reg && IsAct)
        {
            const [X0, Z0] = WorldToScreen(Reg.X0, Reg.Z0);
            const [X1, Z1] = WorldToScreen(Reg.X1, Reg.Z1);
            Ctx.strokeStyle = '#d8e4e2';
            Ctx.lineWidth   = 1;
            Ctx.setLineDash([5, 5]);
            Ctx.strokeRect(X0, Z0, X1 - X0, Z1 - Z0);
            Ctx.setLineDash([]);
        }
    });

    Ctx.restore();
}

PaintCanvas.addEventListener('pointerdown', (Ev) =>
{
    PaintCanvas.setPointerCapture(Ev.pointerId);
    if (Ev.button === 1 || Ev.button === 2 || Ev.altKey)
    {
        MaskStrokeGesture = { Type: 'pan', LastX: Ev.clientX, LastY: Ev.clientY };
        return;
    }
    const Layer = Layers[ActiveIndex];
    if (!Layer || Layer.Strokes.length >= 256) return;
    PushDocumentSnapshot();
    const Pt = ScreenToWorld2D(Ev.clientX, Ev.clientY);
    const Stroke = { Points: [[Math.round(Pt[0]), Math.round(Pt[1])]], Radius: PaintView.BrushRadius, Erase: PaintView.Erase || Ev.shiftKey };
    Layer.Strokes.push(Stroke);
    MaskStrokeGesture = { Type: 'draw', Stroke, LastPt: Pt };
    DrawMaskPaintCanvas();
});

PaintCanvas.addEventListener('pointermove', (Ev) =>
{
    if (!MaskStrokeGesture) return;
    if (MaskStrokeGesture.Type === 'pan')
    {
        PaintView.CenterX -= (Ev.clientX - MaskStrokeGesture.LastX) * PaintView.MetresPerPixel;
        PaintView.CenterZ -= (Ev.clientY - MaskStrokeGesture.LastY) * PaintView.MetresPerPixel;
        MaskStrokeGesture.LastX = Ev.clientX;
        MaskStrokeGesture.LastY = Ev.clientY;
        DrawMaskPaintCanvas();
        return;
    }
    const Pt = ScreenToWorld2D(Ev.clientX, Ev.clientY);
    if (Math.hypot(Pt[0] - MaskStrokeGesture.LastPt[0], Pt[1] - MaskStrokeGesture.LastPt[1]) >= Math.max(12, PaintView.BrushRadius * 0.28))
    {
        MaskStrokeGesture.Stroke.Points.push([Math.round(Pt[0]), Math.round(Pt[1])]);
        MaskStrokeGesture.LastPt = Pt;
        DrawMaskPaintCanvas();
    }
});

PaintCanvas.addEventListener('pointerup', () =>
{
    if (MaskStrokeGesture?.Type === 'draw')
    {
        const Layer = Layers[ActiveIndex];
        RecomputeRegionBoundsFromStrokes(Layer);
        ScheduleLayerBuild(ActiveIndex);
        RefreshUI();
    }
    MaskStrokeGesture = null;
});

PaintCanvas.addEventListener('wheel', (Ev) =>
{
    Ev.preventDefault();
    PaintView.MetresPerPixel = Math.max(0.25, Math.min(12.0, PaintView.MetresPerPixel * Math.exp(Ev.deltaY * 0.001)));
    DrawMaskPaintCanvas();
}, { passive: false });

//------------------------------------------------------------------------------------------------------------------------
//                                      KEYBOARD SHORTCUTS & RENDER LOOP
//------------------------------------------------------------------------------------------------------------------------

window.addEventListener('keydown', (Ev) =>
{
    const Typing = Ev.target?.closest?.('input:not([type="checkbox"]):not([type="range"]),select,textarea,[contenteditable="true"]');
    if (Typing) return;

    if ((Ev.ctrlKey || Ev.metaKey) && Ev.key.toLowerCase() === 'z')
    {
        Ev.preventDefault();
        if (Ev.shiftKey) PerformRedo();
        else PerformUndo();
        return;
    }
    if ((Ev.ctrlKey || Ev.metaKey) && Ev.key.toLowerCase() === 'y')
    {
        Ev.preventDefault();
        PerformRedo();
        return;
    }
    if (Ev.ctrlKey || Ev.metaKey || Ev.altKey) return;

    switch (Ev.key.toLowerCase())
    {
        case '1': SetWorkspaceMode('mask'); break;
        case '2': SetWorkspaceMode('landscape'); break;
        case 'g': if (WorkspaceMode === 'landscape') { Ev.preventDefault(); StackPanel.OpenMenu(false); } break;
        case 'o': ActiveTool = 'orbit'; BrushHover.Active = false; SyncSculptControlsUI(); break;
        case 'p': ActiveTool = 'pan';   BrushHover.Active = false; SyncSculptControlsUI(); break;
        case 'b': SelectSculptTool('clay'); break;
        case 'v': SelectSculptTool('carve'); break;
        case 'k': SelectSculptTool('smooth'); break;
        case 'f': SelectSculptTool('flatten'); break;
        case 'escape':
            if (['clay', 'carve', 'smooth', 'flatten'].includes(ActiveTool))
            {
                ActiveTool = 'orbit';
                BrushHover.Active = false;
                SyncSculptControlsUI();
            }
            break;
        case '[':
            SculptSettings.Radius = Math.max(6, SculptSettings.Radius - 4);
            SyncSculptControlsUI();
            break;
        case ']':
            SculptSettings.Radius = Math.min(160, SculptSettings.Radius + 4);
            SyncSculptControlsUI();
            break;
        case 'home':
            FrameLayer(ActiveIndex);
            break;
    }
});

window.addEventListener('resize', () =>
{
    if (WorkspaceMode === 'mask') DrawMaskPaintCanvas();
});

// Boot parallel Web Worker SDF builders and start 60 FPS ray-space render loop
InitWorkerPool();
RefreshUI();
ScheduleAllLayersBuild(false);
ReportStatus('Building Global 3D SDF layers in parallel (1024×1024×256 Desert + 2× 512×512×128)…');

function AnimationLoop(NowMs)
{
    CameraNav.Step(NowMs);

    if (WorkspaceMode === 'landscape' && Renderer)
    {
        const IsSculptTool = ['clay', 'carve', 'smooth', 'flatten'].includes(ActiveTool);
        Renderer.RenderFrame({
            Camera,
            Ocean: OceanState,
            TimeSeconds: NowMs * 0.001,
            SunDirection: ComputeSunDirection(),
            BrushState: IsSculptTool && BrushHover.Active
                ? {
                    Active: true,
                    Mode: PointerGesture?.EffectiveMode || SculptSettings.Mode,
                    Center: BrushHover.Center,
                    Radius: SculptSettings.Radius,
                    Falloff: SculptSettings.Falloff
                }
                : null
        });
    }

    requestAnimationFrame(AnimationLoop);
}

requestAnimationFrame(AnimationLoop);
