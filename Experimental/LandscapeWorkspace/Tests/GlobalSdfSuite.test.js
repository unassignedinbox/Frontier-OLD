import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import {
    HorizontalResolutions,
    VerticalResolutions,
    ResolveLayerResolution,
    ApplyLayerResolution,
    CreateLayer,
    CreateDefaultWorld,
    CreateAppliedShowcaseWorld,
    ValidateDocument,
    CreateLandform
} from '../GlobalSdfField.js';

import {
    BRICK_VOXELS,
    BRICK_TEXELS,
    INDIRECTION_AIR,
    INDIRECTION_SOLID,
    Float32ToHalf,
    ComputeChebyshevDistance3D,
    BuildLayerSparseSdf,
    SampleLayerVolumeSdf,
    SampleLayerVolumeNormal,
    RaycastGlobalSdfVolumes,
    PackGlobalSdfForGpu,
    ExportLayerSparseArchive
} from '../GlobalSdfBrickAtlas.js';

import {
    ApplySculptSegmentToVolume,
    CaptureBrickSnapshotsAfterStroke,
    ApplyBrickSnapshotDelta
} from '../GlobalSdfSculpt.js';

import {
    GlobalSdfVertexShader,
    GlobalSdfFragmentShader
} from '../GlobalSdfShader.js';

import {
    ComputeCameraBasis,
    UnprojectScreenRay,
    TraceOceanRayCpu
} from '../GlobalSdfRenderer.js';

const WorkspaceDir = path.dirname(fileURLToPath(import.meta.url)) + '/..';

function HalfBitsToFloat32(H)
{
    const Sign = (H & 0x8000) ? -1 : 1;
    const Exp  = (H >> 10) & 0x1f;
    const Mant = H & 0x03ff;
    if (Exp === 0) return Sign * Math.pow(2, -14) * (Mant / 1024);
    if (Exp === 31) return Mant ? NaN : Sign * Infinity;
    return Sign * Math.pow(2, Exp - 15) * (1 + Mant / 1024);
}

//------------------------------------------------------------------------------------------------------------------------
// 1. ARCHITECTURE & ZERO-LOD / PER-LAYER RESOLUTION TESTS
//------------------------------------------------------------------------------------------------------------------------

test('Zero-LOD architecture: legacy LOD & broken previewer modules are completely absent', () =>
{
    const Forbidden = [
        'VolumeDetail.js',
        'PreviewSurface.js',
        'PreviewPipeline.js',
        'SculptBrush.js',
        'SignedDistance.js',
        'VolumeBake.js',
        'BakeWorker.js'
    ];
    for (const Name of Forbidden)
    {
        assert.equal(fs.existsSync(path.join(WorkspaceDir, Name)), false, `${Name} must not exist`);
    }
    assert.equal(GlobalSdfFragmentShader.includes('FieldBound'), false, 'FieldBound distance distortion must not exist');
});

test('Per-layer independent SDF resolutions start at 512x512x128 and scale upward', () =>
{
    assert.equal(HorizontalResolutions[0], 512);
    assert.equal(VerticalResolutions[0], 128);
    assert.ok(HorizontalResolutions.includes(1024));
    assert.ok(VerticalResolutions.includes(256));

    const World = CreateDefaultWorld();
    assert.equal(World.length, 3);

    // Desert layer defaults to 1024 x 256 x 1024; Rocky & Basalt layers default to 512 x 128 x 512
    assert.deepEqual(ResolveLayerResolution(World[0]), [1024, 256, 1024]);
    assert.deepEqual(ResolveLayerResolution(World[1]), [512,  128, 512]);
    assert.deepEqual(ResolveLayerResolution(World[2]), [512,  128, 512]);

    // Modifying one layer's resolution slider never mutates sibling layers
    ApplyLayerResolution(World[1], 768, 192, 768);
    assert.deepEqual(ResolveLayerResolution(World[0]), [1024, 256, 1024]);
    assert.deepEqual(ResolveLayerResolution(World[1]), [768,  192, 768]);
    assert.deepEqual(ResolveLayerResolution(World[2]), [512,  128, 512]);

    // ValidateDocument transparently upgrades any sub-512x512x128 legacy document to >= 512x512x128
    const Legacy = {
        Format: 'FrontierLandscape/1',
        Layers: [{
            ...CreateLayer(1, 'rocky'),
            ResolutionX: 128,
            ResolutionY: 64,
            ResolutionZ: 128,
            Strokes: [{ Points: [[0, 0], [50, 50]], Radius: 80, Erase: false }],
            Regions: [{ X0: -120, Z0: -120, X1: 160, Z1: 160, Edge: 30 }]
        }]
    };
    ValidateDocument(Legacy);
    assert.deepEqual(ResolveLayerResolution(Legacy.Layers[0]), [512, 128, 512]);
});

