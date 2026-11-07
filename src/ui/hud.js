/**
 * hud — DOM instrument cluster + tactical minimap.
 *
 * The minimap is a *tactical* view (a rotating window ahead of the craft) rather
 * than a whole-course overview, because a 12 km one-shot run drawn to scale is
 * just a line — useless at 400 km/h.
 */

import { clamp, clamp01 } from '../core/mathx.js';
import { MS_TO_KMH } from '../config.js';

const MAP_W = 240;
const MAP_H = 172;
const MAP_RANGE = 900; // metres of corridor shown ahead/behind

export class Hud {
  constructor(root, course) {
    this.course = course;
    this.root = root;
    // Reseeding rebuilds the HUD; drop the previous cluster first.
    document.getElementById('hud')?.remove();
    this._build();

    this.mapCtx = this.mapCanvas.getContext('2d');
    this._toastTimer = 0;
    this._countdownTimer = 0;
    this._dmgTimer = 0;
    this._lastCombo = 1;
  }

  _build() {
    const el = document.createElement('div');
    el.id = 'hud';
    el.innerHTML = `
      <div class="hud-corner tl">
        <div id="pos">P–<span class="of">/ –</span></div>
        <div id="clock">0:00.000</div>
        <div id="ghostLine" class="none">NO GHOST</div>
        <div id="sector">SECTOR 1 / ${this.course.sectorCount}</div>
      </div>

      <div class="hud-corner tc">
        <div id="gates">GATE 00 / 00</div>
        <div id="combo">x1</div>
        <div id="gatesProgress"><i></i></div>
      </div>

      <div class="hud-corner tr">
        <div id="mapWrap">
          <canvas id="map" width="${MAP_W}" height="${MAP_H}"></canvas>
          <div id="mapLabel">TACTICAL</div>
        </div>
      </div>

      <div class="hud-corner bl">
        <div id="flowWrap">
          <div class="meterLabel">FLOW</div>
          <div class="bar" id="flowBar"><i></i></div>
          <div id="tachyon">TACHYON CORE</div>
          <div class="bar" id="boostBar"><i></i></div>
        </div>
      </div>

      <div class="hud-corner br">
        <div id="speed">000</div>
        <div id="speedUnit">KM / H</div>
        <div id="gearState"></div>
      </div>

      <div class="hud-corner cc">
        <div id="countdown"></div>
        <div id="toast"></div>
      </div>
      <div id="wrongway">RETURN TO GATE</div>
      <div id="surgeFx"></div>
      <div id="dmgFx"></div>
    `;
    root.appendChild(el);
    this.el = el;

    const q = (id) => el.querySelector('#' + id);
    this.dom = {
      pos: q('pos'), clock: q('clock'), sector: q('sector'), ghost: q('ghostLine'),
      gates: q('gates'), combo: q('combo'), progress: q('gatesProgress i'),
      flowBar: q('flowBar i'), boostBar: q('boostBar i'), flowWrap: q('flowWrap'),
      tachyon: q('tachyon'), speed: q('speed'), gear: q('gearState'),
      countdown: q('countdown'), toast: q('toast'), wrongway: q('wrongway'),
      surgeFx: q('surgeFx'), dmgFx: q('dmgFx')
    };
    this.mapCanvas = q('map');
  }

  // -------------------------------------------------------------------------
  toast(msg, hold = 1.5) {
    this.dom.toast.textContent = msg;
    this.dom.toast.classList.add('show');
    this._toastTimer = hold;
  }

  countdown(n) {
    this.dom.countdown.textContent = n <= 0 ? 'GO' : String(n);
    this.dom.countdown.classList.add('show');
    this._countdownTimer = n <= 0 ? 0.7 : 1.0;
  }

  flashDamage() {
    this.dom.dmgFx.style.opacity = '1';
    this._dmgTimer = 0.28;
  }

