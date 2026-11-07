/**
 * race — run state machine, scoring, collision and standings.
 *
 * A "run" replaces the lap: one start line, one finish line, four sector splits.
 * Standings are computed from distance travelled along the corridor, so the
 * race has real positions and overtakes without ever being a circuit.
 */

import * as THREE from 'three';
import { CFG } from '../config.js';
import { Vehicle, yawFromDirection } from './vehicle.js';
import { AiDriver } from './ai.js';
import { GhostRecorder, GHOST_SAMPLE_INTERVAL } from './ghost.js';
import { Rng, clamp, clamp01 } from '../core/mathx.js';

const CRAFT_RADIUS = 4.0;
const COUNTDOWN = 3.2;

export class Race {
  constructor(cfg, course) {
    this.cfg = cfg;
    this.course = course;
    this.rng = new Rng(cfg.course.seed ^ 0x5eed);

    this.state = 'menu'; // menu | countdown | racing | finished | paused
    this.time = 0;
    this.countdown = COUNTDOWN;
    this.elapsed = 0;
    this.worldTimeScale = 1;

    this.events = {
      onGate: null, onMiss: null, onImpact: null, onBoost: null,
      onSurge: null, onFinish: null, onCountdown: null, onSkim: null,
      onPosition: null, onSector: null, onStart: null
    };

    this.recorder = new GhostRecorder();
    this.ghostPlayback = null;
    this.ghostVehicle = null;
    this.ghostActive = false;
    this.ghostFinished = false;
    this.ghostTime = 0;

    this._buildSpatialHash();
    this._buildCraft();
    this.reset();
    // reset() arms the countdown; the run must not start ticking until the
    // player actually launches it, otherwise the grid goes green behind the
    // title screen. The app moves us to 'countdown' via reset() on launch.
    this.state = 'menu';
  }

  // -------------------------------------------------------------------------
  _buildSpatialHash() {
    this.pylonBucketSize = 220;
    this.pylonBuckets = new Map();
    this.course.pylons.forEach((p, i) => {
      p.id = i;
      const k = Math.floor(p.z / this.pylonBucketSize);
      let arr = this.pylonBuckets.get(k);
      if (!arr) { arr = []; this.pylonBuckets.set(k, arr); }
      arr.push(p);
    });
  }

  _buildCraft() {
    const terrain = (x, z) => this.course.terrainHeight(x, z);

    this.player = new Vehicle(this.cfg, {
      name: 'YOU', color: 0x37e6ff, isPlayer: true, terrain
    });

    this.rivals = [];
    this.drivers = [];
    for (const profile of this.cfg.rivals) {
      const v = new Vehicle(this.cfg, { name: profile.name, color: profile.color, terrain });
      const d = new AiDriver(this.cfg, profile, this.course, v, this.rng);
      v.onSurge = () => { if (v === this.player) this.events.onSurge && this.events.onSurge(); };
      this.rivals.push(v);
      this.drivers.push(d);
    }

    this.player.onSurge = () => {
      this.events.onSurge && this.events.onSurge();
      this.worldTimeScale = this.cfg.flow.surgeTimeDilation;
    };

    this.craft = [this.player, ...this.rivals];
    for (const v of this.craft) {
      v.arc = 0;
      v.sampleIndex = 0;
      v.rank = 0;
      v.gateIndex = 0;
    }
  }

  // -------------------------------------------------------------------------
  reset() {
    const course = this.course;
    this.state = 'countdown';
    this.countdown = COUNTDOWN;
    this.elapsed = 0;
    this.time = 0;
    this.worldTimeScale = 1;
    this.lastCountdownBeep = 4;

    for (const g of course.gates) { g.claimed = false; g.missed = false; }
    for (const pad of course.boostPads) pad.used = false;
    this.skimmed = new Set();
    this.recorder.reset();
    this.ghostTime = 0;
    this.ghostActive = !!(this.ghostPlayback && !this.ghostPlayback.empty);
    this.ghostFinished = false;
    this.splits = [];
    this.bestCombo = 0;
    this.score = 0;
    this.wrongWay = false;
    this.raceClock = 0;

    // Stagger the grid laterally behind the start line.
    const start = course.samples[0];
    const right = course.rights[0];
    const back = course.tangents[0];
    const gridSlots = [
      { lat: 0, back: 0, v: this.player },
      { lat: -22, back: 16, v: this.rivals[0] },
      { lat: 22, back: 16, v: this.rivals[1] },
      { lat: 0, back: 34, v: this.rivals[2] }
    ];
    for (const slot of gridSlots) {
      if (!slot.v) continue;
      const pos = start.clone()
        .addScaledVector(right, slot.lat)
        .addScaledVector(back, -slot.back);
      pos.y = course.terrainHeight(pos.x, pos.z) + this.cfg.craft.hoverHeight;
      slot.v.reset(pos, yawFromDirection(back));
      slot.v.arc = 0;
      slot.v.sampleIndex = 0;
      slot.v.gateIndex = 0;
      slot.v.rank = 0;
    }
    this.drivers.forEach((d) => d.reset());
    this.nextGateIndex = 0;
    this._prevGateS = undefined;
    this._prevGateIndex = -1;
    this._lastGateTime = undefined;
    this.combo = 1;
    this.updateStandings();
  }

