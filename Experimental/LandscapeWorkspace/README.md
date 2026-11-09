# Frontier · Global 3D SDF Landscape Authoring Workspace

Clean-sheet author-time **Global 3D Signed Distance Field (SDF)** landscape editor and volumetric sculptor built for WebGL2.

## Architecture Highlights

1. **Zero LOD — Native Full-Resolution Per-Layer 3D SDF Lattices**
   - Every landscape layer owns an independent 3D SDF lattice starting at **`512 × 512 × 128`** and scaling upward (`512`, `640`, `768`, `896`, `1024`, `1280`, `1536`, `2048` horizontal; `128`–`512` vertical) alongside its own biome material, stratigraphic bedding, roughness, foundation depth, and procedural CSG landform stack.
   - Default world ships with **Desert mesas & dunes at `1024 × 1024 × 256`** paired with **Rocky landscape (`512 × 512 × 128`)** and **Basalt headland (`512 × 512 × 128`)**.
   - Narrow-band $8^3$-voxel bricks are stored as **`10 × 10 × 10` IEEE-754 `R16F` texel blocks** (with a 1-voxel shared border on all 6 faces) inside a shared 3D texture atlas (`480 × 480 × 10·N_z`, staying strictly under the 512-texel universal WebGL2 3D texture ceiling) for single-instruction hardware `GL_LINEAR` trilinear filtering with zero seam discontinuities and zero atlas bleeding.

2. **Zero Clipping — Camera-Relative Raymarching + Exact $L_\infty$ Chebyshev Empty-Space Skipping**
   - Fullscreen ray-space evaluation starts directly at `max(tBoxEnter, 0.0)` with zero polygon near/far plane clipping and automatic interior cave escape when orbiting underground.
   - Unallocated exterior bricks store an exact 26-neighbour 2-pass $L_\infty$ Chebyshev distance transform in an `RGBA8UI` 3D indirection grid, jumping rays directly to the exit of empty $(2R+1)^3$ brick super-boxes with zero overshoot even on anisotropic voxel pitches.
   - Deep interior rock bricks use a dedicated solid-core sentinel (`254 -> -Band`) so subterranean caves and tunnels are never hollow.

3. **Zero Water / SDF Fighting — Unified Ray-Space Spectral Ocean Integration**
   - Preserves the 24-wave world-space aperiodic spectral ocean (`OceanSurface.js`) and intersects it analytically along the **exact same camera ray `[0, tSdf]`** in `highp float`.
   - Eliminates polygon depth-buffer precision quantization and Z-fighting completely while supporting dynamic wave run-up on sloped beaches, wet shoreline darkening, Beer-Lambert optical underwater refraction, shallow caustics, and depth-driven coastal surf foam.

4. **Real-Time 60 FPS 3D Volumetric Sculpting & Brick-Delta Undo/Redo**
   - Four true 3D SDF brushes (**Clay `+`**, **Carve `−`**, **Smooth**, **Flatten**) operate directly in 3D world space across overhangs, arches, vertical cliffs, and caves.
   - Dynamic brick allocation pulls from pre-allocated atlas headroom slots (`SculptHeadroomBricks`), tightening local $L_\infty$ Chebyshev cones on the fly and uploading only modified `10 × 10 × 10` `R16F` sub-blocks via `gl.texSubImage3D`.
   - `Ctrl+Z` / `Ctrl+Y` applies per-brick `Float32`/`Float16` snapshots in `< 1 ms`.

## Commands

```bash
npm ci
npm test
npm run dev
npm run build
```
