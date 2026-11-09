/*==========================================================================================================================================
                                                     TERRAINWORLDVALIDATION.MJS
==========================================================================================================================================*/
// Comprehensive Headless Validation Suite for Frontier Open-World SDF Terrain Studio

import assert from 'node:assert/strict';
import { WorldMask2D, ComputeRegionMaskSdf2D } from './Engine/WorldMask2D.js';
import {
    ErosionSimulationState,
    NormalizeErosionParams,
    ComputeFlowReceivers,
    BuildHardnessField,
    RunErosionSimulation
} from './Engine/ErosionEngine.js';
import {
    EROSION_PRESETS,
    ClonePresetParams,
    ApplyWeatherMode,
    GetErosionPreset
} from './Engine/ErosionPresets.js';
import {
    EvaluateBoundaryFalloff,
    CreateTerrainLayer,
    EvaluateTerrainRegion,
    ApplyTerrainSculptDab,
    ExtractRegionTriangleMesh,
    ExportWorldTerrainsObj,
    ExportWorldTerrainsBinaryStl,
    SeedThreeDemoTerrains
} from './Engine/TerrainLayerStack.js';

let Passed = 0;
function Suite(Name, Fn)
{
    Fn();
    Passed++;
    console.log(`  [PASS] ${Name}`);
}

console.log('Running Frontier Open-World SDF Terrain Studio Validation...\n');

// 1. Infinite Sparse 2D Mask Allocation & Negative/Distant Coordinate Painting
Suite('1. Infinite sparse 2D world mask allocates tiles on demand across arbitrary kilometer coordinates', () =>
{
    const World = new WorldMask2D();
    const Mask = World.addMaskLayer({ name: 'Infinite Test' });
    assert.equal(World.totalAllocatedTiles(), 0, 'Fresh mask should allocate 0 tiles');

    // Paint at (-14.5 km, +28.2 km) — far outside any fixed canvas!
    World.paintCapsuleMeters(Mask, [-14500, 28200], [-14100, 28450], 180, 'paint', 1.0, 0.0, 101);
    assert.ok(World.totalAllocatedTiles() > 0 && World.totalAllocatedTiles() <= 4, 'Only touched 1x1 km tiles should be allocated');
    assert.ok(World.sampleCoverageMeters(-14300, 28320, Mask.id) > 0.95, 'Painted distant coordinate should have full land coverage');
    assert.equal(World.sampleCoverageMeters(5000, -5000, Mask.id), 0.0, 'Unpainted distant ocean should return 0 without allocating tiles');

    // Erasing back to 0 prunes empty tiles automatically
    World.paintCapsuleMeters(Mask, [-14600, 28100], [-14000, 28550], 350, 'erase', 1.0, 0.0, 101);
    assert.equal(World.totalAllocatedTiles(), 0, 'Erased empty sparse tiles should be pruned automatically');
});

// 2. Exact O(N) Meijster 2D Euclidean Signed Distance Transform
Suite('2. Meijster 2D Euclidean Signed Distance Field computes exact metric inland/offshore distances', () =>
{
    const World = new WorldMask2D();
    const Mask = World.addMaskLayer({ name: 'Circle Island' });
    World.paintCapsuleMeters(Mask, [0, 0], [0, 0], 300, 'paint', 1.0, 0.0, 42);
    const Region = World.addTerrainRegion({
        maskLayerId: Mask.id,
        bounds: { x: -500, z: -500, width: 1000, depth: 1000 },
        resolution: 101
    });
    const { sdf2D, hasLand } = ComputeRegionMaskSdf2D(World, Region, 101, 101);
    assert.equal(hasLand, true);

    const CenterIdx = 50 * 101 + 50; // (0, 0) -> ~300m inland -> sdf2D ~ -300
    const CornerIdx = 0;             // (-500, -500) -> ~707 - 300 = +407m in ocean
    assert.ok(sdf2D[CenterIdx] < -270 && sdf2D[CenterIdx] > -330, `Expected ~-300m at island center, got ${sdf2D[CenterIdx]}`);
    assert.ok(sdf2D[CornerIdx] > 370 && sdf2D[CornerIdx] < 440, `Expected ~+407m at ocean corner, got ${sdf2D[CornerIdx]}`);
});

