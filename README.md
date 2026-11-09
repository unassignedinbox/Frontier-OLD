# Frontier

Frontier's interactive authoring workspaces.

## Experimental Remastered

- **SDF Terrain Studio** (`Experimental Remastered/TerrainWorldEditor/`) — infinite sparse 2D mask world, independent toggleable Terrain Bounds regions, thin-slab SDF, multi-terrain 3D ocean landscape, per-layer edge→center / cliff falloff, spline landforms, sculpting and .OBJ/.STL export.
- **SDF Sculpt Studio** (`Experimental Remastered/SdfSculptEditor/`) — real-time RGBA16F volumetric clay sculpting + CSG primitives.

## Quick start

```bash
python3 "Experimental Remastered/TerrainWorldEditor/Serve.py"   # Terrain Studio, port 8088
python3 "Experimental Remastered/SdfSculptEditor/Serve.py"      # Clay Studio,   port 8087
```
