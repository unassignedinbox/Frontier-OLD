# Erosion Physics — Research Notes & Implementation Rationale

This document records the research behind `Engine/ErosionEngine.js`, why the reference studio's
erosion was replaced rather than ported, and what each process can and cannot do. It is the
justification for every default in `Engine/ErosionPresets.js`.

---

## 1. Why the reference scheme was not physics-based

The reference studio (`arena/68c84c0d-frontier`) erodes with a **droplet/particle advection**
scheme: N droplets are spawned, traced down a gradient, and each picks up or drops sediment
against a heuristic "carrying capacity" proportional to `speed × volume × slope`. This is the
technique popularised by Musgrave-era heightfield work and by many game-terrain tools.

It produces convincing imagery, but mechanically it is not a landscape evolution model:

| Property | Droplet scheme | Physical requirement |
| --- | --- | --- |
| Drainage network | Emergent from stochastic walkers; no trunk channels guaranteed | Must route by steepest descent over a depression-filled surface |
| Base level | None — deposition can raise or lower any cell independently | Rivers cannot incise below their outlet |
| Mass balance | Not conserved; "sediment" is a per-droplet scalar with no closure | Erosion = deposition + export |
| Parameter meaning | `erosionStrength`, `sedimentCapacity` are unitless knobs | `K`, `m`, `n` have known ranges from field data |
| Determinism | Droplet order and float rounding drift | Same seed must reproduce exactly |
| Lakes | No concept of a basin; a depression is just a low spot | A lake is a closed depression holding a water surface |

That last row is the direct cause of the flooding you saw: **nothing in the reference knew what a
valley floor was.** Without a filled surface and an outlet, the water has no datum to sit at, so
the ocean sheet and the sediment both behave as if every low point is a hole to be filled.

Two more of your observations have the same root cause:

- *"wind on dunes looks good, wind on canyons didn't get the sandstone cuts."* Correct — and it is
  not a bug. See §5.
- *"pitting / thermal expansion doesn't seem to work."* Thermal expansion operates at grain scale
  and cannot be represented on a 10–50 m grid. See §6.

---

## 2. What this engine implements

The two end-member fluvial models that real landscape-evolution codes use, plus the surface
processes that sit around them.

### 2.1 Detachment-limited stream power

```
∂h/∂t = U − K · A^m · S^n
```

where `h` is elevation (m), `U` uplift (m/yr), `A` drainage area (m²), `S` local channel slope,
`K` bedrock erodibility (yr⁻¹ for the unit-coefficient form), and `m`, `n` exponents.

- `m ≈ 0.5`, `n ≈ 1` is the classical unit stream-power case and reproduces the observed
  slope–area concavity `θ = m/n ≈ 0.5` of natural river long-profiles (Whipple & Tucker 1999).
- In steady state the concavity is set by `m/n`, so the preset `m`/`n` values are the control
  that makes a river profile look like a river profile rather than a straight ramp.

**Solver.** Implicit, `O(n)`, topological sweep (Braun & Willett 2013 — the FastScape scheme).
Nodes are visited in the priority-flood pop order, which runs downstream → upstream, so the
receiver's *new* elevation is always already known:

```
h_new = (h_old + Δt·U + C·h_recv) / (1 + C),    C = K_eff · A^m · Δt / Δx      (n = 1)
```

For `n ≠ 1` a short fixed-point iteration is used with a hard clamp at the receiver elevation.
The scheme is unconditionally stable, so the studio can take large time steps without the Courant
limitation that plagues explicit schemes.

`K_eff` is modulated per cell by rock resistance, by regolith cover (weathered rock detaches far
more easily than bedrock), and by local rainfall relative to the field reference:

```
K_eff = K₀ · RockFactor(hardness) · SoilBoost(regolith) · (rain / rain_ref)^m
```

`K₀ = 10^(−7 + 2.2·erodibility)` so the studio's 0–1 control maps onto the 10⁻⁷–10⁻⁴ yr⁻¹ range
quoted for natural bedrock channels.

### 2.2 Transport-limited sediment routing

Deposition is the other half of the cycle and is what the reference lacked entirely. Each cell
receives the flux of its donors, adds the material it just eroded, and retains only what its
capacity allows; the excess aggrades the bed:

```
Q_capacity = K_c · A^m · S^n · LakeDamp
Q_out      = Q_in + E − min(Q_in + E, Q_capacity)
Δh         = (Q_in + E − Q_out) / cellArea
```

`K_c` is deliberately set just *below* the stream-power `K`, so steep catchments stay
detachment-limited (they incise) while low-gradient reaches aggrade — the detachment/transport
transition that produces alluvial fans, valley fill and lake deltas.

`LakeDamp → 0.04` inside a lake, so a basin traps its sediment. That is how a lake fills in and
builds a delta instead of overflowing.

### 2.3 Depression filling and flow routing

