# FRONTIER — Design Document

Working prototype for a high-speed flow racer in the spirit of *Tachyon Flow*,
built against three hard constraints from the brief:

1. **Not a straight runner.**
2. **Real steering, not lane switching.**
3. **No track.** Not a circuit, not a road, not a groove.

---

## 1. Pillars

### Pillar 1 — The route is a line through space, not a surface

The usual way to make a "fast game" is a track: a ribbon of tarmac with a racing
line, walls, and a lap counter. That is exactly what this is not.

Here the generator lays a spline through open space and carves a canyon around it.
The canyon floor sits ~60 m below the spline, the shoulders rise into walls about
120 m off the centreline, and above that it is open sky. **Nothing paints a road.**
There is no tarmac, no lane markings, no barriers.

The only thing that defines the route is a chain of rings hung through the air.
Critically, the rings move on **both** axes:

```js
// src/world/course.js
const lat  = n.fbm1(i * 0.21 + 7, 3) * gateLateral * halfWidth;  // ±74 m side to side
const vert = n.fbm1(i * 0.17 + 55, 3) * gateVertical;            // ±46 m up and down
```

A lane runner moves you between discrete x-positions on a flat plane. This asks you
to fly a 3-D curve. The test suite asserts this is actually true of every generated
corridor — if a future tuning pass flattened the gates, `gates force real 3-D
steering` would fail:

```
✔ gates force real 3-D steering: they move laterally AND vertically
✔ course is a point-to-point run, not a circuit
```

### Pillar 2 — Steering is a physical model

The single most important line in the codebase is the grip step in
`src/sim/vehicle.js`:

```js
this.forwardSpeed = this.velocity.dot(this.heading);
this._lat.copy(this.velocity).addScaledVector(this.heading, -this.forwardSpeed);
this._lat.multiplyScalar(Math.exp(-grip * dt));
this.velocity.copy(this.heading).multiplyScalar(this.forwardSpeed).add(this._lat);
```

Velocity is decomposed into the part pointing where the nose points and the part
that isn't. Only the second part is scrubbed, and only exponentially — never
instantly. Consequences:

- **Inertia is real.** Yaw the nose and the mass keeps its old direction for a
  fifth of a second. You must plan the arc, or brake, or commit to a slide.
- **Steady-state slip is speed-dependent.** Lateral slip settles at
  `v·ω / grip`, so at 460 km/h full lock holds a ~12° slip angle on grip and a
  ~38° angle in drift. The craft behaves differently at every speed.
- **Drift is a gear, not a button effect.** `SHIFT` drops grip from 5.2 to 2.6 and
  raises yaw authority 1.4×. You trade stability for turn-in and get paid flow for
  holding the slide.

A runner has none of this because it has no lateral velocity. There is a test for
that too:

```
✔ the mass keeps going straight after the nose turns (this is the drift)
✔ steering rotates the heading continuously, not in discrete lanes   (180 frames of half-lock -> >170 distinct headings)
```

### Pillar 3 — A run replaces a lap

No laps means no circuit, but "race" still has to mean something. So:

- **Point to point.** Start line, four sector splits, finish line. ~13 km, ~100 s.
- **Standings from arc length.** Position is distance travelled along the corridor,
  so there are four real places and real overtakes — no rubber-banding.
- **Rivals share the player's model.** `AiDriver` produces a control sample and
  feeds the *same* `Vehicle.update`. Rivals drift, overshoot and scrub speed, so
  when VANTA comes past you it is because she flew a better line, not because a
  script said so.
- **Seeded and shareable.** One integer determines the whole corridor. It lives in
  the URL (`#seed=…`), so "beat this run" is a link.

---

## 2. The scoring layer: why skimming matters

A racer with only gates becomes a game of threading hoops. The second resource is
what gives it teeth.

**Flow** is earned three ways: gates (scaled by precision, speed and chain),
sustained drift, and **skimming** — passing within 22 m of corridor structure
without touching it.

```js
// src/sim/race.js
if (dist < py.r + CRAFT_RADIUS + 1.5) continue;  // that is a collision, not a skim
this.skimmed.add(py.id);
const intensity = clamp01(1 - (dist - py.r) / (R - py.r)) * (0.5 + p.speedNorm);
```

The structure that generates flow is the same structure that ends your run if you
misjudge it by four metres. That is the risk/reward spine: **the fast line is also
the scary one.** This is the mechanic to protect above all others in production —
it is what makes the open corridor feel like a decision space rather than scenery.

Full flow fires **Tachyon Surge**: raised ceiling (620 km/h), and the world dilates
to 0.55× while you stay at full rate. Rivals fall into slow motion. It is the
fantasy beat the whole thing is built to deliver.

---

## 3. Failure and recovery

An early build bricked the run on a single missed gate. That is wrong for this
genre, and it was caught by the integration test rather than by playtesting:

```
✖ a full run is completable end to end
  AssertionError: run did not finish in 400.0s (claimed 0/82)
```

Two causes, two fixes:

1. **Rings were 15 m radius at 460 km/h.** The recorded miss was 17.4 m off-plane
   against a 15.5 m ring — a near-miss by two metres. Rings are now 24–40 m, which
   is the scale this speed actually needs.
2. **A crossed plane only counted in one direction.** Overshooting a ring made it
   permanently unclaimable. Rings now claim on a crossing in *either* direction, so
   the recovery is a 180° and a re-thread — expensive in time, never fatal. The AI
   was taught the same manoeuvre, which is what lets the harness prove a seed is
   completable.

Three more were found by reading the browser-only code rather than by testing,
because none of them are reachable headlessly:

