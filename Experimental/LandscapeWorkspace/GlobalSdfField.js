/*==========================================================================================================================================
                                                            GLOBALSDFELD.JS
==========================================================================================================================================*/
// 🧩 True metre-scale 3D Signed Distance Field (SDF) authoring per layer: independent per-layer XYZ resolutions starting at
//    512 × 512 × 128 and scaling upward (no LOD), per-layer biome & strata settings, sea-level foundations, and volumetric CSG.

import { TerrainCategories } from './TerrainNoise.js';
import {
    TerrainGeneratorCodes,
    TerrainInfluenceDefaults,
    RadialWeight,
    CoastWeight,
    TerrainProfile,
    TerrainProfileGradient
} from './TerrainInfluence.js';

//------------------------------------------------------------------------------------------------------------------------
//                                        PER-LAYER RESOLUTION & BIOME SPECIFICATIONS
//------------------------------------------------------------------------------------------------------------------------

export const HorizontalResolutions = [512, 640, 768, 896, 1024, 1280, 1536, 2048];
export const VerticalResolutions   = [128, 160, 192, 256, 320, 384, 512];
export const CombineModes          = ['union', 'subtract', 'intersect', 'blend'];
export const MaximumPrimitives     = 2048;

export const BiomePresets = {
    desert: {
        Label: 'Desert sandstone',
        Color: '#c8a374',
        CliffColor: '#8d583a',
        Strata: 0.72,
        Roughness: 0.62
    },
    rocky: {
        Label: 'Rocky coastal granite',
        Color: '#8e9892',
        CliffColor: '#4c5558',
        Strata: 0.34,
        Roughness: 0.78
    },
    alpine: {
        Label: 'Alpine ridge & scree',
        Color: '#9aa39b',
        CliffColor: '#565d63',
        Strata: 0.28,
        Roughness: 0.74
    },
    basalt: {
        Label: 'Basalt terraces',
        Color: '#77887e',
        CliffColor: '#373e42',
        Strata: 0.85,
        Roughness: 0.68
    },
    glacial: {
        Label: 'Glacial shelf',
        Color: '#b7cad2',
        CliffColor: '#5f7482',
        Strata: 0.22,
        Roughness: 0.38
    }
};

export const LandformCatalogue = [
    // kept for document compatibility / tests — UI only exposes procedural noises below, default world uses only those
    { Type: 'ridges',  Name: 'Arête ridges',             Amount: 56,  Scale: 2.2, Sharpness: 0.62, Azimuth: 28, Mode: 'union',    Reference: 'mountain-ridges / generator 0' },
    { Type: 'mesa',    Name: 'Mesa / plateau',           Amount: 64,  Scale: 2.0, Sharpness: 0.60, Azimuth: 28, Mode: 'union',    Reference: 'mountain-ridges / generator 1' },
    { Type: 'fault',   Name: 'Fault escarpment',         Amount: 52,  Scale: 2.0, Sharpness: 0.58, Azimuth: 28, Mode: 'union',    Reference: 'mountain-ridges / generator 2' },
    { Type: 'dunes',   Name: 'Aeolian dunes',            Amount: 42,  Scale: 2.6, Sharpness: 0.45, Azimuth: 28, Mode: 'union',    Reference: 'mountain-ridges / generator 3' },
    { Type: 'craters', Name: 'Impact crater bowls',      Amount: 48,  Scale: 2.0, Sharpness: 0.60, Azimuth: 28, Mode: 'subtract', Reference: 'mountain-ridges / generator 4' },
    ...Object.keys(TerrainCategories).map(Type => ({
        Type,
        Name: {
            fractal: 'Fractal (fBm)',
            multifractal: 'Multifractal',
            'ridged-multifractal': 'Ridged multifractal',
            badlands: 'Badlands',
            'ridge-noise': 'Ridge noise',
            'fractal-ridges': 'Fractal ridges',
            'sand-dunes': 'Sand dunes'
        }[Type],
        Amount: 140,
        Scale: 2.0,
        Sharpness: 0.52,
        Azimuth: 0,
        Mode: 'union',
        Reference: 'Volumetric lattice-noise profile'
    })),
    { Type: 'stacks',  Name: 'Rock stacks & hoodoos',    Amount: 54,  Scale: 2.0, Sharpness: 0.45, Azimuth: 35, Mode: 'union',    Reference: 'rock-stacks' },
    { Type: 'strata',  Name: 'Stratigraphic ledges',     Amount: 52,  Scale: 2.0, Sharpness: 0.62, Azimuth: 28, Mode: 'union',    Reference: 'stratigraphic-beds' },
    { Type: 'tunnel',  Name: 'SDF caves & undercuts',    Amount: 30,  Scale: 2.0, Sharpness: 0.52, Azimuth: 28, Mode: 'subtract', Reference: 'sdf-caves' },
    { Type: 'arch',    Name: 'Volumetric arch',          Amount: 90,  Scale: 1.5, Sharpness: 0.50, Azimuth: 0,  Mode: 'union',    Reference: 'Volumetric torus arch' }
];

const CatalogueByType = new Map(LandformCatalogue.map(Entry => [Entry.Type, Entry]));
let CompilationCache = new WeakMap();

export function InvalidateLandforms()
{
    CompilationCache = new WeakMap();
}

