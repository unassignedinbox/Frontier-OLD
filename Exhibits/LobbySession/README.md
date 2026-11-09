# Studio Lobby Session, Slope Equilibrium Placement, Cinematic Camera & Runtime Transition

Evidence for the Project Everest Studio Lobby startup, dynamic 1-to-5 vehicle roster, two distinct session placement policies (`Stand` showcase with brake hold vs `TerrainEquilibrium` 4-corner slope-fitted Proving Ground stations with brake hold), VehicleHMI + Lobby-only cinematic camera, rig/route transition camera, and live `Lobby` ↔ `Proving Grounds` level switching across both the Frontier Editor (`Frontier.exe` / `EverestWorkspace`) and the standalone Vulkan `Everest` runtime (`Everest.exe` / `Everest`).

## Architecture Ownership

| Component | Path | Responsibility |
|---|---|---|
| `SessionPolicy` & `SessionSpace` | `Frontier/Engine/Session/{SessionPolicy.h,SessionSpace.h,SessionSpace.cpp}` | Reusable session policy (`Stand` vs `TerrainEquilibrium`, `EquilibriumPlacement`, `EngageBrakesOnPlacement`), lowest-free-slot roster allocation, ready gate, countdown & round timers |
| `PhysicsVehicle` & `PhysicsExtension` | `Frontier/Extensions/Physics/{PhysicsVehicle,PhysicsExtension}.{h,cpp}` | 4-corner downward raycast terrain plane fitting (slope pitch + roll + static strut sag + radial tyre deflection), automatic brake-hold latch releasing on throttle (`> 0.02`), and static `PhysicsShape::Triangles` Jolt mesh colliders for ramps & structures |
| `VehicleCameraSolver` | `Frontier/Engine/Rendering/SceneViewport/VehicleCameraSolver.{h,cpp}` | 1:1 C++20 port of VehicleHMI 8 camera rigs, `ChaseCameraSolver`, `DrivingCameraMotion`, **Lobby-only** 7.0 s entrance dolly + 6-shot idle cinematic tour, and `EaseCinematic` rig/route transition camera |
| `LobbySessionPolicy` & `LobbySession` | `Projects/Everest/Source/Session/{LobbySessionPolicy,LobbySession}.{h,cpp}` | Everest 5-stand Studio Lobby (`FALCON-1` hero turntable + `Lobby Slot 2..5`) & 5-station Proving Grounds (`Launch Straight`, `Slope Ramp 12.7°`, `Skidpad`, `Slalom`, `Hangar`) policies |
| `Lobby.levelspace` & `Lobby` Archive | `Projects/Everest/Content/Levels/Lobby.levelspace`, `EngineContent/EnvironmentArchives/Lobby/` | Enclosed studio environment (`environment_archive = "Lobby"`), solo hero `ControlVehicle` + 4 hidden guest slot `Geometry` records + 5 studio luminaires |
| `EverestRuntimeMain` | `Projects/Everest/Source/EverestRuntimeMain.cpp` | Standalone `FRONTIER_EDITOR == 0` Vulkan runtime (`GLFW_NO_API`, zero OpenGL) sharing `RenderingExtension` and `PhysicsExtension` |

## Verification & Visual Evidence

- **`LobbySessionValidation`** (`ctest -R LobbySessionValidation`): deterministic CTest verifying solo startup (`1` car on centre turntable, `Lobby Slot 2..5` hidden), dynamic joins `1 → 5` and middle-slot departure/reclamation, `SceneGeometryOccupancy` 100% coverable binding on `Lobby.facetspace`, 7.0 s studio entrance dolly, 6-shot Lobby cinematic tour with 0.7 s fade veil, `EaseCinematic` rig-to-rig transitions (`Orbit` / `Cockpit` / `HMI`), route fade transition into `ProvingGroundVehicle.levelspace`, proof that the Lobby cinematic tour never engages outdoors, and `TerrainEquilibrium` + brake-hold + uphill progressive-clutch climb over the `12.68°` Proving Ground `Jump ramp`. Ledger: `Results/LobbySessionValidation.md`.
- **`LobbyWorkspaceCapture`**: offscreen Vulkan/SwiftShader GPU + Frontier Editor UI captures in `Images/`:
  - `Images/01-EverestLobbySoloStartup.png` — Default Everest startup showing 1 hero car on the centre turntable with empty ring stands and the full Editor UI.
  - `Images/02-EverestLobbyThreeDrivers.png` — Dynamic join of 2 guest drivers (`3/5` active vehicles).
  - `Images/03-EverestLobbyFullFiveDrivers.png` — Full `5/5` vehicle roster across the centre turntable and 4 ring stands.
  - `Images/04-EverestLobbyCinematicTour.png` — Lobby-only idle cinematic camera tour framing.
  - `Images/05-EverestProvingGroundTransition.png` — Live transition from `Lobby` to `ProvingGround` on the same `RenderingExtension` instance with VehicleHMI chase camera.
  - `Images/06-EverestProvingGroundSlopeRampEquilibrium.png` — `ControlVehicle` deployed onto Proving Ground Station 1 (`Slope Ramp 12.7°`) at 4-corner slope-aligned equilibrium with brakes engaged.
