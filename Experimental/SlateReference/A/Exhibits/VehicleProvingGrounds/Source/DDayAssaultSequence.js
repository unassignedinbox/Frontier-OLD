//============================================================================================================================================
//                                                        DDAYASSAULTSEQUENCE.JS
//============================================================================================================================================
// 🧩 D-Day Militarized Zone & Race-to-the-Wall Live Combat Director ("dday"):
//    • 6 Advancing Allied Assault Tanks rolling toward the Atlantic Fortress Wall (X = +318 m) — provide true 3D ray-tested
//      ballistic cover + dynamic obstacle shields for the player's car, take heavy cannon/MG fire, burn, and explode into wrecks!
//    • 8 Rotating Twin-Barrel Machine-Gun Turrets & 6 Heavy 155mm Fortress/Wall Cannons firing live 3D tracers & ballistic shells
//    • 4 WWII Attack Aircraft: 2 Dive-Bombers dropping sticks of finned 250 kg Aerial Bombs + 2 Stricken Fighter-Bombers catching
//      flak fire and crashing into/ahead of the cars with screaming dive-sirens & massive crash explosions!
//    • 30 Armed Anti-Tank Teller-Mines that detonate on wheel/tank contact with upward Newtonian blast impulses & tyre punctures
//    • Tactical Cover Detection (Tank Shield vs Bunker/Barricade Cover), Vehicle Armor Integrity, Wall Breach Victory Timer,
//      and optional AI Assault Autopilot.

import { LightType } from "./LightingSystem.js";
import { CourseMaterial } from "./MaterialSpecification.js";
import {
    DDayWallX,
    DDayBreachGoalX,
    DDayBreachHalfY,
    DDaySpawn,
    DDayMgTurretSpecs,
    DDayCannonSpecs,
    DDayTankSpecs,
    DDayMineSpecs,
    DDayStaticCoverBoxes
} from "./DDayStructure.js";
import {
    ConstructRigidTransform,
    ConstructAxisQuaternion,
    MultiplyQuaternion,
    RotateVector,
    ClampScalar
} from "./TransformProjection.js";

//------------------------------------------------------------------------------------------------------------------------
//                                  POOL CAPACITIES & DETERMINISTIC BATTLEFIELD PRNG
//------------------------------------------------------------------------------------------------------------------------

export const DDayBombPoolSize   = 12;
export const DDayShellPoolSize  = 10;
export const DDayTracerPoolSize = 24;
export const DDayAircraftCount  = 4;

const ParkedUnderground = Object.freeze([0.0, 0.0, -55.0]);
const IdentityQuat      = Object.freeze([0.0, 0.0, 0.0, 1.0]);

function CreateBattleRng(Seed = 19440606)
{
    let State = (Seed >>> 0) || 1;
    return () =>
    {
        State = (Math.imul(State, 1664525) + 1013904223) >>> 0;
        return State / 4294967296.0;
    };
}

function WrapAngle(Rad)
{
    let A = Rad;
    while (A >  Math.PI) A -= 2.0 * Math.PI;
    while (A < -Math.PI) A += 2.0 * Math.PI;
    return A;
}

function StepAngleToward(Current, Target, MaxDelta)
{
    const Diff = WrapAngle(Target - Current);
    if (Math.abs(Diff) <= MaxDelta)
        return Target;
    return WrapAngle(Current + Math.sign(Diff) * MaxDelta);
}

// 📝 Exact 3D Slab Ray-Segment vs AABB intersection on t in [0.02, 0.98] for ballistic cover checks!
export function SegmentIntersectsBox3D(P0, P1, BoxMin, BoxMax)
{
    let TMin = 0.02;
    let TMax = 0.98;
    for (let Axis = 0; Axis < 3; ++Axis)
    {
        const Origin = P0[Axis];
        const Dir    = P1[Axis] - Origin;
        if (Math.abs(Dir) < 1.0e-6)
        {
            if (Origin < BoxMin[Axis] || Origin > BoxMax[Axis])
                return false;
        }
        else
        {
            let T1 = (BoxMin[Axis] - Origin) / Dir;
            let T2 = (BoxMax[Axis] - Origin) / Dir;
            if (T1 > T2)
            {
                const Tmp = T1;
                T1 = T2;
                T2 = Tmp;
            }
            if (T1 > TMin) TMin = T1;
            if (T2 < TMax) TMax = T2;
            if (TMin > TMax)
                return false;
        }
    }
    return true;
}

function OrientationFromVelocity(Vel, Roll = 0.0)
{
    const Horiz = Math.hypot(Vel[0], Vel[1]);
    const Yaw   = Horiz > 1.0e-4 ? Math.atan2(Vel[1], Vel[0]) : 0.0;
    const Pitch = -Math.atan2(Vel[2], Math.max(1.0e-4, Horiz));
    const QYaw   = ConstructAxisQuaternion([0, 0, 1], Yaw);
    const QPitch = ConstructAxisQuaternion([0, 1, 0], Pitch);
    const QRoll  = ConstructAxisQuaternion([1, 0, 0], Roll);
    return MultiplyQuaternion(QYaw, MultiplyQuaternion(QPitch, QRoll));
}

//------------------------------------------------------------------------------------------------------------------------
//                                             DDAY ASSAULT SEQUENCE
//------------------------------------------------------------------------------------------------------------------------