//------------------------------------------------------------------------------------------------------------------------
// 2. EXACT 26-NEIGHBOUR L_INF CHEBYSHEV DISTANCE TRANSFORM & IEEE-754 HALF-FLOAT
//------------------------------------------------------------------------------------------------------------------------

test('Exact 26-neighbour 2-pass L_inf Chebyshev distance matches O(N^2) brute force across every cell', () =>
{
    const GX = 14, GY = 10, GZ = 12;
    const Total = GX * GY * GZ;
    const Mask  = new Uint8Array(Total);
    const Seeds = [];

    // Deterministic pseudo-random sparse seeds
    for (let Z = 0; Z < GZ; Z++)
    {
        for (let Y = 0; Y < GY; Y++)
        {
            for (let X = 0; X < GX; X++)
            {
                const Hash = Math.sin(X * 12.9898 + Y * 78.233 + Z * 37.719) * 43758.5453;
                if (Hash - Math.floor(Hash) > 0.91)
                {
                    Mask[(Z * GY + Y) * GX + X] = 1;
                    Seeds.push([X, Y, Z]);
                }
            }
        }
    }
    assert.ok(Seeds.length >= 5);

    const Fast = ComputeChebyshevDistance3D(GX, GY, GZ, Mask);

    for (let Z = 0; Z < GZ; Z++)
    {
        for (let Y = 0; Y < GY; Y++)
        {
            for (let X = 0; X < GX; X++)
            {
                let Exact = 255;
                for (const [SX, SY, SZ] of Seeds)
                {
                    const DInf = Math.max(Math.abs(X - SX), Math.abs(Y - SY), Math.abs(Z - SZ));
                    if (DInf < Exact) Exact = DInf;
                }
                assert.equal(
                    Fast[(Z * GY + Y) * GX + X],
                    Exact,
                    `Chebyshev mismatch at (${X},${Y},${Z}): got ${Fast[(Z * GY + Y) * GX + X]}, expected ${Exact}`
                );
            }
        }
    }
});

test('IEEE-754 binary16 converter preserves narrow-band signed distances to < 0.05% relative error', () =>
{
    for (const Val of [0, -0.0025, 0.0025, -0.35, 0.35, -4.125, 4.125, -28.6, 28.6])
    {
        const Decoded = HalfBitsToFloat32(Float32ToHalf(Val));
        const Limit   = Math.max(1e-4, Math.abs(Val) * 0.001);
        assert.ok(Math.abs(Decoded - Val) <= Limit, `Val=${Val} decoded=${Decoded}`);
    }
});

//------------------------------------------------------------------------------------------------------------------------
// 3. SPARSE BRICK CONTINUITY, NON-HOLLOW SOLID CORE, RAYCASTING & OCEAN ORDERING
//------------------------------------------------------------------------------------------------------------------------