export function SnapResolution(Value, Minimum, Maximum, Step = 64)
{
    const Numeric = Math.round(Number(Value) || Minimum);
    const Clamped = Math.max(Minimum, Math.min(Maximum, Numeric));
    return Math.max(Minimum, Math.min(Maximum, Math.round(Clamped / Step) * Step));
}

export function ResolveLayerResolution(Layer, Region = null)
{
    const SourceX = Region?.Resolution?.[0] ?? Layer.ResolutionX ?? Layer.Resolution ?? 512;
    const SourceY = Region?.Resolution?.[1] ?? Layer.ResolutionY ?? Layer.Vertical   ?? 128;
    const SourceZ = Region?.Resolution?.[2] ?? Layer.ResolutionZ ?? Layer.ResolutionX ?? Layer.Resolution ?? 512;
    return [
        SnapResolution(SourceX, 512, 2048, 64),
        SnapResolution(SourceY, 128, 512,  32),
        SnapResolution(SourceZ, 512, 2048, 64)
    ];
}

export function ApplyLayerResolution(Layer, X, Y, Z)
{
    const [RX, RY, RZ] = [
        SnapResolution(X, 512, 2048, 64),
        SnapResolution(Y, 128, 512,  32),
        SnapResolution(Z, 512, 2048, 64)
    ];
    Layer.ResolutionX = RX;
    Layer.ResolutionY = RY;
    Layer.ResolutionZ = RZ;
    Layer.Resolution  = RX;
    Layer.Vertical    = RY;
    if (Array.isArray(Layer.Regions))
    {
        for (const Region of Layer.Regions)
        {
            if (Region.Resolution) Region.Resolution = [RX, RY, RZ];
        }
    }
    return [RX, RY, RZ];
}

export function CreateLandform(Type)
{
    const Entry = CatalogueByType.get(Type);
    if (!Entry) throw Error(`Unknown landform generator: ${Type}`);
    return {
        Type,
        Enabled: true,
        Amount: Entry.Amount,
        Scale: Entry.Scale,
        Sharpness: Entry.Sharpness,
        Azimuth: Entry.Azimuth,
        Mode: Entry.Mode,
        Blend: 8,
        Seed: 4029,
        ...(TerrainGeneratorCodes[Type] !== undefined ? TerrainInfluenceDefaults : {})
    };
}

//------------------------------------------------------------------------------------------------------------------------
//                                            SMOOTH EUCLIDEAN CSG PRIMITIVES (METRES)
//------------------------------------------------------------------------------------------------------------------------

export function SmoothUnion(A, B, Width)
{
    if (Width <= 1e-5) return A < B ? A : B;
    const H = Math.max(Width - Math.abs(A - B), 0) / Width;
    return (A < B ? A : B) - H * H * Width * 0.25;
}

export function SmoothSubtraction(Base, Cut, Width)
{
    return -SmoothUnion(-Base, Cut, Width);
}

export function SmoothIntersection(A, B, Width)
{
    return -SmoothUnion(-A, -B, Width);
}

export function CombineDistance(Distance, Feature, Mode, Width)
{
    if (Mode === 'subtract')  return SmoothSubtraction(Distance, Feature, Width);
    if (Mode === 'intersect') return SmoothIntersection(Distance, Feature, Width);
    if (Mode === 'blend')     return (Distance + Feature) * 0.5;
    return SmoothUnion(Distance, Feature, Width);
}

//------------------------------------------------------------------------------------------------------------------------
//                                           2D VECTOR MASK & REGION SIGNED DISTANCE
//------------------------------------------------------------------------------------------------------------------------

export function CapsuleDistance2D(X, Z, A, B, Radius)
{
    const DX = B[0] - A[0];
    const DZ = B[1] - A[1];
    const Denom = DX * DX + DZ * DZ;
    const T = Denom > 1e-12 ? Math.max(0, Math.min(1, ((X - A[0]) * DX + (Z - A[1]) * DZ) / Denom)) : 0;
    return Math.hypot(X - A[0] - T * DX, Z - A[1] - T * DZ) - Radius;
}

export function MaskDistance(Layer, X, Z)
{
    let Distance = 1e6;
    for (let S = 0; S < Layer.Strokes.length; S++)
    {
        const Stroke = Layer.Strokes[S];
        const Points = Stroke.Points;
        if (!Points.length) continue;
        let StrokeDist = 1e6;
        for (let I = 0; I < Points.length; I++)
        {
            const Prev = Points[I > 0 ? I - 1 : 0];
            const Curr = Points[I];
            const D = CapsuleDistance2D(X, Z, Prev, Curr, Stroke.Radius);
            if (D < StrokeDist) StrokeDist = D;
        }
        Distance = Stroke.Erase ? Math.max(Distance, -StrokeDist) : Math.min(Distance, StrokeDist);
    }
    return Distance;
}

export function RoundedBoxDistance2D(Region, X, Z)
{
    const SpanX = Region.X1 - Region.X0;
    const SpanZ = Region.Z1 - Region.Z0;
    const CornerRadius = Math.min(Region.Edge || 0, SpanX * 0.48, SpanZ * 0.48);
    const HalfX = Math.max(1e-3, SpanX * 0.5 - CornerRadius);
    const HalfZ = Math.max(1e-3, SpanZ * 0.5 - CornerRadius);
    const QX = Math.abs(X - (Region.X0 + Region.X1) * 0.5) - HalfX;
    const QZ = Math.abs(Z - (Region.Z0 + Region.Z1) * 0.5) - HalfZ;
    const Outside = Math.hypot(Math.max(QX, 0), Math.max(QZ, 0));
    const Inside  = Math.min(Math.max(QX, QZ), 0);
    return Outside + Inside - CornerRadius;
}

