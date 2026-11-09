import assert from "node:assert/strict";
import path from "node:path";

export default {
    BeforeFrame(Host, Frame)
    {
        const CapDir = process.env.CAPTURE_DIR || "";
        if (Frame === 2)
        {
            Host.FixedStep = 1 / 30;
            assert.equal(Host.LevelKey, "dday", "Harness must boot D-Day · Militarized Wall Run");
            assert.ok(Host.DDay && Host.DDay.Enabled, "Host.DDay must be initialized and enabled");
            assert.ok(Host.Scene.DDayBlock, "Scene.DDayBlock must be allocated");
        }
        if (Frame === 8 && CapDir && globalThis.SaveCapture)
        {
            globalThis.SaveCapture(path.join(CapDir, "01_beachhead_start.png"));
        }
        if (Frame === 10)
        {
            // Jump into Tank Column & Pillbox Line right behind Lead Sherman Tank 0 for cover
            const T0 = Host.DDay.Tanks[0];
            Host.Vehicle.Respawn([T0.Position[0] - 6.0, T0.Position[1], 0.65], 0.0);
            Host.PositionVehicle();
        }
        if (Frame === 16)
        {
            // Trigger an adjacent tank explosion + stricken aircraft dive right ahead of the car!
            Host.DDay.DetonateTank(Host.DDay.Tanks[1]);
            Host.DDay.LaunchKamikazeDive(2, [Host.Vehicle.Position[0] + 22.0, Host.Vehicle.Position[1] - 3.5, 0.45]);
        }
        if (Frame === 24 && CapDir && globalThis.SaveCapture)
        {
            globalThis.SaveCapture(path.join(CapDir, "02_tanks_bunkers_combat.png"));
        }
        if (Frame === 26)
        {
            // Jump to Artillery Gauntlet facing the towering Atlantic Fortress Wall & Breach Gate + detonate a nearby mine
            Host.Vehicle.Respawn([236.0, 0.0, 0.65], 0.0);
            Host.PositionVehicle();
            const NearMine = Host.DDay.Mines.findIndex((M) => !M.Detonated && M.Position[0] > 240.0);
            if (NearMine >= 0)
                Host.DDay.TriggerMine(NearMine, false);
        }
        if (Frame === 38 && CapDir && globalThis.SaveCapture)
        {
            globalThis.SaveCapture(path.join(CapDir, "03_wall_approach_barrage.png"));
        }
        if (Frame === 42)
        {
            // Sprint across the Atlantic Fortress Wall Breach Gate!
            Host.Vehicle.Respawn([312.0, 0.0, 0.65], 0.0);
            Host.PositionVehicle();
        }
        if (Frame === 52 && CapDir && globalThis.SaveCapture)
        {
            globalThis.SaveCapture(path.join(CapDir, "04_wall_breach_victory.png"));
        }
    },
    Report(Host)
    {
        const Sum = Host.DDay.QuerySummary();
        assert.equal(Sum.WallReached, true, "D-Day harness must reach and breach the fortified Atlantic Wall");
        assert.ok(Sum.Stats.TanksDestroyed >= 1, "At least one Allied Tank must have exploded into a burning wreck");
        assert.ok(Sum.Stats.MinesDetonated >= 1, "At least one Teller-mine must have detonated");
        console.log(
            `dday assault integration: WallBreached=${Sum.WallReached}, ActiveTanks=${Sum.ActiveTanks}/6, ` +
            `TanksDestroyed=${Sum.Stats.TanksDestroyed}, MinesDetonated=${Sum.Stats.MinesDetonated}, Armor=${Math.round(Sum.PlayerArmor)}%`
        );
    }
};