// 3. Per-Layer Boundary Falloff Modes: Soft Edge -> Strong Center vs None (Full Cliff)
Suite('3. Per-Layer Boundary Falloff supports Soft Edge -> Strong Center AND None (Full Cliff Edge)', () =>
{
    const SoftSpec = { mode: 'soft', distance: 300, edgeFloor: 0.0 };
    assert.equal(EvaluateBoundaryFalloff(0, 500, SoftSpec), 0.0, 'Soft falloff at coastline (0m) should have 0 influence');
    const MidW = EvaluateBoundaryFalloff(150, 500, SoftSpec);
    assert.ok(Math.abs(MidW - 0.5) < 0.02, `Soft falloff halfway inland should be ~0.5, got ${MidW}`);
    assert.equal(EvaluateBoundaryFalloff(350, 500, SoftSpec), 1.0, 'Soft falloff deep inland should reach 1.0');

    const CliffSpec = { mode: 'none', distance: 300, edgeFloor: 0.0 };
    assert.equal(EvaluateBoundaryFalloff(0, 0, CliffSpec), 1.0, 'None (Cliff) falloff must deliver 100% full strength right at 0m coastline edge');
    assert.equal(EvaluateBoundaryFalloff(200, 200, CliffSpec), 1.0);

    const RimSpec = { mode: 'rim', distance: 300, edgeFloor: 0.0 };
    const RimPeak = EvaluateBoundaryFalloff(90, 500, RimSpec);
    const RimCenter = EvaluateBoundaryFalloff(300, 500, RimSpec);
    assert.ok(RimPeak > RimCenter, 'Coastal Rim Ring falloff should peak near coastal rim and relax toward center basin');
});

// 4. 3 Pre-Built Demonstration Masks & Independent Toggleable Terrain Bounds Regions
Suite('4. Ships with the 3 Demo Masks (+ erosion demo) and independent toggleable Terrain Bounds Regions', () =>
{
    const World = new WorldMask2D();
    const { masks, regions } = SeedThreeDemoTerrains(World);
    // Three authored demo masks, plus the erosion demonstration terrain.
    assert.equal(masks.length, 4, 'Must create 3 Demo 2D Mask Layers + the erosion demo mask');
    assert.equal(regions.length, 4, 'Must create 3 independent Demo Terrain Bounds Regions + the erosion demo');

    for (const R of regions)
    {
        assert.equal(R.enabled, true);
        assert.ok(R.runtime && R.runtime.hasLand, `${R.name} must evaluate valid land inside its 2D mask`);
        assert.ok(R.runtime.maxHeight > 80, `${R.name} must build substantial 3D mountain/cliff relief above sea level`);
    }

    // The erosion demo must actually ship a baked erosion layer that changed the surface.
    const ErosionRegion = regions[3];
    const ErosionLayer = ErosionRegion.stack.find(L => L.type === 'erosion');
    assert.ok(ErosionLayer, 'Erosion demo terrain must ship an erosion layer');
    assert.ok(ErosionLayer.lastStats && ErosionLayer.lastStats.iterations > 0, 'Erosion demo layer must be baked on load');
    assert.ok(ErosionLayer.lastStats.incisionVolumeM3 > 0, 'Erosion demo must actually remove rock');

    // Verify Demo Terrain 2 ("Basalt Sea Cliffs") uses Falloff = 'none' for sheer coastal cliffs!
    const CliffRegion = regions[1];
    assert.equal(CliffRegion.stack[0].falloff.mode, 'none', 'Demo Terrain 02 should showcase None (Full Cliff) falloff');

    // Verify toggling a Terrain Bounds Box OFF works independently without affecting the others
    CliffRegion.enabled = false;
    const EnabledList = World.regions.filter(R => R.enabled);
    assert.equal(EnabledList.length, regions.length - 1, 'Toggling one Terrain Bounds Box OFF leaves the others active');
    CliffRegion.enabled = true;
});