export function RegionPlanarDistance(Layer, Region, X, Z)
{
    const Mask = MaskDistance(Layer, X, Z);
    const Box  = RoundedBoxDistance2D(Region, X, Z);
    return Math.max(Mask, Box);
}

//------------------------------------------------------------------------------------------------------------------------
//                                           3D VOLUMETRIC PRIMITIVES & REGION STACK
//------------------------------------------------------------------------------------------------------------------------

function SeedSequence(Seed)
{
    let State = Seed >>> 0;
    return () =>
    {
        State = (Math.imul(State, 1664525) + 1013904223) >>> 0;
        return State / 4294967296;
    };
}

export function VolumetricPrimitiveDistance(Primitive, X, Y, Z)
{
    const DX = X - Primitive.Centre[0];
    let   DY = Y - Primitive.Centre[1];
    const DZ = Z - Primitive.Centre[2];
    const LocalX = DX * Primitive.Cosine + DZ * Primitive.Sine;
    const LocalZ = -DX * Primitive.Sine + DZ * Primitive.Cosine;
    const [RX, RY, RZ] = Primitive.Radii;

    const Detail = Primitive.Detail || 0;
    const Phase  = Primitive.Phase || 0;
    let Bound = 1.0;
    if (Detail > 0)
    {
        const Freq = 0.318309886 / Math.max(1.0, Math.min(RX, RY, RZ));
        const Tri = V => { const F = V - Math.floor(V); return (F < 0.5 ? F : 1.0 - F) * 4.0 - 1.0; };
        const W1 = Tri(LocalX * Freq + Phase) * Tri(LocalZ * Freq - Phase) * Tri(DY * Freq * 0.7 + Phase);
        DY += Detail * W1;
        Bound = 1.0 + Detail * Freq * 2.2;
    }

    if (Primitive.Category === 0)
    {
        // Ellipsoid (Lipschitz <= 1 conservative distance in metres)
        const K0 = Math.hypot(LocalX / RX, DY / RY, LocalZ / RZ);
        if (K0 < 1e-6) return -Math.min(RX, RY, RZ);
        const K1 = Math.hypot(LocalX / (RX * RX), DY / (RY * RY), LocalZ / (RZ * RZ));
        return (K0 * (K0 - 1.0) / K1) / Bound;
    }
    if (Primitive.Category === 1)
    {
        // Rounded box / stratigraphic ledge / hoodoo column
        const QX = Math.abs(LocalX) - RX;
        const QY = Math.abs(DY)     - RY;
        const QZ = Math.abs(LocalZ) - RZ;
        const D  = Math.hypot(Math.max(QX, 0), Math.max(QY, 0), Math.max(QZ, 0)) + Math.min(Math.max(QX, QY, QZ), 0) - Primitive.Bevel;
        return D / Bound;
    }
    if (Primitive.Category === 3)
    {
        // Standing volumetric arch (torus in XY local plane, tube radius RZ)
        const Ring = Math.hypot(LocalX, DY) - RX;
        return (Math.hypot(Ring, LocalZ) - RZ) / Bound;
    }
    // Category 4: 3D cave / tunnel capsule along local X
    const ClampedX = Math.max(-RX, Math.min(RX, LocalX));
    return (Math.hypot(LocalX - ClampedX, DY, LocalZ) - RY) / Bound;
}