`PriorityFloodFill` (Barnes/Heywood style, `O(n log n)`) produces a **filled** surface where every
closed depression is raised to its spill point, plus a pop order and a lake mask.

- The filled surface is used **only** for routing. Erosion still uses the true heights, so lakes
  remain lakes.
- The ocean is seeded as an outlet (all border cells and every cell at or below sea level), which
  is what gives the system a base level.
- Receivers are chosen by steepest descent **on the filled surface**, with flats resolved by pop
  rank. Because the fill guarantees a monotone path to an outlet, the flow graph is acyclic by
  construction — no cycle detection needed, and no infinite loops possible (verified by suite 15).

### 2.4 Lateral processes

| Process | Model | Signature it produces |
| --- | --- | --- |
| Thermal talus | Pairwise mass-conserving transfer above the critical repose angle | Scree aprons, softened cliffs |
| Aeolian | Shear-driven deflation of windward flanks, lee deposition, phase-shifted bedforms | Transverse dunes, yardangs |
| Glacial | Shallow-ice diffusion, `D = C·H⁵·‖∇s‖²`, erosion ∝ `(H·‖∇s‖)^n` | U-shaped troughs, cirques |
| Chemical | Arrhenius-like bedrock → regolith conversion, solute export | Deep soils on humid lowland, bare rock on ridges |
| Frost pitting | Poisson field of fatigue sites scaled by freeze–thaw cycles and face exposure | Pocked cliff faces |

Glacial ice uses Glen's flow law with `n = 3`, which is why the diffusion coefficient goes as
`H⁵`: thick ice flows enormously faster than thin ice, so ice concentrates in and deepens pre-existing
hollows. That is the mechanism that converts a V-valley into a U-valley, and it cannot be
reproduced by a uniform smoothing filter.

### 2.5 Stratigraphy — the control that actually makes landform character

`BuildHardnessField` lays down beds with a dip and strike, a hard-cap/soft-middle profile, and
optional columnar jointing. Differential erodibility between beds is the single largest control on
landform character: alternating hard caprock and soft shale gives the stair-step cliff/bench
profile of a canyon. Without it, every landscape converges on the same smooth, scale-invariant
hill — which is precisely the failure mode of a droplet scheme with a single `rockHardness` scalar.

---

## 3. Calibration

Rates are real; the clock is compressed. A bake of `iterations × dtKyr` years (36 × 12 kyr =
432 kyr by default) is a **compressed geological history**, so absolute amplitudes are scaled to
be readable at authoring scale (a few hundred metres of relief) rather than true orogenic values
(which would take 10⁵–10⁶ yr to show).

| Quantity | Range used | Real-world anchor |
| --- | --- | --- |
| `K` (bedrock) | 10⁻⁷ – 10⁻⁴ yr⁻¹ | 10⁻⁶ – 10⁻⁵ typical for bedrock channels |
| `m`, `n` | 0.5, 1.0 (0.5–1.2 / 0.5–2.5 allowed) | unit stream power; `θ = m/n ≈ 0.5` |
| Rainfall | 15 – 2200 mm/yr | Namib ≈ 15; Icelandic highlands ≈ 2000 |
| Temperature | −20 – 26 °C | matches the landscape each preset models |
| Lapse rate | 6.2 °C/km | standard tropospheric |
| Repose angle | 26 – 40° | 30–35° for angular scree; lower for wet/debris flows |
| Uplift | 0 – 0.09 mm/yr (compressed) | Himalaya ≈ 1 mm/yr real; see §7 |

---

## 4. Determinism, and shared weather groups

Every stochastic element (pitting sites, dune phase noise, bed variability) draws from a seeded
mulberry32 PRNG and an integer-hash value noise, so a preset reproduces bit-identically. Suite 11
asserts `max |Δh| == 0` between two identical runs, and that changing the seed produces a genuinely
different landscape.

**Shared weather groups** solve the "simulate two layers with the same weather" requirement:
layers with the same `weatherGroup` resolve a common climate (median rainfall, temperature, wind
speed and azimuth across the group) and, in `uniform` mode, the same iteration count and `dt`.
Two terrains bounded into one world therefore evolve in lockstep and match at their shared seam,
which is what you need for a continuous open world rather than a patchwork of independent islands.

---

## 5. Why wind will not cut a canyon (and what to do instead)

Your observation that wind works on dunes but not on canyon walls is geomorphologically correct,
and it is not a limitation of the implementation. Three physical reasons:

1. **Momentum.** Aeolian sediment transport is driven by near-surface shear, which scales with
   wind speed but acts only on loose, dry, sand-sized material. It cannot abrade intact bedrock at
   any useful rate at the scale of a canyon wall.
2. **Grain size.** Saltation moves 0.1–0.5 mm grains. Canyon walls are bedrock. Wind polishes and
   flutes rock; it does not excavate a 200 m gorge.
3. **Loading.** Above ~50 mm/yr rainfall, vegetation and moisture bind the surface and shut
   saltation down entirely. Most canyon-cutting climates are wetter than that in their history.