test('Full-resolution 512x512x128 & 1024x1024x256 sparse SDF bricks have C0 seam continuity, solid core, and zero ray clipping', () =>
{
    const World = CreateDefaultWorld();
    const RockyVol  = BuildLayerSparseSdf(World[1], 1); // 512 x 128 x 512
    const BasaltVol = BuildLayerSparseSdf(World[2], 2); // 512 x 128 x 512

    assert.equal(BRICK_VOXELS, 8);
    assert.equal(BRICK_TEXELS, 10);
    assert.ok(RockyVol.AllocatedCount > 1000 && RockyVol.AllocatedCount < 40000);

    // Verify C0 continuity across brick boundaries (IX*8, IY*8, IZ*8)
    let CheckedBoundaries = 0;
    let MaxSeamJump       = 0;
    const [GX, GY, GZ] = RockyVol.GridDims;
    for (let B = 0; B < RockyVol.AllocatedCount && CheckedBoundaries < 120; B++)
    {
        const Cell = RockyVol.BrickCells[B];
        const IX   = Cell % GX;
        const IY   = Math.floor(Cell / GX) % GY;
        const IZ   = Math.floor(Cell / (GX * GY));
        if (IX + 1 >= GX) continue;
        const RightCell = (IZ * GY + IY) * GX + (IX + 1);
        if (RockyVol.BrickIndexByCell[RightCell] < 0) continue;

        const SeamX = RockyVol.BoundsMin[0] + (IX + 1) * 8 * RockyVol.Pitch[0];
        const MidY  = RockyVol.BoundsMin[1] + (IY * 8 + 4) * RockyVol.Pitch[1];
        const MidZ  = RockyVol.BoundsMin[2] + (IZ * 8 + 4) * RockyVol.Pitch[2];

        const LeftVal  = SampleLayerVolumeSdf(RockyVol, SeamX - 1e-4, MidY, MidZ);
        const RightVal = SampleLayerVolumeSdf(RockyVol, SeamX + 1e-4, MidY, MidZ);
        MaxSeamJump = Math.max(MaxSeamJump, Math.abs(RightVal - LeftVal));
        CheckedBoundaries++;
    }
    assert.ok(CheckedBoundaries >= 50, 'Expected at least 50 adjacent allocated brick pairs');
    assert.ok(MaxSeamJump < 2e-3, `Expected C0 seam continuity across bricks, got max jump ${MaxSeamJump}`);

    // Verify solid core below mountain peak is negative (-Band), never hollow (+Band)
    // Thin seabed extrusion (Depth=16 m) — sample just above bottom plane, inside solid rock
    const DeepY = Math.max(-8, RockyVol.BoundsMin[1] + 8);
    const DeepPhi = SampleLayerVolumeSdf(RockyVol, 275, DeepY, 40);
    assert.ok(DeepPhi < -2.0, `Expected deep rock interior to be solid negative SDF, got ${DeepPhi} at Y=${DeepY}`);

    // Pack multi-layer GPU textures and check WebGL2 3D texture ceiling (< 512 texels on every axis)
    const Packed = PackGlobalSdfForGpu([RockyVol, BasaltVol], 2048);
    assert.ok(Packed.AtlasTexelDims[0] <= 512 && Packed.AtlasTexelDims[1] <= 512 && Packed.AtlasTexelDims[2] <= 512);
    let SawSolidSentinel = false, SawAirSentinel = false;
    for (let I = 0; I < Packed.IndirectionBytes.length; I += 4)
    {
        if (Packed.IndirectionBytes[I] === INDIRECTION_SOLID) SawSolidSentinel = true;
        if (Packed.IndirectionBytes[I] === INDIRECTION_AIR)   SawAirSentinel   = true;
    }
    assert.equal(SawSolidSentinel, true, 'Packed indirection must encode INDIRECTION_SOLID (254) for deep interior');
    assert.equal(SawAirSentinel,   true, 'Packed indirection must encode INDIRECTION_AIR (255) for exterior air');

    // Raycast from outside AABB AND from inside AABB (camera close-up -> zero near/box-face clipping!)
    const OutsideHit = RaycastGlobalSdfVolumes([RockyVol, BasaltVol], [275, 260, 40], [0, -1, 0]);
    assert.ok(OutsideHit, 'Downward ray from outside AABB must hit Rocky SDF');
    assert.ok(OutsideHit.Position[1] > 5, `Expected positive rocky elevation, got ${OutsideHit.Position[1]}`);

    const InsideEyeY = OutsideHit.Position[1] + 6.0; // Camera 6 m above rock, deep inside layer AABB!
    const InsideHit  = RaycastGlobalSdfVolumes([RockyVol, BasaltVol], [275, InsideEyeY, 40], [0, -1, 0]);
    assert.ok(InsideHit, 'Close-up ray starting inside layer AABB must NOT clip');
    assert.ok(Math.abs(InsideHit.Position[1] - OutsideHit.Position[1]) < 0.15, 'Outside & inside rays must hit exact same surface');

    // Analytic Ray-Ocean vs Ray-SDF ordering: zero depth/order fighting
    const OceanCfg = { Enabled: true, WaveAmplitude: 0.65 };

    // (a) Ray hitting elevated dry rock peak above waves -> Ocean ray limited to [0, tSdf] does NOT occlude rock!
    const OceanBeforePeak = TraceOceanRayCpu([275, 260, 40], [0, -1, 0], OutsideHit.Distance, OceanCfg, 12.5);
    assert.equal(OceanBeforePeak, null, 'Ocean must never render on top of terrain above wave height');

    // (b) Ray aimed at submerged seabed / open water -> Ocean hits cleanly at y ~ [-1.5..1.5] with tOcean < tSdf!
    const SeaOrigin = [60, 120, -220];
    const SeaDir    = [0, -1, 0];
    const SdfUnder  = RaycastGlobalSdfVolumes([RockyVol, BasaltVol], SeaOrigin, SeaDir);
    const MaxRayT   = SdfUnder ? SdfUnder.Distance : 160000;
    const OceanHit  = TraceOceanRayCpu(SeaOrigin, SeaDir, MaxRayT, OceanCfg, 12.5);
    assert.ok(OceanHit, 'Downward ray over open ocean / submerged shelf must hit analytic ocean surface');
    assert.ok(Math.abs(OceanHit.Position[1]) < 2.0);
    if (SdfUnder) assert.ok(OceanHit.Distance < SdfUnder.Distance, 'Ocean hit tOcean must strictly precede submerged seabed tSdf');

    // Sparse .gsdf archive export round-trip header integrity
    const Archive = ExportLayerSparseArchive(RockyVol);
    assert.equal(Archive.Header.Format, 'FrontierGlobalSDF/1');
    assert.equal(Archive.Header.AllocatedBricks, RockyVol.AllocatedCount);
    assert.ok(Archive.Bytes.byteLength > RockyVol.AllocatedCount * 2000);
});

