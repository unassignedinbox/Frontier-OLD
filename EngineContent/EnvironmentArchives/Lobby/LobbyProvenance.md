# Studio Lobby Environment Archive Provenance

| File | Encoding | Origin & Purpose |
|---|---|---|
| `Lobby.facetspace` | `FSP1` v1 binary | Enclosed VehicleHMI Studio Lobby scene topology: hero `ControlVehicle` (`[0..5]`), 4 guest showcase stands (`[6..29]`), glossy studio floor & turntables (`[41]`), cyclorama cove (`[30]`), ceiling (`[31]`), overhead softbox (`[40]`), truss lattice (`[32..39, 226..229]`), and surface-card ground tiles (`[42..225]`). |
| `Lobby.nestingspace` | `NSP1` v1 binary | Virtualized geometry cluster DAG exported directly from `Lobby.facetspace` via `NestingArchiveExport`. |
| `Materials.bin` | 48 B × family | Studio PBR material channels (metallic paint liveries, satin floor, softbox diffuser, cyclorama plaster, brushed aluminium truss). |
| `VehicleProfile.bin` | Binary LUT | ControlVehicle cabin/wheel/caliper profile shared with `ProvingGround`. |
| `EffectProfile.bin` | Binary LUT | Exhaust/brake effect profile table. |
| `CloudProfile.bin`, `CloudShadowProfile.bin` | Binary LUT | Neutral sky fallback buffers for `IlluminationHost` descriptor bindings when indoor studio lighting is active. |
