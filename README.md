# FRONTIER

A flow-racing prototype in the spirit of *Tachyon Flow* — built specifically to
**not** be a lane runner and **not** be a track racer.

```
npm install
npm run dev        # playable build at http://localhost:5173
npm test           # 52 headless tests against the real sim
npm run build      # production bundle
node tools/simrun.js [seed] [seconds]   # headless run harness / balance check
```

---

## The brief, and how this answers it

| Brief | What that rules out | What this does instead |
|---|---|---|
| "not a fully straight runner" | Constant forward velocity, obstacles arriving on rails | Velocity is a free 3-D vector. You can brake, stall, reverse your line, loop back. |
| "ability to steer (not like runner games)" | 3-lane snapping, tap-left/tap-right | Analogue yaw + pitch with inertia and tyre-style grip. The nose turns instantly; the mass takes ~0.2 s to follow. |
| "not a track racing game, I don't want a track" | Closed circuit, laps, tarmac, racing line painted on a road | A one-shot point-to-point **run** through a generated canyon. No road surface, no lap counter, no walls. |

### The three load-bearing decisions

**1. There is no track. There is a corridor.**
The course generator lays a spline through open space, carves a canyon around it,
and then hangs a chain of rings through the sky. The canyon is ~230 m wide and
open above. Nothing paints a road, nothing fences you in. The only structure that
defines the route is the gate chain — and it moves **laterally and vertically**,
so a correct line is a 3-D curve you have to fly, not a groove you fall into.

**2. Steering is a physical model, not a lane index.**
`Vehicle` keeps a velocity vector separate from the heading it is pointing in.
Grip scrubs the lateral component off over time. That gap *is* the drift:

- flick the stick at 460 km/h and the nose comes round on frame 1 while the mass
  keeps going straight for a fifth of a second;
- hold `SHIFT` and grip drops from 5.2 to 2.6, giving a ~38° slip angle — a
  committed slide you can steer through and get paid flow for.

A lane runner cannot produce this. It has no lateral velocity to speak of.

**3. A run replaces a lap.**
Start line, four sector splits, finish line, ~13 km, about 100 seconds. Standings
come from distance travelled along the corridor, so there are real positions and
real overtakes against three rivals — without ever being a circuit. Every corridor
is generated from a seed held in the URL (`#seed=…`), so a run is reproducible and
shareable.

---

## Controls

| Key | Action |
|---|---|
| `A` / `D` | steer (analogue) |
| `Q` / `E` | altitude — climb / dive |
| `S` | air brake |
| `SPACE` | boost |
| `SHIFT` | drift — trades grip for turn-in, earns flow |
| mouse | fine steering (click to capture) |
| `C` / `R` | camera / restart |
| `M` / `ESC` | mute / pause |
| gamepad | left stick steer, `RT` boost, `LT` drift |

Throttle is automatic. This is a game about the line you fly, not about throttle
management.

---

## The loop

- **Gates** are the route and the score. Passing one through the centre pays more
  flow and boost than clipping the rim. Passing outside it costs the chain — but
  the ring stays open and you can turn back through it. A miss costs time, never
  the run.
- **Flow** is the risk meter. Gates feed it, sustained drifts feed it, and
  *skimming* — passing close to corridor structure without touching it — feeds it
  hardest. That last one is the mechanic that most separates this from a runner:
  the fast line is also the scary one.
- **Tachyon Surge** fires at full flow: raised speed ceiling, dilated world, the
  rivals fall into slow motion while you do not.
- **Rivals** (VANTA / KESTREL / ORPHEUS) run the *same* flight model under a
  pursuit controller, with distinct skill, aggression and boost discipline. They
  drift and overshoot like you do, so an overtake reads as an overtake.
- **Your ghost** is the fourth opponent and the one that matters. Finish a run and
  it is recorded; beat it and the recording becomes the new ghost for that seed.
  It flies translucent alongside you, appears on the tactical map, and the HUD
  shows a live delta compared at **equal progress**, not equal clock — the only
  comparison that means anything on a point-to-point run. Because corridors are
  seeded, a ghost is exactly comparable run to run, and `#seed=…` makes "beat this"
  a link you can send someone.

---

## Architecture

```
src/
  config.js            every tuning number, in one place
  core/
    mathx.js           scalar math + seeded RNG + value noise (no deps)
    input.js           keyboard / mouse / gamepad -> one normalized sample
    audio.js           procedural WebAudio engine, no sample assets
  world/
    course.js          procedural corridor: spline, terrain, gates, structure
  sim/
    vehicle.js         hover flight model (heading vs velocity, grip, drift)
    ai.js              rival pursuit brain
    race.js            run state machine, scoring, collision, standings
    ghost.js           run recording, replay, per-seed best-time store
  render/
    scene.js           sky, canyon mesh, gate rings, structure, speed streaks
    craft.js           hull, engine bloom, ground cushion, ribbon trail
  ui/
    hud.js  hud.css    instrument cluster + tactical minimap
    overlay.js         title / pause / results
tests/                 43 headless tests against the real modules
tools/simrun.js        headless run harness
```

The split is deliberate: `core/`, `world/` and `sim/` never touch the DOM or
WebGL. They are pure ES modules importing only `three` for its math types, which
is why the whole game can be simulated — and unit-tested — headlessly:

```
$ node tools/simrun.js 424242 200
seed 424242 · 34 gates · 13.24 km · 244 structures · gen 19 ms
finished   : true in 100.32s
gates      : 34/34  (missed 0)
top speed  : 616 km/h
impacts    : 9   skims: 15   surges: 15
```

That harness is how you balance the game without opening a browser, and it is how
the test suite proves a seed is actually flyable.

---

## Tuning

Everything with a feel to it is a number in `src/config.js`:

| Want to change | Knob |
|---|---|
| How loose the craft is | `craft.grip` (5.2), `craft.gripDrift` (2.6) |
| Turn-in at speed | `craft.yawRate`, `craft.yawRateHighSpeed` |
| Acceleration curve | `craft.thrust` (26 m/s² → ~6 s to top speed) |
| How forgiving rings are | `course.gateRadiusMin/Max` (24–40 m) |
| How hard the route weaves | `course.wander`, `course.climb`, `course.gateLateral`, `course.gateVertical` |
| Run length | `course.nodes` × `course.nodeSpacing` |
| Rival difficulty | `rivals[].skill`, `.aggression`, `.boostUse` |
| Surge payoff | `flow.surgeDuration`, `flow.surgeTimeDilation` |
| How far you can see | `scene.js` fog density (0.00055) — a gameplay value, not just atmosphere |

The sim runs on a fixed 120 Hz timestep with an accumulator, so these numbers mean
the same thing on a 60 Hz laptop and a 165 Hz monitor.

See [`docs/DESIGN.md`](docs/DESIGN.md) for the design rationale and the
production/AAA conversion plan.
