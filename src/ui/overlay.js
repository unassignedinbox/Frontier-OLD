/**
 * overlay — title / pause / results screens.
 */

import { fmtTime } from './hud.js';
import { MS_TO_KMH } from '../config.js';

const CONTROLS = [
  ['A / D', 'steer  (analogue — the nose turns, the mass keeps going)'],
  ['Q / E', 'altitude — climb and dive'],
  ['S', 'air brake'],
  ['SPACE', 'boost'],
  ['SHIFT', 'drift — trade grip for turn-in and flow'],
  ['MOUSE', 'fine steering (click to capture)'],
  ['C / R', 'camera / restart'],
  ['M / ESC', 'mute / pause'],
  ['PAD', 'left stick steer, RT boost, LT drift']
];

export class Overlay {
  constructor(root) {
    this.root = root;
    this.el = document.createElement('div');
    this.el.id = 'overlay';
    root.appendChild(this.el);
    this.onAction = null;
    this.el.addEventListener('click', (e) => {
      const b = e.target.closest('button[data-act]');
      if (b && this.onAction) this.onAction(b.dataset.act);
    });
    this.kind = null;
  }

  get visible() { return !this.el.classList.contains('hidden'); }
  hide() { this.el.classList.add('hidden'); this.kind = null; }
  show(kind) { this.el.classList.remove('hidden'); this.kind = kind; }

  title(seed, bestTime = null) {
    this.show('title');
    const best = bestTime != null
      ? `<div style="font-size:11px;letter-spacing:.24em;color:#7cffb2;margin-top:14px">YOUR BEST ON THIS CORRIDOR &nbsp; ${fmtTime(bestTime)}</div>`
      : `<div style="font-size:11px;letter-spacing:.24em;color:#4c7288;margin-top:14px">NO TIME RECORDED ON THIS CORRIDOR</div>`;
    this.el.innerHTML = `
      <div>
        <h1>FRONTIER</h1>
        <div class="sub">FLOW RACING PROTOTYPE &nbsp;·&nbsp; BUILD 0.1</div>
        <div class="panel">
          <div class="pillars">
            <div>NO TRACK</div><div>NO LANES</div><div>NO LAPS</div>
            <div>ANALOGUE STEERING</div><div>ONE-SHOT RUNS</div>
          </div>
          <div style="font-size:11px;letter-spacing:.2em;color:#8fb4c8;line-height:1.9;margin:18px 0 20px">
            A single point-to-point run through a generated canyon, ~12 km, four sectors,
            ${'gate chain'} weaving laterally <b style="color:#9ff6ff">and</b> vertically.
            Fly the line, skim the structure for flow, and beat three rivals to the finish.
          </div>
          <div class="controls">
            ${CONTROLS.map(([k, v]) => `<b>${k}</b><span>${v}</span>`).join('')}
          </div>
          <button data-act="start">LAUNCH RUN</button>
          <button class="ghost" data-act="reseed">NEW CORRIDOR</button>
          ${best}
          <div id="seedLine">CORRIDOR SEED ${seed}</div>
        </div>
      </div>`;
  }

  pause() {
    this.show('pause');
    this.el.innerHTML = `
      <div>
        <h1 style="font-size:46px">PAUSED</h1>
        <div class="sub">FLOW CORE IDLE</div>
        <div class="panel">
          <div class="controls">${CONTROLS.map(([k, v]) => `<b>${k}</b><span>${v}</span>`).join('')}</div>
          <button data-act="resume">RESUME</button>
          <button class="ghost" data-act="restart">RESTART RUN</button>
          <button class="ghost" data-act="menu">ABANDON</button>
        </div>
      </div>`;
  }

  results(res) {
    this.show('results');
    const won = res.rank === 1;
    const medals = ['1ST', '2ND', '3RD', '4TH'];
    const rows = res.standings.map((s, i) => `
      <div class="row ${s.rank === 1 ? 'win' : 'lose'}">
        <span>${medals[i] || (i + 1)} &nbsp; ${s.name}</span>
        <b>${s.finished ? fmtTime(s.time) : 'DNF'}</b>
      </div>`).join('');
    const splits = res.splits.map((s, i) => `
      <div class="row"><span>SECTOR ${i + 1}</span><b>${fmtTime(s)}</b></div>`).join('');

    this.el.innerHTML = `
      <div>
        <h1 style="font-size:${won ? 62 : 52}px;color:${won ? '#7cffb2' : '#ffffff'}">${won ? 'RUN WON' : 'RUN COMPLETE'}</h1>
        ${res.newBest ? '<div class="sub" style="color:#7cffb2;letter-spacing:.36em">NEW CORRIDOR RECORD — SAVED AS YOUR GHOST</div>' : ''}
        <div class="sub">YOU FINISHED ${medals[(res.rank || 1) - 1] || res.rank}</div>
        <div class="panel">
          <div id="results">
            <div class="row"><span>TOTAL TIME</span><b>${fmtTime(res.time)}</b></div>
            <div class="row"><span>TOP SPEED</span><b>${Math.round(res.topSpeed * MS_TO_KMH)} KM/H</b></div>
            <div class="row"><span>GATES</span><b>${res.gates} / ${res.total}</b></div>
            <div class="row"><span>BEST CHAIN</span><b>x${res.bestCombo}</b></div>
            <div class="row"><span>FLOW SCORE</span><b>${res.score.toLocaleString()}</b></div>
            ${res.ghostBest != null ? `<div class="row"><span>GHOST TO BEAT</span><b>${fmtTime(res.ghostBest)}</b></div>` : ''}
            <hr/>
            ${splits}
            <hr/>
            ${rows}
          </div>
          <div style="margin-top:22px">
            <button data-act="restart">RUN IT AGAIN</button>
            <button class="ghost" data-act="reseed">NEW CORRIDOR</button>
          </div>
        </div>
      </div>`;
  }
}
