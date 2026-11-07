/**
 * main — composition root.
 *
 * Wires renderer -> world -> sim -> HUD, runs the sim on a fixed timestep so
 * physics is identical on a 60 Hz laptop and a 165 Hz monitor, and owns the
 * camera rig. Everything tunable lives in src/config.js.
 */

import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';

import { CFG } from './config.js';
import { clamp, damp, Rng } from './core/mathx.js';
import { Input } from './core/input.js';
import { AudioEngine } from './core/audio.js';
import { createCourse } from './world/course.js';
import { Race } from './sim/race.js';
import { GhostStore } from './sim/ghost.js';
import { World } from './render/scene.js';
import { CraftView } from './render/craft.js';
import { Hud } from './ui/hud.js';
import { Overlay } from './ui/overlay.js';

// ---------------------------------------------------------------------------
// Seed handling: a corridor is fully reproducible from its seed, so we keep it
// in the URL. Shareable runs are a core pillar of the flow-racing pitch.
// ---------------------------------------------------------------------------
function readSeed() {
  const m = /[#&]seed=(\d+)/.exec(location.hash);
  return m ? (parseInt(m[1], 10) >>> 0) : (CFG.course.seed >>> 0);
}
function writeSeed(seed) {
  history.replaceState(null, '', `#seed=${seed}`);
}

const FIXED_DT = 1 / 120;
const MAX_SUBSTEPS = 8;

class CameraRig {
  constructor(camera, cfg) {
    this.camera = camera;
    this.cfg = cfg.camera;
    this.mode = 0; // 0 chase, 1 close, 2 cinematic
    this.focus = new THREE.Vector3();
    this.desired = new THREE.Vector3();
    this.look = new THREE.Vector3();
    this.shake = 0;
    this.roll = 0;
    this._v = new THREE.Vector3();
    this._up = new THREE.Vector3(0, 1, 0);
  }

  addShake(amount) { this.shake = Math.min(2.4, this.shake + amount); }
  cycleMode() { this.mode = (this.mode + 1) % 3; return this.mode; }

  snap(player) {
    this.focus.copy(player.position);
    this.look.copy(player.position);
  }

  update(dt, player) {
    const c = this.cfg;
    const modes = [
      { dist: c.distance, height: c.height, fov: c.fovBase, damp: c.posDamp },
      { dist: 11.5, height: 4.2, fov: c.fovBase + 6, damp: 13 },
      { dist: 46, height: 17, fov: c.fovBase - 10, damp: 4.5 }
    ];
    const m = modes[this.mode];

    // Trail the craft rather than locking to it: lag reads as weight.
    this.desired.copy(player.position)
      .addScaledVector(player.heading, -m.dist)
      .addScaledVector(this._up, m.height);

    // Swing the camera out to the inside of a turn.
    this._v.copy(player.right).multiplyScalar(-player.roll * 7.5);
    this.desired.add(this._v);

    this.focus.x = damp(this.focus.x, this.desired.x, m.damp, dt);
    this.focus.y = damp(this.focus.y, this.desired.y, m.damp * 0.85, dt);
    this.focus.z = damp(this.focus.z, this.desired.z, m.damp, dt);

    this.look.x = damp(this.look.x, player.position.x + player.heading.x * c.lookAhead, c.lookDamp, dt);
    this.look.y = damp(this.look.y, player.position.y + player.heading.y * c.lookAhead + 2.5, c.lookDamp, dt);
    this.look.z = damp(this.look.z, player.position.z + player.heading.z * c.lookAhead, c.lookDamp, dt);

    this.shake = Math.max(0, this.shake - this.shake * c.shakeDecay * dt - 0.002);
    const s = this.shake;

    this.camera.position.set(
      this.focus.x + (Math.random() * 2 - 1) * s * 1.6,
      this.focus.y + (Math.random() * 2 - 1) * s * 1.6,
      this.focus.z + (Math.random() * 2 - 1) * s * 1.6
    );
    this.camera.lookAt(this.look);

    this.roll = damp(this.roll, player.roll * 0.55, 6, dt);
    this.camera.rotateZ(this.roll);

    const fov = m.fov
      + player.speedNorm * c.fovSpeedGain * 22
      + (player.boosting ? c.fovBoost : 0)
      + player.surgeNorm * c.fovSurge;
    if (Math.abs(this.camera.fov - fov) > 0.01) {
      this.camera.fov = fov;
      this.camera.updateProjectionMatrix();
    }
  }
}

// ---------------------------------------------------------------------------
class App {
  constructor() {
    this.seed = readSeed();
    writeSeed(this.seed);

    this.root = document.getElementById('app');
    this.renderer = new THREE.WebGLRenderer({
      antialias: true,
      powerPreference: 'high-performance',
      stencil: false
    });
    this.renderer.setPixelRatio(Math.min(devicePixelRatio || 1, CFG.quality.pixelRatioCap));
    this.renderer.setSize(innerWidth, innerHeight);
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.06;
    this.root.appendChild(this.renderer.domElement);

    this.scene = new THREE.Scene();
    this.camera = new THREE.PerspectiveCamera(CFG.camera.fovBase, innerWidth / innerHeight, 0.5, 12000);
    this.rig = new CameraRig(this.camera, CFG);

    this.input = new Input();
    this.audio = new AudioEngine();
    this.overlay = new Overlay(this.root);
    this.overlay.onAction = (a) => this.onAction(a);

    this.clock = new THREE.Clock();
    this.acc = 0;
    this.fps = 60;
    this._frames = 0;
    this._fpsTimer = 0;
    this._muted = false;
    this._resultsShown = false;
    this._cooldown = 0;
    this._bootDetail = document.getElementById('boot-detail');

    this.buildRun(this.seed);

    addEventListener('resize', () => this.onResize());
    addEventListener('keydown', (e) => this.onKey(e));

    this.overlay.title(this.seed, this.race.ghostBestTime);
    this.loop = this.loop.bind(this);
    requestAnimationFrame(() => {
      const boot = document.getElementById('boot');
      if (boot) boot.style.display = 'none';
      requestAnimationFrame(this.loop);
    });
  }

  // -------------------------------------------------------------------------
  buildRun(seed) {
    this.seed = seed >>> 0;
    writeSeed(this.seed);
    const cfg = structuredClone(CFG);
    cfg.course.seed = this.seed;
    this.cfg = cfg;

    if (this._bootDetail) this._bootDetail.textContent = 'generating corridor…';
    const t0 = performance.now();
    this.course = createCourse(cfg);
    this.race = new Race(cfg, this.course);
    const genMs = (performance.now() - t0).toFixed(0);
    if (this._bootDetail) {
      this._bootDetail.textContent =
        `${this.course.gateCount} gates · ${(this.course.totalLength / 1000).toFixed(1)} km · ${this.course.pylons.length} structures · ${genMs} ms`;
    }

    // Dispose the previous run's visuals if we are reseeding in-place.
    if (this.world) {
      this.scene.clear();
      this.craftViews.forEach((v) => v.dispose());
    }

    // Attach this seed's stored best run, if we have one.
    this.race.setGhost(GhostStore.load(this.seed));

    this.world = new World(this.scene, this.course, cfg);
    this.craftViews = [];
    for (const v of this.race.craft) {
      const view = new CraftView(v, cfg);
      this.scene.add(view.group);
      this.scene.add(view.groundGlow);
      if (view.trail) this.scene.add(view.trail);
      this.craftViews.push(view);
    }
    if (this.race.ghostVehicle) {
      const gv = new CraftView(this.race.ghostVehicle, cfg, { ghost: true });
      gv.group.visible = true;
      this.scene.add(gv.group);
      if (gv.trail) this.scene.add(gv.trail);
      this.craftViews.push(gv);
      this.ghostView = gv;
    } else {
      this.ghostView = null;
    }

    this.hud = new Hud(this.root, this.course);
    this.terrain = (x, z) => this.course.terrainHeight(x, z);

    this._buildComposer();
    this.wireEvents();

    this.rig.snap(this.race.player);
    this.rig.shake = 0;
    this._resultsShown = false;
    this._cooldown = 0;
    this.acc = 0;
    this._prevGateS = undefined;
    this.race._prevGateS = undefined;
    return genMs;
  }

  _buildComposer() {
    // A reseed builds a new chain; release the previous render targets.
    if (this.composer) {
      for (const pass of this.composer.passes) pass.dispose && pass.dispose();
      this.composer.dispose && this.composer.dispose();
    }
    this.composer = new EffectComposer(this.renderer);
    this.composer.addPass(new RenderPass(this.scene, this.camera));
    if (CFG.quality.bloom) {
      this.bloom = new UnrealBloomPass(
        new THREE.Vector2(innerWidth, innerHeight),
        CFG.quality.bloomStrength,
        CFG.quality.bloomRadius,
        CFG.quality.bloomThreshold
      );
      this.composer.addPass(this.bloom);
    }
    this.composer.addPass(new OutputPass());
    this.composer.setSize(innerWidth, innerHeight);
  }

  wireEvents() {
    const e = this.race.events;
    const p = this.race.player;

    e.onGate = (gate, combo, gained) => {
      this.audio.gate(combo);
      this.hud.toast(`GATE +${Math.round(gained)} FLOW`, 0.9);
      this.rig.addShake(0.06);
      void gate;
    };
    e.onMiss = () => {
      this.audio.miss();
      this.hud.toast('GATE MISSED — CHAIN LOST', 1.6);
    };
    e.onImpact = (v, sev) => {
      if (v !== p) return;
      this.audio.impact();
      this.rig.addShake(sev * CFG.craft.impactShake);
      this.hud.flashDamage();
    };
    e.onBoost = () => this.audio.boost();
    e.onSurge = () => {
      this.audio.surge();
      this.hud.toast('TACHYON SURGE', 1.6);
      this.rig.addShake(0.35);
    };
    e.onSkim = (pylon, intensity) => {
      if (intensity > 0.55) this.audio.beep(880 + intensity * 500, 0.07, 0.05);
    };
    e.onCountdown = (n) => {
      this.hud.countdown(n);
      this.audio.beep(n > 0 ? 440 : 880, n > 0 ? 0.14 : 0.3, 0.16);
    };
    e.onPosition = (rank, prev) => {
      this.hud.toast(rank < prev ? `P${rank}  ▲` : `P${rank}  ▼`, 1.2);
      this.audio.beep(rank < prev ? 1046 : 392, 0.12, 0.1);
    };
    e.onSector = (i, t) => this.hud.toast(`SECTOR ${i + 1}  ${t.toFixed(2)}s`, 2.2);
    e.onStart = () => { this.hud.toast('GO', 0.8); this.rig.addShake(0.25); };
    e.onFinish = () => { this._cooldown = 14; };
  }

  // -------------------------------------------------------------------------
  onAction(act) {
    this.ensureAudio();
    switch (act) {
      case 'start':
        this.beginRun();
        break;
      case 'resume':
        this.overlay.hide();
        this.race.pause(false);
        this.input.requestLock();
        break;
      case 'restart':
        this.beginRun();
        break;
      case 'menu':
        this.race.state = 'menu';
        this.overlay.title(this.seed, this.race.ghostBestTime);
        break;
      case 'reseed': {
        const s = (new Rng((Math.random() * 0xffffffff) >>> 0).int(1, 2 ** 31 - 1)) >>> 0;
        this.overlay.hide();
        this.buildRun(s);
        this.overlay.title(this.seed, this.race.ghostBestTime);
        break;
      }
    }
  }

  /** Launch (or relaunch) a run: clears per-run state the loop depends on. */
  beginRun() {
    this.overlay.hide();
    this.race.reset();          // -> 'countdown'
    this.rig.snap(this.race.player);
    this._resultsShown = false; // without this the second finish shows nothing
    this._cooldown = 0;
    this.acc = 0;
    this.input.requestLock();
  }

  ensureAudio() {
    if (!this.audio.ctx) this.audio.init();
    if (this.audio.ctx && this.audio.ctx.state === 'suspended') this.audio.ctx.resume();
  }

  onKey(e) {
    if (e.code === 'KeyM') {
      this._muted = !this._muted;
      this.audio.setMuted(this._muted);
      this.hud.toast(this._muted ? 'AUDIO MUTED' : 'AUDIO ON', 1);
    }
    if (this.overlay.visible) {
      if (e.code === 'Enter' || e.code === 'Space') {
        const act = this.overlay.kind === 'pause' ? 'resume' : this.overlay.kind === 'results' ? 'restart' : 'start';
        this.onAction(act);
      }
      return;
    }
    if (e.code === 'Escape') {
      if (this.race.pause(true)) this.overlay.pause();
      else if (this.overlay.kind === 'pause') { this.overlay.hide(); this.race.pause(false); }
    }
    if (e.code === 'KeyR' && !e.repeat) {
      this.beginRun();
      this.hud.toast('RUN RESET', 0.9);
    }
    if (e.code === 'KeyC' && !e.repeat) {
      const m = this.rig.cycleMode();
      this.hud.toast(['CHASE CAM', 'COCKPIT CAM', 'CINEMATIC CAM'][m], 1.1);
    }
  }

  onResize() {
    const w = innerWidth, h = innerHeight;
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
    this.renderer.setSize(w, h);
    this.composer.setSize(w, h);
    if (this.bloom) this.bloom.setSize(w, h);
  }

  // -------------------------------------------------------------------------
  loop() {
    requestAnimationFrame(this.loop);

    const dtRaw = Math.min(this.clock.getDelta(), 0.1);
    const dt = dtRaw;
    this._frames++;
    this._fpsTimer += dtRaw;
    if (this._fpsTimer >= 0.5) {
      this.fps = this._frames / this._fpsTimer;
      this._frames = 0;
      this._fpsTimer = 0;
      const m = document.getElementById('fpsMeter');
      if (m) m.textContent = `${this.fps.toFixed(0)} FPS · SEED ${this.seed} · ${this.course.gateCount} GATES`;
    }

    const cmd = this.input.sample(dt);
    const simActive = this.race.state === 'racing' || this.race.state === 'countdown';

    if (simActive) {
      this.acc += dt;
      let steps = 0;
      while (this.acc >= FIXED_DT && steps < MAX_SUBSTEPS) {
        this.race.step(FIXED_DT, cmd);
        this.acc -= FIXED_DT;
        steps++;
      }
      if (steps === MAX_SUBSTEPS) this.acc = 0; // never spiral
    } else if (this.race.state === 'finished') {
      // Let rivals complete so the finishing order is a real result.
      if (this._cooldown > 0) {
        this._cooldown -= dt;
        const pending = this.race.stepCooldown(dt);
        if (pending === 0) this._cooldown = Math.min(this._cooldown, 1.6);
      }
      if (!this._resultsShown && this._cooldown <= 0) {
        this._resultsShown = true;
        this.race.settleResults();
        const res = this.race.results();
        // A completed run becomes next run's opponent, if it was faster.
        const newBest = GhostStore.saveIfBest(this.seed, this.race.recorder);
        if (newBest) this.race.setGhost(GhostStore.load(this.seed));
        res.ghostBest = this.race.ghostBestTime;
        res.newBest = newBest;
        this.overlay.results(res);
      }
    }

    // --- present -------------------------------------------------------------
    if (this.ghostView) {
      this.ghostView.group.visible = this.race.state !== 'menu';
      this.ghostView.trail.visible = this.ghostView.group.visible;
    }
    for (let i = 0; i < this.craftViews.length; i++) {
      this.craftViews[i].update(dt, this.terrain);
    }
    this.rig.update(dt, this.race.player);
    this.world.update(dt, this.race, this.camera);
    this.hud.update(this.race, dt);

    const p = this.race.player;
    this.audio.update(p.speedNorm, 1, p.slipRatio, p.surgeNorm);

    if (this.bloom) {
      this.bloom.strength = CFG.quality.bloomStrength + p.surgeNorm * 0.85 + p.speedNorm * 0.18;
    }

    this.composer.render();
  }
}

// FPS readout element
const fpsEl = document.createElement('div');
fpsEl.id = 'fpsMeter';
document.body.appendChild(fpsEl);

window.addEventListener('error', (e) => {
  const boot = document.getElementById('boot');
  if (boot) {
    boot.style.display = 'grid';
    boot.innerHTML = `<div style="color:#ff6b6b">RUNTIME ERROR</div><div style="color:#8fa8b8;font-size:11px;max-width:640px">${e.message}</div>`;
  }
});

new App();
