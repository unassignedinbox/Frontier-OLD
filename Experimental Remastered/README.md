# Experimental Remastered

Unified **Sun Inspector + Fluid Editor + Vehicle Lab** studio interface across Frontier's interactive authoring workspaces. Shared layout, control, typography, and diagnostic rules are documented in [UI_COMPONENT_GUIDELINES.md](./UI_COMPONENT_GUIDELINES.md).

- **SDF Terrain Studio (`Experimental Remastered/TerrainWorldEditor/`)** — Infinite sparse-tile 2D world mask, independent toggleable Terrain Bounds regions, thin-slab SDF architecture, multi-terrain 3D ocean landscape, per-layer edge→center / cliff falloff, spline landforms, sculpting, **physically-based multi-process erosion**, water-body rendering and `.OBJ`/`.STL` export.
- **SDF Sculpt Studio (`Experimental Remastered/SdfSculptEditor/`)** — Real-time WebGL2 hybrid `RGBA16F` 3D volumetric clay sculpting + 24 non-destructive analytical CSG primitives, 10 volumetric sculpting brushes, interactive brush falloff curve editor, 7 studio shading/diagnostic lenses, Dual Surface Nets watertight mesh extraction, and native Frontier `SDF1` `.sdfspace` / `.obj` / `.stl` / `.toml` interchange.

## SDF Terrain Studio

### Authoring pipeline

1. **2D mask editor** — paint the landmass anywhere in an infinite world; masks are sparse 1×1 km tiles, so only touched tiles allocate memory. Draw a **Terrain Bounds box** around a shape to make it an independently toggleable, independently simulated region.
2. **3D landscape** — each bounds region evaluates its own layer stack into a **thin-slab SDF** (a flat body that starts just below sea level, so nothing is wasted on deep-underground volume). Terrain is converted to triangles only at the end, for display and export.
3. **Layer stack** — composable generators (hybrid multifractal, ridged, Voronoi, billow), spline landforms (river / canyon / ridge / graded ramp / 3D arch), sculpt passes, and **erosion layers**. Each layer has its own boundary falloff: soft edge→center, full cliff at the edge, continental, plateau rim, coastal rim ring, bounds feather or a custom curve.
4. **Export** — `.OBJ` / binary `.STL`, plus a `.json` project that round-trips masks, regions, layers and erosion parameters (transient caches are deliberately excluded).

### Erosion

Erosion layers are non-destructive: they consume the surface produced *below them* in the stack and hand the result upward, so a river spline or a sculpt pass placed above an erosion layer correctly modifies the eroded surface.

**Physics** — detachment-limited stream power `∂h/∂t = U − K·A^m·S^n` solved with the implicit `O(n)` topological sweep of Braun & Willett (2013), plus transport-limited sediment routing and deposition, thermal talus, aeolian saltation, shallow-ice glacial flow (Glen n=3), chemical weathering and frost pitting, over a stratified bed-hardness field. See [TerrainWorldEditor/ErosionPhysics.md](./TerrainWorldEditor/ErosionPhysics.md) for the literature review, calibration and known limitations.

**19 presets** (Icelandic Highlands, Himalayan Karakoram, Layered Plateau, Alpine Divide, Fault Escarpment, Volcanic Caldera, Badlands Domes, Coastal Massif, Grand Canyon, Stuðlagil Basalt, Fjaðrárgljúfur, Namib Erg, Sahara Tassili, Rugged Desert Crags, Desert Oasis, Icelandic Green Highlands, Lunar Mare, Yardang Corridor, Glacial Lake Watershed) and **6 weather modes** (All Weather, Sky Rain, Glacial Blizzard, Aeolian Wind, Rockfall & Avalanche, Storm Cell).

**Shared weather groups** let several erosion layers — across different terrains — resolve one identical climate, so two regions bounded into the same world stay geologically consistent at their seam.

**Diagnostics** — two extra shading lenses (Erosion Depth, Deposition/Fill), a Gaea-style surface palette select, and a mass-balance readout (`incision ≈ deposition + export`) that makes physically wrong parameters obvious.

**Water** — lakes and trunk rivers are rendered from the simulated hydrology as their own pass, separate from the ocean sheet. Three guards prevent the classic "everything floods" failure: ocean alpha fades where its wave sheet rises above the datum, sea cells are never classified as lakes, and open ground inside a mask can never be drowned more than `maxLandDrownedM` below sea level.

### Validation

```bash
cd "Experimental Remastered/TerrainWorldEditor"
node TerrainWorldValidation.mjs      # 16 suites
```

Covers sparse mask allocation, the exact Euclidean SDF, falloff modes, demo seeding, thin-slab meshing, sculpting, spline gizmo grading, drainage networks, mass conservation, the anti-flood guard across all presets, determinism, process isolation, stratified hardness, weather groups, and pathological flow-routing surfaces.

## Quick start

```bash
# SDF Terrain Studio (port 8088, bound to 0.0.0.0)
python3 "Experimental Remastered/TerrainWorldEditor/Serve.py"

# SDF Sculpt Studio (port 8087, bound to 0.0.0.0)
python3 "Experimental Remastered/SdfSculptEditor/Serve.py"
```