So in this engine `aeolian` is deliberately *transport-limited and sand-gated*: it is strong on
dry, gently-sloped, above-sea-level surfaces and essentially inert on steep wet rock. When you
enable wind on a canyon preset, the correct outcome is fluting and dune sheets in the valley floor,
not new gorge geometry.

**What actually cuts canyon walls**: differential bedrock hardness (the strata control), plus
stream-power incision along the trunk river, plus talus removal at the base. The `grand-canyon` and
`layered-plateau` presets show the difference cleanly — drop `caprockContrast` and the stair-step
profile collapses into a smooth V.

If you want wind to look dramatic on rock, the lever is `deflationHardness` *together with*
strata: softer beds under a hard cap will undercut and produce yardangs (the
`sahara-tassili-erg` and `yardang-wind-corridor` presets do exactly this).

---

## 6. Pitting, frost and why "thermal expansion" cannot be simulated here

- **Thermal expansion / insolation cracking** operates at the grain scale (millimetres) and is
  modulated by mineralogy and daily temperature swing. On a 10–50 m grid it has no resolvable
  geometry. Modelling it as a heightfield operation would just be a noise stamp pretending to be
  physics.
- **Frost shattering** *is* resolvable, because it is driven by freeze–thaw cycle count and face
  exposure, both of which are grid quantities. This is what `pitting` implements: a Poisson field
  of fatigue sites, each growing with cycles × exposure, with a smooth radial falloff so you get
  pocked faces rather than uniform roughness.
- **Salt weathering** is folded into the same model via the moisture and temperature terms.

For the "ice expands" idea you mentioned: expansion is the *mechanism*, cycle count is the
*observable*. Modelling cycles is defensible; modelling expansion on this grid is not.

---

## 7. The flooding bug: diagnosis and fix

Three independent causes, all fixed:

1. **Ocean alpha was near-opaque** — `alpha = mix(0.84, 0.96, …)`. At 84–96% opacity the water
   sheet paints over the terrain it grazes, so any coastline reads as "flooded". Now the alpha
   fades with **wave height above the datum**: any part of the sheet standing above sea level is,
   by definition, water climbing terrain it does not cover, and it is faded out.

2. **Every sea cell counted as a lake** — the priority flood raises all cells to the spill point,
   and at the coast that means sea level, so the whole ocean satisfied `filled > height`. The
   deposition term then tried to *fill the sea*. Lakes are now masked to closed basins **inside
   the authored land mask** and must exceed a depth threshold (`lakeDepthM`, default 1.2 m) so a
   one-cell noise pit is not mistaken for water.

3. **No floor on eroded land** — a valley could be incised below sea level and disappear under the
   ocean plane. There is now a hard guard: inside the mask, open ground may never be deeper than
   `maxLandDrownedM` (default 2.5 m) below sea level. Enclosed lake basins are exempt, because
   that is what a lake *is*.

Suite 10 asserts this across **all 19 presets**: zero drowned cells, per preset.

---

## 8. Mass conservation and validation

`Engine/ErosionEngine.js` tracks volumes per process. Suite 9 asserts the routed budget closes:

```
incision ≈ deposition + export      (tolerance 2%)
```

Talus and chemical weathering are tracked separately because they move material without passing
it through the sediment flux. Two real bugs were caught by this suite and fixed:

- outlet flux was **double-counted** in the export total (added again after the sweep), closing
  at only 55%;
- the talus step clamped cells to the slab floor, silently deleting material (mass vanished).

Both are exactly the class of defect that a "looks good" renderer hides forever.

Current validation: **16 suites** in `TerrainWorldValidation.mjs`, plus a jsdom UI smoke test that
builds the erosion inspector and exercises all 45 sliders, 9 switches and 4 selects.

---

## 9. Known limitations / next steps

Honest list, roughly in priority order:

1. **Bake is synchronous per evaluation.** It is cached and debounced, and
   `ErosionSimulationState.step()` exists specifically so it can be spread across frames, but the
   inspector still triggers one blocking evaluation. Moving it into a Worker is the next step.
2. **Resolution is bounded** (`Nx`/`Nz` capped, ~256 max). Fine rills below the grid scale are
   aliased. A multi-resolution or sparse-tile bake would fix this and is the natural companion to
   the existing 1 km sparse mask tiles.
3. **No wave/coastal erosion.** Cliffs are set by the mask and the falloff mode, not by wave
   undercutting. This is the obvious next process for coastal presets.
4. **No vegetation feedback.** Real landscapes couple erosion to vegetation cover, which switches
   saltation and rilling on and off. The hooks are there (`rainMmYr` gates sand already).
5. **Uplift is compressed**, by design — but that means steady-state relief is not physically
   meaningful in absolute metres. What *is* meaningful is the shape.
6. **SLD/multi-direction flow** would improve ridge and divide fidelity versus the current single
   steepest-descent receiver.