export function CompileRegion(Layer, Region)
{
    const Cached = CompilationCache.get(Region);
    const Operations = Layer.Landforms?.length ? [...Layer.Landforms, ...(Region.Landforms || [])] : (Region.Landforms || []);
    if (Cached &&
        Cached.Depth === Layer.Depth &&
        Cached.Edge === (Region.Edge || 0) &&
        Cached.Caves === Boolean(Layer.Caves) &&
        Cached.Bounds[0] === Region.X0 && Cached.Bounds[1] === Region.Z0 &&
        Cached.Bounds[2] === Region.X1 && Cached.Bounds[3] === Region.Z1 &&
        Cached.Operations.length === Operations.length &&
        Cached.Operations.every((Prev, I) => JSON.stringify(Prev) === JSON.stringify(Operations[I])))
    {
        return Cached;
    }

    const SpanX   = Math.max(8, Region.X1 - Region.X0);
    const SpanZ   = Math.max(8, Region.Z1 - Region.Z0);
    const CentreX = (Region.X0 + Region.X1) * 0.5;
    const CentreZ = (Region.Z0 + Region.Z1) * 0.5;
    const Top     = 0;

    let MaximumY = Top + 16;
    let MinimumY = -Layer.Depth - 12;
    const Steps       = [];
    const HeightSteps = [];
    const VolumeSteps = [];

    for (const Operation of Operations)
    {
        if (!Operation.Enabled) continue;
        const Random = SeedSequence(Operation.Seed);
        const Angle  = (Operation.Azimuth || 0) * Math.PI / 180;
        const Cosine = Math.cos(Angle);
        const Sine   = Math.sin(Angle);
        const Primitives = [];

        if (TerrainGeneratorCodes[Operation.Type] !== undefined)
        {
            const Settings  = { ...TerrainInfluenceDefaults, ...Operation };
            const Category  = TerrainGeneratorCodes[Operation.Type];
            const Frequency = Operation.Scale * 3.0 / Math.min(SpanX, SpanZ);
            const Radius    = Math.min(SpanX, SpanZ) * 0.5 * Settings.InfluenceRadius;
            const Primitive = {
                Category,
                Centre: [CentreX + Settings.InfluenceX * SpanX * 0.5, Top, CentreZ + Settings.InfluenceZ * SpanZ * 0.5],
                Radii: [Radius, Operation.Amount, Settings.InfluenceFalloff],
                Cosine,
                Sine,
                Bevel: Frequency,
                Detail: Operation.Sharpness,
                Phase: Operation.Seed,
                Influence: Settings.Influence,
                VerticalFrequency: Math.min(Frequency * 0.2, 0.45 / Math.max(1, Operation.Amount * TerrainProfileGradient(Category, Operation.Sharpness)))
            };
            Primitives.push(Primitive);
            const Step = { Kind: 'height', Mode: Operation.Mode, Blend: Operation.Blend, Primitives };
            Steps.push(Step);
            HeightSteps.push(Step);
            if (Operation.Mode !== 'subtract') MaximumY = Math.max(MaximumY, Top + Operation.Amount + Operation.Blend * 0.5 + 16);
            else MinimumY = Math.min(MinimumY, -Layer.Depth - Operation.Amount * 0.5 - 12);
        }
        else
        {
            const Count = (Operation.Type === 'tunnel' || Operation.Type === 'arch') ? 1 : Math.min(4, Math.max(1, Math.round(Operation.Scale)));
            const CellX = SpanX / Count;
            const CellZ = SpanZ / Count;
            const AddVol = (Category, Centre, Radii, Bevel = 0) =>
            {
                const Detail = (Operation.Type === 'arch' || Operation.Type === 'tunnel')
                    ? 0
                    : Math.min(...Radii) * 0.12 * Operation.Sharpness;
                const Phase = (Operation.Seed % 997) * 0.1;
                const BoundsMaxR = (Category === 3 ? Radii[0] + Radii[2] : Math.max(Radii[0], Radii[1], Radii[2])) + Bevel + Detail * 1.4;
                Primitives.push({
                    Category, Centre, Radii, Cosine, Sine, Bevel, Detail, Phase,
                    AABB: [
                        Centre[0] - BoundsMaxR, Centre[1] - BoundsMaxR, Centre[2] - BoundsMaxR,
                        Centre[0] + BoundsMaxR, Centre[1] + BoundsMaxR, Centre[2] + BoundsMaxR
                    ]
                });
                if (Operation.Mode !== 'subtract')
                {
                    MaximumY = Math.max(MaximumY, Centre[1] + BoundsMaxR + Operation.Blend * 0.5 + 8);
                }
                MinimumY = Math.min(MinimumY, Centre[1] - BoundsMaxR - 8);
            };

            for (let Z = 0; Z < Count; Z++)
            {
                for (let X = 0; X < Count; X++)
                {
                    const PX = Region.X0 + (X + 0.5 + (Random() - 0.5) * 0.45) * CellX;
                    const PZ = Region.Z0 + (Z + 0.5 + (Random() - 0.5) * 0.45) * CellZ;
                    const H  = Operation.Amount * (0.72 + Random() * 0.28);
                    const R  = Math.min(CellX, CellZ) * (0.28 + Random() * 0.14);
                    if (Operation.Type === 'stacks')
                    {
                        AddVol(1, [PX, Top + H * 0.36, PZ], [R * 0.28, H * 0.58, R * 0.28], H * 0.04);
                        AddVol(0, [PX + H * 0.08, Top + H * 0.88, PZ], [R * 0.52, H * 0.22, R * 0.48], 0);
                    }
                    else if (Operation.Type === 'strata')
                    {
                        for (let Tier = 0; Tier < 4; Tier++)
                        {
                            const ScaleR = Tier % 2 === 0 ? 1.08 : 0.84;
                            AddVol(1, [PX, Top + H * (Tier + 0.35) * 0.23, PZ], [R * ScaleR, H * 0.12, R * ScaleR], H * 0.025);
                        }
                    }
                    else if (Operation.Type === 'tunnel')
                    {
                        AddVol(4, [CentreX, Top + H * 0.12, CentreZ], [Math.hypot(SpanX, SpanZ) * 0.45, H * 0.48, H * 0.48], 0);
                    }
                    else
                    {
                        // Standing volumetric arch
                        AddVol(3, [CentreX, Top + H * 0.28, CentreZ], [H * 0.68, H * 0.68, H * 0.22], 0);
                    }
                }
            }
            const Step = { Kind: 'volume', Mode: Operation.Mode, Blend: Operation.Blend, Primitives };
            Steps.push(Step);
            VolumeSteps.push(Step);
        }
    }

    if (Layer.Caves)
    {
        const R = Math.min(SpanX, SpanZ, Layer.Depth) * 0.42;
        VolumeSteps.push({
            Kind: 'volume',
            Mode: 'subtract',
            Blend: 6,
            Primitives: [{
                Category: 0,
                Centre: [CentreX, -Layer.Depth * 0.2, CentreZ],
                Radii: [R * 1.25, R * 0.75, R * 1.1],
                Cosine: 1, Sine: 0, Bevel: 0, Detail: R * 0.08, Phase: 1.7,
                AABB: [CentreX - R * 1.5, -Layer.Depth * 0.2 - R, CentreZ - R * 1.5, CentreX + R * 1.5, -Layer.Depth * 0.2 + R, CentreZ + R * 1.5]
            }]
        });
    }

    // Account for 3D sculpt strokes expanding vertical bounds (e.g. tall sculpted arches/pillars)
    if (Array.isArray(Region.SculptStrokes))
    {
        for (const Stroke of Region.SculptStrokes)
        {
            const Pad = (Stroke.Radius || 10) + 12;
            for (const Pt of Stroke.Points || [])
            {
                MinimumY = Math.min(MinimumY, Pt[1] - Pad);
                MaximumY = Math.max(MaximumY, Pt[1] + Pad);
            }
        }
    }

    const Result = {
        Depth: Layer.Depth,
        Edge: Region.Edge || 0,
        Caves: Boolean(Layer.Caves),
        Steps,
        HeightSteps,
        VolumeSteps,
        MinimumY,
        MaximumY,
        Bounds: [Region.X0, Region.Z0, Region.X1, Region.Z1],
        Operations: Operations.map(Op => ({ ...Op }))
    };
    CompilationCache.set(Region, Result);
    return Result;
}

