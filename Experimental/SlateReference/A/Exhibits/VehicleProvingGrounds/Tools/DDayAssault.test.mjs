import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";

import { LevelCatalog, ResolveLevelDescriptor } from "../Source/LevelStructure.js";
import { SceneStructure } from "../Source/FacetStructure.js";
import { VehicleSolver } from "../Source/VehicleSolver.js";
import {
    DDayShapes, DDaySpawn, DDayWallX, DDayBreachGoalX,
    DDayMgTurretSpecs, DDayCannonSpecs, DDayTankSpecs, DDayMineSpecs,
    ResolveDDayPlacements, ResolveDDayBreakables
} from "../Source/DDayStructure.js";
import { DDayAssaultSequence, SegmentIntersectsBox3D } from "../Source/DDayAssaultSequence.js";
import { CreateStarterStudioProject, CompileStudioProjectForWorld } from "../Source/RoadStudioAdapter.js";

function VerifyWatertight2Manifold(Name, Facets)
{
    assert.ok(Facets.length >= 12, `${Name} should emit non-trivial facet geometry`);
    const Q = (V) => `${Math.round(V[0] * 10000)},${Math.round(V[1] * 10000)},${Math.round(V[2] * 10000)}`;
    const DirectedEdges = new Map();
    let SignedVol6 = 0.0;

    for (const F of Facets)
    {
        const [A, B, C] = F.Corners;
        assert.ok(A.every(Number.isFinite) && B.every(Number.isFinite) && C.every(Number.isFinite), `${Name} corner must be finite`);
        const Keys = [Q(A), Q(B), Q(C)];
        if (Keys[0] === Keys[1] || Keys[1] === Keys[2] || Keys[2] === Keys[0])
            continue;
        for (let E = 0; E < 3; ++E)
        {
            const EdgeKey = `${Keys[E]}->${Keys[(E + 1) % 3]}`;
            DirectedEdges.set(EdgeKey, (DirectedEdges.get(EdgeKey) || 0) + 1);
        }
        SignedVol6 += A[0] * (B[1] * C[2] - B[2] * C[1])
                    - A[1] * (B[0] * C[2] - B[2] * C[0])
                    + A[2] * (B[0] * C[1] - B[1] * C[0]);
    }

    let Unpaired = 0;
    for (const [EdgeKey, Count] of DirectedEdges.entries())
    {
        const [U, V] = EdgeKey.split("->");
        const RevCount = DirectedEdges.get(`${V}->${U}`) || 0;
        if (RevCount !== Count)
            Unpaired++;
    }
    assert.equal(Unpaired, 0, `${Name} must be watertight 2-manifold (found ${Unpaired} unpaired edges)`);
    assert.ok(SignedVol6 > 1.0e-4, `${Name} must have positive outward signed volume (got ${SignedVol6 / 6.0})`);
}