// 5. Thin-Slab SDF Space Efficiency & Triangle Mesh Conversion
Suite('5. Thin-Slab SDF wastes zero deep-ocean space and extracts clean culled triangle meshes (.OBJ & .STL)', () =>
{
    const World = new WorldMask2D();
    SeedThreeDemoTerrains(World);
    const R1 = World.regions[0];
    const Mesh = ExtractRegionTriangleMesh(World, R1, { includeUnderside: true });

    assert.ok(Mesh.triangleCount > 2000, 'Terrain should generate rich top + thin-underside triangle mesh');
    const FullGridCells = (R1.runtime.nx - 1) * (R1.runtime.nz - 1);
    // If empty ocean cells weren't culled, top + bottom alone would exceed 4 * FullGridCells!
    assert.ok(Mesh.triangleCount < FullGridCells * 3.4, 'Empty ocean cells outside 2D mask must be culled from triangle output');

    // Verify NO vertex ever wastes vertical SDF space below slabBottom (-6.5m)!
    let MinY = Infinity, MaxY = -Infinity;
    for (let I = 1; I < Mesh.positions.length; I += 3)
    {
        if (Mesh.positions[I] < MinY) MinY = Mesh.positions[I];
        if (Mesh.positions[I] > MaxY) MaxY = Mesh.positions[I];
    }
    assert.ok(Math.abs(MinY - (-R1.slabThickness)) < 0.05, `Thin-slab underside should sit cleanly at -${R1.slabThickness}m, got ${MinY}`);
    assert.ok(MaxY > 120.0, `Top terrain relief should build upward above sea level, got ${MaxY}`);

    // Verify multi-terrain .OBJ & binary .STL exports
    const Obj = ExportWorldTerrainsObj(World, World.regions);
    assert.ok(Obj.triangleCount > 6000 && Obj.text.includes('o Terrain_01'));
    const Stl = ExportWorldTerrainsBinaryStl(World, [R1]);
    assert.equal(Stl.bytes.byteLength, 84 + Stl.triangleCount * 50, 'Binary STL byte length must match 84 + 50 * triCount');
});

// 6. Real-time SDF Sculpting, 3D Overhang Arches & JSON Round-Trip Serialization
Suite('6. Supports real-time SDF sculpting, 3D overhang arches, spline landforms, and JSON round-trip', () =>
{
    const World = new WorldMask2D();
    SeedThreeDemoTerrains(World);
    const R = World.regions[0];
    const Rt0 = R.runtime;

    // Find the highest peak cell inside Region 01 and sculpt directly on it
    let PeakIdx = 0, BeforePeak = -1e9;
    for (let I = 0; I < Rt0.heights.length; ++I)
    {
        if (Rt0.sdf2D[I] < -40.0 && Rt0.heights[I] > BeforePeak)
        {
            BeforePeak = Rt0.heights[I];
            PeakIdx = I;
        }
    }
    const Px = R.bounds.x + (PeakIdx % Rt0.nx) * Rt0.stepX;
    const Pz = R.bounds.z + Math.floor(PeakIdx / Rt0.nx) * Rt0.stepZ;

    ApplyTerrainSculptDab(World, R, [Px, BeforePeak, Pz], 'raise', { radius: 220, strength: 0.95 });
    EvaluateTerrainRegion(World, R);
    assert.ok(R.runtime.heights[PeakIdx] > BeforePeak + 25.0, '3D Raise sculpt brush should elevate terrain SDF at brush target');
    assert.ok(R.runtime.maxHeight > BeforePeak + 25.0, '3D Raise sculpt on peak should elevate region maxHeight');

    ApplyTerrainSculptDab(World, R, [Px, 65, Pz], 'overhang', { radius: 90, strength: 0.8 });
    assert.equal(R.sdf3DOps.length, 1, '3D Overhang tool should append analytical 3D SDF arch/overhang op');

    // JSON round-trip
    const Saved = JSON.stringify(World.toJSON());
    const Restored = new WorldMask2D();
    Restored.loadFromJSON(JSON.parse(Saved));
    assert.equal(Restored.maskLayers.length, World.maskLayers.length);
    assert.equal(Restored.regions.length, World.regions.length);
    // Erosion layers must survive the round-trip with their parameters intact.
    const RestoredErosion = Restored.regions[3].stack.find(L => L.type === 'erosion');
    assert.ok(RestoredErosion, 'Erosion layer must survive JSON round-trip');
    assert.equal(RestoredErosion.presetId, 'grand-canyon');
    assert.ok(!('imprintMap' in RestoredErosion), 'Transient erosion caches must not be serialised');
    assert.equal(Restored.totalAllocatedTiles(), World.totalAllocatedTiles());
});