  /**
   * Attach the stored best run for this seed so it flies alongside as a rival.
   * The ghost is kinematic — it is a recording, so it takes no part in
   * collision or standings.
   */
  setGhost(playback) {
    this.ghostPlayback = playback && !playback.empty ? playback : null;
    if (!this.ghostPlayback) { this.ghostVehicle = null; this.ghostActive = false; return; }
    if (!this.ghostVehicle) {
      this.ghostVehicle = new Vehicle(this.cfg, {
        name: 'GHOST', color: 0xa8c8ff,
        terrain: (x, z) => this.course.terrainHeight(x, z)
      });
      this.ghostVehicle.speedCapMul = 1;
    }
    // Deliberately not reset(): attaching a ghost must not re-arm the
    // countdown on a race that is already running or still on the menu.
    this.ghostTime = 0;
    this.ghostFinished = false;
    this.ghostActive = true;
    this._sampleGhost();
  }

  _sampleGhost() {
    if (!this.ghostActive || !this.ghostVehicle) return;
    const g = this.ghostPlayback;
    const s = g.sample(this.ghostTime);
    if (!s) return;
    const v = this.ghostVehicle;
    const prev = v.position.clone();
    v.position.set(s.x, s.y, s.z);
    v.yaw = s.yaw;
    v.pitch = s.pitch;
    v.roll = 0;
    // Give the view a plausible speed so its trail and engine read correctly.
    const moved = v.position.distanceTo(prev);
    if (moved > 0) v.speed = moved / GHOST_SAMPLE_INTERVAL;
    if (this.ghostTime >= g.time) { this.ghostActive = false; this.ghostFinished = true; }
  }

  /** Seconds behind your ghost at equal progress. Positive = behind. */
  get ghostDelta() {
    if (!this.ghostPlayback) return null;
    return this.ghostPlayback.deltaAt(this.player.arc, this.elapsed);
  }

  get ghostBestTime() { return this.ghostPlayback ? this.ghostPlayback.time : null; }

  start() {
    this.state = 'racing';
    this.events.onStart && this.events.onStart();
  }

  pause(on) {
    if (on && this.state === 'racing') { this.state = 'paused'; return true; }
    if (!on && this.state === 'paused') { this.state = 'racing'; return true; }
    return false;
  }