async function WithArchiveFetch(Callback)
{
    const PreviousFetch = globalThis.fetch;
    const Root = path.resolve(import.meta.dirname, "..");
    globalThis.fetch = async (Url) =>
    {
        const Rel = String(Url).replace(/^\.\//, "").split("?")[0];
        const Bytes = await fs.readFile(path.join(Root, Rel));
        return { ok: true, arrayBuffer: async () => Bytes.buffer.slice(Bytes.byteOffset, Bytes.byteOffset + Bytes.byteLength) };
    };
    try
    {
        return await Callback();
    }
    finally
    {
        globalThis.fetch = PreviousFetch;
    }
}

test("D-Day level descriptor, all 24 militarized 3D shapes (watertight 2-manifolds) & RoadStudio causeway compile cleanly", () =>
{
    assert.equal(LevelCatalog[0].Key, "dday");
    const Desc = ResolveLevelDescriptor("dday");
    assert.equal(Desc.Key, "dday");
    assert.equal(Desc.DDayAssault, true);
    assert.ok(Desc.Zones.length >= 6);

    const ShapeEntries = Object.entries(DDayShapes);
    assert.equal(ShapeEntries.length, 24);
    for (const [Name, Spec] of ShapeEntries)
        VerifyWatertight2Manifold(`DDayShapes.${Name}`, Spec.Emit());

    const Placements = ResolveDDayPlacements();
    assert.ok(Placements.some((P) => P.Shape === "DDayFortressWallSegment"), "Must place fortified Atlantic Wall segments");
    assert.ok(Placements.some((P) => P.Shape === "DDayWallBreachArch"),      "Must place Atlantic Wall Breach Gate");
    assert.ok(Placements.some((P) => P.Shape === "DDayPillboxBunker"),       "Must place concrete pillbox bunkers");
    assert.ok(Placements.some((P) => P.Shape === "DDayCasemateBunker"),      "Must place artillery casemate bunkers");
    assert.ok(Placements.some((P) => P.Shape === "DDayCoverWall"),           "Must place ballistic cover walls");
    assert.ok(Placements.some((P) => P.Shape === "DDayCzechHedgehog"),       "Must place steel Czech hedgehogs");
    assert.ok(Placements.some((P) => P.Shape === "DDayDragonsTeeth"),        "Must place concrete dragon's teeth");

    const Breakables = ResolveDDayBreakables();
    assert.ok(Breakables.length >= 10, "Must place breakable wooden barricades & concrete blocks");

    const Project = CreateStarterStudioProject("dday");
    const Compiled = CompileStudioProjectForWorld(Project, { LevelKey: "dday" });
    assert.ok(Compiled.VertexCount > 500, "D-Day Assault Causeway & Sapper Flanks must compile road geometry");
});

test("D-Day Scene constructs SDF TankHulls + 124 dynamic combat occupants, and tanks, turrets, cannons, aircraft, mines & Wall Breach work end-to-end", async () =>
{
    await WithArchiveFetch(async () =>
    {
        const Scene = new SceneStructure();
        await Scene.ConstructScene("Archive", 0, "dday");

        assert.ok(Scene.DDayBlock, "Scene.DDayBlock must be allocated on dday level");
        assert.equal(Scene.DDayBlock.TankHullCount, DDayTankSpecs.length);
        assert.ok(Scene.DDayBlock.TankHullFirst + Scene.DDayBlock.TankHullCount <= Scene.DistanceCount, "Advancing TankHulls must sit inside GDF DistanceCount");
        assert.equal(Scene.DDayBlock.MgHeads.length, DDayMgTurretSpecs.length);
        assert.equal(Scene.DDayBlock.CannonBarrels.length, DDayCannonSpecs.length);
        assert.equal(Scene.DDayBlock.Mines.length, DDayMineSpecs.length);
        assert.equal(Scene.DDayBlock.DynamicCount, 124);
        assert.ok(Scene.DDayBlock.DynamicFirst > Scene.DistanceCount, "Dynamic raster combat pool must sit after DistanceCount");
        assert.ok(Scene.Occupants.length < 2047, `Total occupants (${Scene.Occupants.length}) must stay well below IdentityCeiling`);

        const Descriptor = ResolveLevelDescriptor("dday");
        const Vehicle = new VehicleSolver(Scene.QueryObstacleBounds());
        Vehicle.Surfaces = [...Descriptor.Surfaces];
        Vehicle.Respawn(DDaySpawn.Position.slice(), DDaySpawn.Yaw);

        const PuncturedWheels = [];
        const Host = {
            LevelKey:        "dday",
            LevelDescriptor: Descriptor,
            Settings:        { TyreFailure: true },
            Scene,
            Vehicle,
            RoadEditor:      Scene.RoadEditor,
            Sparks:          { Emit() {} },
            PunctureTyre(W)  { PuncturedWheels.push(W); },
            DrivingHud:      { Audio: null },
            Shell:           { Announce() {} }
        };
        Scene.RoadEditor.AttachHost(Host);

        const DDay = new DDayAssaultSequence(Host);
        Host.DDay = DDay;
        assert.equal(DDay.Enabled, true);

        // ① Advancing Allied Tanks move toward the Wall, grant TANK COVER, block ballistic LOS, and explode into wrecks when hit!
        const StartX0 = DDay.Tanks[0].Position[0];
        for (let Step = 0; Step < 45; ++Step)
            DDay.Advance(1.0 / 30.0);
        assert.ok(DDay.Tanks[0].Position[0] > StartX0 + 3.0, "Advancing Allied Tank must roll east (+X) toward the Wall");

        const Tank0 = DDay.Tanks[0];
        Vehicle.Respawn([Tank0.Position[0] - 6.0, Tank0.Position[1], 0.63], 0.0);
        DDay.EvaluatePlayerCover();
        assert.equal(DDay.PlayerCoverState, "TANK COVER", "Car tucked behind advancing Allied Tank must receive TANK COVER");

        const AheadEnemyMuzzle = [Tank0.Position[0] + 45.0, Tank0.Position[1], 1.4];
        const CarCenter        = [Vehicle.Position[0], Vehicle.Position[1], 1.1];
        assert.equal(
            SegmentIntersectsBox3D(
                AheadEnemyMuzzle, CarCenter,
                [Tank0.Position[0] - 3.3, Tank0.Position[1] - 1.8, 0.0],
                [Tank0.Position[0] + 3.3, Tank0.Position[1] + 1.8, 2.8]
            ),
            true
        );
        const Occlusion = DDay.CheckBallisticOcclusion(AheadEnemyMuzzle, CarCenter);
        assert.equal(Occlusion.Occluded, true, "Ballistic fire from ahead bunker must be intercepted by advancing tank");
        assert.equal(Occlusion.Kind, "tank");

        // Hit Tank 0 with lethal 155mm cannon damage -> enters cookoff ("critical") and explodes into burning "wreck"!
        const ArmorBeforeBlast = DDay.PlayerArmor;
        DDay.DamageTank(0, 500.0);
        assert.equal(DDay.Tanks[0].State, "critical", "Destroyed tank enters 1s ammo cookoff warning before exploding");
        for (let Step = 0; Step < 36; ++Step)
            DDay.Advance(1.0 / 30.0);
        assert.equal(DDay.Tanks[0].State, "wreck", "Cooking-off tank must detonate into charred wreck");
        assert.ok(DDay.Stats.TanksDestroyed >= 1);
        assert.ok(DDay.PlayerArmor < ArmorBeforeBlast, "Nearby exposed car takes shockwave damage when adjacent tank explodes");
        assert.ok(DDay.QuerySmokeEmissions().length >= 1, "Exploded tank wreck must emit volumetric fire & smoke plumes");
        assert.ok(DDay.QueryDynamicLightRecords().length >= 3, "Dynamic lights must include blast flashes & Wall searchlights");

        // ② Proximity Landmine detonates on wheel/car contact, heaves VehicleSolver upward, and punctures tyre!
        const Mine0 = DDay.Mines.find((M) => !M.Detonated);
        Vehicle.Respawn([Mine0.Position[0] + 0.35, Mine0.Position[1], 0.63], 0.0);
        DDay.Advance(1.0 / 60.0);
        assert.equal(Mine0.Detonated, true, "Landmine must detonate when car enters trigger radius");
        assert.ok(DDay.Stats.MinesDetonated >= 1);
        assert.ok(Vehicle.LinearVelocity[2] > 2.5, `Mine blast must heave 1.58t VehicleSolver upward (got Vz=${Vehicle.LinearVelocity[2].toFixed(2)} m/s)`);
        assert.ok(PuncturedWheels.length >= 1, "Wheel stepping on Teller-mine must suffer shrapnel puncture");

        // ③ MG turrets fire tracers, 155mm cannons fire arcing shells, and overhead bombers drop aerial bombs!
        Vehicle.Respawn([62.0, 2.0, 0.63], 0.0);
        Vehicle.LinearVelocity = [16.0, 0.0, 0.0];
        for (let Step = 0; Step < 180; ++Step)
            DDay.Advance(1.0 / 30.0);
        assert.ok(DDay.Stats.MgShots > 0, "MG turrets must fire live tracer bursts");
        assert.ok(DDay.Stats.CannonShots > 0, "155mm cannons must fire artillery shells");
        assert.ok(DDay.Stats.BombsDropped > 0, "Overhead bomber aircraft must drop aerial bombs");

        // ④ Stricken Crash-Plane dives and crashes into/near the car with fiery explosion!
        const CrashesBefore = DDay.Stats.AircraftCrashes;
        DDay.LaunchKamikazeDive(2, [Vehicle.Position[0] + 6.0, Vehicle.Position[1], 0.4]);
        assert.equal(DDay.Aircraft[2].State, "diving");
        for (let Step = 0; Step < 160 && DDay.Stats.AircraftCrashes === CrashesBefore; ++Step)
            DDay.Advance(1.0 / 30.0);
        assert.ok(DDay.Stats.AircraftCrashes > CrashesBefore, "Stricken aircraft must crash-explode near the car");

        // ⑤ AI Assault Autopilot weaves hazards and races east (+X) toward the fortified Wall!
        DDay.ResetMission(true);
        Vehicle.Respawn(DDaySpawn.Position.slice(), DDaySpawn.Yaw);
        DDay.SetControlMode("autopilot");
        const AutoStartX = Vehicle.Position[0];
        for (let Step = 0; Step < 120; ++Step)
            DDay.Advance(1.0 / 30.0);
        assert.ok(Vehicle.Position[0] > AutoStartX + 18.0, `AI Assault Autopilot must race east toward Wall (X=${Vehicle.Position[0].toFixed(1)})`);

        // ⑥ Reaching the Atlantic Fortress Wall Breach Gate (X >= 310, |Y| <= 12) completes the Wall Run!
        DDay.SetControlMode("player");
        Vehicle.Respawn([DDayBreachGoalX + 2.0, 0.0, 0.63], 0.0);
        DDay.Advance(1.0 / 60.0);
        assert.equal(DDay.WallReached, true, `Crossing X=${DDayBreachGoalX}m into Wall=${DDayWallX}m Breach Gate must trigger victory`);
    });
});