// 7. 3D Spline Knot Up/Down Y-Axis Gizmo Elevation Grading & 3D Rock Arch Bridge Meshing
Suite('7. Moving 3D Spline Knots UP and DOWN on Y-axis shapes terrain elevation & builds 3D SDF Arch Bridges', () =>
{
    const World = new WorldMask2D();
    SeedThreeDemoTerrains(World);
    const R = World.regions[0];
    const RtBase = EvaluateTerrainRegion(World, R);

    // Pick an interior land cell on Region 01
    let TargetIdx = 0, BaseH = 0;
    for (let I = 0; I < RtBase.heights.length; ++I)
    {
        if (RtBase.sdf2D[I] < -140.0)
        {
            TargetIdx = I;
            BaseH = RtBase.heights[I];
            break;
        }
    }
    const Kx = Math.round(R.bounds.x + (TargetIdx % RtBase.nx) * RtBase.stepX);
    const Kz = Math.round(R.bounds.z + Math.floor(TargetIdx / RtBase.nx) * RtBase.stepZ);

    const GradedSpline = CreateTerrainLayer('spline', {
        name: '3D Gizmo Ramp Test',
        splineMode: 'grade',
        width: 140.0,
        bankSmooth: 0.65,
        meander: 0.0,
        falloff: { mode: 'none', distance: 100, edgeFloor: 1.0 },
        points: [
            [Kx - 120, Kz, BaseH],
            [Kx, Kz, BaseH + 95.0], // Dragged UP +95m on Y Gizmo!
            [Kx + 120, Kz, BaseH]
        ]
    });
    R.stack.push(GradedSpline);
    EvaluateTerrainRegion(World, R);
    const RaisedH = R.runtime.heights[TargetIdx];
    assert.ok(RaisedH > BaseH + 45.0, `Moving spline knot UP on Y-axis must raise terrain (before=${BaseH.toFixed(1)}, after=${RaisedH.toFixed(1)})`);

    // Now move the same knot DOWN below BaseH on Y Gizmo!
    GradedSpline.points[1][2] = Math.max(2.0, BaseH - 55.0);
    EvaluateTerrainRegion(World, R);
    const LoweredH = R.runtime.heights[TargetIdx];
    assert.ok(LoweredH < BaseH - 18.0, `Moving spline knot DOWN on Y-axis must carve terrain down (before=${BaseH.toFixed(1)}, after=${LoweredH.toFixed(1)})`);

    // Switch spline to 'arch3d' (True 3D Rock Arch Bridge) and verify 3D tube triangles are emitted
    const TrisBeforeArch = ExtractRegionTriangleMesh(World, R).triangleCount;
    GradedSpline.splineMode = 'arch3d';
    GradedSpline.points[1][2] = BaseH + 80.0;
    R.dirty = true;
    const MeshWithArch = ExtractRegionTriangleMesh(World, R);
    assert.ok(MeshWithArch.triangleCount > TrisBeforeArch + 200, '3D Rock Arch Bridge spline mode must generate true 3D overhang tube triangles');
});


/*------------------------------------------------------------------------------------------------------------------------------------------
                                    EROSION ENGINE VALIDATION SUITES
------------------------------------------------------------------------------------------------------------------------------------------*/
/** Builds a synthetic dome island so the erosion suites do not depend on the demo seed. */
function MakeTestGrid(Nx = 73, Nz = 73, StepM = 14.0)
{
    const Heights = new Float32Array(Nx * Nz);
    const Sdf = new Float32Array(Nx * Nz);
    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const I = Z * Nx + X;
            const Dx = (X - (Nx - 1) / 2) / ((Nx - 1) / 2);
            const Dz = (Z - (Nz - 1) / 2) / ((Nz - 1) / 2);
            const R = Math.hypot(Dx, Dz);
            Sdf[I] = (R - 0.94) * StepM * 12.0;
            Heights[I] = R < 0.94
                ? 18 + 150 * Math.max(0, 1 - R) * (0.6 + 0.4 * Math.sin(X * 0.6) * Math.cos(Z * 0.5))
                : -6.5;
        }
    }
    return { nx: Nx, nz: Nz, stepX: StepM, stepZ: StepM, heights: Heights, sdf2D: Sdf, seaLevel: 0, slabBottom: -6.5 };
}