  // -------------------------------------------------------------------------
  /** Advance the whole run. `cmd` is the player's input sample. */
  step(dt, cmd) {
    this.time += dt;

    if (this.state === 'countdown') {
      this.countdown -= dt;
      const beep = Math.ceil(this.countdown);
      if (beep < this.lastCountdownBeep && beep >= 0) {
        this.lastCountdownBeep = beep;
        this.events.onCountdown && this.events.onCountdown(beep);
      }
      if (this.countdown <= 0) this.start();
      // Idle hover physics so the craft settle on the deck.
      for (const v of this.craft) v.update(dt, { steer: 0, pitch: 0, throttle: 0, brake: 1, boost: false, drift: false });
      return;
    }

    if (this.state !== 'racing') return;

    this.elapsed += dt;
    this.raceClock = this.elapsed;

    // Tachyon Surge dilates the world: rivals and FX slow, you do not.
    const surge = this.player.surgeActive;
    this.worldTimeScale = surge ? this.cfg.flow.surgeTimeDilation : 1;
    const wdt = dt * this.worldTimeScale;

    // --- player -------------------------------------------------------------
    const prevBoost = this.player.boost;
    this.player.update(dt, cmd);
    if (this.player.boosting && prevBoost > this.player.boost && !this._wasBoosting) {
      this.events.onBoost && this.events.onBoost();
    }
    this._wasBoosting = this.player.boosting;

    // --- rivals -------------------------------------------------------------
    for (const d of this.drivers) {
      const v = d.vehicle;
      if (v.finished) { v.update(wdt, { steer: 0, pitch: 0, throttle: 0, brake: 1, boost: false, drift: false }); continue; }
      d.update(wdt, this.player);
    }

    // --- world interactions -------------------------------------------------
    for (const v of this.craft) {
      this._progress(v);
      this._collidePylons(v);
      this._boostPads(v);
    }
    this._collideCraft();
    this._gates();
    this._skimming();
    this.updateStandings();

    this.recorder.record(this.elapsed, this.player, this.player.arc);
    this.ghostTime += dt;
    this._sampleGhost();

    if (this.player.finished && this.state === 'racing') {
      this.state = 'finished';
      this.events.onFinish && this.events.onFinish(this.results());
    }
  }

  // -------------------------------------------------------------------------
  _progress(v) {
    const n = this.course.nearest(v.position, v.sampleIndex, 80);
    v.sampleIndex = n.index;
    v.arc = n.arc;
    v.lateral = n.lateral;
  }

  _nearbyPylons(z) {
    const k = Math.floor(z / this.pylonBucketSize);
    const out = [];
    for (let i = -1; i <= 1; i++) {
      const b = this.pylonBuckets.get(k + i);
      if (b) for (const p of b) out.push(p);
    }
    return out;
  }

  _collidePylons(v) {
    const list = this._nearbyPylons(v.position.z);
    for (const p of list) {
      const dx = v.position.x - p.x;
      const dz = v.position.z - p.z;
      const distSq = dx * dx + dz * dz;
      const rr = p.r + CRAFT_RADIUS;
      if (distSq > rr * rr) continue;
      if (v.position.y < p.y - 2 || v.position.y > p.y + p.h) continue;

      const dist = Math.sqrt(distSq) || 0.001;
      const nx = dx / dist, nz = dz / dist;
      const n = this._n || (this._n = new THREE.Vector3());
      n.set(nx, 0, nz);

      // Depenetrate slightly past the surface so we are not exactly touching it
      // on the next frame (exactly-touching is what caused crafts to weld on).
      v.position.x = p.x + nx * (rr + 0.4);
      v.position.z = p.z + nz * (rr + 0.4);

      // Slide along the pillar: kill only the velocity going INTO the surface
      // and keep the tangential component, so a glancing blow costs a little
      // speed instead of stopping the craft dead against the structure.
      const into = v.velocity.dot(n);
      if (into < 0) v.velocity.addScaledVector(n, -into * 1.25);

      // Rate-limit the damage event: sustained contact is a scrape, not a
      // per-frame crash. Without this a wedged craft bled to zero and stuck.
      if (v.impactCooldown <= 0) {
        const severity = clamp01(v.speed / this.cfg.craft.topSpeed);
        v.applyImpact(0.4 + severity * 0.5, n);
        v.impactCooldown = 0.45;
        this.events.onImpact && this.events.onImpact(v, 0.5 + severity * 0.5);
      }
    }
  }

  _boostPads(v) {
    for (const pad of this.course.boostPads) {
      if (pad.used) {
        if (this.time >= pad.freeAt) pad.used = false;
        else continue;
      }
      const d = v.position.distanceTo(pad.center);
      if (d > pad.radius) continue;
      pad.used = true;
      pad.freeAt = this.time + 6;
      v.addBoost(pad.amount);
      v.addFlow(6);
      if (v === this.player) this.events.onBoost && this.events.onBoost(pad.amount);
    }
  }

