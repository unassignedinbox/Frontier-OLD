/* Project-Zero Vehicle Lab — browser-local interactive inspector overlay. */
(() => {
  "use strict";

  if (window.ProjectZeroVehicleLab) return;
  window.ProjectZeroVehicleLab = { version: "1.0.0" };

  const icons = {
    close: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m6 6 12 12M18 6 6 18"/></svg>',
    expand: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5"/></svg>',
    rotate: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20 11a8 8 0 1 0 2 5.3M20 4v7h-7"/></svg>',
    play: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 6 9 6-9 6Z"/></svg>',
    pause: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 5v14M16 5v14"/></svg>',
    sliders: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h11m3 0h2M4 17h3m3 0h11M12 4v6M7 14v6"/></svg>',
  };

  const vehicles = {
    control: {
      id: "control",
      name: "Control vehicle",
      short: "CV–01",
      category: "Project Drive / Jolt",
      description: "GRIT drivetrain, forced induction, thermal gas-law tyres and Spring / XPBD / 3D Hex8 FEM carcasses.",
      accent: "#79e1b7",
      mass: 1300,
      torque: 730,
      power: 475,
      powerUnit: "kW",
      zero: 4.62,
      topSpeed: 324,
      brake: 34.9,
      balance: "49 / 51",
      drivetrain: "AWD · active vectoring",
      engine: "4.0 L V8 · forced induction",
      tyre: "3D FEM · 110 kPa cold",
      suspension: 2.0,
      aero: 43,
      color: "Mint graphite",
      gpu: 7.4,
      simulation: 2.31,
      zeroG: 1.16,
    },
    endurance: {
      id: "endurance",
      name: "Endurance GT",
      short: "GT–24",
      category: "Vehicle profile / endurance",
      description: "Long-run calibration with conservative boost, higher cooling reserve and a stable aero balance for repeatable stints.",
      accent: "#9bbcf8",
      mass: 1415,
      torque: 670,
      power: 420,
      powerUnit: "kW",
      zero: 5.08,
      topSpeed: 309,
      brake: 36.8,
      balance: "52 / 48",
      drivetrain: "RWD · e-diff",
      engine: "3.0 L inline-six · twin turbo",
      tyre: "Thermal MF6.1 · 118 kPa cold",
      suspension: 2.35,
      aero: 61,
      color: "Ice blue",
      gpu: 7.1,
      simulation: 2.52,
      zeroG: 1.03,
    },
    rally: {
      id: "rally",
      name: "Terrain mule",
      short: "RM–17",
      category: "Vehicle profile / terrain",
      description: "High-travel chassis profile with event-surface lookahead, traction biasing and robust tyre-carcase diagnostics.",
      accent: "#f2bd74",
      mass: 1488,
      torque: 690,
      power: 386,
      powerUnit: "kW",
      zero: 5.44,
      topSpeed: 267,
      brake: 39.6,
      balance: "54 / 46",
      drivetrain: "AWD · terrain split",
      engine: "2.0 L four · anti-lag turbo",
      tyre: "XPBD carcass · 148 kPa cold",
      suspension: 3.15,
      aero: 28,
      color: "Dune bronze",
      gpu: 7.8,
      simulation: 2.74,
      zeroG: 0.96,
    },
  };

  const tabs = [
    ["overview", "Overview"],
    ["dynamics", "Dynamics"],
    ["powertrain", "Powertrain"],
    ["tires", "Tyres"],
    ["engine", "Engine"],
  ];

  const systemRows = [
    ["overview", "Vehicle Inspector", "asset", "#79e1b7"],
    ["dynamics", "Chassis & Dynamics", "solver", "#9bbcf8"],
    ["powertrain", "Powertrain Graph", "curve", "#f2bd74"],
    ["tires", "Tyre Telemetry", "4 corners", "#c6a7ff"],
    ["engine", "Frame & Runtime", "6 passes", "#72d0e9"],
  ];

  const state = {
    vehicle: "control",
    tab: "overview",
    running: true,
    wheel: "FL",
    view: "Isometric",
    speed: 87,
    rpm: 4580,
    boost: 0.74,
    gForce: 0.62,
    throttle: 68,
    brakeInput: 0,
    steering: -11,
    suspension: 2.0,
    damping: 62,
    aero: 43,
    torqueSplit: 52,
    tyrePressure: 110,
    regen: 18,
    renderScale: 92,
    quality: "Epic",
    mode: "Track",
    selectedTexture: "Set A",
    settings: {
      abs: true,
      traction: true,
      thermal: true,
      stability: false,
      tyreFEM: true,
      fixedStep: true,
      restir: true,
      gpuCulling: true,
      motionVectors: true,
      telemetry: true,
    },
    samples: [],
  };

  let studio;
  let launcher;
  let toast;
  let toastTimer;
  let timer;

  const selectedVehicle = () => vehicles[state.vehicle];
  const byId = (id) => document.getElementById(id);
  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
  const n = (value, digits = 0) => Number(value).toFixed(digits);
  const nowTime = () => new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });

  function boot() {
    if (byId("vehicle-studio")) return;
    seedSamples();
    document.body.insertAdjacentHTML("beforeend", shell());
    studio = byId("vehicle-studio");
    launcher = byId("vehicle-studio-launcher");
    toast = byId("vehicle-studio-toast");
    renderAll();
    bindEvents();
    timer = window.setInterval(tick, 420);
  }

  function shell() {
    return `
      <section id="vehicle-studio" aria-label="Project-Zero Vehicle Lab">
        <div class="vs-app">
          <header class="vs-header">
            <div class="vs-brand" aria-label="Frontier Project-Zero">
              <span class="vs-brand-mark" aria-hidden="true"></span>
              <span class="vs-brand-copy"><strong>FRONTIER</strong><span>Project-Zero / Vehicle Lab</span></span>
            </div>
            <div class="vs-crumb">EDITOR WORKSPACE <span aria-hidden="true">/</span> VEHICLE AUTHORING</div>
            <div class="vs-header-spacer"></div>
            <nav class="vs-top-nav" aria-label="Vehicle workspace views">
              <button class="is-active" type="button" data-action="tab" data-tab="overview">Inspector</button>
              <button type="button" data-action="tab" data-tab="powertrain">Curves</button>
              <button type="button" data-action="tab" data-tab="engine">Runtime</button>
            </nav>
            <div id="vs-live-chip" class="vs-live-chip"><i></i><span>LIVE LOCAL PREVIEW</span></div>
            <button class="vs-close-button" type="button" data-action="close" title="Return to the Project-Zero scene">${icons.close}<span>Scene</span></button>
          </header>
          <div class="vs-layout">
            <aside class="vs-rail" aria-label="Vehicle assets and systems">
              <div id="vs-rail-content"></div>
            </aside>
            <main class="vs-main" aria-label="Vehicle editor viewport">
              <div class="vs-main-inner">
                <div id="vs-context"></div>
                <div id="vs-stage-slot"></div>
                <div id="vs-main-bottom" class="vs-main-bottom"></div>
              </div>
            </main>
            <aside class="vs-inspector" aria-label="Vehicle inspector">
              <div id="vs-inspector-top"></div>
              <div id="vs-inspector-body" class="vs-inspector-body"></div>
            </aside>
          </div>
          <footer id="vs-footer" class="vs-footer"></footer>
        </div>
      </section>
      <button id="vehicle-studio-launcher" class="vs-launcher" type="button" data-action="open" title="Open Vehicle Lab"><i></i><span>Vehicle Lab</span></button>
      <div id="vehicle-studio-toast" class="vs-toast" role="status" aria-live="polite"><i></i><span></span></div>
    `;
  }

  function seedSamples() {
    const vehicle = selectedVehicle();
    state.samples = Array.from({ length: 34 }, (_, index) => {
      const phase = index / 33;
      const wobble = Math.sin(index * 0.74) * 4 + Math.cos(index * 0.29) * 2;
      return {
        speed: clamp(46 + phase * 47 + wobble, 0, vehicle.topSpeed),
        rpm: clamp(2200 + phase * 3050 + Math.sin(index * 0.42) * 420, 900, 7200),
        boost: clamp(0.19 + phase * 0.58 + Math.sin(index * 0.36) * 0.09, 0, 1.2),
      };
    });
  }

  function renderAll() {
    document.documentElement.style.setProperty("--vehicle-accent", selectedVehicle().accent);
    renderRail();
    renderContext();
    renderStage();
    renderMainBottom();
    renderInspector();
    renderFooter();
    syncLiveValues();
  }

  function renderRail() {
    const vehicle = selectedVehicle();
    const assetCards = Object.values(vehicles)
      .map((item) => `
        <button class="vs-asset-card ${item.id === state.vehicle ? "is-active" : ""}" style="--asset-accent:${item.accent}" type="button" data-action="vehicle" data-vehicle="${item.id}">
          <span class="vs-asset-glyph" aria-hidden="true"></span>
          <span class="vs-asset-meta"><strong>${item.name}</strong><span>${item.short} · ${item.category.split("/").pop().trim()}</span></span>
          <small>${item.power} kW</small>
        </button>`)
      .join("");
    const systems = systemRows
      .map(([id, label, meta, colour]) => `
        <button class="vs-system-button ${id === state.tab ? "is-active" : ""}" type="button" data-action="tab" data-tab="${id}" style="--system-colour:${colour}">
          <i aria-hidden="true"></i><span>${label}</span><small>${meta}</small>
        </button>`)
      .join("");

    byId("vs-rail-content").innerHTML = `
      <div class="vs-rail-title"><h2>Vehicle assets</h2><span class="vs-status-note">${Object.keys(vehicles).length} loaded</span></div>
      <div class="vs-asset-stack">${assetCards}</div>
      <div class="vs-rail-separator"></div>
      <div class="vs-rail-title"><h2>Engine systems</h2><span class="vs-status-note">ready</span></div>
      <div class="vs-system-list">${systems}</div>
      <div class="vs-rail-separator"></div>
      <section class="vs-runtime-card" aria-label="Preview runtime status">
        <header><h3>Local runtime</h3><span>${state.running ? "simulating" : "paused"}</span></header>
        <div class="vs-runtime-grid">
          <div><span>Fixed step</span><strong>4.17 ms</strong></div>
          <div><span>Physics</span><strong>${vehicle.simulation.toFixed(2)} ms</strong></div>
          <div><span>Render queue</span><strong>${vehicle.gpu.toFixed(1)} ms</strong></div>
          <div><span>Vehicle</span><strong>${vehicle.short}</strong></div>
        </div>
      </section>
    `;
  }

  function renderContext() {
    const vehicle = selectedVehicle();
    byId("vs-context").innerHTML = `
      <div class="vs-context-bar">
        <div class="vs-context-path"><span>Vehicle editor</span><b>›</b><strong>${vehicle.name}</strong><b>›</b><span>${tabLabel(state.tab)}</span></div>
        <div class="vs-context-spacer"></div>
        <span class="vs-mode-pill"><i></i>${state.view}</span>
        <button class="vs-icon-button" type="button" data-action="view" title="Cycle vehicle camera">${icons.rotate}</button>
        <button class="vs-icon-button" type="button" data-action="focus" title="Focus authored vehicle">${icons.expand}</button>
      </div>`;
  }

  function renderStage() {
    const vehicle = selectedVehicle();
    const accent = vehicle.accent;
    const speedPercent = clamp(state.speed / vehicle.topSpeed, 0, 1);
    const carShift = (state.steering * 0.1).toFixed(1);
    byId("vs-stage-slot").innerHTML = `
      <section class="vs-stage" style="--vehicle-accent:${accent}" aria-label="Interactive vehicle editor viewport">
        <div class="vs-stage-topline"><span><strong>Vehicle authoring view</strong> · browser local</span><span class="vs-stage-compass">${state.view} · 47°</span></div>
        <svg class="vs-vehicle-svg" viewBox="0 0 820 430" role="img" aria-label="Stylised ${vehicle.name} chassis infographic">
          <defs>
            <linearGradient id="vs-body-gradient" x1="0" x2="1" y1="0" y2="1"><stop stop-color="#e1f1e7" stop-opacity=".92"/><stop offset=".35" stop-color="${accent}" stop-opacity=".82"/><stop offset="1" stop-color="#1a3530" stop-opacity=".95"/></linearGradient>
            <filter id="vs-soft-glow"><feGaussianBlur stdDeviation="4"/></filter>
          </defs>
          <path class="vs-grid-line" d="M70 320H750M99 247H721M155 174H665M230 101H590"/>
          <path class="vs-grid-line" d="M185 344 330 71M325 362 397 52M496 362 423 52M635 344 490 71"/>
          <path class="vs-road-line" d="M76 342C235 300 613 300 744 342"/>
          <path class="vs-trail" d="M72 338C205 284 291 335 405 312S617 261 750 335"/>
          <path class="vs-flow-line" d="M122 110C213 80 289 91 363 122S524 167 687 117"/>
          <ellipse class="vs-car-shadow" cx="410" cy="278" rx="196" ry="49"/>
          <g transform="translate(${carShift} 0)">
            <path class="vs-car-silhouette" d="M202 246c17-21 38-42 74-48l54-50c28-24 143-28 182-6l49 49c47 5 86 24 102 51l-16 42-44 10-44-2-21-22H298l-25 25-52-6z"/>
            <path class="vs-cabin" d="m321 195 39-36c31-14 111-17 137-1l39 38-38 12-141-2z"/>
            <path class="vs-cabin" d="m370 161 3 42m80-47-5 48" opacity=".7"/>
            <path class="vs-axle" d="M276 263h260"/>
            <path class="vs-axle" d="M289 290h234"/>
            <path class="vs-hot-line" d="M421 151v110M535 198l42-63M309 201l-44-63"/>
            <rect class="vs-glow" x="217" y="240" width="18" height="8" rx="3"/>
            <rect class="vs-glow" x="642" y="241" width="18" height="8" rx="3"/>
            <circle class="vs-wheel" cx="287" cy="281" r="30"/><circle class="vs-rim" cx="287" cy="281" r="18"/><circle class="vs-brake" cx="287" cy="281" r="5"/>
            <circle class="vs-wheel" cx="535" cy="281" r="30"/><circle class="vs-rim" cx="535" cy="281" r="18"/><circle class="vs-brake" cx="535" cy="281" r="5"/>
            <circle class="vs-wheel" cx="335" cy="281" r="30" opacity=".72"/><circle class="vs-rim" cx="335" cy="281" r="18" opacity=".68"/>
            <circle class="vs-wheel" cx="584" cy="281" r="30" opacity=".72"/><circle class="vs-rim" cx="584" cy="281" r="18" opacity=".68"/>
          </g>
          <text x="109" y="363" fill="${accent}" font-family="inherit" font-size="10" letter-spacing="1.2">SIMULATION PATH / ${Math.round(speedPercent * 100)}% THROTTLE FIELD</text>
        </svg>
        <button class="vs-hotspot vs-hotspot--power ${state.tab === "powertrain" ? "is-active" : ""}" type="button" data-action="tab" data-tab="powertrain"><i></i>Powertrain</button>
        <button class="vs-hotspot vs-hotspot--chassis ${state.tab === "dynamics" ? "is-active" : ""}" type="button" data-action="tab" data-tab="dynamics"><i></i>Chassis solver</button>
        <button class="vs-hotspot vs-hotspot--tires ${state.tab === "tires" ? "is-active" : ""}" type="button" data-action="tab" data-tab="tires" style="--hotspot-colour:#c6a7ff"><i></i>4 tyre bodies</button>
        <button class="vs-hotspot vs-hotspot--render ${state.tab === "engine" ? "is-active" : ""}" type="button" data-action="tab" data-tab="engine" style="--hotspot-colour:#9bbcf8"><i></i>Frame graph</button>
        <div class="vs-stage-readout"><span>Target velocity</span><strong data-live="speed">${Math.round(state.speed)} km/h</strong><small data-live="stage-sub">${state.running ? "Physics tick stable" : "Preview is paused"}</small></div>
        <div class="vs-stage-bottomline"><span>Control input <b data-live="throttle">${Math.round(state.throttle)}%</b></span><span>Slip reserve <b data-live="slip">0.87</b></span></div>
      </section>`;
  }

  function renderMainBottom() {
    const vehicle = selectedVehicle();
    byId("vs-main-bottom").innerHTML = `
      <section class="vs-chart-card" aria-label="Live vehicle telemetry">
        <div class="vs-card-head"><h3>Live telemetry</h3><span class="vs-micro-label">${state.running ? "rolling 14 s" : "captured trace"}</span></div>
        <div class="vs-chart-legend"><span><i style="--legend-colour:${vehicle.accent}"></i>Vehicle speed</span><span><i style="--legend-colour:#f2bd74"></i>Engine speed</span><span><i style="--legend-colour:#9bbcf8"></i>Boost</span></div>
        <div id="vs-telemetry">${telemetrySVG()}</div>
        <div class="vs-chart-footer"><span>PhysicsThread · 240 Hz</span><strong data-live="frame">${vehicle.simulation.toFixed(2)} ms sim / ${vehicle.gpu.toFixed(1)} ms render</strong></div>
      </section>
      <section class="vs-editor-card" aria-label="Vehicle editor input controls">
        <div class="vs-card-head"><h3>Drive input</h3><span class="vs-micro-label">authorable</span></div>
        <div class="vs-controls-mini">
          ${rangeControl("Throttle", "throttle", 0, 100, 1, state.throttle, "%", vehicle.accent)}
          ${rangeControl("Brake", "brakeInput", 0, 100, 1, state.brakeInput, "%", "#f2bd74")}
          ${rangeControl("Steering", "steering", -35, 35, 1, state.steering, "°", "#9bbcf8")}
        </div>
        <div class="vs-mini-actions">
          <button class="vs-action-pill ${state.running ? "" : "is-primary"}" type="button" data-action="run">${state.running ? icons.pause : icons.play}<span>${state.running ? "Pause trace" : "Resume trace"}</span></button>
          <button class="vs-action-pill is-primary" type="button" data-action="save">Save tuning</button>
        </div>
      </section>`;
  }

  function renderInspector() {
    const vehicle = selectedVehicle();
    const title = inspectorTitle();
    byId("vs-inspector-top").innerHTML = `
      <div class="vs-inspector-title-row">
        <div><span class="vs-section-kicker">Vehicle inspector / ${vehicle.category}</span><h2>${title[0]}</h2></div>
        <button class="vs-icon-button" type="button" data-action="reset" title="Restore vehicle defaults">${icons.rotate}</button>
      </div>
      <p class="vs-inspector-description">${title[1]}</p>
      <div class="vs-save-state"><i class="vs-state-dot"></i><span>Changes apply to this local preview</span></div>
      <nav class="vs-inspector-tabs" aria-label="Vehicle inspector pages">${tabs.map(([id, label]) => `<button class="${id === state.tab ? "is-active" : ""}" type="button" data-action="tab" data-tab="${id}">${label}</button>`).join("")}</nav>
    `;

    const renderers = {
      overview: overviewPanel,
      dynamics: dynamicsPanel,
      powertrain: powertrainPanel,
      tires: tyresPanel,
      engine: enginePanel,
    };
    byId("vs-inspector-body").innerHTML = renderers[state.tab]();
  }

  function overviewPanel() {
    const vehicle = selectedVehicle();
    return `
      <div class="vs-summary-grid">
        ${metric("Mass", `${vehicle.mass}`, "kg incl. driver")}
        ${metric("Peak torque", `${vehicle.torque}`, "N·m")}
        ${metric("Peak power", `${vehicle.power}`, vehicle.powerUnit)}
        ${metric("Power / mass", `${Math.round((vehicle.power * 1000) / vehicle.mass)}`, "W/kg")}
        ${metric("0–100 km/h", `${vehicle.zero.toFixed(2)}`, "s")}
        ${metric("Top speed", `${vehicle.topSpeed}`, "km/h")}
      </div>
      <div class="vs-section-title-row"><h2>Vehicle balance</h2><span>${vehicle.balance} F / R</span></div>
      ${radarGraphic()}
      <div class="vs-section-title-row"><h2>Quick choices</h2><span>profile / aids</span></div>
      <div class="vs-setting-stack">
        ${selectField("Carcass formulation", "selectedTexture", state.selectedTexture, ["Set A · 3D FEM", "Set B · analytical", "Set C · XPBD hybrid"])}
        ${selectField("Induction", "mode", state.mode, ["Track", "Street", "Wet calibration", "Qualifying"])}
        ${toggleRow("ABS anti-lock braking", "abs")}
        ${toggleRow("Traction control", "traction")}
        ${toggleRow("Tyre thermal & gas-law pressure", "thermal")}
        ${toggleRow("Electronic stability assist", "stability")}
      </div>
      <section class="vs-info-card"><h3>Authored vehicle data</h3><p>${vehicle.drivetrain} · ${vehicle.engine}. The scene layer exposes drivetrain, tyre, suspension, frame and visual telemetry as authorable inspector records.</p></section>
    `;
  }

  function dynamicsPanel() {
    const vehicle = selectedVehicle();
    return `
      <div class="vs-summary-grid">
        ${metric("Ride frequency", state.suspension.toFixed(2), "Hz")}
        ${metric("Damping", state.damping, "% critical")}
        ${metric("Aero balance", state.aero, "% front")}
        ${metric("Peak lateral", vehicle.zeroG.toFixed(2), "g preview")}
      </div>
      <div class="vs-section-title-row"><h2>Chassis solver</h2><span>Spring / XPBD</span></div>
      ${chassisGraphic()}
      <div class="vs-setting-stack">
        ${rangeControl("Natural frequency", "suspension", 1.2, 4.0, .05, state.suspension, " Hz", vehicle.accent)}
        ${rangeControl("Damping ratio", "damping", 25, 95, 1, state.damping, "%", "#9bbcf8")}
        ${rangeControl("Front aero balance", "aero", 20, 70, 1, state.aero, "%", "#f2bd74")}
        ${rangeControl("Centre torque split", "torqueSplit", 0, 100, 1, state.torqueSplit, "% rear", "#c6a7ff")}
        ${toggleRow("Dedicated fixed-rate PhysicsThread", "fixedStep")}
      </div>
      <section class="vs-thread-card"><div class="vs-thread-head"><h3>Dynamics budget</h3><span>stable</span></div><div class="vs-thread-grid"><div><small>Body solve</small><strong>0.46 ms</strong></div><div><small>Tyre solve</small><strong>1.12 ms</strong></div><div><small>Contacts</small><strong>0.39 ms</strong></div></div></section>
    `;
  }

  function powertrainPanel() {
    const vehicle = selectedVehicle();
    return `
      <div class="vs-summary-grid">
        ${metric("Engine speed", Math.round(state.rpm), "rpm")}
        ${metric("Boost", state.boost.toFixed(2), "bar")}
        ${metric("Wheel torque", Math.round(vehicle.torque * (0.56 + state.throttle / 230)), "N·m")}
        ${metric("Drive mode", state.mode, "local profile")}
      </div>
      <div class="vs-section-title-row"><h2>Power delivery</h2><span>0–7200 rpm</span></div>
      ${powerCurveGraphic()}
      <div class="vs-setting-stack">
        ${selectField("Power mode", "mode", state.mode, ["Track", "Street", "Wet calibration", "Qualifying"])}
        ${rangeControl("Torque split", "torqueSplit", 0, 100, 1, state.torqueSplit, "% rear", vehicle.accent)}
        ${rangeControl("Lift-off regeneration", "regen", 0, 40, 1, state.regen, "%", "#9bbcf8")}
        ${toggleRow("Turbo / supercharger thermal model", "thermal")}
        ${toggleRow("Motion-vector driveline output", "motionVectors")}
      </div>
      <section class="vs-info-card"><h3>Curve package</h3><p>Wide-open throttle curve uses a base torque trace, boost compensation and delivered wheel power. Edit values remain local to this browser session.</p></section>
    `;
  }

  function tyresPanel() {
    const vehicle = selectedVehicle();
    const corners = [
      ["FL", "Front left", 1.0, 76],
      ["FR", "Front right", .98, 74],
      ["RL", "Rear left", 1.08, 81],
      ["RR", "Rear right", 1.05, 79],
    ];
    return `
      <div class="vs-summary-grid">
        ${metric("Cold pressure", Math.round(state.tyrePressure), "kPa")}
        ${metric("Carcass state", state.settings.tyreFEM ? "3D FEM" : "MF6.1", "active model")}
        ${metric("Thermal window", "68–96", "°C")}
        ${metric("Wear multiplier", "×1.00", "per lap")}
      </div>
      <div class="vs-section-title-row"><h2>Corner telemetry</h2><span>${state.wheel} selected</span></div>
      <div class="vs-wheel-grid">${corners.map(([id, label, grip, temperature]) => `
        <button class="vs-wheel-card ${state.wheel === id ? "is-active" : ""}" type="button" data-action="wheel" data-wheel="${id}"><span class="vs-wheel-indicator"></span><b>${label}</b><span>Grip ×${grip.toFixed(2)} · ${temperature}°C</span><small>${id === state.wheel ? "editing" : `${Math.round(state.tyrePressure + (id[0] === "R" ? 3 : 0))} kPa`}</small></button>`).join("")}</div>
      <div class="vs-setting-stack">
        ${rangeControl("Cold pressure", "tyrePressure", 85, 165, 1, state.tyrePressure, " kPa", "#c6a7ff")}
        ${selectField("Compound", "compound", state.compound, ["Set A · Medium", "Set B · Soft", "Set C · Wet"])}
        ${toggleRow("3D FEM carcass deformation", "tyreFEM")}
        ${toggleRow("Tyre telemetry recorder", "telemetry")}
        ${toggleRow("Thermal grip degradation", "thermal")}
      </div>
      <section class="vs-info-card"><h3>${state.wheel} contact patch</h3><p>Pressure, carcass temperature, slip, deflection and contact load are presented per corner. The preview visualises authoring data only; it is not a connected native simulation.</p></section>
    `;
  }

  function enginePanel() {
    const vehicle = selectedVehicle();
    const passes = [
      ["Visibility + skinning", "1.48", 65, vehicle.accent],
      ["Vehicle raster", "0.86", 39, "#9bbcf8"],
      ["ReSTIR GI resolve", "2.12", 92, "#79e1b7"],
      ["Volumetric tyre FX", "0.71", 28, "#c6a7ff"],
      ["TAA / display", "0.63", 24, "#f2bd74"],
    ];
    return `
      <div class="vs-summary-grid">
        ${metric("Frame time", `${(vehicle.gpu + 2.3).toFixed(1)}`, "ms local estimate")}
        ${metric("Render scale", state.renderScale, "%")}
        ${metric("Physics cadence", "240", "Hz")}
        ${metric("Draw instances", "1 284", "visible")}
      </div>
      <section class="vs-engine-card"><h3>Frame graph</h3><p class="vs-engine-subtitle">Vehicle-linked passes · local instrumentation</p><div class="vs-pipeline-list">${passes.map(([name, duration, size, colour]) => `<div class="vs-pipeline-row"><strong>${name}</strong><span class="vs-bar"><i style="--bar-size:${size}%;--bar-colour:${colour}"></i></span><em>${duration} ms</em></div>`).join("")}</div></section>
      <div class="vs-section-title-row"><h2>Runtime controls</h2><span>editor local</span></div>
      <div class="vs-setting-stack">
        ${rangeControl("Render scale", "renderScale", 50, 125, 1, state.renderScale, "%", "#9bbcf8")}
        ${selectField("Quality tier", "quality", state.quality, ["High", "Epic", "Cinematic", "Diagnostic"])}
        ${toggleRow("ReSTIR direct + GI reservoirs", "restir")}
        ${toggleRow("GPU instance culling", "gpuCulling")}
        ${toggleRow("Motion vectors", "motionVectors")}
      </div>
      <section class="vs-thread-card"><div class="vs-thread-head"><h3>Engine threads</h3><span>6 online</span></div><div class="vs-thread-grid"><div><small>Game</small><strong>1.36 ms</strong></div><div><small>Physics</small><strong>${vehicle.simulation.toFixed(2)} ms</strong></div><div><small>Render</small><strong>${vehicle.gpu.toFixed(1)} ms</strong></div></div></section>
      <section class="vs-info-card"><h3>Instrumentation scope</h3><p>Frame timings, visibility and system labels give this static Project-Zero exhibit useful engine context without reporting a native runtime connection.</p></section>
    `;
  }

  function renderFooter() {
    const vehicle = selectedVehicle();
    byId("vs-footer").innerHTML = `
      <span>SCENE <b>Proving Ground / Local</b></span>
      <span>SELECTED <b>${vehicle.short}</b></span>
      <span>PHYSICS <b>240 Hz</b></span>
      <span>RENDER <b data-live="footer-render">${vehicle.gpu.toFixed(1)} ms</b></span>
      <span class="vs-footer-spacer"></span>
      <span>${nowTime()} UTC</span>
      <span>PREVIEW <b>EDITING</b></span>
    `;
  }

  function metric(label, value, unit) {
    return `<div class="vs-metric-card"><span class="vs-micro-label">${label}</span><strong>${value}</strong><small>${unit}</small></div>`;
  }

  function toggleRow(label, key) {
    return `<div class="vs-setting"><span>${label}</span><button class="vs-toggle ${state.settings[key] ? "is-on" : ""}" type="button" role="switch" aria-checked="${state.settings[key] ? "true" : "false"}" data-action="toggle" data-toggle="${key}"><span class="sr-only">${label}</span></button></div>`;
  }

  function selectField(label, key, value, options) {
    return `<div class="vs-form-field"><label><span>${label}</span><strong>${key === "quality" ? "renderer" : "profile"}</strong></label><select data-select="${key}">${options.map((option) => `<option ${option === value ? "selected" : ""}>${option}</option>`).join("")}</select></div>`;
  }

  function rangeControl(label, key, min, max, step, value, unit, colour) {
    const progress = ((Number(value) - Number(min)) / (Number(max) - Number(min))) * 100;
    const display = rangeDisplay(key, value, unit);
    return `<div class="vs-mini-control"><label><span>${label}</span><b data-display="${key}">${display}</b></label><input class="vs-range" type="range" min="${min}" max="${max}" step="${step}" value="${value}" data-range="${key}" style="--range-progress:${progress}%;--range-colour:${colour}"></div>`;
  }

  function rangeDisplay(key, value, unit) {
    if (key === "suspension") return `${Number(value).toFixed(2)}${unit}`;
    if (key === "steering") return `${Number(value) > 0 ? "+" : ""}${Math.round(value)}${unit}`;
    if (key === "torqueSplit") return `${Math.round(value)}${unit}`;
    return `${Math.round(value)}${unit}`;
  }

  function inspectorTitle() {
    const vehicle = selectedVehicle();
    const titles = {
      overview: [vehicle.name, vehicle.description],
      dynamics: ["Chassis & dynamics", "Suspension calibration, aero balance, contact budget and the fixed-rate vehicle solver."],
      powertrain: ["Powertrain editor", "Author the delivered torque curve, drive split, energy recovery and thermal response envelope."],
      tires: ["Tyre inspector", "Inspect each compound, carcass, pressure and thermal window independently at the four contact patches."],
      engine: ["Engine instrumentation", "A compact frame graph and thread budget for vehicle-facing game engine authoring data."],
    };
    return titles[state.tab];
  }

  function tabLabel(tab) {
    return (tabs.find(([id]) => id === tab) || [tab, tab])[1];
  }

  function linePoints(key, max, width = 440, height = 104) {
    const values = state.samples.map((sample) => sample[key]);
    const padX = 6;
    const padY = 8;
    return values.map((value, index) => {
      const x = padX + (index / Math.max(values.length - 1, 1)) * (width - padX * 2);
      const y = height - padY - (value / max) * (height - padY * 2);
      return `${x.toFixed(1)},${clamp(y, 5, height - 5).toFixed(1)}`;
    }).join(" ");
  }

  function telemetrySVG() {
    const vehicle = selectedVehicle();
    const speedLine = linePoints("speed", vehicle.topSpeed, 440, 105);
    const rpmLine = linePoints("rpm", 7200, 440, 105);
    const last = state.samples[state.samples.length - 1] || { speed: 0 };
    const lastY = clamp(105 - 8 - (last.speed / vehicle.topSpeed) * 89, 5, 100);
    return `<svg class="vs-telemetry-chart" viewBox="0 0 440 115" preserveAspectRatio="none" aria-label="Rolling telemetry graph"><defs><linearGradient id="vs-chart-fill" x1="0" y1="0" x2="0" y2="1"><stop stop-color="${vehicle.accent}" stop-opacity=".45"/><stop offset="1" stop-color="${vehicle.accent}" stop-opacity="0"/></linearGradient></defs><path class="chart-grid" d="M4 16H436M4 44H436M4 72H436M4 100H436"/><path class="chart-area" d="M${speedLine.split(" ").join(" L")} L434,106 L6,106Z"/><polyline class="chart-speed" points="${speedLine}"/><polyline class="chart-rpm" points="${rpmLine}"/><circle class="chart-dot" cx="434" cy="${lastY.toFixed(1)}" r="3.4"/><text x="5" y="113" fill="#65706b" font-family="inherit" font-size="8">−14 s</text><text x="406" y="113" fill="#65706b" font-family="inherit" font-size="8">now</text></svg>`;
  }

  function radarGraphic() {
    const vehicle = selectedVehicle();
    const handling = clamp(52 + state.suspension * 11 - Math.abs(state.steering) * .3, 20, 94);
    const acceleration = clamp(44 + state.throttle * .5 + vehicle.power / 18, 20, 96);
    const thermal = state.settings.thermal ? 82 : 48;
    const consistency = state.settings.traction ? 86 : 64;
    const values = [handling, acceleration, thermal, consistency, clamp(75 + state.aero * .2, 20, 96)];
    const points = values.map((value, index) => polarPoint(110, 78, 46 * value / 100, -90 + index * 72)).join(" ");
    const nodes = values.map((value, index) => { const [x, y] = polarPoint(110, 78, 46 * value / 100, -90 + index * 72).split(","); return `<circle class="radar-node" cx="${x}" cy="${y}" r="3"/>`; }).join("");
    const labels = ["Handling", "Power", "Thermal", "Consistency", "Aero"].map((label, index) => { const [x, y] = polarPoint(110, 78, 65, -90 + index * 72).split(","); return `<text class="ring-label" x="${x}" y="${y}" text-anchor="middle">${label.toUpperCase()}</text>`; }).join("");
    return `<div class="vs-infographic"><svg viewBox="0 0 220 158" aria-label="Vehicle balance radar"><circle class="ring" cx="110" cy="78" r="20"/><circle class="ring" cx="110" cy="78" r="35"/><circle class="ring" cx="110" cy="78" r="50"/><path class="cross" d="M110 21v114M56 42l108 72M164 42 56 114"/><polygon class="radar-fill" points="${points}"/>${nodes}${labels}<path class="ring-strong" d="M76 119 A54 54 0 0 0 146 121"/></svg></div>`;
  }

  function chassisGraphic() {
    const vertical = clamp((state.suspension - 1.2) / 2.8, 0, 1);
    const front = 53 + (state.aero - 45) * .46;
    const rear = 53 - (state.aero - 45) * .46;
    return `<div class="vs-infographic"><svg viewBox="0 0 220 158" aria-label="Chassis suspension infographic"><path class="cross" d="M24 118H196M32 41H188M62 27v106M158 27v106"/><path class="ring" d="M44 113h132l-19-56H63Z"/><path class="ring-strong" d="M48 ${106 - vertical * 36} 110 ${78 - vertical * 14} 172 ${106 - vertical * 34}"/><circle class="radar-node" cx="48" cy="${106 - vertical * 36}" r="4"/><circle class="radar-node" cx="172" cy="${106 - vertical * 34}" r="4"/><text class="ring-label" x="33" y="145">F ${Math.round(front)}%</text><text class="ring-label" x="155" y="145">R ${Math.round(rear)}%</text><text class="ring-label" x="110" y="18" text-anchor="middle">RIDE ${state.suspension.toFixed(2)} HZ</text></svg></div>`;
  }

  function polarPoint(cx, cy, radius, degrees) {
    const radians = degrees * Math.PI / 180;
    return `${(cx + Math.cos(radians) * radius).toFixed(1)},${(cy + Math.sin(radians) * radius).toFixed(1)}`;
  }

  function powerCurveGraphic() {
    const vehicle = selectedVehicle();
    const torquePeak = clamp(vehicle.torque / 8.5, 60, 100);
    const powerPeak = clamp(vehicle.power / 5.3, 60, 100);
    const boostTop = clamp(state.boost * 77, 30, 98);
    return `<svg class="vs-power-curve" viewBox="0 0 300 150" preserveAspectRatio="none" aria-label="Power and torque curve"><path class="grid" d="M13 16H290M13 45H290M13 74H290M13 103H290M13 132H290M13 16V132"/><path class="torque" d="M14 118 C50 102 67 56 112 42 S177 ${128 - torquePeak * .54} 212 53 S259 73 290 80"/><path class="power" d="M14 128 C54 126 87 106 120 78 S180 31 220 ${130 - powerPeak} S268 58 290 69"/><path class="boost" d="M14 128 C59 126 71 96 107 89 S167 ${126 - boostTop * .58} 205 62 S267 51 290 57"/><text x="14" y="145">0</text><text x="80" y="145">2400</text><text x="147" y="145">4800</text><text x="260" y="145">7200 rpm</text><text x="14" y="12">delivered curve</text></svg>`;
  }

  function bindEvents() {
    document.addEventListener("click", (event) => {
      const target = event.target.closest ? event.target.closest("[data-action]") : null;
      if (!target) return;
      const action = target.dataset.action;
      if (action === "close") closeStudio();
      if (action === "open") openStudio();
      if (action === "vehicle") selectVehicle(target.dataset.vehicle);
      if (action === "tab") selectTab(target.dataset.tab);
      if (action === "toggle") toggle(target.dataset.toggle);
      if (action === "wheel") { state.wheel = target.dataset.wheel; renderInspector(); }
      if (action === "run") { state.running = !state.running; refreshRunningState(); toastMessage(state.running ? "Telemetry trace resumed" : "Telemetry trace paused"); }
      if (action === "reset") resetTuning();
      if (action === "save") saveTuning();
      if (action === "view") cycleView();
      if (action === "focus") toastMessage("Camera framed on authored vehicle bounds");
    });

    document.addEventListener("input", (event) => {
      const control = event.target.closest ? event.target.closest("[data-range]") : null;
      if (!control) return;
      const key = control.dataset.range;
      state[key] = Number(control.value);
      synchroniseRanges(key);
      syncLiveValues();
    });

    document.addEventListener("change", (event) => {
      const select = event.target.closest ? event.target.closest("[data-select]") : null;
      if (!select) return;
      state[select.dataset.select] = select.value;
      renderInspector();
      toastMessage(`${select.dataset.select === "quality" ? "Quality tier" : "Vehicle profile"} updated`);
    });

    document.addEventListener("keydown", (event) => {
      if ((event.ctrlKey || event.metaKey) && event.shiftKey && event.key.toLowerCase() === "v") {
        event.preventDefault();
        studio.hidden ? openStudio() : closeStudio();
      }
    });
  }

  function selectVehicle(id) {
    if (!vehicles[id] || id === state.vehicle) return;
    state.vehicle = id;
    const vehicle = selectedVehicle();
    state.suspension = vehicle.suspension;
    state.aero = vehicle.aero;
    state.tyrePressure = id === "rally" ? 148 : id === "endurance" ? 118 : 110;
    state.speed = clamp(state.speed, 0, vehicle.topSpeed);
    seedSamples();
    renderAll();
    toastMessage(`${vehicle.name} loaded into Vehicle Lab`);
  }

  function selectTab(tab) {
    if (!tabs.some(([id]) => id === tab)) return;
    state.tab = tab;
    renderRail();
    renderContext();
    renderStage();
    renderInspector();
    document.querySelectorAll(".vs-top-nav button").forEach((button) => button.classList.toggle("is-active", button.dataset.tab === tab));
  }

  function toggle(key) {
    if (!(key in state.settings)) return;
    state.settings[key] = !state.settings[key];
    renderInspector();
    toastMessage(`${formatSetting(key)} ${state.settings[key] ? "enabled" : "disabled"}`);
  }

  function formatSetting(key) {
    return {
      abs: "ABS",
      traction: "Traction control",
      thermal: "Thermal model",
      stability: "Stability assist",
      tyreFEM: "3D FEM carcass",
      fixedStep: "Fixed-rate physics",
      restir: "ReSTIR GI",
      gpuCulling: "GPU culling",
      motionVectors: "Motion vectors",
      telemetry: "Telemetry recorder",
    }[key] || key;
  }

  function synchroniseRanges(key) {
    document.querySelectorAll(`[data-range="${key}"]`).forEach((range) => {
      const min = Number(range.min);
      const max = Number(range.max);
      range.value = state[key];
      range.style.setProperty("--range-progress", `${clamp((state[key] - min) / (max - min) * 100, 0, 100)}%`);
    });
    document.querySelectorAll(`[data-display="${key}"]`).forEach((display) => {
      const source = document.querySelector(`[data-range="${key}"]`);
      const unit = source && source.closest(".vs-mini-control") ? source.closest(".vs-mini-control").querySelector("label b")?.textContent.replace(/[\d.+\-]/g, "") : "";
      const hardcoded = { throttle: "%", brakeInput: "%", steering: "°", suspension: " Hz", damping: "%", aero: "%", torqueSplit: "% rear", tyrePressure: " kPa", regen: "%", renderScale: "%" }[key] || unit;
      display.textContent = rangeDisplay(key, state[key], hardcoded);
    });
  }

  function tick() {
    if (!studio || studio.hidden || !state.running) return;
    const vehicle = selectedVehicle();
    const drive = state.throttle / 100;
    const braking = state.brakeInput / 100;
    const terrainDrag = state.vehicle === "rally" ? .22 : .18;
    const acceleration = drive * (3.4 + vehicle.power / 285) - braking * 9.4 - terrainDrag - state.speed * .009;
    state.speed = clamp(state.speed + acceleration * .42, 0, vehicle.topSpeed);
    state.rpm = clamp(1100 + state.speed * 24.5 + drive * 1390 + Math.sin(Date.now() / 530) * 120, 850, 7200);
    state.boost = clamp(drive * .96 + Math.sin(Date.now() / 730) * .045 + (state.mode === "Qualifying" ? .12 : 0), 0, 1.2);
    state.gForce = clamp(Math.abs(acceleration) * .17 + Math.abs(state.steering) * .012, .04, 1.48);
    state.samples.push({ speed: state.speed, rpm: state.rpm, boost: state.boost });
    if (state.samples.length > 34) state.samples.shift();
    syncLiveValues();
  }

  function syncLiveValues() {
    const vehicle = selectedVehicle();
    const slip = clamp(0.97 - Math.abs(state.steering) * .004 - state.brakeInput * .0014 + (state.settings.traction ? .04 : 0), .61, .99);
    const values = {
      speed: `${Math.round(state.speed)} km/h`,
      throttle: `${Math.round(state.throttle)}%`,
      slip: slip.toFixed(2),
      frame: `${vehicle.simulation.toFixed(2)} ms sim / ${vehicle.gpu.toFixed(1)} ms render`,
      "stage-sub": state.running ? `${Math.round(state.gForce * 100) / 100} g · physics tick stable` : "Preview is paused",
      "footer-render": `${vehicle.gpu.toFixed(1)} ms`,
    };
    Object.entries(values).forEach(([key, value]) => document.querySelectorAll(`[data-live="${key}"]`).forEach((element) => { element.textContent = value; }));
    const telemetry = byId("vs-telemetry");
    if (telemetry) telemetry.innerHTML = telemetrySVG();
  }

  function refreshRunningState() {
    const chip = byId("vs-live-chip");
    if (chip) {
      chip.classList.toggle("is-paused", !state.running);
      chip.querySelector("span").textContent = state.running ? "LIVE LOCAL PREVIEW" : "TRACE PAUSED";
    }
    renderRail();
    renderMainBottom();
    syncLiveValues();
  }

  function resetTuning() {
    const vehicle = selectedVehicle();
    state.throttle = 68;
    state.brakeInput = 0;
    state.steering = -11;
    state.suspension = vehicle.suspension;
    state.damping = 62;
    state.aero = vehicle.aero;
    state.torqueSplit = 52;
    state.tyrePressure = state.vehicle === "rally" ? 148 : state.vehicle === "endurance" ? 118 : 110;
    state.regen = 18;
    state.renderScale = 92;
    state.mode = "Track";
    state.quality = "Epic";
    state.carcass = "Set A · 3D FEM";
    state.compound = "Set A · Medium";
    state.settings = { abs: true, traction: true, thermal: true, stability: false, tyreFEM: true, fixedStep: true, restir: true, gpuCulling: true, motionVectors: true, telemetry: true };
    seedSamples();
    renderAll();
    toastMessage("Vehicle tuning restored to authored baseline");
  }

  function saveTuning() {
    const payload = {
      vehicle: state.vehicle,
      tab: state.tab,
      tuning: {
        suspension: state.suspension,
        damping: state.damping,
        aero: state.aero,
        torqueSplit: state.torqueSplit,
        tyrePressure: state.tyrePressure,
        regen: state.regen,
        renderScale: state.renderScale,
        mode: state.mode,
        quality: state.quality,
        settings: state.settings,
      },
      savedAt: new Date().toISOString(),
    };
    try { localStorage.setItem("Frontier.ProjectZero.VehicleLab.v1", JSON.stringify(payload)); } catch (_) { /* private-mode storage can be unavailable. */ }
    toastMessage("Tuning checkpoint saved locally");
  }

  function cycleView() {
    const views = ["Isometric", "Front profile", "Chassis", "Contact patches"];
    state.view = views[(views.indexOf(state.view) + 1) % views.length];
    renderContext();
    renderStage();
    toastMessage(`${state.view} camera selected`);
  }

  function openStudio() {
    studio.hidden = false;
    launcher.classList.remove("is-visible");
    syncLiveValues();
  }

  function closeStudio() {
    studio.hidden = true;
    launcher.classList.add("is-visible");
  }

  function toastMessage(message) {
    if (!toast) return;
    const text = toast.querySelector("span");
    text.textContent = message;
    toast.classList.add("is-visible");
    window.clearTimeout(toastTimer);
    toastTimer = window.setTimeout(() => toast.classList.remove("is-visible"), 2600);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot, { once: true });
  else boot();
})();