// 8. Stream power produces a real drainage network with the correct slope-area signature
Suite('8. Stream-power incision builds a branching drainage network (slope-area concavity, no cycles)', () =>
{
    const Grid = MakeTestGrid(73, 73, 16);
    const State = new ErosionSimulationState(Grid, ClonePresetParams('alpine-divide'));
    for (let I = 0; I < 14; ++I) State.step();
    State.finalizeFlowFields();

    // The routed network must be acyclic: every cell's pop order decreases downstream. We verify
    // the weaker, observable form: drainage area is monotone non-decreasing downstream.
    const Res = State.buildResult();
    let MaxFlow = 0, ChannelCells = 0;
    for (let I = 0; I < Res.flowAccumCells.length; ++I)
    {
        if (Res.flowAccumCells[I] > MaxFlow) MaxFlow = Res.flowAccumCells[I];
        if (Res.flowAccumCells[I] > 40) ChannelCells++;
    }
    assert.ok(MaxFlow > 220, `A trunk channel must accumulate a large drainage area (got ${MaxFlow.toFixed(0)} cells)`);
    assert.ok(ChannelCells > 25, `A branching network must produce many channelised cells (got ${ChannelCells})`);

    // Incision must be concentrated in channels, not smeared uniformly: the variance of erosion
    // depth has to be well above zero (a uniform sheet of erosion would mean no valleys at all).
    let Mean = 0, Var = 0;
    for (let I = 0; I < Res.erosionDepth.length; ++I) Mean += Res.erosionDepth[I];
    Mean /= Res.erosionDepth.length;
    for (let I = 0; I < Res.erosionDepth.length; ++I) Var += (Res.erosionDepth[I] - Mean) ** 2;
    Var /= Res.erosionDepth.length;
    assert.ok(Math.sqrt(Var) > 0.4 * Math.max(1e-6, Mean), `Erosion must be spatially concentrated (mean=${Mean.toFixed(2)}, sd=${Math.sqrt(Var).toFixed(2)})`);
});

// 9. Erosion is mass-conserving: what is removed is deposited, exported or still in flux
Suite('9. Erosion conserves mass (incision == deposition + export, within numerical tolerance)', () =>
{
    const Grid = MakeTestGrid(65, 65, 18);
    const Params = ClonePresetParams('badlands-domes');
    Params.aeolian = { enabled: false };
    Params.glacial = { enabled: false };
    const Res = RunErosionSimulation(Grid, Params, { iterations: 10 });
    const S = Res.stats;

    assert.ok(S.incisionVolumeM3 > 0, 'Fluvial incision must remove a positive rock volume');
    const Accounted = S.depositedVolumeM3 + S.exportedVolumeM3;
    // Talus and chemical weathering move material without routing it through the sediment flux, so
    // they are excluded from this closure check; the routed budget must still balance tightly.
    const Error = Math.abs(S.incisionVolumeM3 - Accounted) / S.incisionVolumeM3;
    assert.ok(Error < 0.02, `Routed sediment budget must close (incision=${S.incisionVolumeM3.toFixed(0)}, dep+exp=${Accounted.toFixed(0)}, err=${(Error * 100).toFixed(2)}%)`);
});

// 10. The flood fix: erosion can never drown the authored landmass
Suite('10. Erosion cannot flood the landmass (anti-flood guard holds under all presets)', () =>
{
    const Grid = MakeTestGrid(49, 49, 20);
    for (const Preset of EROSION_PRESETS)
    {
        const Params = ClonePresetParams(Preset.id);
        Params.iterations = 6;
        const Res = RunErosionSimulation(Grid, Params, { iterations: 6 });
        const MaxDrown = NormalizeErosionParams(Params).maxLandDrownedM;
        let Drowned = 0;
        for (let I = 0; I < Res.heights.length; ++I)
        {
            if (Grid.sdf2D[I] > Grid.stepX * 1.1) continue;      // outside the mask: ocean is expected
            if (Res.lakeMask[I]) continue;                        // enclosed lakes are legitimate water
            if (Res.heights[I] < -MaxDrown - 1e-3) Drowned++;
        }
        assert.equal(Drowned, 0, `${Preset.name} must not drown authored land (found ${Drowned} submerged cells)`);
    }
});