- **The run started before the player did.** The `Race` constructor armed the
  countdown and the main loop steps the sim during `'countdown'`, so the grid went
  green behind the title screen. A fresh race now sits in `'menu'` until launch.
- **Fog hid the entire gate chain.** `FogExp2` at density 0.0016 is 92% opaque at
  1000 m and 100% at 1500 m — you were flying blind into rings you could not see.
  At 0.00055 the chain still reads at 1500 m (49%) and fades by 4000 m. Fog density
  is now documented as a gameplay value, not atmosphere.
- **Results only ever showed once.** `_resultsShown` was cleared on corridor build
  but not on restart, so finishing a second run produced nothing.

Second bug of the same family: solid impacts re-applied a full speed bleed every
frame, so a craft pressed against a pylon bled to zero and welded itself there
(5347 impacts in one run, 25 seconds stuck at 0 m/s). Collision now removes only
the normal component of velocity so the craft **slides** along the structure, and
rate-limits the damage event to one per 0.45 s. Same run: 9 impacts, no wedge.

---

## 4. What is prototype-grade vs production-grade

Honest inventory, so nobody mistakes this for a shippable build.

**Solid — the design is proven and the code is the reference:**

- The flight model. This is the part worth porting faithfully; it is ~200 lines and
  it is fully unit-tested.
- The procedural corridor. Seeded, deterministic, and validated by 12 invariant
  tests (flyable, finite terrain, gates clear of the deck, sectors partition).
- The scoring/risk loop.
- The headless harness pattern — keep this in production. Being able to fly 10,000
  generated corridors overnight and list the un-flyable ones is worth more than any
  amount of manual QA.

**Deliberately rough — placeholder, replace in production:**

| Area | Now | Production |
|---|---|---|
| Art | Primitive-composite hulls, vertex-coloured flat-shaded terrain | Real mesh, PBR materials, Houdini canyon kit, sculpted hero craft |
| Audio | Procedural WebAudio oscillators | Recorded/layered engine samples, doppler, reverb sends |
| Terrain | One displaced plane per 420×1400 m chunk | Virtual texturing, runtime virtual geometry, streamed clips |
| FX | Additive ribbon trails, line-segment streaks, UnrealBloom | Niagara/VFX Graph: volumetric trails, screen-space distortion, chromatic aberration, motion blur |
| Structure | Two instanced cylinders per pylon | Authored kit, destructibles, varied hazard families |
| Physics | Custom integrator, cylinder-vs-cylinder | Keep the *model*, move the broadphase to the engine |
| Content | One biome, 3 rivals, no meta | Biomes, roster, vehicle classes, ghost racing, leaderboards |
| Netcode | None | Rollback or deterministic-lockstep; the fixed 120 Hz step and seeded RNG are already the right foundation |

---

## 5. Porting to Unreal (or Unity)

The architecture was split so the port is a mapping exercise, not a rewrite.
`core/`, `world/` and `sim/` have **zero** DOM and WebGL dependencies — they import
`three` only for `Vector3`/`Quaternion`.

| This repo | Unreal 5 | Unity |
|---|---|---|
| `config.js` | `UDataAsset` / DataTable rows | ScriptableObject |
| `core/mathx.js` (RNG, noise) | `FRandomStream`, PCG noise | `System.Random`, `Mathf.PerlinNoise` |
| `world/course.js` | PCG graph + a `UCurveFloat` spline, or a C++ generator node | Editor script + `Spline` container |
| `sim/vehicle.js` | `APawn` + custom `Tick` (do **not** use Chaos — the grip model is the game) | `MonoBehaviour.FixedUpdate` |
| `sim/ai.js` | `AAIController` producing the same struct | `MonoBehaviour` on the rival |
| `sim/race.js` | `AGameMode` + `UGameState` | `GameManager` singleton |
| `render/*` | Meshes, Niagara, Lumen | Meshes, VFX Graph, URP/HDRP |
| `ui/*` | UMG | UI Toolkit / uGUI |

**Port order I'd recommend:**

1. `vehicle.js` first, in isolation, on a flat plane. Match the numbers: 26 m/s²
   thrust, 5.2 / 2.6 grip, 1.85 → 1.15 rad/s yaw. If it does not feel like the
   prototype, nothing downstream will.
2. `course.js` next, rendering the spline and rings only. Validate with the same
   12 invariants.
3. Then the scoring loop, then rivals, then art.

**Keep from this prototype regardless of engine:**

- The fixed-timestep accumulator. Variable-rate physics will desync replays and
  netcode.
- The seed-in-URL contract. Reproducible runs are what make async competition and
  ghost racing possible.
- The headless harness. It is the cheapest QA you will ever buy.

---

## 6. Open design questions

Things this prototype does not answer, and should not pretend to:

- **Is the meta enough?** Per-seed time ghosts now give one-shot runs a reason to
  replay, and the seeded URL makes async comparison trivial. What is *not* built:
  online leaderboards per seed, a vehicle roster, or a progression layer between
  runs. Ghosts are the cheap 80%; the question is whether they carry retention.
- **How much verticality is too much?** `gateVertical` at ±46 m feels good at
  460 km/h. At 620 km/h in surge it may read as noise. Needs a playtest sweep.
- **Should a run ever end early?** Currently a miss never ends the run. A "three
  misses and you are out" rule would add stakes — but it also reintroduces the
  failure mode that bricked the earlier build.
- **Vehicle classes.** The model supports per-craft `grip`/`thrust`/`topSpeed`
  already (`speedCapMul` is used by rivals), so a roster is a data change.
- **Multiplayer.** The deterministic seeded generator makes async ghosts
  straightforward; synchronous racing needs the rollback work called out above.