export class DDayAssaultSequence
{
    constructor(Host)
    {
        this.Host        = Host;
        this.Scene       = Host?.Scene || null;
        this.LevelKey    = Host?.LevelKey || "dday";
        this.Enabled     = this.LevelKey === "dday";
        this.Rng         = CreateBattleRng(19440606);
        this.Clock       = 0.0;

        // Player Mission & Tactical State
        this.ControlMode        = "player"; // "player" | "autopilot"
        this.PlayerArmor        = 100.0;
        this.MaxPlayerArmor     = 100.0;
        this.PlayerCoverState   = "EXPOSED"; // "EXPOSED" | "TANK COVER" | "BUNKER COVER"
        this.CoverShieldTankId  = null;
        this.WallReached        = false;
        this.AssaultTimer       = 0.0;
        this.BestAssaultTime    = null;
        this.DistanceToWall     = DDayBreachGoalX - DDaySpawn.Position[0];
        this.WarningBanner      = "";
        this.WarningTimer       = 0.0;
        this.DestroyCooldown    = 0.0;

        // Live Combat Counters (for HUD & deterministic automated verification!)
        this.Stats = {
            TanksDestroyed:  0,
            MinesDetonated:  0,
            BombsDropped:    0,
            BombExplosions:  0,
            AircraftCrashes: 0,
            CannonShots:     0,
            MgShots:         0,
            BlockedByCover:  0,
            RunsCompleted:   0
        };

        // Dynamic Obstacle AABBs registered into VehicleSolver.Obstacles for the 6 Tanks
        this.TankObstacleBoxes = [];

        // ① 6 Allied Assault Tanks
        this.Tanks = DDayTankSpecs.map((Spec, Index) => ({
            Index,
            Id:           Spec.Id,
            Name:         Spec.Name,
            LaneY:        Spec.LaneY,
            BaseSpeed:    Spec.Speed,
            Speed:        Spec.Speed,
            Position:     [Spec.StartX, Spec.LaneY, 0.0],
            Yaw:          0.0,
            TurretYaw:    0.0,
            Armor:        Spec.MaxArmor,
            MaxArmor:     Spec.MaxArmor,
            State:        "advancing", // "advancing" | "critical" | "wreck"
            StateTimer:   0.0,
            FireTimer:    1.4 + Index * 0.75,
            MuzzleFlash:  0.0
        }));

        // ② 8 Rotating Twin-Barrel Machine-Gun Turrets
        this.MgTurrets = DDayMgTurretSpecs.map((Spec, Index) => ({
            Index,
            Id:          Spec.Id,
            Name:        Spec.Name,
            Position:    Spec.Position.slice(),
            Yaw:         Spec.BaseYaw,
            Range:       Spec.Range,
            CycleTimer:  Spec.Phase,
            ShotCooldown:0.0,
            MuzzleFlash: 0.0,
            TargetKind:  "none"
        }));

        // ③ 6 Heavy 155mm Fortress & Wall Artillery Cannons
        this.Cannons = DDayCannonSpecs.map((Spec, Index) => ({
            Index,
            Id:          Spec.Id,
            Name:        Spec.Name,
            Position:    Spec.Position.slice(),
            Yaw:         Spec.BaseYaw,
            Range:       Spec.Range,
            Cooldown:    Spec.Cooldown,
            FireTimer:   Spec.Phase * 0.85 + 0.6,
            MuzzleFlash: 0.0
        }));

        // ④ 30 Anti-Tank Teller-Mines
        this.Mines = DDayMineSpecs.map((Spec, Index) => ({
            Index,
            Id:        Spec.Id,
            Name:      Spec.Name,
            Position:  Spec.Position.slice(),
            Detonated: false
        }));

        // ⑤ Active Projectile Pools (Bombs, 155mm Shells, MG Tracers) & Volumetric Explosion Emitters
        this.Bombs = Array.from({ length: DDayBombPoolSize }, (_, Index) => ({
            Index,
            Active:   false,
            Position: ParkedUnderground.slice(),
            Velocity: [0.0, 0.0, 0.0],
            Whistled: false
        }));

        this.Shells = Array.from({ length: DDayShellPoolSize }, (_, Index) => ({
            Index,
            Active:   false,
            Allied:   false,
            Position: ParkedUnderground.slice(),
            Velocity: [0.0, 0.0, 0.0],
            Age:      0.0
        }));

        this.Tracers = Array.from({ length: DDayTracerPoolSize }, (_, Index) => ({
            Index,
            Active:   false,
            Position: ParkedUnderground.slice(),
            Velocity: [0.0, 0.0, 0.0],
            Age:      0.0
        }));

        this.Explosions = [];

        // ⑥ 4 WWII Attack Aircraft (0,1 = Dive-Bombers dropping bomb salvos; 2,3 = Stricken Crash-Planes diving at cars!)
        this.Aircraft = Array.from({ length: DDayAircraftCount }, (_, Index) => ({
            Index,
            Role:          Index < 2 ? "bomber" : "crasher",
            State:         "flying", // "flying" | "diving" | "skidding"
            Position:      [0.0, 0.0, 24.0],
            Velocity:      [-44.0, 0.0, 0.0],
            Roll:          0.0,
            PropAngle:     Index * 1.2,
            BombsLeft:     3,
            DropTimer:     0.8,
            SortieTimer:   0.0,
            TrailingSmoke: false,
            ImpactTarget:  [0.0, 0.0, 0.0]
        }));

        this.InitializeSorties();
        if (this.Enabled && this.Host?.Vehicle?.Obstacles)
            this.RegisterTankObstacles(this.Host.Vehicle.Obstacles);
        if (this.Enabled)
        {
            this.SyncAllOccupants();
            this.ConstructTacticalHud();
        }
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                                    TACTICAL HUD STRIP & VICTORY BANNER
    //--------------------------------------------------------------------------------------------------------------------

    ConstructTacticalHud()
    {
        if (typeof document === "undefined" || !document.getElementById)
            return;
        const Stage = document.getElementById("stage");
        if (!Stage || typeof document.createElement !== "function")
            return;

        const Strip = document.createElement("div");
        Strip.id        = "dday-hud-strip";
        Strip.className = "dday-hud-strip";
        Strip.innerHTML = `
            <div class="dday-brand-pill">
                <span class="dday-dot"></span>
                <span>OPERATION OVERLORD · D-DAY</span>
            </div>
            <div class="dday-progress-wrap" title="Race east through the Militarized Zone to breach the Atlantic Fortress Wall!">
                <div class="dday-progress-labels">
                    <span id="dday-sector-label">BEACHHEAD</span>
                    <span id="dday-wall-dist">WALL 464 m</span>
                </div>
                <div class="dday-progress-track"><div id="dday-progress-fill" class="dday-progress-fill"></div></div>
            </div>
            <div class="dday-stat-pill" id="dday-armor-pill">
                <span class="dday-stat-label">ARMOR</span>
                <span class="dday-stat-val" id="dday-armor-val">100%</span>
            </div>
            <div class="dday-cover-pill exposed" id="dday-cover-pill">EXPOSED</div>
            <div class="dday-stat-pill">
                <span class="dday-stat-label">TIME</span>
                <span class="dday-stat-val" id="dday-timer-val">00.0s</span>
            </div>
            <div class="dday-btn-group">
                <button type="button" class="dday-btn active" id="dday-btn-player" title="Manual WASD / Arrow Driving">Player</button>
                <button type="button" class="dday-btn" id="dday-btn-ai" title="Watch Tactical AI weave mines, use Tank cover &amp; sprint to the Wall">AI Run</button>
                <button type="button" class="dday-btn restart" id="dday-btn-reset" title="Restart D-Day Assault at Beachhead (R)">Restart</button>
            </div>
        `;
        Stage.appendChild(Strip);
        this.ControlStrip = Strip;

        const Alert = document.createElement("div");
        Alert.id        = "dday-alert-banner";
        Alert.className = "dday-alert-banner hidden";
        Stage.appendChild(Alert);
        this.AlertBannerEl = Alert;

        const Victory = document.createElement("div");
        Victory.id        = "dday-victory-card";
        Victory.className = "dday-victory-card hidden";
        Victory.innerHTML = `
            <div class="dday-victory-kicker">MISSION ACCOMPLISHED</div>
            <div class="dday-victory-title">ATLANTIC WALL BREACHED!</div>
            <div class="dday-victory-stats" id="dday-victory-stats"></div>
            <button type="button" class="dday-victory-btn" id="dday-victory-again">Launch New Assault Run</button>
        `;
        Stage.appendChild(Victory);
        this.VictoryCardEl = Victory;

        const BtnPlayer = Strip.querySelector("#dday-btn-player");
        const BtnAi     = Strip.querySelector("#dday-btn-ai");
        const BtnReset  = Strip.querySelector("#dday-btn-reset");
        const BtnAgain  = Victory.querySelector("#dday-victory-again");

        BtnPlayer?.addEventListener("click", () => this.SetControlMode("player"));
        BtnAi?.addEventListener("click",     () => this.SetControlMode("autopilot"));
        BtnReset?.addEventListener("click",  () => this.RestartAssaultRun());
        BtnAgain?.addEventListener("click",  () => this.RestartAssaultRun());
    }

    SetControlMode(Mode)
    {
        this.ControlMode = Mode === "autopilot" ? "autopilot" : "player";
        this.Host?.DrivingHud?.Audio?.PlayUiClick?.(1.1);
        this.RefreshHudDom();
    }

    RestartAssaultRun()
    {
        this.Host?.DrivingHud?.Audio?.PlayUiClick?.(0.95);
        this.ResetMission(true);
        if (this.Host?.Vehicle)
        {
            this.Host.Vehicle.Respawn([DDaySpawn.Position[0], DDaySpawn.Position[1], 0.63], DDaySpawn.Yaw);
            this.Host.RestoreTyres?.();
            this.Host.AfterVehicleMoved?.();
        }
        this.Host?.Shell?.Announce?.("D-Day Beachhead — Race to the Atlantic Wall!");
    }

    ResetMission(ResetMinesAndTanks = true)
    {
        this.PlayerArmor      = this.MaxPlayerArmor;
        this.PlayerCoverState = "EXPOSED";
        this.WallReached      = false;
        this.AssaultTimer     = 0.0;
        this.WarningBanner    = "";
        this.WarningTimer     = 0.0;
        this.DestroyCooldown  = 0.0;

        if (ResetMinesAndTanks)
        {
            for (const Mine of this.Mines)
                Mine.Detonated = false;

            DDayTankSpecs.forEach((Spec, I) =>
            {
                const T = this.Tanks[I];
                T.Position[0] = Spec.StartX;
                T.Position[1] = Spec.LaneY;
                T.Position[2] = 0.0;
                T.Armor       = Spec.MaxArmor;
                T.State       = "advancing";
                T.StateTimer  = 0.0;
            });
        }

        for (const B of this.Bombs)   B.Active = false;
        for (const S of this.Shells)  S.Active = false;
        for (const R of this.Tracers) R.Active = false;
        this.Explosions.length = 0;

        this.InitializeSorties();
        if (this.VictoryCardEl?.classList)
            this.VictoryCardEl.classList.add("hidden");
        this.SyncAllOccupants();
        this.RefreshHudDom();
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                                      TANK OBSTACLES & BALLISTIC COVER
    //--------------------------------------------------------------------------------------------------------------------

    RegisterTankObstacles(ObstacleList)
    {
        if (!Array.isArray(ObstacleList))
            return;
        this.TankObstacleBoxes = this.Tanks.map(() =>
        {
            const Box = { Minimum: [0, 0, -50], Maximum: [0, 0, -49], DDayTank: true };
            ObstacleList.push(Box);
            return Box;
        });
        this.UpdateTankObstacleBoxes();
    }

    UpdateTankObstacleBoxes()
    {
        for (let I = 0; I < this.Tanks.length; ++I)
        {
            const T   = this.Tanks[I];
            const Box = this.TankObstacleBoxes[I];
            if (!Box)
                continue;
            const [X, Y, Z] = T.Position;
            Box.Minimum[0] = X - 3.12;
            Box.Minimum[1] = Y - 1.62;
            Box.Minimum[2] = Z;
            Box.Maximum[0] = X + 3.12;
            Box.Maximum[1] = Y + 1.62;
            Box.Maximum[2] = Z + (T.State === "wreck" ? 1.85 : 2.35);
        }
    }

    // 📝 Checks if a ballistic line from SourcePos to TargetPos is intercepted by an Allied Tank or Concrete Bunker/Cover Wall!
    CheckBallisticOcclusion(SourcePos, TargetPos)
    {
        // ① Check Moving Allied Tanks (and their smoking wrecks!) first
        for (const T of this.Tanks)
        {
            const [TX, TY, TZ] = T.Position;
            // An exploding tank cannot shield a target from its own detonation!
            if (Math.abs(SourcePos[0] - TX) < 3.45 && Math.abs(SourcePos[1] - TY) < 1.95 && SourcePos[2] < TZ + 2.65)
                continue;
            const Min = [TX - 3.35, TY - 1.85, TZ];
            const Max = [TX + 3.35, TY + 1.85, TZ + 2.55];
            if (SegmentIntersectsBox3D(SourcePos, TargetPos, Min, Max))
                return { Occluded: true, Kind: "tank", Tank: T };
        }
        // ② Check Static Bunkers, Blast Cover Walls & Sandbag Barricades
        for (const B of DDayStaticCoverBoxes)
        {
            // Ignore cover box if the turret itself sits on top of that exact bunker
            if (SourcePos[0] >= B[0] - 0.5 && SourcePos[0] <= B[3] + 0.5 &&
                SourcePos[1] >= B[1] - 0.5 && SourcePos[1] <= B[4] + 0.5 &&
                SourcePos[2] >= B[5] - 0.6)
                continue;
            if (SegmentIntersectsBox3D(SourcePos, TargetPos, [B[0], B[1], B[2]], [B[3], B[4], B[5]]))
                return { Occluded: true, Kind: "bunker", Tank: null };
        }
        return { Occluded: false, Kind: "none", Tank: null };
    }

    EvaluatePlayerCover()
    {
        const Vehicle = this.Host?.Vehicle;
        if (!Vehicle)
            return;
        const CarTarget = [Vehicle.Position[0], Vehicle.Position[1], Vehicle.Position[2] + 0.55];

        // ① Immediate proximity draft cover behind or alongside an Allied Tank
        for (const T of this.Tanks)
        {
            const DX = T.Position[0] - CarTarget[0];
            const DY = Math.abs(T.Position[1] - CarTarget[1]);
            // Car tucked within 12 m behind tank (between -12 m and +3.5 m) or right alongside its flank (< 4.3 m lateral)
            if (DX > -4.0 && DX < 13.5 && DY < 4.25)
            {
                this.PlayerCoverState  = T.State === "critical" ? "TANK COOKOFF!"
                                       : T.State === "wreck"    ? "WRECK SHIELD"
                                       : "TANK COVER";
                this.CoverShieldTankId = T.Id;
                return;
            }
        }

        // ② Check LOS against active enemy threats within 115 m
        let ThreatsInRange = 0;
        let BlockedByTank  = 0;
        let BlockedByWall  = 0;
        for (const Mg of this.MgTurrets)
        {
            const Dist = Math.hypot(Mg.Position[0] - CarTarget[0], Mg.Position[1] - CarTarget[1]);
            if (Dist > Mg.Range)
                continue;
            ThreatsInRange++;
            const Occ = this.CheckBallisticOcclusion(Mg.Position, CarTarget);
            if (Occ.Occluded)
            {
                if (Occ.Kind === "tank") BlockedByTank++;
                else                     BlockedByWall++;
            }
        }

        if (BlockedByTank > 0 && BlockedByTank + BlockedByWall >= Math.max(1, ThreatsInRange))
        {
            this.PlayerCoverState  = "TANK COVER";
            this.CoverShieldTankId = null;
        }
        else if (BlockedByWall > 0 && BlockedByWall >= Math.max(1, ThreatsInRange))
        {
            this.PlayerCoverState  = "BUNKER COVER";
            this.CoverShieldTankId = null;
        }
        else
        {
            // Also check close proximity behind any Tactical Cover Wall (< 5.2 m on West/Leeward side)
            let NearCoverWall = false;
            for (const B of DDayStaticCoverBoxes)
            {
                const CX = 0.5 * (B[0] + B[3]), CY = 0.5 * (B[1] + B[4]);
                if (Math.abs(CarTarget[0] - CX) < 5.5 && Math.abs(CarTarget[1] - CY) < 5.2)
                {
                    NearCoverWall = true;
                    break;
                }
            }
            this.PlayerCoverState  = NearCoverWall ? "BUNKER COVER" : "EXPOSED";
            this.CoverShieldTankId = null;
        }
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                         EXPLOSIONS, SHOCKWAVES, SPARKS & VOLUMETRIC FIRE/SMOKE
    //--------------------------------------------------------------------------------------------------------------------

    TriggerExplosion(Position, Kind = "bomb", BlastRadius = 8.0)
    {
        const Pos = [Position[0], Position[1], Math.max(0.15, Position[2])];
        const IsHuge = Kind === "tank" || Kind === "aircraft";
        const Duration = IsHuge ? 3.8 : (Kind === "bomb" ? 2.8 : 2.1);

        this.Explosions.push({
            Kind,
            Position: Pos,
            Age:      0.0,
            Duration,
            Radius:   BlastRadius,
            Power:    IsHuge ? 1.45 : (Kind === "bomb" ? 1.18 : 0.92)
        });
        if (this.Explosions.length > 10)
            this.Explosions.shift();

        // ① Hot metal & ember spark burst via SparkSequence
        if (this.Host?.Sparks)
        {
            const Strength = IsHuge ? 3.4 : 2.1;
            const Scrapes = [
                { Point: [Pos[0], Pos[1], Pos[2] + 0.35], Velocity: [ 14.0,  6.0, 12.0], Normal: [0, 0, 1], Strength, Wheel: -1, Rim: null },
                { Point: [Pos[0], Pos[1], Pos[2] + 0.35], Velocity: [-14.0, -6.0, 12.0], Normal: [0, 0, 1], Strength, Wheel: -1, Rim: null },
                { Point: [Pos[0], Pos[1], Pos[2] + 0.35], Velocity: [  5.0,-14.0, 11.0], Normal: [0, 0, 1], Strength, Wheel: -1, Rim: null }
            ];
            this.Host.Sparks.Emit(Scrapes, 0.045);
        }

        // ② Glowing GPU debris cube burst via EffectSequence
        if (this.Host?.Effects?.BurstTyre)
        {
            const DebrisFamily = IsHuge ? CourseMaterial.TrafficOrange : CourseMaterial.SteelDark;
            const T = ConstructRigidTransform([Pos[0], Pos[1], Pos[2] + 0.55], IdentityQuat);
            this.Host.Effects.BurstTyre(T, [0.95, 0.95, 0.75], [0.0, 0.0, 4.8], DebrisFamily);
        }

        // ③ Procedural WebAudio explosion concussion
        const Vehicle = this.Host?.Vehicle;
        const DistToCar = Vehicle
            ? Math.hypot(Vehicle.Position[0] - Pos[0], Vehicle.Position[1] - Pos[1], Vehicle.Position[2] - Pos[2])
            : 25.0;
        this.Host?.DrivingHud?.Audio?.PlayExplosionBoom?.(IsHuge ? 1.35 : 1.0, DistToCar, this.Clock);

        // ④ Physical Newtonian Blast Shockwave & Armor Damage on Player Car (if exposed within BlastRadius!)
        if (Vehicle && !Vehicle.Hidden && !Vehicle.Frozen && DistToCar < BlastRadius)
        {
            // Check if a Tank or Concrete Bunker shields the car from non-mine lateral blasts!
            const UnblockableCloseBlast = Kind === "mine" || (Kind === "tank" && DistToCar < 9.2);
            const Shielded = !UnblockableCloseBlast && DistToCar > 2.8 &&
                             this.CheckBallisticOcclusion(Pos, [Vehicle.Position[0], Vehicle.Position[1], Vehicle.Position[2] + 0.5]).Occluded;
            if (!Shielded)
            {
                const Falloff = ClampScalar(1.0 - DistToCar / BlastRadius, 0.15, 1.0);
                const DX = Vehicle.Position[0] - Pos[0];
                const DY = Vehicle.Position[1] - Pos[1];
                const Horiz = Math.max(0.4, Math.hypot(DX, DY));
                const PushMag = (Kind === "mine" ? 13500.0 : (IsHuge ? 16500.0 : 11200.0)) * Falloff;
                const UpMag   = (Kind === "mine" ? 12800.0 : (IsHuge ? 10500.0 :  7200.0)) * Falloff;
                if (typeof Vehicle.ApplyImpulse === "function")
                {
                    Vehicle.ApplyImpulse(
                        [Pos[0], Pos[1], Vehicle.Position[2] - 0.15],
                        [(DX / Horiz) * PushMag, (DY / Horiz) * PushMag, UpMag]
                    );
                }

                const BaseDmg = Kind === "aircraft" ? 34.0 : (Kind === "mine" ? 24.0 : (Kind === "tank" ? 20.0 : 16.0));
                this.ApplyPlayerDamage(BaseDmg * Falloff, Kind);
            }
            else
            {
                this.Stats.BlockedByCover++;
            }
        }
    }

    ApplyPlayerDamage(Amount, Cause = "fire")
    {
        if (this.WallReached || this.DestroyCooldown > 0.0 || this.Host?.Recovery?.Busy)
            return;
        this.PlayerArmor = Math.max(0.0, this.PlayerArmor - Amount);
        if (this.PlayerArmor <= 0.01)
        {
            this.PlayerArmor     = 0.0;
            this.DestroyCooldown = 4.0;
            this.SetWarning("VEHICLE DESTROYED IN ACTION — REDEPLOYING ON BEACHHEAD!", 3.2);
            if (this.Host?.Recovery && !this.Host.Recovery.Busy)
            {
                this.Host.Recovery.Begin("Destroyed in Action", {
                    Position: [DDaySpawn.Position[0], DDaySpawn.Position[1]],
                    Yaw:      DDaySpawn.Yaw
                }, () =>
                {
                    this.PlayerArmor     = this.MaxPlayerArmor;
                    this.AssaultTimer    = 0.0;
                    this.DestroyCooldown = 0.0;
                });
            }
            else if (this.Host?.Vehicle)
            {
                this.Host.Vehicle.Respawn([DDaySpawn.Position[0], DDaySpawn.Position[1], 0.63], DDaySpawn.Yaw);
                this.PlayerArmor     = this.MaxPlayerArmor;
                this.AssaultTimer    = 0.0;
                this.DestroyCooldown = 0.0;
            }
        }
        else if (Cause === "mine")
        {
            this.SetWarning("LANDMINE DETONATION — WATCH FOR RED FUSE CAPS!", 2.2);
        }
    }

    SetWarning(Text, Seconds = 2.2)
    {
        this.WarningBanner = Text;
        this.WarningTimer  = Math.max(this.WarningTimer, Seconds);
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                                 1. MOVING ALLIED ASSAULT TANKS (COVER + EXPLOSIONS)
    //--------------------------------------------------------------------------------------------------------------------

    DamageTank(TankIndex, Damage)
    {
        const T = this.Tanks[TankIndex];
        if (!T || T.State !== "advancing")
            return false;
        T.Armor = Math.max(0.0, T.Armor - Damage);
        if (this.Host?.Sparks)
        {
            this.Host.Sparks.Emit([{
                Point:    [T.Position[0], T.Position[1], T.Position[2] + 1.35],
                Velocity: [-6.0, (this.Rng() - 0.5) * 8.0, 6.5],
                Normal:   [0, 0, 1],
                Strength: 1.35,
                Wheel:    -1,
                Rim:      null
            }], 0.03);
        }
        if (T.Armor <= 0.0)
        {
            T.State      = "critical";
            T.StateTimer = 0.95; // ~1s ammo-rack cookoff warning before full detonation!
            if (this.CoverShieldTankId === T.Id)
                this.SetWarning(`${T.Name.toUpperCase()} HIT — AMMO COOKING OFF! MOVE!`, 2.0);
            return true;
        }
        return false;
    }

    DetonateTank(T)
    {
        if (T.State === "wreck")
            return;
        T.State      = "wreck";
        T.StateTimer = 6.5;
        T.Armor      = 0.0;
        this.Stats.TanksDestroyed++;
        this.TriggerExplosion([T.Position[0], T.Position[1], T.Position[2] + 0.9], "tank", 11.5);
        this.Host?.Shell?.Announce?.(`${T.Name} destroyed by fortress fire!`);
    }

    AdvanceTanks(Step)
    {
        const Vehicle = this.Host?.Vehicle;
        for (const T of this.Tanks)
        {
            T.MuzzleFlash = Math.max(0.0, T.MuzzleFlash - Step * 7.5);
            if (T.State === "advancing")
            {
                T.Position[0] += T.Speed * Step;
                // Aim turret slightly toward nearest ahead Bunker/Wall bastion
                const TargetYaw = Math.sin(this.Clock * 0.7 + T.Index * 1.3) * 0.28;
                T.TurretYaw = StepAngleToward(T.TurretYaw, TargetYaw, Step * 1.2);

                // Allied tank fires counter-battery shell toward the Atlantic Wall!
                T.FireTimer -= Step;
                if (T.FireTimer <= 0.0 && T.Position[0] < DDayWallX - 38.0)
                {
                    T.FireTimer   = 4.2 + this.Rng() * 2.4;
                    T.MuzzleFlash = 1.0;
                    const MuzzleX = T.Position[0] + Math.cos(T.TurretYaw) * 4.35;
                    const MuzzleY = T.Position[1] + Math.sin(T.TurretYaw) * 4.35;
                    this.SpawnShell([MuzzleX, MuzzleY, 1.88], [56.0 * Math.cos(T.TurretYaw), 56.0 * Math.sin(T.TurretYaw), 4.6], true);
                }

                // When an untouched Tank reaches the Atlantic Wall anti-tank ditch (X >= +292 m), it comes under point-blank Wall fire or recycles
                if (T.Position[0] >= 292.0)
                    this.DetonateTank(T);
            }
            else if (T.State === "critical")
            {
                T.Position[0] += T.Speed * 0.35 * Step;
                T.StateTimer  -= Step;
                if (T.StateTimer <= 0.0)
                    this.DetonateTank(T);
            }
            else if (T.State === "wreck")
            {
                T.StateTimer -= Step;
                if (T.StateTimer <= 0.0)
                {
                    // Fresh reinforcement Tank rolls off the Beachhead Landing Craft!
                    T.Position[0] = -158.0 - this.Rng() * 16.0;
                    T.Position[1] = T.LaneY;
                    T.Armor       = T.MaxArmor;
                    T.State       = "advancing";
                    T.StateTimer  = 0.0;
                }
            }

            // Kinematic chassis safety push so car never tunnels into a moving Tank
            if (Vehicle && !Vehicle.Hidden)
            {
                const OverlapX = (3.12 + 2.22) - Math.abs(Vehicle.Position[0] - T.Position[0]);
                const OverlapY = (1.62 + 1.02) - Math.abs(Vehicle.Position[1] - T.Position[1]);
                if (OverlapX > 0.0 && OverlapY > 0.0 && Vehicle.Position[2] < 2.6)
                {
                    if (OverlapX < OverlapY)
                    {
                        const DirX = Vehicle.Position[0] >= T.Position[0] ? 1.0 : -1.0;
                        Vehicle.Position[0] += DirX * (OverlapX + 0.03);
                        const TankVelX = T.State === "advancing" ? T.Speed : 0.0;
                        if ((Vehicle.LinearVelocity[0] - TankVelX) * DirX < 0.0)
                            Vehicle.LinearVelocity[0] = TankVelX + DirX * 3.5;
                    }
                    else
                    {
                        const DirY = Vehicle.Position[1] >= T.Position[1] ? 1.0 : -1.0;
                        Vehicle.Position[1] += DirY * (OverlapY + 0.03);
                        if (Vehicle.LinearVelocity[1] * DirY < 0.0)
                            Vehicle.LinearVelocity[1] *= -0.35;
                    }
                }
            }
        }
        this.UpdateTankObstacleBoxes();
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                         2. MACHINE-GUN TURRETS & HEAVY 155MM FORTRESS CANNONS
    //--------------------------------------------------------------------------------------------------------------------

    SpawnTracer(Origin, Velocity)
    {
        const Slot = this.Tracers.find((R) => !R.Active) || this.Tracers[0];
        Slot.Active      = true;
        Slot.Position[0] = Origin[0];
        Slot.Position[1] = Origin[1];
        Slot.Position[2] = Origin[2];
        Slot.Velocity[0] = Velocity[0];
        Slot.Velocity[1] = Velocity[1];
        Slot.Velocity[2] = Velocity[2];
        Slot.Age         = 0.0;
        this.Stats.MgShots++;
    }

    SpawnShell(Origin, Velocity, Allied = false)
    {
        const Slot = this.Shells.find((S) => !S.Active) || this.Shells[0];
        Slot.Active      = true;
        Slot.Allied      = Allied;
        Slot.Position[0] = Origin[0];
        Slot.Position[1] = Origin[1];
        Slot.Position[2] = Origin[2];
        Slot.Velocity[0] = Velocity[0];
        Slot.Velocity[1] = Velocity[1];
        Slot.Velocity[2] = Velocity[2];
        Slot.Age         = 0.0;
        if (!Allied)
            this.Stats.CannonShots++;
    }

    AdvanceTurretsAndCannons(Step)
    {
        const Vehicle = this.Host?.Vehicle;
        const CarPos  = Vehicle ? Vehicle.Position : null;

        // ① 8 Twin-Barrel Machine-Gun Turrets
        for (const Mg of this.MgTurrets)
        {
            Mg.MuzzleFlash  = Math.max(0.0, Mg.MuzzleFlash - Step * 14.0);
            Mg.CycleTimer   = (Mg.CycleTimer + Step) % 2.15;
            Mg.ShotCooldown = Math.max(0.0, Mg.ShotCooldown - Step);

            // Pick target: Player Car if in range (prioritized if exposed), otherwise nearest advancing Allied Tank
            let TargetPos = null;
            if (CarPos && !Vehicle.Hidden)
            {
                const DCar = Math.hypot(CarPos[0] - Mg.Position[0], CarPos[1] - Mg.Position[1]);
                if (DCar < Mg.Range && CarPos[0] < Mg.Position[0] + 24.0)
                    TargetPos = [CarPos[0] + (Vehicle.LinearVelocity?.[0] || 0) * 0.22, CarPos[1], CarPos[2] + 0.45];
            }
            if (!TargetPos)
            {
                let BestD = Mg.Range;
                for (const T of this.Tanks)
                {
                    if (T.State !== "advancing") continue;
                    const DT = Math.hypot(T.Position[0] - Mg.Position[0], T.Position[1] - Mg.Position[1]);
                    if (DT < BestD)
                    {
                        BestD     = DT;
                        TargetPos = [T.Position[0], T.Position[1], 1.25];
                    }
                }
            }

            if (!TargetPos)
                continue;

            const DesiredYaw = Math.atan2(TargetPos[1] - Mg.Position[1], TargetPos[0] - Mg.Position[0]);
            Mg.Yaw = StepAngleToward(Mg.Yaw, DesiredYaw, Step * 2.8);

            const InBurstWindow = Mg.CycleTimer < 0.92;
            if (InBurstWindow && Mg.ShotCooldown <= 0.0 && Math.abs(WrapAngle(DesiredYaw - Mg.Yaw)) < 0.32)
            {
                Mg.ShotCooldown = 0.125;
                Mg.MuzzleFlash  = 1.0;
                const SpreadY = (this.Rng() - 0.5) * 0.038;
                const YawShot = Mg.Yaw + SpreadY;
                const DX = TargetPos[0] - Mg.Position[0], DY = TargetPos[1] - Mg.Position[1], DZ = TargetPos[2] - (Mg.Position[2] + 0.42);
                const Horiz = Math.max(1.0, Math.hypot(DX, DY));
                const Speed = 86.0;
                const Origin = [
                    Mg.Position[0] + Math.cos(Mg.Yaw) * 2.35,
                    Mg.Position[1] + Math.sin(Mg.Yaw) * 2.35,
                    Mg.Position[2] + 0.42
                ];
                this.SpawnTracer(Origin, [
                    Math.cos(YawShot) * Speed,
                    Math.sin(YawShot) * Speed,
                    (DZ / Horiz) * Speed
                ]);
                if (CarPos)
                {
                    const DistCar = Math.hypot(CarPos[0] - Mg.Position[0], CarPos[1] - Mg.Position[1]);
                    if (DistCar < 85.0)
                        this.Host?.DrivingHud?.Audio?.PlayMgBurst?.(DistCar, this.Clock);
                }
            }
        }

        // ② 6 Heavy 155mm Fortress & Wall Artillery Cannons
        for (const Can of this.Cannons)
        {
            Can.MuzzleFlash = Math.max(0.0, Can.MuzzleFlash - Step * 5.0);
            Can.FireTimer  -= Step;

            // Alternate between hammering advancing Allied Tanks (so tanks get shot & explode dramatically!) and shelling the Car!
            let TargetPos = null;
            const PreferTank = ((Can.Index + Math.floor(this.Clock * 0.35)) % 2) === 0;
            const FindTankTarget = () =>
            {
                let Best = null, BestD = Can.Range;
                for (const T of this.Tanks)
                {
                    if (T.State !== "advancing") continue;
                    const DT = Math.hypot(T.Position[0] - Can.Position[0], T.Position[1] - Can.Position[1]);
                    if (DT < BestD && T.Position[0] < Can.Position[0] + 10.0)
                    {
                        BestD = DT;
                        const FlightT = DT / 58.0;
                        Best = [T.Position[0] + T.Speed * FlightT, T.Position[1], 0.9];
                    }
                }
                return Best;
            };
            const FindCarTarget = () =>
            {
                if (!CarPos || Vehicle?.Hidden) return null;
                const DC = Math.hypot(CarPos[0] - Can.Position[0], CarPos[1] - Can.Position[1]);
                if (DC > Can.Range || CarPos[0] > Can.Position[0] + 15.0) return null;
                const FlightT = DC / 58.0;
                return [
                    CarPos[0] + (Vehicle.LinearVelocity?.[0] || 0) * FlightT * 0.55,
                    CarPos[1] + (Vehicle.LinearVelocity?.[1] || 0) * FlightT * 0.55,
                    0.4
                ];
            };

            TargetPos = PreferTank ? (FindTankTarget() || FindCarTarget()) : (FindCarTarget() || FindTankTarget());
            if (!TargetPos)
                continue;

            const DesiredYaw = Math.atan2(TargetPos[1] - Can.Position[1], TargetPos[0] - Can.Position[0]);
            Can.Yaw = StepAngleToward(Can.Yaw, DesiredYaw, Step * 1.45);

            if (Can.FireTimer <= 0.0 && Math.abs(WrapAngle(DesiredYaw - Can.Yaw)) < 0.25)
            {
                Can.FireTimer   = Can.Cooldown + this.Rng() * 0.8;
                Can.MuzzleFlash = 1.0;

                const Muzzle = [
                    Can.Position[0] + Math.cos(Can.Yaw) * 7.1,
                    Can.Position[1] + Math.sin(Can.Yaw) * 7.1,
                    Can.Position[2] + 1.85
                ];
                const DX = TargetPos[0] - Muzzle[0];
                const DY = TargetPos[1] - Muzzle[1];
                const DZ = TargetPos[2] - Muzzle[2];
                const HorizDist = Math.max(8.0, Math.hypot(DX, DY));
                const HorizSpeed = 58.0;
                const FlightTime = HorizDist / HorizSpeed;
                const Gravity    = 13.5;
                const VZ = (DZ + 0.5 * Gravity * FlightTime * FlightTime) / FlightTime;

                this.SpawnShell(Muzzle, [
                    (DX / HorizDist) * HorizSpeed,
                    (DY / HorizDist) * HorizSpeed,
                    VZ
                ], false);

                if (CarPos)
                {
                    const DistCar = Math.hypot(CarPos[0] - Muzzle[0], CarPos[1] - Muzzle[1]);
                    if (DistCar < 130.0)
                        this.Host?.DrivingHud?.Audio?.PlayExplosionBoom?.(0.55, DistCar, this.Clock);
                }
            }
        }
    }

    AdvanceProjectiles(Step)
    {
        const Vehicle = this.Host?.Vehicle;
        const CarPos  = Vehicle && !Vehicle.Hidden ? Vehicle.Position : null;

        // ① MG Tracer Bolts
        for (const R of this.Tracers)
        {
            if (!R.Active) continue;
            const Prev = R.Position.slice();
            R.Position[0] += R.Velocity[0] * Step;
            R.Position[1] += R.Velocity[1] * Step;
            R.Position[2] += R.Velocity[2] * Step;
            R.Age         += Step;

            if (R.Age > 1.5 || R.Position[2] <= 0.05)
            {
                R.Active = false;
                continue;
            }

            // Check hit against Tanks / Static Cover first!
            const Occ = this.CheckBallisticOcclusion(Prev, R.Position);
            if (Occ.Occluded)
            {
                if (Occ.Kind === "tank" && Occ.Tank)
                    this.DamageTank(Occ.Tank.Index, 3.2);
                if (CarPos && Math.hypot(R.Position[0] - CarPos[0], R.Position[1] - CarPos[1]) < 14.0)
                {
                    this.Stats.BlockedByCover++;
                    this.Host?.DrivingHud?.Audio?.PlayArmorRicochet?.(true, this.Clock);
                }
                R.Active = false;
                continue;
            }

            // Check direct hit against exposed Player Car
            if (CarPos && Math.abs(R.Position[0] - CarPos[0]) < 2.35 &&
                Math.abs(R.Position[1] - CarPos[1]) < 1.18 &&
                Math.abs(R.Position[2] - (CarPos[2] + 0.35)) < 0.95)
            {
                R.Active = false;
                this.ApplyPlayerDamage(3.8, "mg");
                this.Host?.DrivingHud?.Audio?.PlayArmorRicochet?.(false, this.Clock);
                this.Host?.Sparks?.Emit?.([{
                    Point:    R.Position.slice(),
                    Velocity: [-R.Velocity[0] * 0.15, (this.Rng() - 0.5) * 6.0, 4.5],
                    Normal:   [0, 0, 1],
                    Strength: 1.15,
                    Wheel:    -1,
                    Rim:      null
                }], 0.025);
            }
        }

        // ② 155mm Ballistic Artillery Shells
        const ShellGravity = 13.5;
        for (const S of this.Shells)
        {
            if (!S.Active) continue;
            const Prev = S.Position.slice();
            S.Velocity[2] -= ShellGravity * Step;
            S.Position[0] += S.Velocity[0] * Step;
            S.Position[1] += S.Velocity[1] * Step;
            S.Position[2] += S.Velocity[2] * Step;
            S.Age         += Step;

            if (S.Allied)
            {
                if (S.Position[0] >= DDayWallX - 3.5 || S.Position[2] <= 0.1 || S.Age > 4.5)
                {
                    S.Active = false;
                    this.TriggerExplosion([S.Position[0], S.Position[1], Math.max(0.5, S.Position[2])], "cannon", 5.5);
                }
                continue;
            }

            // Check direct shell hit on an Allied Tank (deals massive 44 HP armor damage -> 2-3 hits blow up a Tank!)
            let HitSomething = false;
            for (const T of this.Tanks)
            {
                if (Math.abs(S.Position[0] - T.Position[0]) < 3.45 &&
                    Math.abs(S.Position[1] - T.Position[1]) < 1.95 &&
                    S.Position[2] < 2.65)
                {
                    this.DamageTank(T.Index, 44.0);
                    this.TriggerExplosion(S.Position, "cannon", 7.5);
                    S.Active     = false;
                    HitSomething = true;
                    break;
                }
            }
            if (HitSomething) continue;

            // Check hit on Concrete Cover Wall / Bunker or Ground
            const Occ = this.CheckBallisticOcclusion(Prev, S.Position);
            if (Occ.Occluded || S.Position[2] <= 0.12 || S.Age > 5.0)
            {
                if (Occ.Occluded)
                    this.Stats.BlockedByCover++;
                this.TriggerExplosion([S.Position[0], S.Position[1], Math.max(0.15, S.Position[2])], "cannon", 7.8);
                S.Active = false;
            }
        }

        // ③ Falling 250 kg Finned Aerial Bombs
        const BombGravity = 15.5;
        for (const B of this.Bombs)
        {
            if (!B.Active) continue;
            B.Velocity[2] -= BombGravity * Step;
            B.Position[0] += B.Velocity[0] * Step;
            B.Position[1] += B.Velocity[1] * Step;
            B.Position[2] += B.Velocity[2] * Step;

            if (!B.Whistled && B.Position[2] < 13.0 && CarPos &&
                Math.hypot(B.Position[0] - CarPos[0], B.Position[1] - CarPos[1]) < 48.0)
            {
                B.Whistled = true;
                this.Host?.DrivingHud?.Audio?.PlayDiveSiren?.(this.Clock);
            }

            // Check direct bomb hit on Tank or Ground
            let Detonated = B.Position[2] <= 0.2;
            if (!Detonated)
            {
                for (const T of this.Tanks)
                {
                    if (Math.abs(B.Position[0] - T.Position[0]) < 3.4 &&
                        Math.abs(B.Position[1] - T.Position[1]) < 1.9 &&
                        B.Position[2] < 2.5)
                    {
                        this.DamageTank(T.Index, 65.0);
                        Detonated = true;
                        break;
                    }
                }
            }
            if (Detonated)
            {
                B.Active = false;
                this.Stats.BombExplosions++;
                this.TriggerExplosion([B.Position[0], B.Position[1], 0.25], "bomb", 8.8);
            }
        }
    }

    //--------------------------------------------------------------------------------------------------------------------
    //             3. AIRCRAFT SQUADRON: DIVE-BOMBERS DROPPING BOMBS + CRASHING INTO THE CARS!
    //--------------------------------------------------------------------------------------------------------------------

    InitializeSorties()
    {
        const BaseX = this.Host?.Vehicle?.Position?.[0] ?? DDaySpawn.Position[0];
        this.Aircraft.forEach((Plane, Index) =>
        {
            this.ResetAircraftSortie(Plane, BaseX, Index * 1.45);
        });
    }

    ResetAircraftSortie(Plane, AnchorX = -120.0, DelayOffset = 0.0)
    {
        const clampedX = ClampScalar(AnchorX, -140.0, 260.0);
        if (Plane.Role === "bomber")
        {
            // Sweeps Westward or Diagonally low over the assault corridor, dropping sticks of 3 finned bombs!
            const LaneSign = Plane.Index === 0 ? 1.0 : -1.0;
            Plane.State         = "flying";
            Plane.Position      = [clampedX + 165.0 + DelayOffset * 28.0, LaneSign * (6.0 + this.Rng() * 14.0), 19.5 + Plane.Index * 3.0];
            Plane.Velocity      = [-46.0, -LaneSign * 4.5, 0.0];
            Plane.Roll          = LaneSign * 0.14;
            Plane.BombsLeft     = 3;
            Plane.DropTimer     = 1.15 + DelayOffset * 0.4;
            Plane.TrailingSmoke = false;
        }
        else
        {
            // Stricken / Kamikaze Attack Fighter: approaches high, catches flak fire, and dives straight toward the car path!
            const Side = Plane.Index === 2 ? 1.0 : -1.0;
            Plane.State         = "flying";
            Plane.Position      = [clampedX + 150.0 + DelayOffset * 34.0, Side * 44.0, 25.5];
            Plane.Velocity      = [-40.0, -Side * 10.5, -1.2];
            Plane.Roll          = Side * 0.25;
            Plane.BombsLeft     = 1;
            Plane.DropTimer     = 0.85;
            Plane.SortieTimer   = 1.45 + (Plane.Index === 3 ? 2.1 : 0.0);
            Plane.TrailingSmoke = false;
        }
    }

    DropBombFromAircraft(Plane)
    {
        const Bomb = this.Bombs.find((B) => !B.Active);
        if (!Bomb)
            return null;
        Bomb.Active      = true;
        Bomb.Position[0] = Plane.Position[0];
        Bomb.Position[1] = Plane.Position[1];
        Bomb.Position[2] = Plane.Position[2] - 0.75;
        Bomb.Velocity[0] = Plane.Velocity[0] * 0.48;
        Bomb.Velocity[1] = Plane.Velocity[1] * 0.48;
        Bomb.Velocity[2] = -3.5;
        Bomb.Whistled    = false;
        this.Stats.BombsDropped++;
        return Bomb;
    }

    // 📝 Public helper (also callable from tests!) to immediately trigger a stricken aircraft crash dive toward TargetPos!
    LaunchKamikazeDive(PlaneIndex = 2, TargetPos = null)
    {
        const Plane = this.Aircraft[PlaneIndex] || this.Aircraft[2];
        const Vehicle = this.Host?.Vehicle;
        const Dest = TargetPos || (Vehicle
            ? [
                ClampScalar(Vehicle.Position[0] + (Vehicle.LinearVelocity?.[0] || 0.0) * 1.15 + 14.0, -150.0, 304.0),
                ClampScalar(Vehicle.Position[1] + (this.Rng() - 0.5) * 4.5, -52.0, 52.0),
                0.35
              ]
            : [0.0, 0.0, 0.35]);

        Plane.State         = "diving";
        Plane.TrailingSmoke = true;
        Plane.ImpactTarget  = Dest.slice();

        const DX = Dest[0] - Plane.Position[0];
        const DY = Dest[1] - Plane.Position[1];
        const DZ = Dest[2] - Plane.Position[2];
        const Dist = Math.max(1.0, Math.hypot(DX, DY, DZ));
        const DiveSpeed = 48.0;
        Plane.Velocity = [(DX / Dist) * DiveSpeed, (DY / Dist) * DiveSpeed, (DZ / Dist) * DiveSpeed];
        Plane.Roll     = (DY >= 0 ? 1.0 : -1.0) * 0.46;

        // Mid-air AA flak burst sparks when plane gets hit & begins its screaming dive!
        this.Host?.Sparks?.Emit?.([{
            Point:    Plane.Position.slice(),
            Velocity: [0.0, 0.0, 6.0],
            Normal:   [0, 0, 1],
            Strength: 2.0,
            Wheel:    -1,
            Rim:      null
        }], 0.03);

        if (Vehicle && Math.hypot(Dest[0] - Vehicle.Position[0], Dest[1] - Vehicle.Position[1]) < 68.0)
        {
            this.SetWarning("INCOMING STRICKEN AIRCRAFT CRASH — EVADE!", 2.1);
            this.Host?.DrivingHud?.Audio?.PlayDiveSiren?.(this.Clock);
        }
        return Plane;
    }

    AdvanceAircraft(Step)
    {
        const Vehicle = this.Host?.Vehicle;
        const CarPos  = Vehicle ? Vehicle.Position : DDaySpawn.Position;

        for (const Plane of this.Aircraft)
        {
            Plane.PropAngle = (Plane.PropAngle + Step * 44.0) % (2.0 * Math.PI);

            if (Plane.State === "flying")
            {
                Plane.Position[0] += Plane.Velocity[0] * Step;
                Plane.Position[1] += Plane.Velocity[1] * Step;
                Plane.Position[2] += Plane.Velocity[2] * Step;

                if (Plane.BombsLeft > 0)
                {
                    Plane.DropTimer -= Step;
                    const DistX = Math.abs(Plane.Position[0] - CarPos[0]);
                    if (Plane.DropTimer <= 0.0 && DistX < 135.0 && Plane.Position[0] > -155.0 && Plane.Position[0] < 305.0)
                    {
                        this.DropBombFromAircraft(Plane);
                        Plane.BombsLeft--;
                        Plane.DropTimer = 0.44;
                    }
                }

                if (Plane.Role === "crasher")
                {
                    Plane.SortieTimer -= Step;
                    if (Plane.SortieTimer <= 0.0 && Plane.Position[0] > CarPos[0] - 12.0)
                    {
                        this.LaunchKamikazeDive(Plane.Index);
                        continue;
                    }
                }

                if (Plane.Position[0] < CarPos[0] - 115.0 || Plane.Position[0] < -205.0)
                    this.ResetAircraftSortie(Plane, CarPos[0], 0.5);
            }
            else if (Plane.State === "diving")
            {
                Plane.Position[0] += Plane.Velocity[0] * Step;
                Plane.Position[1] += Plane.Velocity[1] * Step;
                Plane.Position[2] += Plane.Velocity[2] * Step;
                Plane.Roll        += Step * 1.35; // dramatic stricken spiral bank!

                // Check direct mid-air / low-altitude crash into the Player's Car!
                let Crashed = Plane.Position[2] <= 0.75;
                if (!Crashed && Vehicle && !Vehicle.Hidden)
                {
                    const HitCar = Math.abs(Plane.Position[0] - Vehicle.Position[0]) < 4.2 &&
                                   Math.abs(Plane.Position[1] - Vehicle.Position[1]) < 3.6 &&
                                   Math.abs(Plane.Position[2] - (Vehicle.Position[2] + 0.6)) < 2.2;
                    if (HitCar)
                        Crashed = true;
                }
                // Or crashing directly into an Allied Tank!
                if (!Crashed)
                {
                    for (const T of this.Tanks)
                    {
                        if (Math.abs(Plane.Position[0] - T.Position[0]) < 4.6 &&
                            Math.abs(Plane.Position[1] - T.Position[1]) < 3.2 &&
                            Plane.Position[2] < 2.8)
                        {
                            this.DamageTank(T.Index, 85.0);
                            Crashed = true;
                            break;
                        }
                    }
                }

                if (Crashed)
                {
                    Plane.Position[2] = 0.55;
                    Plane.State       = "skidding";
                    Plane.SortieTimer = 2.4;
                    Plane.Velocity    = [Plane.Velocity[0] * 0.28, Plane.Velocity[1] * 0.28, 0.0];
                    this.Stats.AircraftCrashes++;
                    this.TriggerExplosion(Plane.Position, "aircraft", 10.5);
                }
            }
            else if (Plane.State === "skidding")
            {
                // Burning fuselage skids across the tarmac throwing sparks before recycling into next sortie
                Plane.Position[0] += Plane.Velocity[0] * Step;
                Plane.Position[1] += Plane.Velocity[1] * Step;
                Plane.Velocity[0] *= Math.max(0.0, 1.0 - 2.2 * Step);
                Plane.Velocity[1] *= Math.max(0.0, 1.0 - 2.2 * Step);
                Plane.SortieTimer -= Step;
                if (Plane.SortieTimer <= 0.0)
                    this.ResetAircraftSortie(Plane, CarPos[0], 0.8);
            }
        }
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                                    4. EXPLODING ANTI-TANK LANDMINES
    //--------------------------------------------------------------------------------------------------------------------

    TriggerMine(MineIndex, TriggeredByCar = true, WheelIndex = -1)
    {
        const Mine = this.Mines[MineIndex];
        if (!Mine || Mine.Detonated)
            return false;
        Mine.Detonated = true;
        this.Stats.MinesDetonated++;
        this.TriggerExplosion([Mine.Position[0], Mine.Position[1], 0.18], "mine", 6.8);

        if (TriggeredByCar && WheelIndex >= 0 && this.Host?.PunctureTyre && this.Host?.Settings?.TyreFailure)
            this.Host.PunctureTyre(WheelIndex, false); // shrapnel slow leak on the wheel that stepped on the mine!
        return true;
    }

    AdvanceMines()
    {
        const Vehicle = this.Host?.Vehicle;
        const CarActive = Vehicle && !Vehicle.Hidden && !Vehicle.Frozen;

        for (let I = 0; I < this.Mines.length; ++I)
        {
            const Mine = this.Mines[I];
            if (Mine.Detonated)
                continue;
            const [MX, MY] = Mine.Position;

            // ① Check Player Car chassis & 4 wheels
            if (CarActive && Math.abs(Vehicle.Position[0] - MX) < 3.6 && Math.abs(Vehicle.Position[1] - MY) < 2.8 && Vehicle.Position[2] < 1.65)
            {
                let HitWheel     = 0;
                let MinWheelDist = Infinity;
                let MinDist      = Math.hypot(Vehicle.Position[0] - MX, Vehicle.Position[1] - MY);
                if (Vehicle.QueryWheelTransform)
                {
                    for (let W = 0; W < 4; ++W)
                    {
                        const WT = Vehicle.QueryWheelTransform(W);
                        const WD = Math.hypot(WT[12] - MX, WT[13] - MY);
                        if (WD < MinWheelDist)
                        {
                            MinWheelDist = WD;
                            HitWheel     = W;
                        }
                        if (WD < MinDist)
                            MinDist = WD;
                    }
                }
                if (MinDist < 1.95)
                {
                    this.TriggerMine(I, true, HitWheel);
                    continue;
                }
            }

            // ② Check Advancing Allied Tanks rolling onto a mine!
            for (const T of this.Tanks)
            {
                if (T.State !== "advancing") continue;
                if (Math.abs(T.Position[0] - MX) < 3.1 && Math.abs(T.Position[1] - MY) < 1.7)
                {
                    this.TriggerMine(I, false, -1);
                    this.DamageTank(T.Index, 36.0);
                    break;
                }
            }
        }
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                      5. TACTICAL AI ASSAULT AUTOPILOT (MINE AVOIDANCE + TANK COVER + WALL SPRINT)
    //--------------------------------------------------------------------------------------------------------------------

    ComputeAutopilotCommand()
    {
        const Vehicle = this.Host?.Vehicle;
        if (!Vehicle || this.WallReached)
            return { Throttle: 0.0, Brake: 0.85, Steer: 0.0, Handbrake: false };

        const [VX, VY] = Vehicle.Position;
        const Speed    = Math.hypot(Vehicle.LinearVelocity[0], Vehicle.LinearVelocity[1]);

        // Goal: steer into the Central Atlantic Wall Breach Gate (Y = 0 at X >= 270 m), or draft near nearest intact Center Tank!
        let DesiredY = 0.0;
        if (VX < 255.0)
        {
            let BestTank = null, BestScore = Infinity;
            for (const T of this.Tanks)
            {
                if (T.State !== "advancing" || Math.abs(T.LaneY) > 18.0) continue;
                const DX = T.Position[0] - VX;
                if (DX > 4.0 && DX < 65.0 && DX < BestScore)
                {
                    BestScore = DX;
                    BestTank  = T;
                }
            }
            if (BestTank)
            {
                const DX = BestTank.Position[0] - VX;
                // Draft behind tank at DX > 9 m, then swing around to overtake cleanly if closer!
                DesiredY = DX < 11.5
                    ? (BestTank.LaneY > 0.0 ? BestTank.LaneY - 5.2 : BestTank.LaneY + 5.2)
                    : BestTank.LaneY * 0.82;
            }
        }

        // Repulsive potential around armed Landmines, Static Obstacles & Diving Aircraft Impact Points!
        let RepulseY = 0.0;
        for (const Mine of this.Mines)
        {
            if (Mine.Detonated) continue;
            const DX = Mine.Position[0] - VX;
            const DY = VY - Mine.Position[1];
            if (DX > -1.5 && DX < 28.0 && Math.abs(DY) < 4.5)
            {
                const Sign = Math.abs(DY) < 0.25 ? (Mine.Position[1] >= 0.0 ? -1.0 : 1.0) : Math.sign(DY);
                RepulseY += Sign * (4.8 - Math.abs(DY)) * (1.0 - DX / 30.0) * 2.2;
            }
        }

        for (const Plane of this.Aircraft)
        {
            if (Plane.State !== "diving") continue;
            const [IX, IY] = Plane.ImpactTarget;
            const DX = IX - VX, DY = VY - IY;
            if (DX > -6.0 && DX < 42.0 && Math.abs(DY) < 9.5)
            {
                const Sign = DY >= 0.0 ? 1.0 : -1.0;
                RepulseY += Sign * 7.5;
            }
        }

        const TargetY     = ClampScalar(DesiredY + RepulseY, -10.5, 10.5);
        const LookaheadX  = Math.min(DDayBreachGoalX + 12.0, VX + 22.0);
        const DesiredYaw  = Math.atan2(TargetY - VY, LookaheadX - VX);
        const Fwd         = Vehicle.QueryForward();
        const CurrentYaw  = Math.atan2(Fwd[1], Fwd[0]);
        const HeadingErr  = WrapAngle(DesiredYaw - CurrentYaw);

        const Steer       = ClampScalar(HeadingErr * 2.15, -1.0, 1.0);
        const TargetSpeed = Math.abs(HeadingErr) > 0.35 ? 24.0 : 38.0;
        const Throttle    = Speed < TargetSpeed ? 0.92 : 0.25;
        const Brake       = Speed > TargetSpeed + 8.0 ? 0.45 : 0.0;

        return { Throttle, Brake, Steer, Handbrake: false };
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                                   FRAME ADVANCE & WALL VICTORY CHECK
    //--------------------------------------------------------------------------------------------------------------------

    Advance(StepSeconds)
    {
        if (!this.Enabled)
            return false;
        const Step = ClampScalar(Number(StepSeconds) || 0.0, 0.0, 0.05);
        if (Step <= 0.0)
            return false;

        this.Clock           += Step;
        this.DestroyCooldown  = Math.max(0.0, this.DestroyCooldown - Step);
        if (this.WarningTimer > 0.0)
            this.WarningTimer = Math.max(0.0, this.WarningTimer - Step);

        // If AI Assault Autopilot is active, advance vehicle physics with tactical autopilot!
        const Vehicle = this.Host?.Vehicle;
        if (this.ControlMode === "autopilot" && Vehicle && !this.Host?.Recovery?.Busy)
        {
            const Cmd = this.ComputeAutopilotCommand();
            this.Host.Recovery?.Advance?.(Step);
            this.Host.Fracture?.ResolveImpacts?.(Vehicle, Step);
            Vehicle.Advance(Cmd, Step);
            Vehicle.AdvanceTyreWear(Step);
        }

        // Advance all 4 Militarized Combat Subsystems
        this.AdvanceTanks(Step);
        this.AdvanceTurretsAndCannons(Step);
        this.AdvanceAircraft(Step);
        this.AdvanceProjectiles(Step);
        this.AdvanceMines();
        this.EvaluatePlayerCover();

        // Field armor stabilization when sheltered behind Allied Tank or Concrete Bunker cover
        if (!this.WallReached && this.PlayerArmor > 0.0 &&
            (this.PlayerCoverState === "TANK COVER" || this.PlayerCoverState === "BUNKER COVER"))
        {
            this.PlayerArmor = Math.min(this.MaxPlayerArmor, this.PlayerArmor + Step * 3.5);
        }

        // Decay active volumetric explosions
        for (const E of this.Explosions)
            E.Age += Step;
        this.Explosions = this.Explosions.filter((E) => E.Age < E.Duration);

        // Update Wall Distance & Check Victory Threshold at the Atlantic Fortress Wall Breach Gate!
        if (Vehicle)
        {
            this.DistanceToWall = Math.max(0.0, DDayBreachGoalX - Vehicle.Position[0]);
            if (!this.WallReached && !Vehicle.Hidden && this.PlayerArmor > 0.0)
            {
                this.AssaultTimer += Step;
                if (Vehicle.Position[0] >= DDayBreachGoalX && Math.abs(Vehicle.Position[1]) <= DDayBreachHalfY)
                {
                    this.WallReached = true;
                    this.Stats.RunsCompleted++;
                    if (this.BestAssaultTime === null || this.AssaultTimer < this.BestAssaultTime)
                        this.BestAssaultTime = this.AssaultTimer;
                    this.Host?.DrivingHud?.Audio?.PlayVictoryFanfare?.(this.Clock);
                    this.Host?.Shell?.Announce?.(`ATLANTIC WALL BREACHED in ${this.AssaultTimer.toFixed(1)} s!`);
                    this.ShowVictoryCard();
                }
            }
        }

        this.SyncAllOccupants();
        this.RefreshHudDom();
        return true;
    }

    ShowVictoryCard()
    {
        if (!this.VictoryCardEl)
            return;
        const StatsEl = this.VictoryCardEl.querySelector("#dday-victory-stats");
        if (StatsEl)
        {
            StatsEl.textContent =
                `Time ${this.AssaultTimer.toFixed(1)}s` +
                (this.BestAssaultTime !== null ? ` · Best ${this.BestAssaultTime.toFixed(1)}s` : "") +
                ` · Armor ${Math.round(this.PlayerArmor)}% · Cover Saves ${this.Stats.BlockedByCover}`;
        }
        this.VictoryCardEl.classList.remove("hidden");
    }

    RefreshHudDom()
    {
        if (!this.ControlStrip)
            return;
        const TotalRun = DDayBreachGoalX - DDaySpawn.Position[0];
        const Progress = ClampScalar(1.0 - this.DistanceToWall / TotalRun, 0.0, 1.0);

        const SectorEl = this.ControlStrip.querySelector("#dday-sector-label");
        const DistEl   = this.ControlStrip.querySelector("#dday-wall-dist");
        const FillEl   = this.ControlStrip.querySelector("#dday-progress-fill");
        const ArmorVal = this.ControlStrip.querySelector("#dday-armor-val");
        const ArmorPil = this.ControlStrip.querySelector("#dday-armor-pill");
        const CoverPil = this.ControlStrip.querySelector("#dday-cover-pill");
        const TimerEl  = this.ControlStrip.querySelector("#dday-timer-val");
        const BtnPlay  = this.ControlStrip.querySelector("#dday-btn-player");
        const BtnAi    = this.ControlStrip.querySelector("#dday-btn-ai");

        if (SectorEl)
        {
            SectorEl.textContent = this.WallReached ? "WALL BREACHED"
                : Progress < 0.20 ? "SECTOR 1 · BEACHHEAD"
                : Progress < 0.45 ? "SECTOR 2 · PILLBOX & MINES"
                : Progress < 0.72 ? "SECTOR 3 · TANK & MG ALLEY"
                : Progress < 0.90 ? "SECTOR 4 · ARTILLERY & BOMBS"
                : "FINAL SPRINT · THE WALL";
        }
        if (DistEl)   DistEl.textContent = this.WallReached ? "GOAL REACHED!" : `WALL ${Math.round(this.DistanceToWall)} m`;
        if (FillEl)   FillEl.style.width = `${(Progress * 100.0).toFixed(1)}%`;
        if (ArmorVal) ArmorVal.textContent = `${Math.round(this.PlayerArmor)}%`;
        if (ArmorPil) ArmorPil.classList.toggle("critical", this.PlayerArmor < 35.0);
        if (CoverPil)
        {
            CoverPil.textContent = this.PlayerCoverState;
            CoverPil.className   = `dday-cover-pill ${
                this.PlayerCoverState.includes("COVER") ? "shielded" :
                this.PlayerCoverState.includes("COOKOFF") ? "danger" : "exposed"
            }`;
        }
        if (TimerEl)  TimerEl.textContent = `${this.AssaultTimer.toFixed(1)}s`;
        if (BtnPlay)  BtnPlay.classList.toggle("active", this.ControlMode === "player");
        if (BtnAi)    BtnAi.classList.toggle("active", this.ControlMode === "autopilot");

        if (this.AlertBannerEl)
        {
            const ActiveWarn = this.WarningTimer > 0.0 && this.WarningBanner;
            this.AlertBannerEl.classList.toggle("hidden", !ActiveWarn);
            if (ActiveWarn)
                this.AlertBannerEl.textContent = this.WarningBanner;
        }
    }

    //--------------------------------------------------------------------------------------------------------------------
    //                   GPU OCCUPANT TRANSFORM SYNCHRONIZATION, VOLUMETRIC PLUMES & CLUSTERED LIGHTS
    //--------------------------------------------------------------------------------------------------------------------

    SyncAllOccupants()
    {
        const Block = this.Scene?.DDayBlock;
        if (!Block)
            return;

        const SetOcc = (OccIdx, Pos, Quat, Moving = true) =>
        {
            const Occ = this.Scene.Occupants[OccIdx];
            if (!Occ) return;
            Occ.PreviousTransform.set(Occ.Transform);
            Occ.Transform = ConstructRigidTransform(Pos, Quat);
            Occ.Moving    = Moving;
        };

        // ① 6 Allied Tanks (Hull in SDF Distance Span + Rotating Turret + Destroyed Wreck Hull)
        for (let I = 0; I < this.Tanks.length; ++I)
        {
            const T = this.Tanks[I];
            if (T.State === "wreck")
            {
                SetOcc(Block.TankHulls[I],   ParkedUnderground, IdentityQuat, false);
                SetOcc(Block.TankTurrets[I], ParkedUnderground, IdentityQuat, false);
                SetOcc(Block.TankWrecks[I],  T.Position, ConstructAxisQuaternion([0, 0, 1], T.Yaw), false);
            }
            else
            {
                const QHull   = ConstructAxisQuaternion([0, 0, 1], T.Yaw);
                const QTurret = ConstructAxisQuaternion([0, 0, 1], T.TurretYaw);
                SetOcc(Block.TankHulls[I],   T.Position, QHull, true);
                SetOcc(Block.TankTurrets[I], [T.Position[0], T.Position[1], T.Position[2] + 1.38], QTurret, true);
                SetOcc(Block.TankWrecks[I],  ParkedUnderground, IdentityQuat, false);
            }
        }

        // ② 8 Rotating Twin-Barrel Machine-Gun Turrets
        for (let I = 0; I < this.MgTurrets.length; ++I)
        {
            const Mg = this.MgTurrets[I];
            SetOcc(Block.MgBases[I], Mg.Position, IdentityQuat, false);
            SetOcc(Block.MgHeads[I], [Mg.Position[0], Mg.Position[1], Mg.Position[2] + 0.72],
                   ConstructAxisQuaternion([0, 0, 1], Mg.Yaw), true);
        }

        // ③ 6 Heavy 155mm Fortress & Wall Artillery Cannons
        for (let I = 0; I < this.Cannons.length; ++I)
        {
            const Can = this.Cannons[I];
            SetOcc(Block.CannonBases[I],   Can.Position, IdentityQuat, false);
            SetOcc(Block.CannonBarrels[I], [Can.Position[0], Can.Position[1], Can.Position[2] + 1.32],
                   ConstructAxisQuaternion([0, 0, 1], Can.Yaw), true);
        }

        // ④ 4 Attack Aircraft (Fuselage + Spinning Nose Propeller)
        for (let I = 0; I < this.Aircraft.length; ++I)
        {
            const P = this.Aircraft[I];
            const QPlane = P.State === "skidding"
                ? MultiplyQuaternion(ConstructAxisQuaternion([0, 0, 1], Math.atan2(P.Velocity[1], P.Velocity[0] || -1.0)),
                                     ConstructAxisQuaternion([1, 0, 0], 0.35))
                : OrientationFromVelocity(P.Velocity, P.Roll);
            SetOcc(Block.AircraftBodies[I], P.Position, QPlane, true);
            const NoseWorld = RotateVector(QPlane, [3.62, 0.0, 0.11]);
            const QProp = MultiplyQuaternion(QPlane, ConstructAxisQuaternion([1, 0, 0], P.PropAngle));
            SetOcc(Block.AircraftProps[I], [
                P.Position[0] + NoseWorld[0],
                P.Position[1] + NoseWorld[1],
                P.Position[2] + NoseWorld[2]
            ], QProp, true);
        }

        // ⑤ Bombs, Shells, Tracers & Anti-Tank Mines
        for (let I = 0; I < DDayBombPoolSize; ++I)
        {
            const B = this.Bombs[I];
            if (B.Active)
                SetOcc(Block.Bombs[I], B.Position, OrientationFromVelocity(B.Velocity), true);
            else
                SetOcc(Block.Bombs[I], ParkedUnderground, IdentityQuat, false);
        }

        for (let I = 0; I < DDayShellPoolSize; ++I)
        {
            const S = this.Shells[I];
            if (S.Active)
                SetOcc(Block.Shells[I], S.Position, OrientationFromVelocity(S.Velocity), true);
            else
                SetOcc(Block.Shells[I], ParkedUnderground, IdentityQuat, false);
        }

        for (let I = 0; I < DDayTracerPoolSize; ++I)
        {
            const R = this.Tracers[I];
            if (R.Active)
                SetOcc(Block.Tracers[I], R.Position, OrientationFromVelocity(R.Velocity), true);
            else
                SetOcc(Block.Tracers[I], ParkedUnderground, IdentityQuat, false);
        }

        for (let I = 0; I < this.Mines.length; ++I)
        {
            const M = this.Mines[I];
            if (!M.Detonated)
                SetOcc(Block.Mines[I], M.Position, IdentityQuat, false);
            else
                SetOcc(Block.Mines[I], ParkedUnderground, IdentityQuat, false);
        }
    }

    // 📝 Real 3D GPU Fluid Plume & Fireball sources fed into SmokeSequence every frame!
    QuerySmokeEmissions()
    {
        if (!this.Enabled)
            return [];
        const CarPos = this.Host?.Vehicle?.Position || [0, 0, 0];
        const Emissions = [];

        // ① Active Bomb / Mine / Cannon / Tank / Aircraft Fireballs & Soot Plumes (within 75 m of camera/car toroidal fluid domain)
        for (const E of this.Explosions)
        {
            if (Math.hypot(E.Position[0] - CarPos[0], E.Position[1] - CarPos[1]) > 72.0)
                continue;
            const TNorm   = E.Age / E.Duration;
            const Burning = TNorm < 0.42;
            const Fade    = Math.max(0.0, 1.0 - TNorm);
            Emissions.push({
                Active:          true,
                Burning,
                Strength:        2.2 * E.Power * Fade,
                Position:        [E.Position[0], E.Position[1], E.Position[2] + 0.35],
                Hub:             [E.Position[0], E.Position[1], E.Position[2] + 0.45],
                Radius:          (Burning ? 1.15 : 0.95) * E.Power,
                Expansion:       Burning ? 7.2 * E.Power : 2.6,
                SmokeRate:       (Burning ? 26.0 : 48.0) * Fade * E.Power,
                SootRate:        (Burning ? 42.0 : 28.0) * Fade * E.Power,
                FlameRate:       Burning ? 34.0 * E.Power * (1.0 - TNorm * 1.8) : 0.0,
                HeatRate:        Burning ? 84.0 * E.Power : 14.0 * Fade,
                Velocity:        [0.0, 0.0, Burning ? 4.8 : 2.2],
                Plume:           Burning ? {
                    Offset:    [0.0, 0.0, 1.15],
                    Radius:    1.10 * E.Power,
                    SmokeRate: 30.0 * E.Power,
                    SootRate:  38.0 * E.Power,
                    HeatRate:  44.0 * E.Power,
                    Expansion: 3.8
                } : null,
                CombustionPower: Burning ? 180000.0 * E.Power : 0.0
            });
        }

        // ② Damaged / Cooking-off / Wrecked Allied Tanks near the car
        for (const T of this.Tanks)
        {
            if (T.Armor > 55.0 && T.State === "advancing")
                continue;
            if (Math.hypot(T.Position[0] - CarPos[0], T.Position[1] - CarPos[1]) > 64.0)
                continue;
            const Burning = T.State === "critical" || (T.State === "wreck" && T.StateTimer > 3.2);
            Emissions.push({
                Active:          true,
                Burning,
                Strength:        Burning ? 1.85 : 1.15,
                Position:        [T.Position[0] - 0.4, T.Position[1], T.Position[2] + 1.25],
                Hub:             [T.Position[0],       T.Position[1], T.Position[2] + 1.35],
                Radius:          Burning ? 0.85 : 0.55,
                Expansion:       Burning ? 4.6  : 1.8,
                SmokeRate:       Burning ? 22.0 : 32.0,
                SootRate:        Burning ? 34.0 : 18.0,
                FlameRate:       Burning ? 22.0 : 0.0,
                HeatRate:        Burning ? 54.0 : 8.0,
                Velocity:        [0.0, 0.0, 2.4],
                Plume:           null,
                CombustionPower: Burning ? 95000.0 : 0.0
            });
        }

        // ③ Stricken Diving / Skidding Aircraft Engine Fire Trail
        for (const P of this.Aircraft)
        {
            if (!P.TrailingSmoke && P.State !== "skidding")
                continue;
            if (Math.hypot(P.Position[0] - CarPos[0], P.Position[1] - CarPos[1]) > 68.0 || P.Position[2] > 18.0)
                continue;
            Emissions.push({
                Active:          true,
                Burning:         true,
                Strength:        1.75,
                Position:        P.Position.slice(),
                Hub:             P.Position.slice(),
                Radius:          0.75,
                Expansion:       4.2,
                SmokeRate:       26.0,
                SootRate:        36.0,
                FlameRate:       20.0,
                HeatRate:        48.0,
                Velocity:        [0.0, 0.0, 2.2],
                Plume:           null,
                CombustionPower: 85000.0
            });
        }

        return Emissions;
    }

    // 📝 Real-time Clustered Dynamic Lights: Explosion Flashes, Cannon/MG Muzzle Blasts, Wall Breach Beacons & Searchlights!
    QueryDynamicLightRecords()
    {
        if (!this.Enabled)
            return [];
        const CarPos = this.Host?.Vehicle?.Position || [0, 0, 0];
        const Out    = [];

        // ① Explosion Fireball Flashes (highest visual priority!)
        for (const E of this.Explosions)
        {
            if (E.Age > 0.85) continue;
            const Flash = Math.max(0.0, 1.0 - E.Age / 0.85);
            Out.push({
                X: E.Position[0], Y: E.Position[1], Z: E.Position[2] + 1.8,
                Type:      LightType.Omni,
                Direction: [0.0, 0.0, -1.0],
                Colour:    [1.0, 0.56, 0.14],
                Intensity: 240.0 * E.Power * Flash,
                Reach:     42.0 * E.Power
            });
        }

        // ② Heavy Cannon & MG Turret Muzzle Flashes near the car
        for (const Can of this.Cannons)
        {
            if (Can.MuzzleFlash <= 0.05) continue;
            if (Math.hypot(Can.Position[0] - CarPos[0], Can.Position[1] - CarPos[1]) > 140.0) continue;
            Out.push({
                X: Can.Position[0] + Math.cos(Can.Yaw) * 7.2,
                Y: Can.Position[1] + Math.sin(Can.Yaw) * 7.2,
                Z: Can.Position[2] + 1.85,
                Type:      LightType.Omni,
                Direction: [Math.cos(Can.Yaw), Math.sin(Can.Yaw), 0.0],
                Colour:    [1.0, 0.62, 0.18],
                Intensity: 185.0 * Can.MuzzleFlash,
                Reach:     34.0
            });
        }

        for (const Mg of this.MgTurrets)
        {
            if (Mg.MuzzleFlash <= 0.05) continue;
            if (Math.hypot(Mg.Position[0] - CarPos[0], Mg.Position[1] - CarPos[1]) > 95.0) continue;
            Out.push({
                X: Mg.Position[0] + Math.cos(Mg.Yaw) * 2.4,
                Y: Mg.Position[1] + Math.sin(Mg.Yaw) * 2.4,
                Z: Mg.Position[2] + 0.45,
                Type:      LightType.Omni,
                Direction: [Math.cos(Mg.Yaw), Math.sin(Mg.Yaw), 0.0],
                Colour:    [1.0, 0.68, 0.22],
                Intensity: 88.0 * Mg.MuzzleFlash,
                Reach:     20.0
            });
        }

        // ③ Atlantic Fortress Wall Breach Gate Emerald/Gold Guidance Flares & Sweeping Watchtower Searchlights!
        Out.push({
            X: DDayWallX - 3.5, Y:  0.0, Z: 7.2,
            Type:      LightType.Floodlight,
            Direction: [-1.0, 0.0, -0.22],
            Colour:    this.WallReached ? [0.20, 1.0, 0.55] : [1.0, 0.78, 0.25],
            Intensity: 165.0,
            Reach:     64.0
        });
        const SweepY = Math.sin(this.Clock * 0.85) * 0.45;
        Out.push({
            X: 314.0, Y:  15.6, Z: 17.8,
            Type:      LightType.HighBeam,
            Direction: [-0.88, SweepY, -0.26],
            Colour:    [0.92, 0.96, 1.0],
            Intensity: 190.0,
            Reach:     85.0
        });
        Out.push({
            X: 314.0, Y: -15.6, Z: 17.8,
            Type:      LightType.HighBeam,
            Direction: [-0.88, -SweepY, -0.26],
            Colour:    [0.92, 0.96, 1.0],
            Intensity: 190.0,
            Reach:     85.0
        });

        return Out.slice(0, 22);
    }

    // 📝 Dynamic tactical targets exposed to the 3D AR-HUD (`DrivingHudSequence`) for live windshield/chase reticles!
    QueryTacticalHudTargets(CarPos)
    {
        if (!this.Enabled || !CarPos)
            return [];
        const Targets = [];

        // ① Atlantic Wall Breach Objective Marker
        const WallDist = Math.hypot(DDayBreachGoalX - CarPos[0], 0.0 - CarPos[1]);
        if (WallDist < 220.0)
        {
            Targets.push({
                Id:            "dday:wall_breach",
                Kind:          "station",
                Label:         "WALL BREACH",
                SubLabel:      `${Math.round(this.DistanceToWall)} m`,
                Position:      [DDayBreachGoalX, 0.0, 3.8],
                HalfExtent:    [2.5, 9.2, 3.6],
                ForceSeverity: "info",
                MaxDist:       220.0
            });
        }

        // ② Nearest Advancing Allied Tanks (Mobile Shield Cover!)
        for (const T of this.Tanks)
        {
            if (T.State === "wreck") continue;
            const D = Math.hypot(T.Position[0] - CarPos[0], T.Position[1] - CarPos[1]);
            if (D < 58.0)
            {
                const IsCookoff = T.State === "critical";
                Targets.push({
                    Id:            `dday:${T.Id}`,
                    Kind:          IsCookoff ? "obstacle" : "station",
                    Label:         IsCookoff ? "TANK COOKOFF!" : `TANK COVER ${Math.round(T.Armor)}%`,
                    SubLabel:      `${Math.round(D)} m`,
                    Position:      [T.Position[0], T.Position[1], 1.45],
                    Velocity:      [T.Speed, 0.0, 0.0],
                    HalfExtent:    [3.15, 1.65, 1.45],
                    ForceSeverity: IsCookoff ? "critical" : "info",
                    HighlightRed:  IsCookoff,
                    MaxDist:       58.0
                });
            }
        }

        // ③ Armed Landmines directly ahead
        for (const M of this.Mines)
        {
            if (M.Detonated) continue;
            const DX = M.Position[0] - CarPos[0];
            const D  = Math.hypot(DX, M.Position[1] - CarPos[1]);
            if (DX > -4.0 && D < 36.0)
            {
                const Urgent = D < 16.0 && Math.abs(M.Position[1] - CarPos[1]) < 3.4;
                Targets.push({
                    Id:            `dday:${M.Id}`,
                    Kind:          "obstacle",
                    Label:         "MINE",
                    SubLabel:      `${Math.round(D)} m`,
                    Position:      [M.Position[0], M.Position[1], 0.35],
                    HalfExtent:    [0.65, 0.65, 0.35],
                    ForceSeverity: Urgent ? "critical" : "caution",
                    HighlightRed:  true,
                    MaxDist:       36.0
                });
            }
        }

        // ④ Diving Crash-Aircraft & Falling Bombs
        for (const P of this.Aircraft)
        {
            if (P.State !== "diving") continue;
            const D = Math.hypot(P.Position[0] - CarPos[0], P.Position[1] - CarPos[1]);
            if (D < 95.0)
            {
                Targets.push({
                    Id:            `dday:plane_${P.Index}`,
                    Kind:          "rail",
                    Label:         "AIRCRAFT CRASH!",
                    SubLabel:      `${Math.round(D)} m`,
                    Position:      P.Position.slice(),
                    Velocity:      P.Velocity.slice(),
                    HalfExtent:    [3.8, 4.8, 1.2],
                    ForceSeverity: D < 52.0 ? "critical" : "caution",
                    HighlightRed:  true,
                    MaxDist:       95.0
                });
            }
        }

        for (const B of this.Bombs)
        {
            if (!B.Active) continue;
            const D = Math.hypot(B.Position[0] - CarPos[0], B.Position[1] - CarPos[1]);
            if (D < 58.0)
            {
                Targets.push({
                    Id:            `dday:bomb_${B.Index}`,
                    Kind:          "obstacle",
                    Label:         "FALLING BOMB",
                    SubLabel:      `${Math.round(B.Position[2])}m ALT`,
                    Position:      B.Position.slice(),
                    Velocity:      B.Velocity.slice(),
                    HalfExtent:    [0.9, 0.5, 0.5],
                    ForceSeverity: D < 26.0 ? "critical" : "caution",
                    HighlightRed:  true,
                    MaxDist:       58.0
                });
            }
        }

        return Targets;
    }

    QuerySummary()
    {
        return {
            Enabled:         this.Enabled,
            ControlMode:     this.ControlMode,
            PlayerArmor:     this.PlayerArmor,
            CoverState:      this.PlayerCoverState,
            DistanceToWall:  this.DistanceToWall,
            WallReached:     this.WallReached,
            AssaultTimer:    this.AssaultTimer,
            BestAssaultTime: this.BestAssaultTime,
            ActiveTanks:     this.Tanks.filter((T) => T.State === "advancing").length,
            ActiveMines:     this.Mines.filter((M) => !M.Detonated).length,
            Stats:           { ...this.Stats }
        };
    }
}
