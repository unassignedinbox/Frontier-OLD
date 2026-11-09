# Frontier & Project Everest — Clean `56279e1` Baseline Audit & Ongoing Plan

## 1. Active Baseline & Repository Lineage

- **Active Session Branch**: `arena/bacd63a6-frontier` (`unassignedinbox/Frontier`)
- **Clean C++ Engine Baseline**: `56279e19b12141b6b4dcd2c69b7c75bc4c21d8e4` (`2026-11-04 14:05:54Z` — *"Naming pass on the new code: ResolveGizmoCamera, RecordConfiguration, packets, Solver"*, parent `ac013a1~1` on `streamlinkinbox/Frontier` `arena/88b72faa-frontier`)
- **Preserved Web Exhibit**: `Experimental/SlateReference/A/Exhibits/VehicleProvingGrounds` (`1345c33` — D-Day Militarized Wall Run, isolated under `Experimental/`)
- **Why `56279e1` Was Chosen**:
  - **Zero C++ Fluid code** (`FluidSimulation` / `FluidDynamics` / `PlumeSpace` not yet introduced)
  - **Zero C++ Vehicle / Lobby code** (`PhysicalDynamics/Vehicle` / `LobbySession` / `VehicleCameraSolver` not yet introduced)
  - **Zero quality-degrading render optimizations** (untouched 1:1 WebGPU-parity Vulkan ReSTIR GI, VirtualizedGeometry I1–I5, AnalyticIllumination, PlanetaryAtmosphere, GPU Picking, Shared Gizmo, JoltPhysics v5.6.0 `PhysicsSpace`, `NetworkingSpace`, `ForceField`, and GPU `ParticleSimulation`)

### Quality-Degrading Render Optimization Branches Excluded
| Repository | Branch | Commit & Date (UTC) | Excluded Changes |
|---|---|---|---|
| `c7egoist/Frontier` | `arena/c60e4ba2-frontier` | `647481e` (`2026-11-08 03:18Z`) | Vulkan upload & TLAS refresh shortcuts (`DeviceExchange.cpp`, `RayTracingSpace.cpp`) |
| `streamlinkinbox/Frontier` | `arena/38ffdb11-frontier` | `57cd024` / `5ed0fca` (`2026-11-07 19:45Z`) | Perf Lab skip/cull switches (`tlasReuse`, `cascadeReuse`, `fogCull`, `glassCull`) |
| `streamlinkinbox/Frontier` | `arena/617c06b6-frontier` | `80ca407` (`2026-11-07 19:21Z`) | Render optimization pass + motion blur rewrite |
| `streamlinkinbox/Frontier` | `arena/8ff1f043-frontier` | `e0c9a82` .. `78d573a` (`2026-11-07`) | VehicleHMI performance shortcuts + TAA blur |
| `streamlinkinbox/Frontier` | `arena/4356640b-frontier` | `6278e5e` / `532c198` (`2026-11-06`) | Opt-in F2 performance execution path |
| `unassignedinbox/Frontier` | `arena/4f1f47a8-frontier` & `arena/42169529-frontier` | `8088578` / `63b109e` (`2026-11-05`) | Phase 3 render optimizations & TLAS skip |

---

## 2. Ground-Truth Feature Audit at `56279e1`

| Area | Native C++ at `56279e1` | Reference / Donor Source | Target Action |
|---|---|---|---|
| **Renderer & Shaders** | 🟢 Pure 6-archive Vulkan ReSTIR GI, VirtualizedGeometry, AnalyticIllumination, PlanetaryAtmosphere (`127` SPIR-V archives, `0` failures) | Baseline `56279e1` | Preserve visual quality 100%; zero skip/cull optimizations |
| **Camera System & Lens Flare** | ❌ Viewport flight only; no `Camera` record; `OpticalFlare` (`Lens Flare`) mistakenly parented under `Solar` (`Sun`) | `SultanAladin/Frontier-` `GI-Updated-Html-Version` (`CameraOptics.h`, `CameraInspectorPanel.cpp`) | Add `Editor Camera` + `Vehicle Camera` records & switching, `CameraOptics` (FOV/Focal/Aperture/Focus), subtle `CameraShake`, 3-mode `MotionBlur` dropdown (`Off`/`Camera`/`Scene`), move `OpticalFlare` under `Camera` |
| **Environment & Dynamic Prop Collisions** | 🟡 Jolt v5.6.0 `PhysicsSpace` (`Alt+S` Simulate/Restore) using box bounds; multi-copy objects held static | `PhysicsSpace` + facet triangle colliders (`ed17496`) | Real Jolt static triangle mesh colliders on environment geometry + movable dynamic prop (cone) collisions without requiring fracture |
| **Fluid Editor** | ❌ Only `FluidPlumeInspector` stub | Approved 1:1 C++ port from `ed17496` (`FluidSimulationSequence` + `FluidEditor` + 18 `.fluidspace` presets) | **Completed design approved by user** — port intact 1:1 without redesigning or breaking it |
| **Dockable ForceField / Wind Editor** | 🟡 Native `ForceField` + `Wind` + 16 `ForceSource` types + `Field` view | `56279e1` + dockable `ForceFieldWorkspace` | Expose as Construct-menu dockable editor with 2D/3D views, distinct glowing shape/colour per forcefield, and trigger timeline |
| **Dockable Particle Editor & VFX** | 🟡 Native GPU `ParticleSimulation` (30 SPIR-V entries, 52 presets, lightning, ForceField coupling) in modal window | `56279e1` + `c7egoist/Frontier` `arena/eb353da2-frontier` (`acb0502`) | Convert to Construct-menu dockable tab/window + verify presets & VFX (Lightning, Black Hole, Derez) with stored SPIR-V parity |
| **Dockable Fracture Editor** | 🟡 `FractureRecord` / `FractureEditor` placeholder | `c7egoist/Frontier` `arena/eb353da2-frontier` (`acb0502`) + `VoronoiFracture` | Construct-menu dockable `FractureEditor` with real 3D clipped Voronoi cell physics & Sun-card styling |
| **Vehicle, Tyres, `.vcurves`, Workbench, Lobby/ProvingGround HMI, Smoke, Sparks & Derez** | ❌ Not in `56279e1` | `ed17496` vehicle/plume/sparks + `VehicleProvingGrounds` `EffectSequence` (Derez) | Resting hub/suspension equilibrium spawn (no sunken wheels/bounce), stopped-vehicle gizmo, HMI active in **both** Lobby & Proving Ground, Sun-style Vehicle/Tyre inspectors, `.vcurves` `CurveEditor`, dockable `VehicleWorkbench`, slip-only tyre smoke, hard-impact sparks, and GPU Vehicle Derez/Rez |
| **Engine vs. Project Separation & Project Designer** | 🟡 `PrimitiveGeometryGenerator` / `VolumeDistanceArchive` reference `Projects/Everest/Content`; no `ProjectClassRegistry` yet | Clean `Frontier/Engine` vs `Projects/Everest` boundary | Decouple content search roots, keep Everest mechanics in `Projects/Everest`, implement `ProjectClassRegistry` + `Docs/ProjectDesigner.md` |