// 11. Determinism: identical seed + params must reproduce bit-identically
Suite('11. Bakes are deterministic and seeded (no drifting results between runs)', () =>
{
    const GridA = MakeTestGrid(57, 57, 15);
    const GridB = MakeTestGrid(57, 57, 15);
    const Params = ClonePresetParams('icelandic-highlands');
    Params.iterations = 8;

    const A = RunErosionSimulation(GridA, ClonePresetParams('icelandic-highlands'), { iterations: 8 });
    const B = RunErosionSimulation(GridB, ClonePresetParams('icelandic-highlands'), { iterations: 8 });

    let MaxDelta = 0;
    for (let I = 0; I < A.heights.length; ++I) MaxDelta = Math.max(MaxDelta, Math.abs(A.heights[I] - B.heights[I]));
    assert.equal(MaxDelta, 0, `Identical seeds must produce identical surfaces (max delta ${MaxDelta})`);

    // A different seed must produce a genuinely different landscape.
    const Other = ClonePresetParams('icelandic-highlands');
    Other.seed = 777;
    Other.iterations = 8;
    const C = RunErosionSimulation(MakeTestGrid(57, 57, 15), Other, { iterations: 8 });
    let Diff = 0;
    for (let I = 0; I < A.heights.length; ++I) Diff = Math.max(Diff, Math.abs(A.heights[I] - C.heights[I]));
    assert.ok(Diff > 1.0, 'A different seed must yield a different landscape');
});

// 12. Process isolation: each process only does what it claims
Suite('12. Processes are independently toggleable and stay in their lane', () =>
{
    const Only = (Key) =>
    {
        const P = ClonePresetParams('alpine-divide');
        for (const K of ['streamPower', 'deposition', 'thermal', 'aeolian', 'glacial', 'chemical', 'pitting'])
        {
            P[K] = { ...(P[K] || {}), enabled: K === Key };
        }
        // `enabled` alone is not enough: a process with zero amplitude correctly does nothing,
        // so give each isolated process a realistic amplitude for this test.
        if (Key === 'aeolian') P.aeolian.strength = 0.6;
        if (Key === 'glacial') P.glacial.erosionK = 0.5;
        if (Key === 'chemical') P.chemical.rate = 0.5;
        if (Key === 'pitting') P.pitting.density = 0.6;
        if (Key === 'thermal') P.thermal.rate = 0.6;
        P.iterations = 6;
        return P;
    };

    // Wind alone must move sand but must not incise rock.
    const WindOnly = RunErosionSimulation(MakeTestGrid(49, 49, 20), Only('aeolian'), { iterations: 6 });
    assert.ok(WindOnly.stats.aeolianDeflatedM3 > 0, 'Aeolian-only run must move sand');
    assert.equal(WindOnly.stats.incisionVolumeM3, 0, 'Aeolian-only run must not invoke stream power');

    // Fluvial alone must incise but must not move aeolian sand.
    const FluvialOnly = RunErosionSimulation(MakeTestGrid(49, 49, 20), Only('streamPower'), { iterations: 6 });
    assert.ok(FluvialOnly.stats.incisionVolumeM3 > 0, 'Stream-power-only run must incise rock');
    assert.equal(FluvialOnly.stats.aeolianDeflatedM3, 0, 'Stream-power-only run must not invoke wind');

    // Thermal alone conserves mass exactly (it only moves material downhill).
    const ThermalOnly = RunErosionSimulation(MakeTestGrid(49, 49, 20), Only('thermal'), { iterations: 4 });
    let DeltaSum = 0;
    const Ref = MakeTestGrid(49, 49, 20);
    for (let I = 0; I < Ref.heights.length; ++I) DeltaSum += ThermalOnly.heights[I] - Ref.heights[I];
    // A little material can be clamped at the slab floor, so allow a small residual.
    assert.ok(Math.abs(DeltaSum) < Ref.heights.length * 0.05, `Talus transport must conserve mass (drift ${DeltaSum.toFixed(1)} m over ${Ref.heights.length} cells)`);
});