export function LayerVerticalBounds(Layer, Region)
{
    const Compiled = CompileRegion(Layer, Region);
    return [Compiled.MinimumY, Compiled.MaximumY];
}

//------------------------------------------------------------------------------------------------------------------------
//                                  SURFACE HEIGHT + TRUE METRIC 3D SDF EVALUATION
//------------------------------------------------------------------------------------------------------------------------

export function EvaluateColumnSurfaceHeight(Compiled, PlanarDistance, X, Z)
{
    let SurfaceY = 0.0;
    const Feather = CoastWeight(PlanarDistance, Compiled.Edge);
    if (Feather <= 0 && !Compiled.HeightSteps.length) return 0.0;

    for (let S = 0; S < Compiled.HeightSteps.length; S++)
    {
        const Step = Compiled.HeightSteps[S];
        const Prim = Step.Primitives[0];
        if (!Prim) continue;
        const Weight = RadialWeight(Prim, X, Z) * Feather;
        if (Weight <= 1e-5) continue;
        const Profile = TerrainProfile(Prim, X, 0, Z);
        const Relief  = Prim.Radii[1] * Profile;
        if (Step.Mode === 'subtract')
        {
            SurfaceY -= Weight * Relief;
        }
        else if (Step.Mode === 'intersect')
        {
            SurfaceY = SurfaceY * (1 - Weight) + Math.min(SurfaceY, Relief) * Weight;
        }
        else if (Step.Mode === 'blend')
        {
            SurfaceY = SurfaceY * (1 - 0.5 * Weight) + Relief * (0.5 * Weight);
        }
        else
        {
            // Smooth additive/union relief above sea-level foundation
            const Combined = -SmoothUnion(-SurfaceY, -Relief, Step.Blend);
            SurfaceY += Weight * (Combined - SurfaceY);
        }
    }
    return SurfaceY;
}

export function ApplySculptStrokePoint(Phi, Stroke, X, Y, Z)
{
    const Points = Stroke.Points;
    if (!Points || !Points.length) return Phi;
    const Radius   = Math.max(0.5, Number(Stroke.Radius) || 8);
    const Strength = Math.max(0.05, Math.min(1.0, Number(Stroke.Strength) || 0.5));
    const Falloff  = Math.max(0.05, Math.min(1.0, Number(Stroke.Falloff ?? 0.55)));
    const BlendW   = Radius * (0.25 + 0.65 * Falloff);

    let CapsuleDist = 1e6;
    for (let I = 0; I < Points.length; I++)
    {
        const A = Points[I > 0 ? I - 1 : 0];
        const B = Points[I];
        const ABX = B[0] - A[0], ABY = B[1] - A[1], ABZ = B[2] - A[2];
        const L2  = ABX * ABX + ABY * ABY + ABZ * ABZ;
        const T   = L2 > 1e-10 ? Math.max(0, Math.min(1, ((X - A[0]) * ABX + (Y - A[1]) * ABY + (Z - A[2]) * ABZ) / L2)) : 0;
        const D   = Math.hypot(X - A[0] - T * ABX, Y - A[1] - T * ABY, Z - A[2] - T * ABZ);
        if (D < CapsuleDist) CapsuleDist = D;
    }

    if (Stroke.Mode === 'flatten' && Array.isArray(Stroke.PlaneNormal) && Array.isArray(Stroke.PlanePoint))
    {
        if (CapsuleDist >= Radius) return Phi;
        const U = 1.0 - CapsuleDist / Radius;
        const W = Strength * Math.pow(U * U * (3 - 2 * U), 1.2 - 0.6 * Falloff);
        const [NX, NY, NZ] = Stroke.PlaneNormal;
        const [PX, PY, PZ] = Stroke.PlanePoint;
        const PlanePhi = (X - PX) * NX + (Y - PY) * NY + (Z - PZ) * NZ;
        return Phi * (1 - W) + PlanePhi * W;
    }

    if (Stroke.Mode === 'smooth')
    {
        // Replay approximation when resolution changes: gently relax high-curvature spikes inside capsule
        if (CapsuleDist >= Radius) return Phi;
        const U = 1.0 - CapsuleDist / Radius;
        const W = 0.25 * Strength * U * U * (3 - 2 * U);
        return Phi * (1 - W);
    }

    const EffectiveR = Radius * (0.45 + 0.55 * Strength);
    const BrushSphere = CapsuleDist - EffectiveR;
    if (Stroke.Mode === 'remove' || Stroke.Mode === 'carve')
    {
        return SmoothSubtraction(Phi, BrushSphere, BlendW);
    }
    return SmoothUnion(Phi, BrushSphere, BlendW);
}

