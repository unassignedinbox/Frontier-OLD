# Project-Zero Vehicle Lab

This directory recovers the self-contained **Project-Zero Editor** browser exhibit from the repository history and layers a browser-local **Vehicle Lab** over it.

## Preview

Serve this directory with any static server:

```sh
python3 -m http.server 4173 --bind 0.0.0.0
```

Open the served root. The Vehicle Lab opens by default; use **Scene** in its top-right corner to return to the underlying Project-Zero exhibit. The compact **Vehicle Lab** launcher brings it back. `Ctrl`/`Cmd` + `Shift` + `V` toggles the lab as well.

## What is interactive

- Choose among three local vehicle authoring profiles from the left rail.
- Select **Overview**, **Dynamics**, **Powertrain**, **Tyres**, or **Engine** from either the rail, stage callouts, or inspector tabs.
- Adjust input, chassis, powertrain, tyre, and renderer controls. Sliders update the vehicle telemetry trace and dashboard readouts.
- Pause/resume the trace, select each tyre contact patch, toggle subsystems, reset defaults, or save a browser-local tuning checkpoint.
- The **Engine** page presents an explicitly labelled local instrumentation budget: frame graph, physics cadence, render scale, renderer options, and thread timing context.

The graphics and telemetry are an interactive static preview, not a connection to a native engine process. Vehicle facts used for the Control vehicle surface are drawn from the recovered Frontier vehicle prototypes and inspector captures; the other profiles are clearly local preview profiles.

## Files

- `index.html` — original standalone Project-Zero bundle with two asset references injected.
- `vehicle-studio.css` — scoped visual system and responsive layout.
- `vehicle-studio.js` — state, interaction handling, SVG infographics, controls, local persistence, and live telemetry simulation.
