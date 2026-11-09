# LobbySessionValidation

Verification of Studio Lobby startup, dynamic 1-to-5 vehicle roster, Lobby-only cinematic camera, transition camera, and level switching.

| Status | Claim |
|---|---|
| 🟢 | QueryLobbyStudioLuminaires provides overhead softbox flood + 4 cyclorama cove washes (5 total) |
| 🟢 | Studio luminaire intensities match authored VehicleHMI Lobby calibration (950 / 430) |
| 🟢 | Lobby.facetspace decodes cleanly via FSP1 SceneStructure |
| 🟢 | Lobby.facetspace contains 230 occupants with ControlVehicle [0] and Hero plates [5] |
| 🟢 | Lobby.levelspace loads cleanly into WorldSpace |
| 🟢 | Lobby.levelspace declares environment_archive = 'Lobby' |
| 🟢 | All 9 Lobby Geometry records (5 studio architecture + 4 guest car stands) bind cleanly with zero diagnostics |
| 🟢 | Solo Lobby hides all 4 guest stand cars (occupants 6..29) by default |
| 🟢 | Hero plates (occupant 5) excluded from Coverable_ sweep so hero vehicle stays intact |
| 🟢 | ControlCentrePanel initializes with Everest.projectspace |
| 🟢 | EverestWorkspace boots into Studio Lobby by default (startup_level = 'lobby-level') |
| 🟢 | Solo startup shows 1 hero car on the centre turntable |
| 🟢 | Hero ControlVehicle is visible on centre turntable with showcase physics off in Studio Lobby |
| 🟢 | Lobby Slot 2 is hidden when solo |
| 🟢 | Lobby Slot 3 is hidden when solo |
| 🟢 | Lobby Slot 4 is hidden when solo |
| 🟢 | Lobby Slot 5 is hidden when solo |
| 🟢 | Player 2 join activates Lobby Slot 2 |
| 🟢 | Player 3 join activates Lobby Slot 3 |
| 🟢 | Player 4 join activates Lobby Slot 4 |
| 🟢 | Player 5 join activates Lobby Slot 5 |
| 🟢 | 6th player join is refused cleanly at MaximumParticipants = 5 |
| 🟢 | Middle guest departure hides only Lobby Slot 3 while keeping occupied stands visible |
| 🟢 | Next joining driver reclaims lowest free stand (Lobby Slot 3) |
| 🟢 | ResetSolo returns Studio Lobby to 1 hero car on the centre turntable |
| 🟢 | Lobby camera begins on the 7.0 s EaseCinematic studio entrance dolly |
| 🟢 | 7.0 s entrance dolly settles cleanly into studio Orbit with veil lifted |
| 🟢 | Lobby cinematic tour engages automatically after 10 s untouched attract idle (Shot 1/6) |
| 🟢 | Lobby cinematic tour fades through black veil (0.7 s cover/reveal) into Shot 2/6 |
| 🟢 | Visitor activity immediately stands down Lobby cinematic tour |
| 🟢 | Rig transition camera eases smoothly toward HMI Tablet framing (2.1 s EaseCinematic) |
| 🟢 | HMI Tablet transition settles cleanly with 0.01 m near clip plane |
| 🟢 | Route transition camera fades to black and switches live to ProvingGroundVehicle.levelspace |
| 🟢 | Lobby cinematic camera is strictly Lobby-only and never engages on Proving Grounds (even after 130 s idle) |
| 🟢 | All 8 VehicleHMI camera rigs (Chase, Orbit, Cockpit, HMI, Passenger, Hood, Tyre FL, Tyre RL) cycle cleanly |
| 🟢 | Lobby uses Stand placement (brake hold) while Proving Grounds uses 5-station TerrainEquilibrium placement (brake hold) |
| 🟢 | ProvingGround.facetspace decodes cleanly for mesh collision & slope-equilibrium verification |
| 🟢 | Jump ramp resolves exact world-space triangles for Jolt MeshShape slope collision |
| 🟢 | DeployProvingGroundStation(1) configures ControlVehicle onto the 12.68 deg Jump ramp station |
| 🟢 | Proving Ground Jump ramp station pitches ControlVehicle to 12.68 deg slope normal with brake hold latched |
| 🟢 | ControlVehicle holds stationary (< 0.03 m/s) on Proving Ground Jump ramp until throttle is applied |
| 🟢 | Throttle releases brake hold and progressive clutch climbs ControlVehicle up Proving Ground Jump ramp |
| 🟢 | Two-way level switch returns cleanly from Proving Grounds to Studio Lobby |

Total checks: 43, failures: 0.