  // -------------------------------------------------------------------------
  update(race, dt) {
    const d = this.dom;
    const p = race.player;
    const cfg = race.cfg;

    // --- position + timing ---------------------------------------------------
    const total = race.craft.length;
    d.pos.innerHTML = `P${p.rank || '-'}<span class="of">/ ${total}</span>`;
    const t = race.state === 'racing' || race.state === 'finished' ? race.elapsed : 0;
    d.clock.textContent = fmtTime(t);
    // --- ghost delta: compared at equal progress, not equal clock ----------
    if (!race.ghostPlayback) {
      d.ghost.textContent = 'NO GHOST — SET A TIME';
      d.ghost.className = 'none';
    } else if (race.ghostBestTime != null && !race.running) {
      d.ghost.textContent = `GHOST BEST ${fmtTime(race.ghostBestTime)}`;
      d.ghost.className = 'none';
    } else {
      const gd = race.ghostDelta;
      if (gd == null) {
        d.ghost.textContent = 'GHOST';
        d.ghost.className = 'none';
      } else {
        const behind = gd > 0;
        d.ghost.textContent = `${behind ? '+' : '\u2212'}${Math.abs(gd).toFixed(2)}s GHOST`;
        d.ghost.className = behind ? 'behind' : 'ahead';
      }
    }

    const sector = clamp((race.sectorForGate(Math.min(race.nextGateIndex, race.course.gateCount - 1)) ?? 0) + 1, 1, race.course.sectorCount);
    d.sector.textContent = `SECTOR ${sector} / ${race.course.sectorCount}`;

    // --- gate chain ----------------------------------------------------------
    const gIdx = Math.min(race.nextGateIndex, race.course.gateCount);
    d.gates.textContent = `GATE ${String(gIdx).padStart(2, '0')} / ${String(race.course.gateCount).padStart(2, '0')}`;
    d.progress.style.width = `${(gIdx / race.course.gateCount) * 100}%`;

    const combo = race.comboValue;
    if (combo > 1) {
      d.combo.textContent = `x${combo} CHAIN`;
      d.combo.classList.add('on');
    } else {
      d.combo.classList.remove('on');
    }
    this._lastCombo = combo;

    // --- meters --------------------------------------------------------------
    const flowPct = clamp01(p.flow / cfg.flow.max) * 100;
    d.flowBar.style.width = `${flowPct}%`;
    d.flowWrap.classList.toggle('ready', p.flow >= cfg.flow.max * 0.995 || p.surgeActive);
    d.tachyon.textContent = p.surgeActive
      ? `TACHYON SURGE  ${p.surge.toFixed(1)}s`
      : flowPct > 99 ? 'TACHYON READY' : 'TACHYON CORE';
    d.boostBar.style.width = `${clamp01(p.boost / cfg.boost.max) * 100}%`;

    // --- speedo --------------------------------------------------------------
    const kmh = Math.round(p.speed * MS_TO_KMH);
    d.speed.textContent = String(kmh).padStart(3, '0');
    d.gear.className = '';
    if (p.surgeActive) { d.gear.textContent = '▲ TACHYON SURGE'; d.gear.classList.add('surge'); }
    else if (p.boosting) { d.gear.textContent = '▲ BOOST'; d.gear.classList.add('boost'); }
    else if (p.drifting && p.slipRatio > 0.18) { d.gear.textContent = '≋ DRIFT'; d.gear.classList.add('boost'); }
    else if (p.scrape > 0.05) { d.gear.textContent = '▽ DECK CONTACT'; }
    else d.gear.textContent = '';

    // --- wrong way -----------------------------------------------------------
    const ww = race.running && race.checkWrongWay();
    d.wrongway.classList.toggle('show', !!ww);

    // --- fx overlays ---------------------------------------------------------
    d.surgeFx.style.opacity = String(p.surgeNorm * 0.9);
    if (this._dmgTimer > 0) {
      this._dmgTimer -= dt;
      if (this._dmgTimer <= 0) d.dmgFx.style.opacity = '0';
    }
    if (this._toastTimer > 0) {
      this._toastTimer -= dt;
      if (this._toastTimer <= 0) d.toast.classList.remove('show');
    }
    if (this._countdownTimer > 0) {
      this._countdownTimer -= dt;
      if (this._countdownTimer <= 0) d.countdown.classList.remove('show');
    }

    this._drawMap(race);
  }