---

## 3. Phased Implementation Roadmap (Commit & Push Every Milestone)

- [x] **Phase 0 — Clean `56279e1` Baseline Restore & Plan**: Reset `arena/bacd63a6-frontier` tree to `56279e1` (+ keep `Experimental/` D-Day exhibit), verify all 6 shader sets (`0` failures), write `Plan/ongoing.md`, commit & push to `origin/arena/bacd63a6-frontier`.
- [ ] **Phase 1 — Camera System, Optics, Shake, 3-Mode Motion Blur & Lens Flare Under Camera**:
  - `CameraOptics.h` (`SultanAladin/Frontier-` thin-lens FOV/Focal/Pupil/Hyperfocal/Sharpness math) + `CameraEffects.{h,cpp}` (subtle-by-default `CameraShakeComponent` + 3-mode `CameraMotionBlurComponent`: `Off`, `Camera`, `Scene`).
  - `CameraConfiguration`, `CameraRecord`, `CameraInspector` (Sun/Optics card styling, ray schematic, 8-blade iris, interactive log focus diagram, shake controls, 3-mode blur dropdown, `"Open Lens Flare ↗"` subcomponent card).
  - Relocate `OpticalFlare` (`Lens Flare`) from `Solar` (`Sun`) to `Camera` across `WorldSpace`, `SolarInspector`, `EverestWorkspace`, and `.levelspace` files; wire `Editor Camera` <-> `Vehicle Camera` switching preserving editor camera flight.
- [ ] **Phase 2 — Jolt Environment Triangle & Dynamic Prop Collisions + Dockable ForceField, Particle & Fracture Editors**:
  - Real Jolt triangle colliders for static environment shapes + non-fracture dynamic Jolt bodies for movable props (cones).
  - Dockable Construct-menu tabs for `ForceFieldWorkspace` (16 glowing 2D/3D shapes + trigger timeline), `ParticleEditor` (presets + Lightning, Black Hole, Derez), and `FractureEditor` (`c7egoist` `acb0502` clipped 3D Voronoi physics).
- [ ] **Phase 3 — Approved 1:1 Native C++ Fluid Editor & Modular Plume/ForceField/Particle Coupling**:
  - Port the accepted 1:1 C++ `FluidEditor` + `FluidSimulationSequence` + 18 `.fluidspace` presets cleanly without altering its design; modular coupling with `ForceField` & `ParticleSpace`.
- [ ] **Phase 4 — Vehicle, Tyres, `.vcurves` Curve Editor, Dockable Vehicle Workbench, Lobby + Proving Ground HMI, Slip-Only Smoke, Hard-Impact Sparks & Vehicle Derez**:
  - Resting equilibrium spawn (`ResolveRestingHubLocal` + `ResolveEquilibriumPose`), stopped-vehicle spatial gizmo, HMI enabled in both `Lobby.levelspace` and `ProvingGroundVehicle.levelspace`.
  - Sun-quality `VehicleInspector` & `TyreInspector` tiles, reusable `CurveEditor` with `.vcurves` TOML codec, `Projects/Everest` dockable `VehicleWorkbench`, slip-only tyre smoke, metal/concrete ballistic sparks, and GPU `EffectProfile` Vehicle Derez/Rez sequence.
- [ ] **Phase 5 — Engine/Project Boundary Audit & Project Designer Architecture**:
  - Project-neutral `ContentSearchRegistry`, `ProjectClassRegistry` C++ foundation (Inspector, icon, codec, custom `.vcurves`/`.fluidspace` extension, dockable editor key, workflow pins), `Docs/ProjectDesigner.md`, full CTest + offscreen GPU verification.
