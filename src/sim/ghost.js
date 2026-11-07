/**
 * ghost — record a run, replay it as a rival made of your own past.
 *
 * This is the meta layer for a game with no laps and no circuit. There is no
 * leaderboard position to grind and no championship to accumulate, so the
 * opponent that always matters is your own previous best line through *this*
 * corridor. Corridors are seeded, so a ghost is exactly comparable run to run.
 *
 * Storage is one JSON blob per seed in localStorage. Pure module: the recorder
 * and playback have no DOM dependency, so they unit-test headlessly.
 */

export const GHOST_SAMPLE_INTERVAL = 0.1; // seconds between recorded frames

/** Frames are flat arrays: [t, x, y, z, yaw, pitch, arc]. */
export class GhostRecorder {
  constructor(interval = GHOST_SAMPLE_INTERVAL) {
    this.interval = interval;
    this.frames = [];
    this._acc = Infinity; // force a sample on the first record()
  }

  reset() {
    this.frames.length = 0;
    this._acc = Infinity;
  }

  /** @param {number} t race clock @param {object} v vehicle @param {number} arc */
  record(t, v, arc) {
    const dt = t - (this.frames.length ? this.frames[this.frames.length - 1][0] : -Infinity);
    if (dt < this.interval) return;
    this.frames.push([t, v.position.x, v.position.y, v.position.z, v.yaw, v.pitch, arc]);
  }

  get duration() { return this.frames.length ? this.frames[this.frames.length - 1][0] : 0; }
  get count() { return this.frames.length; }

  serialize() {
    return JSON.stringify({
      v: 1,
      time: +this.duration.toFixed(3),
      f: this.frames.map((f) => f.map((n) => +n.toFixed(2)))
    });
  }
}

export class GhostPlayback {
  constructor(frames, time = null) {
    this.frames = frames;
    this.time = time ?? (frames.length ? frames[frames.length - 1][0] : 0);
  }

  static deserialize(str) {
    try {
      const o = JSON.parse(str);
      if (!o || !Array.isArray(o.f) || o.f.length < 2) return null;
      return new GhostPlayback(o.f, o.time);
    } catch {
      return null;
    }
  }

  get empty() { return this.frames.length === 0; }

  /** Interpolated transform at race clock `t`. Clamped at both ends. */
  sample(t) {
    const f = this.frames;
    if (!f.length) return null;
    if (t <= f[0][0]) return this._at(0);
    if (t >= f[f.length - 1][0]) return this._at(f.length - 1);

    // Frames are time-ordered and evenly spaced, so this search is short.
    let lo = 0, hi = f.length - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (f[mid][0] <= t) lo = mid; else hi = mid;
    }
    const a = f[lo], b = f[hi];
    const span = Math.max(1e-6, b[0] - a[0]);
    const k = (t - a[0]) / span;
    return {
      x: a[1] + (b[1] - a[1]) * k,
      y: a[2] + (b[2] - a[2]) * k,
      z: a[3] + (b[3] - a[3]) * k,
      yaw: lerpAngle(a[4], b[4], k),
      pitch: a[5] + (b[5] - a[5]) * k
    };
  }

  _at(i) {
    const f = this.frames[i];
    return { x: f[1], y: f[2], z: f[3], yaw: f[4], pitch: f[5] };
  }

  /**
   * Time delta against the ghost at equal *progress*, not equal clock — the
   * only comparison that means anything on a point-to-point run.
   * Positive = you are behind your ghost.
   */
  deltaAt(arc, playerTime) {
    const f = this.frames;
    if (f.length < 2) return null;
    let best = 0, bestD = Infinity;
    for (let i = 0; i < f.length; i++) {
      const d = Math.abs(f[i][6] - arc);
      if (d < bestD) { bestD = d; best = i; }
    }
    return playerTime - f[best][0];
  }
}

function lerpAngle(a, b, t) {
  let d = (b - a) % (Math.PI * 2);
  if (d > Math.PI) d -= Math.PI * 2;
  if (d < -Math.PI) d += Math.PI * 2;
  return a + d * t;
}

/** localStorage wrapper, safe in Node where localStorage does not exist. */
export const GhostStore = {
  key(seed) { return `frontier:ghost:${seed}`; },

  available() {
    return typeof localStorage !== 'undefined';
  },

  load(seed) {
    if (!this.available()) return null;
    try {
      const raw = localStorage.getItem(this.key(seed));
      return raw ? GhostPlayback.deserialize(raw) : null;
    } catch {
      return null;
    }
  },

  bestTime(seed) {
    const g = this.load(seed);
    return g ? g.time : null;
  },

  /** Persist only if this run beats the stored best. Returns true if saved. */
  saveIfBest(seed, recorder) {
    if (!this.available() || recorder.count < 2) return false;
    const prev = this.bestTime(seed);
    if (prev !== null && recorder.duration >= prev) return false;
    try {
      localStorage.setItem(this.key(seed), recorder.serialize());
      return true;
    } catch {
      return false; // quota or private mode: a run is never lost over a ghost
    }
  },

  clear(seed) {
    if (!this.available()) return;
    try { localStorage.removeItem(this.key(seed)); } catch { /* ignore */ }
  }
};