  _collideCraft() {
    const list = this.craft;
    for (let i = 0; i < list.length; i++) {
      for (let j = i + 1; j < list.length; j++) {
        const a = list[i], b = list[j];
        const dx = b.position.x - a.position.x;
        const dy = b.position.y - a.position.y;
        const dz = b.position.z - a.position.z;
        const d2 = dx * dx + dy * dy + dz * dz;
        const rr = (CRAFT_RADIUS * 2.2) ** 2;
        if (d2 > rr || d2 < 1e-6) continue;
        const d = Math.sqrt(d2);
        const push = (CRAFT_RADIUS * 2.2 - d) * 0.5;
        const nx = dx / d, ny = dy / d, nz = dz / d;
        a.position.x -= nx * push; a.position.y -= ny * push; a.position.z -= nz * push;
        b.position.x += nx * push; b.position.y += ny * push; b.position.z += nz * push;
        const n = new THREE.Vector3(nx, ny, nz);
        a.velocity.addScaledVector(n, -2.5);
        b.velocity.addScaledVector(n, 2.5);
      }
    }
  }

  /**
   * Gate claiming. The gate is a disc in 3-D space: we watch the sign of the
   * signed distance to its plane and, on the crossing frame, test the radius in
   * the plane. Passing outside the ring is a miss — recoverable, not fatal,
   * because the run is about the line you fly, not about staying on rails.
   */
  _gates() {
    const gates = this.course.gates;
    const p = this.player;
    const g = gates[this.nextGateIndex];
    if (!g) return;

    const nx = g.normal.x, ny = g.normal.y, nz = g.normal.z;
    const s = (p.position.x - g.center.x) * nx + (p.position.y - g.center.y) * ny + (p.position.z - g.center.z) * nz;

    if (this._prevGateS === undefined || this._prevGateIndex !== this.nextGateIndex) {
      this._prevGateS = s;
      this._prevGateIndex = this.nextGateIndex;
      return;
    }

    // The ring normal points DOWN-corridor, so an approaching craft reads
    // negative (upstream of the plane) and positive once it has gone through.
    // Crossing counts in EITHER direction: overshoot a ring and you can turn
    // back through it. A miss costs the chain, it does not cost the run.
    if ((this._prevGateS <= 0 && s > 0) || (this._prevGateS >= 0 && s < 0)) {
      // Crossing frame: project into the ring plane.
      const px = p.position.x - g.center.x - nx * s;
      const py = p.position.y - g.center.y - ny * s;
      const pz = p.position.z - g.center.z - nz * s;
      const radial = Math.sqrt(px * px + py * py + pz * pz);

      if (radial <= g.radius * 1.06) this._claimGate(g, radial);
      else {
        g.missed = true;
        this.lastMissRadial = radial;
        this.lastMissRadius = g.radius;
        this._missGate(g);
      }
    }
    this._prevGateS = s;

    // Safety net: if the player somehow ends up well beyond the final ring.
    if (this.nextGateIndex === gates.length - 1 && p.arc > this.course.totalLength - 40) {
      this._claimGate(g, 0);
    }
  }

  _claimGate(g, radial) {
    const p = this.player;
    g.claimed = true;
    g.missed = false; // a late claim after an overshoot clears the marker
    const now = this.elapsed;
    const since = now - (this._lastGateTime ?? now);
    this._lastGateTime = now;

    if (since <= this.cfg.flow.comboWindow) this.combo = Math.min(this.cfg.flow.comboMax, (this.combo || 1) + 1);
    else this.combo = 1;
    this.bestCombo = Math.max(this.bestCombo, this.combo || 1);

    const precision = 1 - clamp01(radial / g.radius);
    const gained = this.cfg.flow.gateBase * (1 + (this.combo - 1) * 0.35) * (0.7 + p.speedNorm * 0.8) * (1 + precision * 0.5);
    p.addFlow(gained);
    p.addBoost(this.cfg.boost.gateReward * (0.6 + precision * 0.6));
    this.score += Math.round(gained * 10);

    const sector = this.sectorForGate(g.index);
    if (sector !== null && g.index === this.course.sectors[sector].lastGate) {
      this.splits[sector] = now;
      this.events.onSector && this.events.onSector(sector, now);
    }

    this.nextGateIndex++;
    p.gateIndex = this.nextGateIndex;
    this.events.onGate && this.events.onGate(g, this.combo, gained, precision);

    if (this.nextGateIndex >= this.course.gates.length) {
      p.finished = true;
      p.finishTime = now;
    }
  }

  _missGate(g) {
    this.combo = 1;
    this._lastGateTime = this.elapsed;
    this.events.onMiss && this.events.onMiss(g);
  }

