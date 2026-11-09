# Frontier — Experimental Remastered SDF Terrain & Clay Studios

Zero-build, real-time WebGL2 **Signed Distance Field (SDF)** studios styled in Frontier's **Experimental Remastered** (**Sun Inspector + Fluid Editor + Vehicle Lab**) dark warm-amber studio theme:

1. **[Open-World SDF Terrain Studio](./Experimental%20Remastered/TerrainWorldEditor/)** (`Experimental Remastered/TerrainWorldEditor/`)
   - **Infinite Sparse-Tile 2D World Mask Editor**: Paint continents, islands, bays, and fjords across an infinite kilometer coordinate space (`256×256` sparse tiles allocated only where painted) and draw independent, toggleable **Terrain Bounds Boxes** (`TerrainRegion`).
   - **Seamless 2D ↔ 3D + Ocean Workflow**: Switch instantly between `1 · 2D World Mask`, `2 · 3D Landscape + Ocean`, and `3 · Split 2D + 3D`, viewing and editing **one terrain individually, selected terrains, or all enabled terrains simultaneously** in a shared animated 3D Ocean.
   - **Space-Efficient Thin-Slab SDF Architecture**: 2D masks initialize as a thin flat surface near sea level (`-6.5 m` underside wafer) and build upward — zero SDF budget or triangles wasted deep under the ocean — while supporting 3D sculpting/overhangs, Catmull-Rom splines, and watertight `.OBJ` / binary `.STL` triangle mesh conversion.
   - **Composable Base Landform Layer Stack & Per-Layer Boundary Falloffs**: Includes `Hybrid Multifractal`, `Ridged Mountain Noise`, `Voronoi / Mesa Blocks`, `Billow Hills & Dunes`, `Terrace & Strata`, `Spline Landform`, and `SDF Sculpt`, each with configurable **Boundary Falloff** (`Soft Edge → Strong Center`, **`None — Full Cliff at Edge`**, `Continental Power`, `Plateau Rim`, `Coastal Rim Ring`, `Bounds Box Feather`, and `Custom Curve`).
   - **Physically Based Multi-Process Erosion Engine** (`Engine/ErosionPhysicsEngine.js`): a deterministic CPU continuum solver with **six coupled processes** —
     - **Hydrology & Fluvial**: virtual-pipe shallow water + Manning friction (`v = n⁻¹ h^⅔ S^½`), Whipple–Tucker stream-power incision (`E = Kb Qᵐ Sⁿ`) on D8 drainage-area discharge, Exner sediment transport, Green–Ampt soil-depth infiltration + baseflow, and **coastline sink drainage** so rain never floods hillslopes (thin films render as wet rock only; real rivers/pools render as hydrostatic water).
     - **Pitting**: Hallet–Walder segregation ice (−8…−3 °C) + 9 % freeze–thaw expansion + diurnal insolation **thermal-expansion fatigue**, with crack-cluster diffusion nucleating tafoni honeycomb pits & spalls — protected from instant talus fill.
     - **Wind**: Bagnold saltation + **34° slipface avalanches** (migrating barchan/star dunes), **Venturi canyon-corridor acceleration**, saltation-curtain **basal undercutting** (0.2–2.5 m bounce zone) and 3D cross-bedded differential abrasion (slot canyons, alcoves, fins, yardangs).
     - **Seasonal Climate**: latitude + **9 climate regimes** driving day-of-year temperature, rain-vs-snow partitioning, freeze nights, spring snowmelt floods & dry-season wind cycles.
     - **Rockfall**: continuum cliff calving + **ballistic runout** + chute scour + grain-size-sorted talus aprons (coarse 35° / fine 26° repose) — terrain-native, no rigid meshes.
     - **Chemical**: carbonate/evaporite dissolution (karst), silicate hydrolysis (clay saprolite), haloclasty salt cracking, and downhill `Fe`/`Cu`/`Mn`/salt stain transport.
   - **Solo or Shared-Weather Simulation Groups**: simulate terrains **1-by-1**, or bind **2+ mask layers to ONE shared weather/pattern/seasonal cycle** (identical daily climate samples everywhere). Live **Play/Pause/Step/Bake-N-Years** transport + Seasonal Calendar HUD.
   - **23 Erosion Presets** (`Engine/ErosionPresets.js`): all `c7egoist/Frontier` landscapes (Icelandic Highlands, Himalayan Karakoram, Layered Plateau, Grand Canyon, Stuðlagil Basalt, Namib Erg, Yardang Corridor, Lunar Mare, …) re-engineered for the physics solver, plus tafoni/slot-canyon/karst showcases and a twin shared-weather demo.
   - **Gaea-Style Texturing & Data Maps** (`Engine/GaeaTexturingAndStamps.js`): **25 inspectable data maps** (Flow, Velocity, Shear, Wear, Deposition, Talus, Sand, Pitting, Tafoni, Snow, Ice, Moisture, Insolation, Convexity, Cavity, Coast Distance, 3D Strata, Fe/Cu/Mn/Salt stains, …) as false-color viewport lenses + PNG/RAW export, **9 procedural geological stamps** (cross-bedded sandstone, hexagonal basalt, tafoni pits, braided alluvium, aeolian ripples, talus scree, karst grikes, cracked playa, glacial striations) baked as persistent sculpt layers, and **8 SatMap multi-stop color ramps** with composed surface PNG export.
   - **3 Pre-Built Demonstration Masks & Terrains**: `Highland Massif` (Soft + Continental falloffs + River Valley spline), `Basalt Sea Cliffs` (`None` falloff for sheer ocean cliffs + Voronoi mesa + strata benches), and `Caldera Atoll` (Coastal Rim Ring falloff + Canyon breach).

2. **[3D SDF Clay Sculpt Studio](./Experimental%20Remastered/SdfSculptEditor/)** (`Experimental Remastered/SdfSculptEditor/`)
   - Real-time hybrid `RGBA16F` 3D volumetric clay sculpting + 24 non-destructive analytical CSG primitives, elastic auto-expanding domain bounds, Dual Surface Nets meshing, and `.sdfspace` interchange.

## Launch

```bash
python3 "Experimental Remastered/SdfSculptEditor/Serve.py"
```

## Validation

```bash
node "Experimental Remastered/TerrainWorldEditor/TerrainWorldValidation.mjs"   # 14/14 suites (mask · SDF · slab · sculpt · spline · erosion physics · presets · Gaea maps)
node "Experimental Remastered/SdfSculptEditor/SdfSculptValidation.mjs"          # 9/9 suites
```