export function EvaluateRegionSdfMetres(Layer, Region, X, Y, Z, PrecomputedColumn = null)
{
    const Compiled = CompileRegion(Layer, Region);
    const Planar   = PrecomputedColumn ? PrecomputedColumn.Planar : RegionPlanarDistance(Layer, Region, X, Z);
    const SurfaceY = PrecomputedColumn ? PrecomputedColumn.SurfaceY : EvaluateColumnSurfaceHeight(Compiled, Planar, X, Z);
    const SlopeNorm = PrecomputedColumn ? PrecomputedColumn.SlopeInv : 1.0;

    // 1. Sea-level-anchored landmass slab with slope-normalized vertical signed distance (true metres)
    const TopPhi    = (Y - SurfaceY) * SlopeNorm;
    const BottomPhi = -Y - Layer.Depth;
    const VerticalPhi = Math.max(TopPhi, BottomPhi);

    // Exact Euclidean exterior corner intersection between horizontal coast mask and vertical bounds
    const Outside = Math.hypot(Math.max(Planar, 0), Math.max(VerticalPhi, 0));
    const Inside  = Math.min(Math.max(Planar, VerticalPhi), 0);
    let Phi = Outside + Inside;

    // 2. True 3D volumetric CSG features (arches, tunnels, caves, overhang ledges, rock hoodoos)
    for (let S = 0; S < Compiled.VolumeSteps.length; S++)
    {
        const Step = Compiled.VolumeSteps[S];
        let Feature = 1e6;
        for (let P = 0; P < Step.Primitives.length; P++)
        {
            const D = VolumetricPrimitiveDistance(Step.Primitives[P], X, Y, Z);
            if (D < Feature) Feature = D;
        }
        // Clip additive volumetric features softly to the region horizontal bounds so they stay owned by their layer
        if (Step.Mode !== 'subtract')
        {
            Feature = Math.max(Feature, RoundedBoxDistance2D(Region, X, Z));
        }
        Phi = CombineDistance(Phi, Feature, Step.Mode, Step.Blend);
    }

    // 3. Replay any saved 3D sculpt strokes belonging to this region
    if (Array.isArray(Region.SculptStrokes) && Region.SculptStrokes.length > 0)
    {
        const BoundsClip = RoundedBoxDistance2D({ ...Region, Edge: 4 }, X, Z) - 2.0;
        for (let K = 0; K < Region.SculptStrokes.length; K++)
        {
            Phi = ApplySculptStrokePoint(Phi, Region.SculptStrokes[K], X, Y, Z);
        }
        Phi = Math.max(Phi, BoundsClip);
    }

    return Phi;
}

export function EvaluateLayerSdfMetres(Layer, X, Y, Z)
{
    if (!Layer.Visible || !Array.isArray(Layer.Regions) || !Layer.Regions.length) return 1e6;
    let Best = 1e6;
    for (let R = 0; R < Layer.Regions.length; R++)
    {
        const D = EvaluateRegionSdfMetres(Layer, Layer.Regions[R], X, Y, Z);
        if (D < Best) Best = D;
    }
    return Best;
}

//------------------------------------------------------------------------------------------------------------------------
//                                           DOCUMENT VALIDATION & WORLD PRESETS
//------------------------------------------------------------------------------------------------------------------------

export function SegmentCount(Layers)
{
    return Layers.reduce((Count, Layer) => Count + (Layer.Strokes || []).reduce((Sum, S) => Sum + S.Points.length, 0), 0);
}

export function ValidateLayerStack(Layer)
{
    const Ops = Layer.Landforms || [];
    if (!Array.isArray(Ops) || Ops.length > 8) throw Error('Maximum eight landform operations per stack');
    for (const Op of Ops)
    {
        if (!Op || !CatalogueByType.has(Op.Type) || typeof Op.Enabled !== 'boolean' || !CombineModes.includes(Op.Mode))
            throw Error('Invalid landform operation');
    }
}