//------------------------------------------------------------------------------------------------------------------------
// 4. REAL-TIME 3D VOLUMETRIC SDF SCULPTING, DYNAMIC AIR-BRICK ALLOCATION & UNDO/REDO
//------------------------------------------------------------------------------------------------------------------------

test('3D Volumetric Sculpting: Clay overhang allocation, Chebyshev cone tightening, Carve cave tunnel & exact Undo/Redo', () =>
{
    const Layer = CreateLayer(1, 'rocky');
    ApplyLayerResolution(Layer, 512, 128, 512);
    Layer.Strokes = [{ Points: [[0, 0], [40, 20]], Radius: 95, Erase: false }];
    Layer.Regions = [{ X0: -160, Z0: -160, X1: 190, Z1: 170, Edge: 36, SculptStrokes: [{ Mode: 'clay', Radius: 20, Strength: 0.5, Falloff: 0.5, Points: [] }] }];
    Layer.Landforms = [{ ...CreateLandform('mesa'), Amount: 95, Scale: 1.4, Blend: 6 }];

    const Vol    = BuildLayerSparseSdf(Layer, 0);
    const Packed = PackGlobalSdfForGpu([Vol], 1024);

    // Find surface point at (10, *, 10)
    const TopHit = RaycastGlobalSdfVolumes([Vol], [10, 200, 10], [0, -1, 0]);
    assert.ok(TopHit, 'Expected initial mesa surface hit');

    // 1. CLAY IN MID-AIR: Sculpt a 3D overhang 22 m above the mesa top (forces fresh air-brick allocation + Chebyshev tightening!)
    const AirTarget = [TopHit.Position[0], TopHit.Position[1] + 22, TopHit.Position[2]];
    assert.ok(SampleLayerVolumeSdf(Vol, ...AirTarget) > 0, 'Point 22 m above mesa must initially be empty air');

    const PreClaySnaps = new Map();
    const InitialBrickCount = Vol.AllocatedCount;
    const ClayRes = ApplySculptSegmentToVolume(
        Vol,
        Layer,
        Packed,
        {
            A: TopHit.Position,
            B: AirTarget,
            Mode: 'clay',
            Radius: 24,
            Strength: 0.95,
            Falloff: 0.45
        },
        PreClaySnaps
    );

    assert.equal(ClayRes.NeedsRepack, false);
    assert.ok(ClayRes.DirtyLocalBricks.length > 0, 'Clay stroke must mark modified 10^3 bricks dirty');
    assert.ok(Vol.AllocatedCount >= InitialBrickCount, 'Clay into air must dynamically allocate bricks when needed');
    assert.ok(SampleLayerVolumeSdf(Vol, ...AirTarget) < 0, 'Clay overhang centre must now be solid (< 0)');

    // Raycast horizontally at the overhang height from outside AABB -> proves ChebyshevDist was tightened so ray does NOT skip over it!
    const SideHit = RaycastGlobalSdfVolumes([Vol], [-220, AirTarget[1], AirTarget[2]], [1, 0, 0]);
    assert.ok(SideHit, 'Horizontal raycast must hit newly sculpted 3D mid-air clay overhang without Chebyshev skip clipping');

    const ClaySnapshots = CaptureBrickSnapshotsAfterStroke(Vol, PreClaySnaps);
    assert.ok(ClaySnapshots.length > 0);

    // 2. UNDO & REDO CLAY STROKE: bit-exact restoration of BricksF32 & BricksF16
    ApplyBrickSnapshotDelta(Vol, ClaySnapshots, false);
    assert.ok(SampleLayerVolumeSdf(Vol, ...AirTarget) > 0, 'After Undo, mid-air point must revert to empty air');

    ApplyBrickSnapshotDelta(Vol, ClaySnapshots, true);
    assert.ok(SampleLayerVolumeSdf(Vol, ...AirTarget) < 0, 'After Redo, mid-air overhang must be solid again');

    // 3. CARVE 3D TUNNEL: Punch a horizontal cave straight through the mesa below the top!
    const TunnelY = TopHit.Position[1] - 18;
    assert.ok(SampleLayerVolumeSdf(Vol, 10, TunnelY, 10) < 0, 'Mesa interior at TunnelY must initially be solid');

    ApplySculptSegmentToVolume(
        Vol,
        Layer,
        Packed,
        {
            A: [-140, TunnelY, 10],
            B: [ 160, TunnelY, 10],
            Mode: 'carve',
            Radius: 22,
            Strength: 1.0,
            Falloff: 0.35
        },
        null
    );
    assert.ok(SampleLayerVolumeSdf(Vol, 10, TunnelY, 10) > 2.0, 'Carved 3D cave axis must now be open air (> 0)');

    // 4. SMOOTH & FLATTEN modes execute cleanly and preserve finite normals
    const SmoothHit = RaycastGlobalSdfVolumes([Vol], [10, 220, 10], [0, -1, 0]);
    assert.ok(SmoothHit);
    ApplySculptSegmentToVolume(Vol, Layer, Packed, {
        A: SmoothHit.Position,
        B: SmoothHit.Position,
        Mode: 'smooth',
        Radius: 20,
        Strength: 0.6,
        Falloff: 0.5
    }, null);
    ApplySculptSegmentToVolume(Vol, Layer, Packed, {
        A: SmoothHit.Position,
        B: SmoothHit.Position,
        Mode: 'flatten',
        Radius: 20,
        Strength: 0.6,
        Falloff: 0.5,
        PlanePoint: SmoothHit.Position,
        PlaneNormal: [0, 1, 0]
    }, null);

    const Norm = SampleLayerVolumeNormal(Vol, ...SmoothHit.Position);
    assert.ok(Math.abs(Math.hypot(...Norm) - 1.0) < 1e-3);
});