  // -------------------------------------------------------------------------
  _drawMap(race) {
    const ctx = this.mapCtx;
    const p = race.player;
    const W = MAP_W, H = MAP_H;
    const cx = W / 2, cy = H * 0.72;
    const scale = (H * 0.62) / MAP_RANGE;

    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = 'rgba(4,10,18,.55)';
    ctx.fillRect(0, 0, W, H);

    const yaw = p.yaw;
    const cs = Math.cos(yaw), sn = Math.sin(yaw);
    const toMap = (wx, wz) => {
      const dx = wx - p.position.x, dz = wz - p.position.z;
      const lx = dx * cs - dz * sn;
      const lz = dx * sn + dz * cs;
      return [cx + lx * scale, cy - lz * scale];
    };

    // --- corridor centreline -------------------------------------------------
    const c = this.course;
    const i0 = Math.max(0, p.sampleIndex - 40);
    const i1 = Math.min(c.sampleCount - 1, p.sampleIndex + Math.ceil(MAP_RANGE / 6) + 20);
    ctx.strokeStyle = 'rgba(55,230,255,.20)';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    let started = false;
    for (let i = i0; i <= i1; i += 3) {
      const s = c.samples[i];
      if (!s) continue;
      const [mx, my] = toMap(s.x, s.z);
      if (mx < -30 || mx > W + 30 || my < -30 || my > H + 30) { started = false; continue; }
      if (!started) { ctx.moveTo(mx, my); started = true; } else ctx.lineTo(mx, my);
    }
    ctx.stroke();

    // --- boost pads ----------------------------------------------------------
    for (const pad of c.boostPads) {
      if (Math.abs(pad.center.z - p.position.z) > MAP_RANGE) continue;
      const [mx, my] = toMap(pad.center.x, pad.center.z);
      ctx.fillStyle = pad.used ? 'rgba(124,255,178,.18)' : 'rgba(124,255,178,.75)';
      ctx.beginPath(); ctx.arc(mx, my, 2.6, 0, Math.PI * 2); ctx.fill();
    }

    // --- gate chain ----------------------------------------------------------
    for (let i = 0; i < c.gates.length; i++) {
      const g = c.gates[i];
      if (Math.abs(g.center.z - p.position.z) > MAP_RANGE) continue;
      const [mx, my] = toMap(g.center.x, g.center.z);
      if (mx < -20 || mx > W + 20 || my < -20 || my > H + 20) continue;
      const isNext = i === race.nextGateIndex;
      ctx.beginPath();
      ctx.arc(mx, my, isNext ? 6 : 3.6, 0, Math.PI * 2);
      ctx.strokeStyle = g.claimed ? 'rgba(46,230,107,.65)'
        : g.missed ? 'rgba(255,59,59,.75)'
        : isNext ? 'rgba(159,246,255,1)' : 'rgba(47,111,150,.7)';
      ctx.lineWidth = isNext ? 2.2 : 1.2;
      ctx.stroke();
      if (isNext) {
        ctx.fillStyle = 'rgba(159,246,255,.18)';
        ctx.beginPath(); ctx.arc(mx, my, 6, 0, Math.PI * 2); ctx.fill();
      }
    }

    // --- ghost ---------------------------------------------------------------
    if (race.ghostVehicle && race.state !== 'menu') {
      const gp = race.ghostVehicle.position;
      if (Math.abs(gp.z - p.position.z) < MAP_RANGE) {
        const [mx, my] = toMap(gp.x, gp.z);
        ctx.save();
        ctx.translate(mx, my);
        ctx.rotate(-(race.ghostVehicle.yaw - yaw));
        ctx.fillStyle = 'rgba(168,200,255,.85)';
        ctx.beginPath();
        ctx.moveTo(0, -4.4); ctx.lineTo(3.2, 3.6); ctx.lineTo(-3.2, 3.6);
        ctx.closePath(); ctx.fill();
        ctx.restore();
      }
    }

    // --- rivals --------------------------------------------------------------
    for (const v of race.rivals) {
      const [mx, my] = toMap(v.position.x, v.position.z);
      if (mx < 0 || mx > W || my < 0 || my > H) continue;
      ctx.save();
      ctx.translate(mx, my);
      ctx.rotate(-(v.yaw - yaw));
      ctx.fillStyle = '#' + v.color.toString(16).padStart(6, '0');
      ctx.beginPath();
      ctx.moveTo(0, -5); ctx.lineTo(3.6, 4); ctx.lineTo(-3.6, 4);
      ctx.closePath(); ctx.fill();
      ctx.restore();
    }

    // --- player --------------------------------------------------------------
    ctx.save();
    ctx.translate(cx, cy);
    ctx.fillStyle = '#ffffff';
    ctx.beginPath();
    ctx.moveTo(0, -7); ctx.lineTo(5, 5.5); ctx.lineTo(0, 2.6); ctx.lineTo(-5, 5.5);
    ctx.closePath(); ctx.fill();
    ctx.restore();

    // range ring
    ctx.strokeStyle = 'rgba(55,230,255,.10)';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(cx, cy, H * 0.62, 0, Math.PI * 2); ctx.stroke();
  }
}

export function fmtTime(t) {
  if (!isFinite(t) || t < 0) t = 0;
  const m = Math.floor(t / 60);
  const s = Math.floor(t % 60);
  const ms = Math.floor((t % 1) * 1000);
  return `${m}:${String(s).padStart(2, '0')}.${String(ms).padStart(3, '0')}`;
}