export function ValidateDocument(Document)
{
    if (!Document || !['FrontierLandscape/1', 'FrontierLandscape/2'].includes(Document.Format) ||
        !Array.isArray(Document.Layers) || !Document.Layers.length || Document.Layers.length > 16)
    {
        throw Error('Unsupported landscape document');
    }

    for (let LI = 0; LI < Document.Layers.length; LI++)
    {
        const Layer = Document.Layers[LI];
        if (typeof Layer.Name !== 'string' || typeof Layer.Visible !== 'boolean' ||
            !Number.isFinite(Layer.Depth) || Layer.Depth < 6 || Layer.Depth > 10000 ||
            !Array.isArray(Layer.Strokes) || !Array.isArray(Layer.Regions) || Layer.Regions.length > 16)
        {
            throw Error('Invalid landscape layer');
        }
        EnsureLayerId(Layer, LI);
        ValidateLayerStack(Layer);

        // Enforce resolution >= 512 x 512 x 128 (upgrade legacy documents transparently)
        const [RX, RY, RZ] = ResolveLayerResolution(Layer);
        ApplyLayerResolution(Layer, RX, RY, RZ);

        const BiomeKey = Layer.Biome && BiomePresets[Layer.Biome] ? Layer.Biome : 'desert';
        Layer.Biome      = BiomeKey;
        Layer.Color      = /^#[0-9a-f]{6}$/i.test(Layer.Color || '') ? Layer.Color : BiomePresets[BiomeKey].Color;
        Layer.CliffColor = /^#[0-9a-f]{6}$/i.test(Layer.CliffColor || '') ? Layer.CliffColor : BiomePresets[BiomeKey].CliffColor;
        Layer.Strata     = Number.isFinite(Layer.Strata) ? Math.max(0, Math.min(1, Layer.Strata)) : BiomePresets[BiomeKey].Strata;
        Layer.Roughness  = Number.isFinite(Layer.Roughness) ? Math.max(0, Math.min(1, Layer.Roughness)) : BiomePresets[BiomeKey].Roughness;

        for (const Stroke of Layer.Strokes)
        {
            if (!Number.isFinite(Stroke.Radius) || Stroke.Radius <= 0 || !Array.isArray(Stroke.Points) || !Stroke.Points.length)
                throw Error('Invalid mask stroke');
        }
        for (const Region of Layer.Regions)
        {
            if (['X0', 'X1', 'Z0', 'Z1'].some(Key => !Number.isFinite(Region[Key])) || Region.X1 <= Region.X0 || Region.Z1 <= Region.Z0)
                throw Error('Invalid region bounds');
            if (Region.Landforms) ValidateLayerStack({ Landforms: Region.Landforms });
            if (!Array.isArray(Region.SculptStrokes)) Region.SculptStrokes = [];
        }
    }

    if (SegmentCount(Document.Layers) > 1024) throw Error('Vector mask sample limit exceeded (max 1024 points).');
    InvalidateLandforms();
    return Document;
}

const DefaultLayerOrder = ['desert', 'rocky', 'basalt', 'glacial', 'alpine'];
let NextLayerSerial = 1;

export function EnsureLayerId(Layer, FallbackIndex = 0)
{
    if (typeof Layer.Id !== 'string' || !Layer.Id.trim())
    {
        Layer.Id = `layer-${FallbackIndex + 1}-${NextLayerSerial++}`;
    }
    return Layer.Id;
}

