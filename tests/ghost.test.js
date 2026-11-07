/**
 * Ghost record / replay, and the regression that the run waits for the player.
 */
import { test, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import * as THREE from 'three';
import { CFG } from '../src/config.js';
import { createCourse } from '../src/world/course.js';
import { Race } from '../src/sim/race.js';
import { AiDriver } from '../src/sim/ai.js';
import { Rng } from '../src/core/mathx.js';
import {
  GhostRecorder, GhostPlayback, GhostStore, GHOST_SAMPLE_INTERVAL
} from '../src/sim/ghost.js';

const DT = 1 / 120;
const AUTO = { name: 'AUTO', tag: 'TEST', color: 0xffffff, skill: 0.995, aggression: 0, boostUse: 0.6, lineOffset: 0 };

// A real Map-backed localStorage so GhostStore's actual code path runs in Node.
function installLocalStorage() {
  const map = new Map();
  globalThis.localStorage = {
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: (k) => map.delete(k),
    clear: () => map.clear(),
    key: (i) => [...map.keys()][i] ?? null,
    get length() { return map.size; }
  };
  return map;
}

beforeEach(() => { installLocalStorage(); localStorage.clear(); });

// ---------------------------------------------------------------------------
test('a run does not start until the player launches it', () => {
  const cfg = structuredClone(CFG);
  const course = createCourse(cfg);
  const race = new Race(cfg, course);
  // Regression: the constructor used to arm the countdown, so the grid went
  // green behind the title screen before anyone pressed a button.
  assert.equal(race.state, 'menu', 'a fresh race must sit on the menu');
  assert.equal(race.running, false);
  race.reset();
  assert.equal(race.state, 'countdown', 'launching must arm the countdown');
  race.start();
  assert.equal(race.state, 'racing');
});

test('the recorder samples at its interval, not every frame', () => {
  const cfg = structuredClone(CFG);
  const course = createCourse(cfg);
  const race = new Race(cfg, course);
  const driver = new AiDriver(cfg, AUTO, course, race.player, new Rng(5));
  race.start();
  for (let i = 0; i < 120 * 10; i++) race.step(DT, driver.computeCommand(DT, null));

  const frames = race.recorder.count;
  // ~10 s at 120 Hz is 1200 frames; at 0.1 s sampling it must be ~100.
  assert.ok(frames > 80 && frames < 130, `expected ~100 frames, got ${frames}`);
  assert.ok(frames < 1200 / 5, 'recorder is sampling far too often');
  for (let i = 1; i < race.recorder.frames.length; i++) {
    const dt = race.recorder.frames[i][0] - race.recorder.frames[i - 1][0];
    assert.ok(dt >= GHOST_SAMPLE_INTERVAL - 1e-9, `frame gap ${dt} below the interval`);
  }
});

test('serialize / deserialize round-trips a run', () => {
  const r = new GhostRecorder();
  const v = { position: new THREE.Vector3(1.234, 5.678, 9.012), yaw: 0.5, pitch: -0.1 };
  for (let i = 0; i < 40; i++) {
    v.position.set(i * 10, 50 + Math.sin(i * 0.1) * 20, i * 90);
    v.yaw = Math.sin(i * 0.05);
    r.record(i * 0.1, v, i * 90);
  }
  const back = GhostPlayback.deserialize(r.serialize());
  assert.ok(back, 'deserialize returned null');
  assert.equal(back.frames.length, r.count);
  assert.ok(Math.abs(back.time - r.duration) < 0.01);
  assert.ok(GhostPlayback.deserialize('not json') === null);
  assert.ok(GhostPlayback.deserialize('{"v":1,"f":[]}') === null);
});

test('playback interpolates between frames and clamps at the ends', () => {
  const frames = [
    [0.0, 0, 10, 0, 0.0, 0.0, 0],
    [0.1, 100, 20, 0, 1.0, 0.2, 100],
    [0.2, 200, 30, 0, 2.0, 0.4, 200]
  ];
  const g = new GhostPlayback(frames, 0.2);

  const mid = g.sample(0.05);
  assert.ok(Math.abs(mid.x - 50) < 1e-6, `midpoint x ${mid.x}`);
  assert.ok(Math.abs(mid.y - 15) < 1e-6);
  assert.ok(Math.abs(mid.pitch - 0.1) < 1e-6);

  const before = g.sample(-5);
  assert.equal(before.x, 0, 'must clamp before the first frame');
  const after = g.sample(99);
  assert.equal(after.x, 200, 'must clamp past the last frame');
});

test('yaw interpolation takes the short way round', () => {
  const frames = [
    [0.0, 0, 0, 0, Math.PI - 0.1, 0, 0],
    [0.1, 0, 0, 0, -Math.PI + 0.1, 0, 0]
  ];
  const g = new GhostPlayback(frames, 0.1);
  const s = g.sample(0.05);
  // The short arc between +3.04 and -3.04 rad crosses +/-PI, not zero.
  assert.ok(Math.abs(s.yaw) > Math.PI - 0.2, `yaw took the long way round: ${s.yaw}`);
});

test('deltaAt is signed by progress, positive meaning behind', () => {
  const frames = [
    [10.0, 0, 0, 0, 0, 0, 1000],
    [10.1, 0, 0, 90, 0, 0, 1090]
  ];
  const g = new GhostPlayback(frames, 10.1);
  // Player reaches arc 1000 at t=12: two seconds behind the ghost.
  assert.ok(g.deltaAt(1000, 12) > 1.9);
  // Player reaches arc 1000 at t=8: two seconds ahead.
  assert.ok(g.deltaAt(1000, 8) < -1.9);
});

test('GhostStore keeps only the faster run per seed', () => {
  const mk = (dur) => {
    const r = new GhostRecorder();
    const v = { position: new THREE.Vector3(), yaw: 0, pitch: 0 };
    for (let i = 0; i <= Math.round(dur / 0.1); i++) {
      v.position.set(0, 50, i * 100);
      r.record(i * 0.1, v, i * 100);
    }
    return r;
  };

  assert.equal(GhostStore.bestTime(7), null, 'nothing stored yet');
  assert.equal(GhostStore.saveIfBest(7, mk(90)), true, 'first run must save');
  assert.ok(Math.abs(GhostStore.bestTime(7) - 90) < 0.2);

  assert.equal(GhostStore.saveIfBest(7, mk(120)), false, 'a slower run must not overwrite');
  assert.ok(Math.abs(GhostStore.bestTime(7) - 90) < 0.2, 'best was clobbered by a slower run');

  assert.equal(GhostStore.saveIfBest(7, mk(75)), true, 'a faster run must overwrite');
  assert.ok(Math.abs(GhostStore.bestTime(7) - 75) < 0.2);

  // Seeds must not bleed into each other.
  assert.equal(GhostStore.bestTime(8), null, 'ghost leaked across seeds');

  GhostStore.clear(7);
  assert.equal(GhostStore.bestTime(7), null);
});

test('a stored ghost flies alongside and reports a live delta', () => {
  const cfg = structuredClone(CFG);
  cfg.course.seed = 424242;
  const course = createCourse(cfg);

  // Record a run.
  const a = new Race(cfg, course);
  const da = new AiDriver(cfg, AUTO, course, a.player, new Rng(11));
  a.start();
  while (a.state === 'racing') a.step(DT, da.computeCommand(DT, null));
  assert.equal(a.player.finished, true);
  const recorded = a.recorder.count;
  assert.ok(recorded > 100, `only recorded ${recorded} frames`);

  // Replay it as a ghost on a fresh race.
  const b = new Race(cfg, course);
  b.setGhost(GhostPlayback.deserialize(a.recorder.serialize()));
  assert.ok(b.ghostVehicle, 'ghost craft was not created');
  assert.equal(b.state, 'menu', 'attaching a ghost must not arm the countdown');
  assert.ok(b.ghostBestTime > 20);

  const db = new AiDriver(cfg, AUTO, course, b.player, new Rng(11));
  b.start();
  let t = 0;
  while (b.state === 'racing' && t < 200) { b.step(DT, db.computeCommand(DT, null)); t += DT; }

  assert.equal(b.player.finished, true, 'replay run did not finish');
  const delta = b.ghostDelta;
  assert.ok(delta != null && Number.isFinite(delta), 'no ghost delta reported');
  // Same seed, same brain, same start -> the two runs should nearly coincide.
  assert.ok(Math.abs(delta) < 8, `identical replay drifted ${delta.toFixed(2)}s from its own ghost`);
});

test('a race with no ghost still runs cleanly', () => {
  const cfg = structuredClone(CFG);
  const course = createCourse(cfg);
  const race = new Race(cfg, course);
  assert.equal(race.ghostVehicle, null);
  assert.equal(race.ghostBestTime, null);
  assert.equal(race.ghostDelta, null);
  const d = new AiDriver(cfg, AUTO, course, race.player, new Rng(3));
  race.start();
  for (let i = 0; i < 120 * 15; i++) race.step(DT, d.computeCommand(DT, null));
  assert.ok(race.player.arc > 500, 'race did not progress without a ghost');
});