  sectorForGate(gateIndex) {
    const sectors = this.course.sectors;
    for (let i = sectors.length - 1; i >= 0; i--) {
      if (gateIndex >= sectors[i].firstGate && gateIndex <= sectors[i].lastGate) return i;
    }
    return null;
  }

  /**
   * Skimming: passing close to corridor structure without touching it feeds the
   * flow meter. This is the risk/reward layer — the fast line is also the scary
   * one — and it is the mechanic that most separates this from a lane runner.
   */
  _skimming() {
    const p = this.player;
    if (p.speed < 28) return;
    const R = this.cfg.craft.nearMissRadius;
    const list = this._nearbyPylons(p.position.z);
    for (const py of list) {
      if (py.kind === 'wall') continue; // shoulders are not worth risking
      if (this.skimmed.has(py.id)) continue;
      const dx = p.position.x - py.x;
      const dz = p.position.z - py.z;
      const distSq = dx * dx + dz * dz;
      if (distSq > R * R) continue;
      const dist = Math.sqrt(distSq);
      if (dist < py.r + CRAFT_RADIUS + 1.5) continue; // that is a collision, not a skim
      if (p.position.y < py.y - 6 || p.position.y > py.y + py.h + 12) continue;
      this.skimmed.add(py.id);
      const intensity = clamp01(1 - (dist - py.r) / (R - py.r)) * (0.5 + p.speedNorm);
      p.registerSkim(intensity);
      this.score += Math.round(intensity * 25);
      this.events.onSkim && this.events.onSkim(py, intensity);
    }
  }

  // -------------------------------------------------------------------------
  updateStandings() {
    const scored = this.craft.map((v) => ({
      v,
      key: v.finished ? Number.POSITIVE_INFINITY : v.arc,
      time: v.finishTime
    }));
    scored.sort((a, b) => {
      if (a.key === b.key) return (a.time || 0) - (b.time || 0);
      return b.key - a.key; // further along the corridor is ahead
    });
    scored.forEach((s, i) => { s.v.rank = i + 1; });
    const prev = this._playerRank;
    this._playerRank = this.player.rank;
    if (prev !== undefined && prev !== this.player.rank) {
      this.events.onPosition && this.events.onPosition(this.player.rank, prev);
    }
    this.standings = scored.map((s) => s.v);
  }

  /** Wrong-way / off-line detection for the HUD nudge. */
  checkWrongWay() {
    const gates = this.course.gates;
    const g = gates[this.nextGateIndex];
    if (!g) return false;
    const s = (this.player.position.x - g.center.x) * g.normal.x +
              (this.player.position.y - g.center.y) * g.normal.y +
              (this.player.position.z - g.center.z) * g.normal.z;
    const along = this.player.velocity.dot(g.normal);
    return s > 30 && along < 4;
  }

  nextGate() { return this.course.gates[this.nextGateIndex] || null; }

  /**
   * After the player crosses the line the run keeps simulating briefly so
   * rivals can finish and the final order is a real result rather than a
   * snapshot. Called from the main loop while the results camera plays.
   */
  stepCooldown(dt) {
    this.elapsed += dt;
    this.time += dt;
    if (this.ghostActive) { this.ghostTime += dt; this._sampleGhost(); }
    let pending = 0;
    for (const d of this.drivers) {
      const v = d.vehicle;
      if (v.finished) continue;
      pending++;
      d.update(dt, this.player);
      this._progress(v);
      this._collidePylons(v);
    }
    this.updateStandings();
    return pending;
  }

  /** Mark every remaining rival as finished at the current clock. */
  settleResults() {
    for (const v of this.rivals) {
      if (!v.finished) { v.finished = true; v.finishTime = this.elapsed; }
    }
    this.updateStandings();
  }

  results() {
    const p = this.player;
    return {
      time: p.finishTime,
      rank: p.rank,
      gates: this.course.gates.filter((g) => g.claimed).length,
      total: this.course.gates.length,
      bestCombo: this.bestCombo,
      score: this.score,
      topSpeed: p.topSpeedReached,
      splits: this.splits.slice(),
      standings: this.standings.map((v) => ({
        name: v.name,
        time: v.finishTime || this.elapsed,
        finished: v.finished,
        rank: v.rank
      }))
    };
  }

  get running() { return this.state === 'racing'; }
  get comboValue() { return this.combo || 1; }
}

export { CRAFT_RADIUS, COUNTDOWN };