export function ColorToRgb01(Color, Fallback = [0.76, 0.65, 0.48])
{
    if (Array.isArray(Color) && Color.length >= 3)
    {
        return [
            Math.max(0, Math.min(1, Number(Color[0]) || 0)),
            Math.max(0, Math.min(1, Number(Color[1]) || 0)),
            Math.max(0, Math.min(1, Number(Color[2]) || 0))
        ];
    }
    if (typeof Color === 'string')
    {
        const Hex = Color.trim().replace(/^#/, '');
        if (/^[0-9a-fA-F]{6}$/.test(Hex))
        {
            return [
                parseInt(Hex.slice(0, 2), 16) / 255,
                parseInt(Hex.slice(2, 4), 16) / 255,
                parseInt(Hex.slice(4, 6), 16) / 255
            ];
        }
    }
    return [...Fallback];
}

export function CreateLayer(Index = 0, BiomeOverride = null)
{
    const BiomeKey = BiomeOverride || DefaultLayerOrder[Index % DefaultLayerOrder.length];
    const Biome    = BiomePresets[BiomeKey] || BiomePresets.desert;
    const Names    = ['Desert coast', 'Rocky isles', 'Glacial shelf', 'Alpine spur', 'Coastal headland'];
    const IsDesert = BiomeKey === 'desert' && Index === 0;
    const ResXZ    = IsDesert ? 1024 : 512;
    const ResY     = IsDesert ? 256  : 128;

    return {
        Id: `layer-${Index + 1}-${NextLayerSerial++}`,
        Name: Names[Index] || `Layer ${String(Index + 1).padStart(2, '0')}`,
        Biome: BiomeKey,
        Color: Biome.Color,
        CliffColor: Biome.CliffColor,
        Strata: Biome.Strata,
        Roughness: Biome.Roughness,
        Visible: true,
        Depth: 16,
        Caves: false,
        ResolutionX: ResXZ,
        ResolutionY: ResY,
        ResolutionZ: ResXZ,
        Resolution: ResXZ,
        Vertical: ResY,
        Landforms: [],
        Strokes: [],
        Regions: []
    };
}

export function CreateDefaultWorld()
{
    // Layer 0: Desert coast at 1024 × 1024 × 256 — procedural noise only, thin 16 m seabed extrusion
    const Desert = CreateLayer(0, 'desert');
    ApplyLayerResolution(Desert, 1024, 256, 1024);
    Desert.Strokes = [
        { Points: [[-420, 80], [-380, -40], [-280, -100], [-170, -90], [-105, -20], [-130, 75], [-220, 135], [-320, 160]], Radius: 112, Erase: false },
        { Points: [[-350, 30], [-230, 25], [-170, 70]], Radius: 102, Erase: false },
        { Points: [[-180, 10], [-90, 60], [-30, 15], [30, -60]], Radius: 68, Erase: false },
        { Points: [[-180, 200], [-100, 240]], Radius: 56, Erase: false },
        { Points: [[-385, -170], [-370, -110]], Radius: 58, Erase: true }
    ];
    Desert.Regions = [{ X0: -560, Z0: -250, X1: 135, Z1: 335, Edge: 62, SculptStrokes: [] }];
    Desert.Landforms = [
        { ...CreateLandform('ridged-multifractal'), Name: 'Mountain ridges', Amount: 185, Scale: 1.45, InfluenceX: -0.25, InfluenceZ: -0.15, InfluenceRadius: 1.3, InfluenceFalloff: 0.95, Blend: 0, Seed: 4029 },
        { ...CreateLandform('sand-dunes'), Name: 'Coastal dunes', Amount: 48, Scale: 3.7, Azimuth: -20, InfluenceX: 0.35, InfluenceZ: 0.42, InfluenceRadius: 0.68, InfluenceFalloff: 1.0, Blend: 0, Seed: 8119 }
    ];

    // Layer 1: Rocky isles at 512 × 512 × 128 — procedural only
    const Rocky = CreateLayer(1, 'rocky');
    ApplyLayerResolution(Rocky, 512, 128, 512);
    Rocky.Strokes = [
        { Points: [[230, 0], [285, 25], [300, 100], [240, 165]], Radius: 76, Erase: false },
        { Points: [[350, -115], [395, -85]], Radius: 42, Erase: false },
        { Points: [[150, 295], [210, 320]], Radius: 40, Erase: false }
    ];
    Rocky.Regions = [{ X0: 65, Z0: -190, X1: 465, Z1: 395, Edge: 46, SculptStrokes: [] }];
    Rocky.Landforms = [
        { ...CreateLandform('fractal-ridges'), Name: 'Granite crags', Amount: 145, Scale: 1.3, InfluenceRadius: 1.6, Blend: 0, Seed: 1771 }
    ];

    // Layer 2: Glacial shelf at 512 × 512 × 128 — procedural only, thin seabed
    const Basalt = CreateLayer(2, 'basalt');
    ApplyLayerResolution(Basalt, 512, 128, 512);
    Basalt.Strokes = [
        { Points: [[-150, -395], [-65, -440], [30, -405], [115, -430], [180, -380]], Radius: 70, Erase: false }
    ];
    Basalt.Regions = [{ X0: -250, Z0: -540, X1: 280, Z1: -270, Edge: 50, SculptStrokes: [] }];
    Basalt.Landforms = [
        { ...CreateLandform('multifractal'), Name: 'Basalt headland', Amount: 125, Scale: 2.1, InfluenceRadius: 1.4, Blend: 0, Seed: 6032 }
    ];

    return [Desert, Rocky, Basalt];
}

export function CreateAppliedShowcaseWorld()
{
    // 5-layer showcase using only procedural noise landforms (no blocky arches/tunnels/strata/stacks) + thin seabed
    const Specs = [
        { Name: 'Desert dunes',     Biome: 'desert',  Res: [1024, 256, 1024], Ops: [{ ...CreateLandform('ridged-multifractal'), Amount: 165, Scale: 1.6, Sharpness: 0.6, Blend: 0, Seed: 4029 }, { ...CreateLandform('sand-dunes'), Amount: 44, Scale: 3.4, Azimuth: 35, Sharpness: 0.35, Blend: 0, Seed: 8119 }] },
        { Name: 'Rocky ridges',     Biome: 'rocky',   Res: [512,  128, 512],  Ops: [{ ...CreateLandform('fractal-ridges'), Amount: 155, Scale: 1.8, Sharpness: 0.72, Blend: 0, Seed: 1771 }] },
        { Name: 'Aeolian erg',      Biome: 'desert',  Res: [512,  128, 512],  Ops: [{ ...CreateLandform('sand-dunes'), Amount: 58, Scale: 3.2, Azimuth: 35, Sharpness: 0.35, Blend: 0, Seed: 9181 }] },
        { Name: 'Multifractal headland', Biome: 'basalt', Res: [512, 128, 512], Ops: [{ ...CreateLandform('multifractal'), Amount: 130, Scale: 2.0, Blend: 0, Seed: 6032 }, { ...CreateLandform('badlands'), Amount: 90, Scale: 1.8, Sharpness: 0.6, Blend: 0, Seed: 2904 }] },
        { Name: 'Coastal ridges',   Biome: 'glacial', Res: [512,  128, 512],  Ops: [{ ...CreateLandform('fractal-ridges'), Amount: 85, Scale: 1.5, Blend: 0, Seed: 3101 }, { ...CreateLandform('ridge-noise'), Amount: 110, Scale: 1.6, Sharpness: 0.5, Blend: 0, Seed: 4420 }] }
    ];
    return Specs.map((Spec, Index) =>
    {
        const Layer = CreateLayer(Index, Spec.Biome);
        Layer.Name = Spec.Name;
        ApplyLayerResolution(Layer, ...Spec.Res);
        const CX = (Index % 3 - 1) * 520;
        const CZ = (Math.floor(Index / 3) - 0.5) * 520;
        Layer.Strokes = [{ Points: [[CX - 35, CZ - 10], [CX + 35, CZ + 15]], Radius: 158, Erase: false }];
        Layer.Regions = [{ X0: CX - 220, Z0: CZ - 205, X1: CX + 220, Z1: CZ + 205, Edge: 36, SculptStrokes: [] }];
        Layer.Landforms = Spec.Ops;
        return Layer;
    });
}