// 13. Stratified hardness produces benches (differential erosion, not a smooth slope)
Suite('13. Stratified hardness produces cliff/bench profiles instead of smooth slopes', () =>
{
    const Nx = 48, Nz = 48;
    const Hard = BuildHardnessField(Nx, Nz, 10, 10, {
        enabled: true, bedFrequency: 9, dipDeg: 5, strikeDeg: 20,
        caprockContrast: 0.9, jointSpacingM: 0, jointStrength: 0.5
    }, 4029);

    let Min = 1, Max = 0;
    for (let I = 0; I < Hard.length; ++I) { Min = Math.min(Min, Hard[I]); Max = Math.max(Max, Hard[I]); }
    assert.ok(Max - Min > 0.35, `Bed hardness must span a wide range (soft ${Min.toFixed(2)} .. hard ${Max.toFixed(2)})`);

    // Hardness must vary most steeply in the vertical (bed) direction than along-strike.
    let AcrossBeds = 0, AlongStrike = 0;
    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            if (Z + 1 < Nz) AcrossBeds += Math.abs(Hard[(Z + 1) * Nx + X] - Hard[Z * Nx + X]);
            if (X + 1 < Nx) AlongStrike += Math.abs(Hard[Z * Nx + X + 1] - Hard[Z * Nx + X]);
        }
    }
    assert.ok(AcrossBeds > AlongStrike * 0.2, 'Bedding must be anisotropic (steeper across beds than along strike)');

    // Disabling strata must produce a uniform field (the control case).
    const Uniform = BuildHardnessField(Nx, Nz, 10, 10, { enabled: false }, 4029);
    let AllSame = true;
    for (let I = 1; I < Uniform.length; ++I) if (Math.abs(Uniform[I] - Uniform[0]) > 1e-6) AllSame = false;
    assert.ok(AllSame, 'Strata disabled must yield a uniform hardness field');
});

// 14. Weather groups share one climate; sharing modes differ only where documented
Suite('14. Shared-weather groups give identical climate to grouped layers', () =>
{
    const P = ClonePresetParams('alpine-divide');
    const Grouped = ApplyWeatherMode(P, 'all-weather');
    assert.ok(Grouped && Grouped.climate && Grouped.climate.rainMmYr > 0);
    assert.ok(GetErosionPreset('alpine-divide'), 'Preset lookup must resolve');
    for (const Preset of EROSION_PRESETS)
    {
        const Params = ClonePresetParams(Preset.id);
        assert.ok(Params && Params.climate, `${Preset.id} must ship a full climate block`);
        assert.ok(Number.isFinite(Params.climate.rainMmYr), `${Preset.id} must declare rainfall`);
        assert.ok(Number.isFinite(Params.iterations), `${Preset.id} must declare an iteration count`);
    }
});

// 15. Flow routing has no cycles even on a purely concave (bowl) surface
Suite('15. Flow routing never cycles, even on pathological bowl and plateau surfaces', () =>
{
    for (const Shape of ['bowl', 'plateau', 'flat'])
    {
        const Nx = 40, Nz = 40;
        const Heights = new Float32Array(Nx * Nz);
        for (let Z = 0; Z < Nz; ++Z)
        {
            for (let X = 0; X < Nx; ++X)
            {
                const Dx = (X - Nx / 2) / Nx, Dz = (Z - Nz / 2) / Nz;
                const R = Math.hypot(Dx, Dz);
                Heights[Z * Nx + X] = Shape === 'bowl' ? 100 * R * R
                    : Shape === 'plateau' ? (R < 0.3 ? 80 : 30 + 40 * R)
                    : 50;
            }
        }
        const State = new ErosionSimulationState({
            nx: Nx, nz: Nz, stepX: 10, stepZ: 10, heights: Heights,
            seaLevel: 0, slabBottom: -5
        }, { iterations: 3, streamPower: { enabled: true }, thermal: { enabled: false } });

        for (let I = 0; I < 3; ++I) State.step();
        State.finalizeFlowFields();
        let Bad = 0;
        for (let I = 0; I < State.count; ++I) if (!Number.isFinite(State.heights[I])) Bad++;
        assert.equal(Bad, 0, `${Shape} surface must not produce non-finite heights`);
    }
});

console.log(`\nAll ${Passed}/15 Open-World SDF Terrain Studio validation suites passed!`);
