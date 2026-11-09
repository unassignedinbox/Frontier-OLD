# Frontier

Frontier's interactive authoring workspaces, starting from the SDF Terrain Studio base
(`9331bf34`) plus a physically-based erosion system.

## Experimental Remastered

- **SDF Terrain Studio** (`Experimental Remastered/TerrainWorldEditor/`) — infinite sparse 2D mask world, independent toggleable Terrain Bounds regions, thin-slab SDF, multi-terrain 3D ocean landscape, per-layer edge→center / cliff falloff, spline landforms, sculpting, **physically-based multi-process erosion** with 19 presets, water-body rendering and `.OBJ`/`.STL` export.
- **SDF Sculpt Studio** (`Experimental Remastered/SdfSculptEditor/`) — real-time RGBA16F volumetric clay sculpting + CSG primitives.

Erosion references live in
[`Experimental Remastered/TerrainWorldEditor/ErosionPhysics.md`](Experimental%20Remastered/TerrainWorldEditor/ErosionPhysics.md).

## Quick start

```bash
python3 "Experimental Remastered/TerrainWorldEditor/Serve.py"   # Terrain Studio, port 8088
python3 "Experimental Remastered/SdfSculptEditor/Serve.py"      # Clay Studio,   port 8087
```

## Validation

```bash
cd "Experimental Remastered/TerrainWorldEditor" && node TerrainWorldValidation.mjs
```