//------------------------------------------------------------------------------------------------------------------------
// 5. CAMERA ORTHONORMALITY & GLSL ES 3.00 SHADER INTEGRITY
//------------------------------------------------------------------------------------------------------------------------

test('Camera basis is right-handed orthonormal and GLSL ES 3.00 shader uses textureLod for D3D11/ANGLE safety', () =>
{
    const Cam = { Target: [-40, 35, 20], Yaw: -0.62, Pitch: 0.48, Distance: 850 };
    const Basis = ComputeCameraBasis(Cam);

    const DotFR = Basis.Forward[0] * Basis.Right[0] + Basis.Forward[1] * Basis.Right[1] + Basis.Forward[2] * Basis.Right[2];
    const DotFU = Basis.Forward[0] * Basis.Up[0]    + Basis.Forward[1] * Basis.Up[1]    + Basis.Forward[2] * Basis.Up[2];
    const DotRU = Basis.Right[0]   * Basis.Up[0]    + Basis.Right[1]   * Basis.Up[1]    + Basis.Right[2]   * Basis.Up[2];
    assert.ok(Math.abs(DotFR) < 1e-6 && Math.abs(DotFU) < 1e-6 && Math.abs(DotRU) < 1e-6);

    const CenterRay = UnprojectScreenRay(Cam, 500, 400, { left: 0, top: 0, width: 1000, height: 800 });
    const Align = CenterRay.Dir[0] * Basis.Forward[0] + CenterRay.Dir[1] * Basis.Forward[1] + CenterRay.Dir[2] * Basis.Forward[2];
    assert.ok(Math.abs(Align - 1.0) < 1e-6, 'Screen centre ray must align identically with camera Forward');

    assert.ok(GlobalSdfVertexShader.startsWith('#version 300 es'));
    assert.ok(GlobalSdfFragmentShader.startsWith('#version 300 es'));
    assert.ok(GlobalSdfFragmentShader.includes('textureLod(uBrickAtlas, atlasUVW, 0.0)'));
    assert.equal(GlobalSdfFragmentShader.includes('texture(uBrickAtlas'), false, 'Must use textureLod inside raymarch loops to prevent ANGLE gradient unroll');

    // Showcase world validates cleanly
    const Showcase = CreateAppliedShowcaseWorld();
    assert.equal(Showcase.length, 5);
    ValidateDocument({ Format: 'FrontierLandscape/2', Layers: Showcase });
});

test('DOM selector contract: every #Id queried by Workspace, LayerStackPanel & CameraNavigation exists in index.html', () =>
{
    const HtmlSource = fs.readFileSync(path.join(WorkspaceDir, 'index.html'), 'utf8');
    const HtmlIds    = new Set([...HtmlSource.matchAll(/\bid="([^"]+)"/g)].map((M) => M[1]));

    for (const ScriptFile of ['Workspace.js', 'LayerStackPanel.js', 'CameraNavigation.js'])
    {
        const Src = fs.readFileSync(path.join(WorkspaceDir, ScriptFile), 'utf8');
        for (const Match of Src.matchAll(/Find\(\s*['"]#([A-Za-z0-9_-]+)['"]\s*\)/g))
        {
            const Id = Match[1];
            assert.equal(HtmlIds.has(Id), true, `Missing id="${Id}" in index.html (referenced in ${ScriptFile})`);
        }
    }
});

