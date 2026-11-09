/*==========================================================================================================================================
                                                        EROSIONENGINE.JS
==========================================================================================================================================*/
/**
 * Physically-Parameterised Multi-Process Terrain Erosion Engine for Frontier's SDF Terrain Studio.
 *
 * DESIGN RESEARCH (see ErosionPhysics.md for the full literature review & units):
 *   The reference studio (arena/68c84c0d-frontier) erodes with a *particle* advection scheme:
 *   N droplets with a heuristic "sediment capacity", traced one at a time. It looks good, but it
 *   is not geomorphically calibrated — it has no drainage network, no base level, no mass
 *   balance, and its parameters do not map onto anything measurable. That is why the reference
 *   floods: nothing in it knows what a valley floor or a lake surface is.
 *
 *   This engine uses the two end-member models that actual landscape-evolution codes use:
 *
 *   1. DETACHMENT-LIMITED stream-power incision (Howard 1994, Whipple & Tucker 1999):
 *
 *          dz/dt = U - K * A^m * S^n
 *
 *      solved with the implicit, O(n) topological sweep of Braun & Willett (2013) - the same
 *      scheme as FastScape. Inherently stable for large dt, and it produces the correct
 *      slope-area concavity (theta = m/n), so river long-profiles and knickpoints are real.
 *
 *   2. TRANSPORT-LIMITED sediment routing (Braun & Cordonnier 2018), which supplies the
 *      deposition half of the cycle: every cell has a carrying capacity, excess sediment
 *      aggrades (alluvial fans, valley fill, lake deltas), deficit incises.
 *
 *   On top of that base, the processes you asked for:
 *      - Thermal / talus relaxation  : mass-conserving scree at a critical repose angle.
 *      - Aeolian saltation           : windward deflation + lee deposition + dune bedforms.
 *      - Glacial (shallow-ice, Glen n=3): ice discharge -> U-shaped troughs, cirques.
 *      - Chemical weathering         : bedrock -> regolith conversion + solute export.
 *      - Frost / salt pitting        : stochastic shattering of exposed rock faces.
 *
 *   Every process is mass-conserving or explicitly mass-accounted, deterministic for a given
 *   seed, and independently toggleable so the studio can simulate one process, a group of
 *   similar layers sharing one weather regime, or the whole stack.
 *
 * UNITS: metres for elevation/length, years for time, m/yr for rates. A^m uses A in m^2.
 */

/*------------------------------------------------------------------------------------------------------------------------------------------
                                                        SMALL MATH / RNG / NOISE
------------------------------------------------------------------------------------------------------------------------------------------*/
const Clamp = (V, Lo, Hi) => (V < Lo ? Lo : V > Hi ? Hi : V);
const Mix = (A, B, T) => A + (B - A) * T;
const Smoothstep = (E0, E1, X) => (E0 === E1 ? (X < E0 ? 0 : 1) : Clamp((X - E0) / (E1 - E0), 0, 1) ** 2 * (3 - 2 * Clamp((X - E0) / (E1 - E0), 0, 1)));
const Deg2Rad = Math.PI / 180.0;

/** Deterministic 32-bit PRNG (mulberry32). Seeded so a preset always reproduces bit-identically. */
export function CreateRandom(Seed = 4029)
{
    let State = (Seed | 0) >>> 0;
    return function Next()
    {
        State = (State + 0x6D2B79F5) >>> 0;
        let T = State;
        T = Math.imul(T ^ (T >>> 15), T | 1);
        T ^= T + Math.imul(T ^ (T >>> 7), T | 61);
        return ((T ^ (T >>> 14)) >>> 0) / 4294967296;
    };
}

function Hash2(Ix, Iz, Seed)
{
    let H = Math.imul(Ix | 0, 374761393) ^ Math.imul(Iz | 0, 668265263) ^ Math.imul(Seed | 0, 2246822519);
    H = Math.imul(H ^ (H >>> 13), 1274126177);
    return ((H ^ (H >>> 16)) >>> 0) / 4294967296;
}

/** Bilinear value noise in [0,1]; deterministic across platforms (pure integer hashing). */
export function ValueNoise2D(X, Z, Seed = 4029)
{
    const Xi = Math.floor(X), Zi = Math.floor(Z);
    const Fx = X - Xi, Fz = Z - Zi;
    const Ux = Fx * Fx * (3 - 2 * Fx);
    const Uz = Fz * Fz * (3 - 2 * Fz);
    const A = Hash2(Xi, Zi, Seed);
    const B = Hash2(Xi + 1, Zi, Seed);
    const C = Hash2(Xi, Zi + 1, Seed);
    const D = Hash2(Xi + 1, Zi + 1, Seed);
    return Mix(Mix(A, B, Ux), Mix(C, D, Ux), Uz);
}

/** Fractal Brownian motion, normalised to [0,1]. */
export function FbmNoise2D(X, Z, Octaves = 4, Seed = 4029)
{
    let Sum = 0, Amp = 0.5, Freq = 1.0, Norm = 0;
    for (let I = 0; I < Octaves; ++I)
    {
        Sum += ValueNoise2D(X * Freq, Z * Freq, Seed + I * 131) * Amp;
        Norm += Amp;
        Amp *= 0.5;
        Freq *= 2.03;
    }
    return Sum / Math.max(1e-6, Norm);
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                              PARAMETER NORMALISATION (SINGLE SOURCE OF TRUTH)
------------------------------------------------------------------------------------------------------------------------------------------*/
export const CLIMATE_PATTERNS = [
    ['Uniform Rainfall', 'uniform'],
    ['Orographic (Rain Shadow)', 'orographic'],
    ['Monsoon Belt (Seasonal)', 'monsoon'],
    ['Humid Coastal', 'coastal'],
    ['Arid Interior', 'arid'],
    ['Polar / Periglacial', 'polar'],
    ['Storm Tracks (Banded)', 'stormTracks'],
    ['Altitude Bands (Snowline)', 'altitudeBands']
];

export const PROCESS_KEYS = ['uplift', 'streamPower', 'deposition', 'thermal', 'aeolian', 'glacial', 'chemical', 'pitting'];

/** Human-facing metadata used by the Inspector UI. */
export const PROCESS_META = {
    uplift:      { label: 'Tectonic Uplift',      badge: 'TECT', auto: true },
    streamPower: { label: 'Stream-Power Incision', badge: 'FLUVIAL' },
    deposition:  { label: 'Sediment Routing',      badge: 'DEPOSIT' },
    thermal:     { label: 'Thermal Talus',         badge: 'MASS-WASTING' },
    aeolian:     { label: 'Aeolian Saltation',     badge: 'WIND' },
    glacial:     { label: 'Glacial Ice Flow',      badge: 'GLACIAL' },
    chemical:    { label: 'Chemical Weathering',   badge: 'CHEM' },
    pitting:     { label: 'Frost & Salt Pitting',  badge: 'MICRO' }
};

const Num = (V, Fallback) => (Number.isFinite(Number(V)) ? Number(V) : Fallback);

/**
 * Fills in every parameter with a physically sane default. Unknown keys are preserved so the
 * studio can round-trip future parameters through JSON without data loss.
 */
export function NormalizeErosionParams(Input = {})
{
    const P = Input || {};
    const Climate = P.climate || {};
    const SP = P.streamPower || {};
    const Dep = P.deposition || {};
    const Thermal = P.thermal || {};
    const Aeolian = P.aeolian || {};
    const Glacial = P.glacial || {};
    const Chemical = P.chemical || {};
    const Pitting = P.pitting || {};
    const Filters = P.filters || {};

    return {
        ...P,
        seed: Num(P.seed, 4029) | 0,
        iterations: Clamp(Math.round(Num(P.iterations, 36)), 1, 400),
        dtKyr: Clamp(Num(P.dtKyr, 12.0), 0.05, 2000.0),
        // Tectonic uplift paced against the (compressed) simulated span. Kept small by default:
        // over 36 x 12 kyr this is the difference between a living range and a 400 m tower.
        upliftMmYr: Clamp(Num(P.upliftMmYr, 0.0), 0.0, 5.0),
        // A depression must be at least this deep to count as a lake (one-cell pits are not lakes).
        lakeDepthM: Clamp(Num(P.lakeDepthM, 1.2), 0.0, 60.0),
        // Hard anti-flood guard: authored land may never be drowned deeper than this. This is the
        // single most important fix for "the water always floods" - erosion can lower a valley,
        // but it can never take the landmass below the sea and hide it under the ocean plane.
        maxLandDrownedM: Clamp(Num(P.maxLandDrownedM, 2.5), 0.0, 40.0),

        climate: {
            ...Climate,
            rainMmYr: Clamp(Num(Climate.rainMmYr, 1150), 0, 12000),
            pattern: CLIMATE_PATTERNS.some(X => X[1] === Climate.pattern) ? Climate.pattern : 'orographic',
            windAzimuthDeg: Num(Climate.windAzimuthDeg, 38.0),
            windSpeed: Clamp(Num(Climate.windSpeed, 0.45), 0, 1),
            meanTemperatureC: Clamp(Num(Climate.meanTemperatureC, 11.0), -60, 60),
            lapseRateCPerKm: Clamp(Num(Climate.lapseRateCPerKm, 6.2), 0, 12),
            seasonalAmplitude: Clamp(Num(Climate.seasonalAmplitude, 0.28), 0, 1),
            seasonPeriodIters: Math.max(2, Math.round(Num(Climate.seasonPeriodIters, 9))),
            freezeThawCyclesPerYr: Clamp(Num(Climate.freezeThawCyclesPerYr, 55), 0, 400)
        },

        streamPower: {
            ...SP,
            enabled: SP.enabled !== false,
            erodibility: Clamp(Num(SP.erodibility, 0.55), 0, 1),  // maps to K (see StreamPowerK)
            m: Clamp(Num(SP.m, 0.50), 0.0, 1.2),
            n: Clamp(Num(SP.n, 1.00), 0.5, 2.5),
            minSlope: Clamp(Num(SP.minSlope, 0.0025), 0.0, 0.3),
            hardRockResistance: Clamp(Num(SP.hardRockResistance, 0.45), 0, 1),
            regolithFactor: Clamp(Num(SP.regolithFactor, 5.5), 1, 40),
            knickpointSharpness: Clamp(Num(SP.knickpointSharpness, 0.35), 0, 1)
        },

        deposition: {
            ...Dep,
            enabled: Dep.enabled !== false,
            capacity: Clamp(Num(Dep.capacity, 0.5), 0, 1),
            fillRate: Clamp(Num(Dep.fillRate, 0.45), 0, 1),
            lakeFill: Clamp(Num(Dep.lakeFill, 0.85), 0, 1),
            maxAggradationM: Clamp(Num(Dep.maxAggradationM, 4.0), 0.02, 60)
        },

        thermal: {
            ...Thermal,
            enabled: Thermal.enabled !== false,
            talusDeg: Clamp(Num(Thermal.talusDeg, 33.0), 12, 62),
            rate: Clamp(Num(Thermal.rate, 0.55), 0, 1),
            passes: Clamp(Math.round(Num(Thermal.passes, 2)), 1, 6)
        },

        aeolian: {
            ...Aeolian,
            enabled: !!Aeolian.enabled,
            strength: Clamp(Num(Aeolian.strength, 0.35), 0, 1),
            saltationLengthM: Clamp(Num(Aeolian.saltationLengthM, 42.0), 2, 400),
            duneWavelengthM: Clamp(Num(Aeolian.duneWavelengthM, 155.0), 12, 2000),
            duneAmplitudeM: Clamp(Num(Aeolian.duneAmplitudeM, 7.5), 0, 60),
            deflationHardness: Clamp(Num(Aeolian.deflationHardness, 0.5), 0, 1)
        },

        glacial: {
            ...Glacial,
            enabled: !!Glacial.enabled,
            elaOffsetM: Num(Glacial.elaOffsetM, -18.0),
            massBalanceGrad: Clamp(Num(Glacial.massBalanceGrad, 0.0075), 0.0005, 0.06),
            flowConstant: Clamp(Num(Glacial.flowConstant, 0.55), 0.02, 1),
            erosionK: Clamp(Num(Glacial.erosionK, 0.42), 0, 1),
            maxIceThicknessM: Clamp(Num(Glacial.maxIceThicknessM, 420), 20, 3000),
            slidingExponent: Clamp(Num(Glacial.slidingExponent, 2.0), 1, 3)
        },

        chemical: {
            ...Chemical,
            enabled: !!Chemical.enabled,
            rate: Clamp(Num(Chemical.rate, 0.35), 0, 1),
            temperatureSensitivity: Clamp(Num(Chemical.temperatureSensitivity, 0.6), 0, 1),
            solubility: Clamp(Num(Chemical.solubility, 0.45), 0, 1),
            carbonateBoost: Clamp(Num(Chemical.carbonateBoost, 0.35), 0, 1)
        },

        pitting: {
            ...Pitting,
            enabled: !!Pitting.enabled,
            density: Clamp(Num(Pitting.density, 0.45), 0, 1),
            depthM: Clamp(Num(Pitting.depthM, 1.6), 0.02, 12),
            siteScaleM: Clamp(Num(Pitting.siteScaleM, 26.0), 2, 400),
            exposureBias: Clamp(Num(Pitting.exposureBias, 0.55), 0, 1)
        },

        filters: {
            ...Filters,
            minElev: Num(Filters.minElev, -1e9),
            maxElev: Num(Filters.maxElev, 1e9),
            minSlopeDeg: Num(Filters.minSlopeDeg, 0),
            maxSlopeDeg: Num(Filters.maxSlopeDeg, 90)
        },

        // Bedrock erodibility imprint (0..1). 1 = soft shale, 0 = unweatherable granite.
        hardnessMap: P.hardnessMap || null,
        // Authored micro-detail that must survive the simulation.
        imprint: {
            amplitudeM: Clamp(Num(P.imprint?.amplitudeM, 3.2), 0, 40),
            scaleM: Clamp(Num(P.imprint?.scaleM, 13.0), 1, 200)
        },
        // Shared-weather group (multiple layers simulated with one identical climate).
        weatherGroup: P.weatherGroup || null,
        preserveShoreline: P.preserveShoreline !== false,
        shoreLiftM: Clamp(Num(P.shoreLiftM, 0.35), 0, 30)
    };
}

/** K in yr^-1 for the stream-power law, from a 0..1 erodibility control. */
export function StreamPowerK(Erodibility)
{
    return Math.pow(10, -7.0 + 2.2 * Clamp(Erodibility, 0, 1));
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                    1. PRIORITY-FLOOD DEPRESSION FILL  (O(n log n), deterministic)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Barnes/Heywood style priority flood. Produces a *filled* surface (every closed depression is
 * raised to its spill point) plus a topological pop order and a lake mask. The filled surface is
 * used ONLY for flow routing and lake-surface rendering; erosion still uses the true heights, so
 * lakes stay lakes (and fill with sediment) instead of being artificially drained.
 */
function PriorityFloodFill(Nx, Nz, Heights, SeaLevel)
{
    const Count = Nx * Nz;
    const Filled = new Float32Array(Count);
    const PopRank = new Int32Array(Count).fill(-1);
    const Order = new Int32Array(Count);
    const IsLake = new Uint8Array(Count);

    // Binary min-heap on (elevation, cellIndex) - ties broken by index for reproducibility.
    const HeapH = new Float32Array(Count + 1);
    const HeapI = new Int32Array(Count + 1);
    let HeapN = 0;

    const Push = (H, Idx) =>
    {
        let I = ++HeapN;
        HeapH[I] = H; HeapI[I] = Idx;
        while (I > 1)
        {
            const P = I >> 1;
            if (HeapH[P] < HeapH[I] || (HeapH[P] === HeapH[I] && HeapI[P] <= HeapI[I])) break;
            const Th = HeapH[P], Ti = HeapI[P];
            HeapH[P] = HeapH[I]; HeapI[P] = HeapI[I];
            HeapH[I] = Th; HeapI[I] = Ti;
            I = P;
        }
    };
    const Pop = () =>
    {
        const OutI = HeapI[1];
        HeapH[1] = HeapH[HeapN]; HeapI[1] = HeapI[HeapN];
        HeapN--;
        let I = 1;
        for (;;)
        {
            const L = I << 1, R = L + 1;
            let Best = I;
            if (L <= HeapN && (HeapH[L] < HeapH[Best] || (HeapH[L] === HeapH[Best] && HeapI[L] < HeapI[Best]))) Best = L;
            if (R <= HeapN && (HeapH[R] < HeapH[Best] || (HeapH[R] === HeapH[Best] && HeapI[R] < HeapI[Best]))) Best = R;
            if (Best === I) break;
            const Th = HeapH[Best], Ti = HeapI[Best];
            HeapH[Best] = HeapH[I]; HeapI[Best] = HeapI[I];
            HeapH[I] = Th; HeapI[I] = Ti;
            I = Best;
        }
        return OutI;
    };

    const Pushed = new Uint8Array(Count);
    // Seed with the domain border AND every cell at/below sea level, so the ocean is an outlet
    // (this is what stops the "everything floods forever" failure mode).
    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const Idx = Z * Nx + X;
            const IsBorder = (X === 0 || X === Nx - 1 || Z === 0 || Z === Nz - 1);
            if (IsBorder || Heights[Idx] <= SeaLevel)
            {
                Filled[Idx] = Math.max(Heights[Idx], SeaLevel);
                if (!Pushed[Idx]) { Push(Filled[Idx], Idx); Pushed[Idx] = 1; }
            }
        }
    }

    let Rank = 0;
    while (HeapN > 0)
    {
        const Idx = Pop();
        const H = Filled[Idx];
        Order[Rank] = Idx;
        PopRank[Idx] = Rank++;
        if (H > Heights[Idx] + 1e-4) IsLake[Idx] = 1;

        const X = Idx % Nx;
        const Z = (Idx - X) / Nx;
        for (let Dz = -1; Dz <= 1; ++Dz)
        {
            const Nz2 = Z + Dz;
            if (Nz2 < 0 || Nz2 >= Nz) continue;
            for (let Dx = -1; Dx <= 1; ++Dx)
            {
                if (Dx === 0 && Dz === 0) continue;
                const Nx2 = X + Dx;
                if (Nx2 < 0 || Nx2 >= Nx) continue;
                const NIdx = Nz2 * Nx + Nx2;
                if (Pushed[NIdx]) continue;
                Pushed[NIdx] = 1;
                Filled[NIdx] = Math.max(Heights[NIdx], H);
                Push(Filled[NIdx], NIdx);
            }
        }
    }
    return { filled: Filled, popRank: PopRank, order: Order, isLake: IsLake };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                    2. SINGLE-FLOW-DIRECTION (STEEPEST DESCENT) ROUTING
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Receiver = steepest D8 neighbour on the *filled* surface. Because the fill guarantees a
 * monotone path to an outlet, and ties are broken by pop rank (already a valid topological
 * order), the resulting graph is always acyclic - no infinite loops, ever.
 */
export function ComputeFlowReceivers(Nx, Nz, Filled, PopRank)
{
    const Count = Nx * Nz;
    const Recv = new Int32Array(Count).fill(-1);
    const RecvDist = new Float32Array(Count);
    const Sqrt2 = Math.SQRT2;

    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const Idx = Z * Nx + X;
            const H = Filled[Idx];
            const MyRank = PopRank[Idx];
            let BestIdx = -1;
            let BestDrop = 0;
            let BestDist = 1;

            for (let Dz = -1; Dz <= 1; ++Dz)
            {
                const Nz2 = Z + Dz;
                if (Nz2 < 0 || Nz2 >= Nz) continue;
                for (let Dx = -1; Dx <= 1; ++Dx)
                {
                    if (Dx === 0 && Dz === 0) continue;
                    const Nx2 = X + Dx;
                    if (Nx2 < 0 || Nx2 >= Nx) continue;
                    const NIdx = Nz2 * Nx + Nx2;

                    // Prefer strictly lower ground; on flats fall back to "popped earlier"
                    // which is exactly the direction towards the outlet of the flat.
                    let Drop = H - Filled[NIdx];
                    if (Drop <= 1e-6) Drop = (PopRank[NIdx] >= 0 && PopRank[NIdx] < MyRank) ? 1e-6 : 0;
                    if (Drop <= 0) continue;

                    const Dist = (Dx !== 0 && Dz !== 0) ? Sqrt2 : 1.0;
                    const Score = Drop / Dist;
                    if (Score > BestDrop + 1e-12)
                    {
                        BestDrop = Score;
                        BestIdx = NIdx;
                        BestDist = Dist;
                    }
                }
            }
            Recv[Idx] = BestIdx;
            RecvDist[Idx] = BestDist;
        }
    }
    return { recv: Recv, recvDist: RecvDist };
}

/** Drainage area in m^2, accumulated in the (already topological) pop order. */
export function ComputeDrainageArea(Nx, Nz, Recv, Order, StepX, StepZ)
{
    const Count = Nx * Nz;
    const Area = new Float64Array(Count);
    const CellArea = StepX * StepZ;
    for (let I = 0; I < Count; ++I) Area[I] = CellArea;

    // Order is downstream -> upstream, so accumulate in reverse (upstream -> downstream).
    for (let K = Count - 1; K >= 0; --K)
    {
        const Idx = Order[K];
        const R = Recv[Idx];
        if (R >= 0) Area[R] += Area[Idx];
    }
    return Area;
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                              3. CLIMATE FIELD (rain, temperature, wind)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Builds a per-cell precipitation field. The pattern controls the *spatial* structure; the
 * orographic term is physical: air forced over a ridge drops its moisture on the windward flank
 * and leaves a rain shadow behind it (this is what makes asymmetric ranges read correctly).
 */
export function BuildClimateField(Nx, Nz, Heights, StepX, StepZ, Params, Iteration = 0, WeatherMask = null)
{
    const Count = Nx * Nz;
    const C = Params.climate;
    const Rain = new Float32Array(Count);
    const Temp = new Float32Array(Count);
    const FreezeThaw = new Float32Array(Count);

    let MinH = Infinity, MaxH = -Infinity;
    for (let I = 0; I < Count; ++I)
    {
        const H = Heights[I];
        if (H < MinH) MinH = H;
        if (H > MaxH) MaxH = H;
    }
    const Span = Math.max(1.0, MaxH - MinH);
    const AzRad = C.windAzimuthDeg * Deg2Rad;
    const WindX = Math.cos(AzRad), WindZ = Math.sin(AzRad);
    const SeasonPhase = (Iteration / C.seasonPeriodIters) * Math.PI * 2.0;
    const Season = 1.0 + C.seasonalAmplitude * Math.sin(SeasonPhase);

    const NormRain = Clamp(C.rainMmYr / 1150.0, 0.0, 6.0);

    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const Idx = Z * Nx + X;
            const Wx = X * StepX, Wz = Z * StepZ;

            // Upwind slope: how much the terrain has risen coming INTO this cell from upwind.
            const Ux = Clamp(Math.round(X - WindX * 2.0), 0, Nx - 1);
            const Uz = Clamp(Math.round(Z - WindZ * 2.0), 0, Nz - 1);
            const UpwindH = Heights[Uz * Nx + Ux];
            const UpwindRise = (Heights[Idx] - UpwindH) / Math.max(1e-3, 2.0 * Math.min(StepX, StepZ));

            let Spatial = 1.0;
            switch (C.pattern)
            {
                case 'uniform':
                    Spatial = 1.0;
                    break;
                case 'orographic':
                    // Windward lift -> more rain; lee (negative rise) -> strong shadow.
                    Spatial = Clamp(1.0 + UpwindRise * 9.0, 0.12, 2.6);
                    break;
                case 'monsoon':
                    Spatial = Clamp(0.72 + 0.55 * Math.sin((Wx * 0.35 + Wz * 0.22) * 0.0021) + UpwindRise * 3.2, 0.15, 2.2) * Season;
                    break;
                case 'coastal':
                    // Moisture decays inland; distance proxy = smooth noise + upwind rise.
                    Spatial = Clamp(1.35 - 0.55 * FbmNoise2D(Wx * 0.0011, Wz * 0.0011, 3, Params.seed + 17) + UpwindRise * 4.0, 0.1, 2.4);
                    break;
                case 'arid':
                    Spatial = Clamp(0.18 + 0.22 * FbmNoise2D(Wx * 0.0009, Wz * 0.0009, 3, Params.seed + 31) + UpwindRise * 1.6, 0.02, 0.75);
                    break;
                case 'polar':
                    Spatial = Clamp(0.35 + 0.30 * FbmNoise2D(Wx * 0.0013, Wz * 0.0013, 3, Params.seed + 53), 0.05, 1.1);
                    break;
                case 'stormTracks':
                    // Banded storm corridors perpendicular to the prevailing wind.
                    Spatial = Clamp(0.45 + 0.85 * Math.pow(Math.max(0.0, Math.sin((Wx * WindX + Wz * WindZ) * 0.0035 + 1.1)), 2.0) + UpwindRise * 2.0, 0.08, 2.3) * Season;
                    break;
                case 'altitudeBands':
                {
                    const Alt = (Heights[Idx] - MinH) / Span;
                    Spatial = Clamp(0.35 + 1.15 * Math.exp(-Math.pow((Alt - 0.55) / 0.30, 2)) + UpwindRise * 3.0, 0.06, 2.2);
                    break;
                }
                default:
                    Spatial = 1.0;
                    break;
            }

            let CellRain = Clamp(Spatial * NormRain, 0.0, 6.0);
            if (WeatherMask) CellRain *= Clamp(WeatherMask[Idx], 0, 6);
            Rain[Idx] = CellRain;

            // Environmental lapse rate: temperature falls with elevation.
            const ElevKm = Math.max(0.0, Heights[Idx]) / 1000.0;
            const T = C.meanTemperatureC - ElevKm * C.lapseRateCPerKm + 3.0 * Math.sin(SeasonPhase);
            Temp[Idx] = T;

            // Freeze-thaw cycles need moisture and a temperature straddling 0 C.
            const Straddle = Math.exp(-Math.pow(T / 9.0, 2));
            FreezeThaw[Idx] = C.freezeThawCyclesPerYr * Straddle * Clamp(CellRain, 0, 3) * 0.5;
        }
    }
    return {
        rain: Rain, temp: Temp, freezeThaw: FreezeThaw, season: Season,
        windX: WindX, windZ: WindZ, windSpeed: C.windSpeed,
        minH: MinH, maxH: MaxH
    };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                    4. DETACHMENT-LIMITED STREAM-POWER INCISION (Braun & Willett 2013)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Implicit, unconditionally-stable sweep. Processed in priority-flood pop order, which runs
 * downstream -> upstream, so the receiver's new elevation is always already known.
 *
 *     h_new = (h_old + dt*U + Keff*dt*A^m*(h_recv^new/dx)) / (1 + Keff*dt*A^m/dx)     (n = 1)
 *
 * For n != 1 a short fixed-point iteration is used with a hard clamp so we can never incise
 * below the receiver (no spurious pits, no negative slopes).
 */
function ApplyStreamPower(Nx, Nz, Heights, SlabBottom, Grid, Sim, Params, Climate, IterDtYears)
{
    const Count = Nx * Nz;
    const { recv, recvDist, area, stepX, stepZ } = Grid;
    const { order } = Sim;
    const SP = Params.streamPower;
    const C = Params.climate;
    const K0 = StreamPowerK(SP.erodibility);
    const UpliftRate = Num(Params.upliftMmYr, 0.0) * 1e-3; // m/yr

    const RainRef = Clamp(C.rainMmYr / 1150.0, 0.05, 4.0);
    let IncisionVolume = 0.0;
    let MaxIncision = 0.0;

    // Erosion depth recorder (positive = material removed).
    for (let K = 0; K < Count; ++K)
    {
        const Idx = order[K];
        const R = recv[Idx];
        const H = Heights[Idx];

        // Tectonic uplift happens everywhere inside the land mask (base-level rise).
        let Base = H;
        if (UpliftRate > 0) Base = H + UpliftRate * IterDtYears;

        if (R < 0)
        {
            Heights[Idx] = Base;
            continue; // Outlet: no incision, this is local base level.
        }

        const Dx = recvDist[Idx] * Math.min(stepX, stepZ);
        const HRecv = Heights[R];
        const Slope = Math.max(0.0, (Base - HRecv) / Math.max(1e-3, Dx));
        if (Slope <= SP.minSlope || Base <= HRecv)
        {
            Heights[Idx] = Base;
            continue;
        }

        // Effective erodibility: rock resistance, regolith cover, and local rain.
        const Hard = Params.hardnessMap ? Clamp(Params.hardnessMap[Idx], 0, 1) : 0.5;
        const RockFactor = Mix(1.0, 0.12, Hard * SP.hardRockResistance);
        const Regolith = Sim.regolith ? Sim.regolith[Idx] : 0;
        const Cover = Clamp(Regolith / Math.max(0.15, 0.9), 0.0, 1.0);
        const SoilBoost = Mix(1.0, SP.regolithFactor, Cover);
        const RainFactor = Clamp(Climate.rain[Idx] / Math.max(0.05, RainRef), 0.03, 3.5);
        const Keff = K0 * RockFactor * SoilBoost * Math.pow(RainFactor, SP.m);

        const AM = Math.pow(Math.max(1.0, area[Idx]), SP.m);

        let HNew;
        if (Math.abs(SP.n - 1.0) < 1e-3)
        {
            const Cfl = Keff * AM * IterDtYears / Math.max(1e-3, Dx);
            HNew = (Base + Cfl * HRecv) / (1.0 + Cfl);
        }
        else
        {
            HNew = Base;
            for (let It = 0; It < 4; ++It)
            {
                const S = Math.max(0.0, (HNew - HRecv) / Math.max(1e-3, Dx));
                const E = Keff * AM * Math.pow(S, SP.n);          // m/yr
                const Target = Base - E * IterDtYears;
                HNew = Clamp(Target, HRecv, Base);                // never below local base level
            }
        }

        // Limit so a single step can never remove an implausible amount (stability + realism).
        const MaxStep = Math.max(0.25, Slope * Dx * 0.9);
        if (Base - HNew > MaxStep) HNew = Base - MaxStep;
        HNew = Math.max(SlabBottom + 0.35, HNew);

        const Removed = Base - HNew;
        if (Removed > 0)
        {
            IncisionVolume += Removed * stepX * stepZ;
            if (Removed > MaxIncision) MaxIncision = Removed;
            Sim.erosionDepth[Idx] += Removed;
            Sim.erodedThisStep[Idx] = Removed;
        }
        Heights[Idx] = HNew;
    }
    return { incisionVolume: IncisionVolume, maxIncision: MaxIncision };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                       5. TRANSPORT-LIMITED SEDIMENT ROUTING & DEPOSITION
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Every cell receives the sediment flux of its donors, adds its own eroded material, and keeps
 * what its carrying capacity allows; the excess aggrades the bed. Lakes (zero slope) have ~zero
 * capacity, so they trap sediment and build deltas - exactly what the reference viewport lacked.
 */
function ApplySedimentRouting(Nx, Nz, Heights, SlabBottom, SeaLevel, Grid, Sim, Params, IterDtYears)
{
    const Count = Nx * Nz;
    const { recv, area, stepX, stepZ } = Grid;
    const { order, isLake } = Sim;
    const Dep = Params.deposition;
    const CellArea = stepX * stepZ;

    const Flux = Sim.fluxVol;
    Flux.fill(0);

    // Capacity coefficient: sits just below the stream-power K so that steep catchments stay
    // detachment-limited (incising) while low-gradient reaches aggrade.
    const Kc = StreamPowerK(Params.streamPower.erodibility) * Mix(0.35, 1.9, Dep.capacity);

    let DepositedVolume = 0.0;
    let ExportedVolume = 0.0;

    for (let K = Count - 1; K >= 0; --K)
    {
        const Idx = order[K];
        const R = recv[Idx];

        const ErodedVol = Sim.erodedThisStep[Idx] * CellArea;
        let Available = Flux[Idx] + ErodedVol;

        if (Available <= 1e-12)
        {
            if (R < 0) ExportedVolume += Math.max(0, Available);
            else Flux[R] += Math.max(0, Available);
            continue;
        }

        if (R < 0)
        {
            // Domain outlet (or ocean): everything leaves the system.
            ExportedVolume += Available;
            continue;
        }

        const H = Heights[Idx];
        const HRecv = Heights[R];
        const Dist = Math.max(1e-3, Math.min(stepX, stepZ));
        const Slope = Math.max(0.0, (H - HRecv) / Dist);
        const AM = Math.pow(Math.max(1.0, area[Idx]), Params.streamPower.m);

        // A lake surface is flat, so capacity collapses and the basin traps its load.
        const LakeDamp = isLake[Idx] ? Mix(1.0, 0.04, Dep.lakeFill) : 1.0;
        const CapacityRate = Kc * AM * Math.pow(Math.max(Slope, 1e-6), Params.streamPower.n) * LakeDamp;
        let CapacityVol = CapacityRate * IterDtYears * CellArea;

        // Cap aggradation per step to keep the scheme stable and avoid tower artefacts.
        const MaxDepositVol = Dep.maxAggradationM * CellArea * Mix(0.35, 1.0, Dep.fillRate);

        if (Available > CapacityVol)
        {
            let Excess = Available - CapacityVol;
            let Deposit = Math.min(Excess, MaxDepositVol * 0.9);

            // Never bury a cell above the height of the water/valley it drains into by much.
            const Ceiling = Math.max(HRecv + 0.2, SeaLevel - 0.5);
            const Room = Math.max(0.0, (Ceiling - Heights[Idx]));
            const RoomVol = Room * CellArea;
            if (Deposit > RoomVol) Deposit = Math.min(Deposit, RoomVol + MaxDepositVol * 0.25);

            if (Deposit > 0)
            {
                Heights[Idx] += Deposit / CellArea;
                Sim.depositionDepth[Idx] += Deposit / CellArea;
                Available -= Deposit;
                DepositedVolume += Deposit;
            }
        }

        Flux[R] += Math.max(0.0, Available);
    }

    // Whatever is still sitting in a terminal cell (closed boundary) is also exported.
    for (let I = 0; I < Count; ++I)
    {
        if (recv[I] < 0) { ExportedVolume += Flux[I]; Flux[I] = 0; }
    }
    return { depositedVolume: DepositedVolume, exportedVolume: ExportedVolume };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                           6. THERMAL / TALUS MASS-WASTING (mass conserving)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Pairwise mass-conserving sweep: where the slope between two neighbours exceeds the critical
 * repose angle, material moves downslope. Produces scree aprons and softens over-steep cliffs.
 */
function ApplyThermalErosion(Nx, Nz, Heights, SlabBottom, StepX, StepZ, Params, IterDtYears)
{
    const T = Params.thermal;
    if (!T.enabled || T.rate <= 0) return { movedVolume: 0 };

    const TanTalus = Math.tan(T.talusDeg * Deg2Rad);
    const CellArea = StepX * StepZ;
    let MovedVolume = 0;

    for (let Pass = 0; Pass < T.passes; ++Pass)
    {
        for (let Z = 0; Z < Nz; ++Z)
        {
            for (let X = 0; X < Nx; ++X)
            {
                const Idx = Z * Nx + X;
                // Right and down neighbours only -> each pair is visited exactly once.
                for (let Dir = 0; Dir < 2; ++Dir)
                {
                    const Nx2 = X + (Dir === 0 ? 1 : 0);
                    const Nz2 = Z + (Dir === 1 ? 1 : 0);
                    if (Nx2 >= Nx || Nz2 >= Nz) continue;
                    const NIdx = Nz2 * Nx + Nx2;
                    const Dist = Dir === 0 ? StepX : StepZ;

                    const Dh = Heights[Idx] - Heights[NIdx];
                    const Limit = TanTalus * Dist;
                    const Excess = Math.abs(Dh) - Limit;
                    if (Excess <= 1e-5) continue;

                    // Move a fraction of the excess; 0.25 is the standard stability-sweep factor.
                    let Move = Excess * 0.25 * Mix(0.15, 1.0, T.rate);
                    const From = Dh > 0 ? Idx : NIdx;
                    const To = Dh > 0 ? NIdx : Idx;
                    Move = Math.min(Move, Math.max(0.0, Heights[From] - SlabBottom));

                    Heights[From] -= Move;
                    Heights[To] += Move;
                    MovedVolume += Move * CellArea;
                }
            }
        }
    }

    // Prevent any cell being pushed under the thin-slab floor by diffusion.
    for (let I = 0; I < Heights.length; ++I)
    {
        if (Heights[I] < SlabBottom + 0.2) Heights[I] = SlabBottom + 0.2;
    }
    return { movedVolume: MovedVolume };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                        7. AEOLIAN SALTATION & DUNE BEDFORMS
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Transport-limited wind model. Shear velocity rises with the wind and with windward exposure,
 * so windward flanks deflate and the lee flank aggrades (the classic asymmetry of a dune field).
 * A bedform term adds a phase-shifted ripple whose wavelength grows with fetch, which is what
 * turns a flat sand sheet into transverse dunes.
 */
function ApplyAeolian(Nx, Nz, Heights, SlabBottom, SeaLevel, StepX, StepZ, Params, Climate, IterDtYears)
{
    const A = Params.aeolian;
    if (!A.enabled || A.strength <= 0) return { deflatedVolume: 0, depositedVolume: 0 };

    const Count = Nx * Nz;
    const WindX = Climate.windX, WindZ = Climate.windZ;
    const CellArea = StepX * StepZ;

    // Effective sand availability: dry (low rain), gently sloped, low-elevation surfaces.
    const Sand = new Float32Array(Count);
    let SandTotal = 0;
    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const Idx = Z * Nx + X;
            const Xm = Math.max(0, X - 1), Xp = Math.min(Nx - 1, X + 1);
            const Zm = Math.max(0, Z - 1), Zp = Math.min(Nz - 1, Z + 1);
            const DhDx = (Heights[Z * Nx + Xp] - Heights[Z * Nx + Xm]) / ((Xp - Xm || 1) * StepX);
            const DhDz = (Heights[Zp * Nx + X] - Heights[Zm * Nx + X]) / ((Zp - Zm || 1) * StepZ);
            const Slope = Math.hypot(DhDx, DhDz);

            const Dryness = Clamp(1.0 - Climate.rain[Idx] * 0.55, 0.15, 1.0);
            const Flatness = 1.0 - Smoothstep(0.35, 1.1, Slope);
            const AboveSea = Smoothstep(SeaLevel - 1.0, SeaLevel + 4.0, Heights[Idx]);
            Sand[Idx] = Dryness * Flatness * AboveSea;
            SandTotal += Sand[Idx];
        }
    }

    let Deflated = 0, Deposited = 0;
    const Rate = A.strength * Mix(0.004, 0.10, A.saltationLengthM / 400.0) * Climate.windSpeed;
    const Wavelength = Math.max(8.0, A.duneWavelengthM);

    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const Idx = Z * Nx + X;
            const SandHere = Sand[Idx];
            if (SandHere <= 0.02) continue;

            // Windward exposure: terrain rising into the wind accelerates near-surface flow.
            const Ux = Clamp(Math.round(X - WindX * 1.6), 0, Nx - 1);
            const Uz = Clamp(Math.round(Z - WindZ * 1.6), 0, Nz - 1);
            const UpwindH = Heights[Uz * Nx + Ux];
            const Rise = (Heights[Idx] - UpwindH) / Math.max(1e-3, 1.6 * StepX);

            const Exposure = Clamp(0.55 + Rise * 6.0, 0.05, 2.2);
            const Deflation = Rate * IterDtYears * SandHere * Exposure * Mix(1.0, 0.15, A.deflationHardness);

            // Lee side of the same ridge: flow separates, so sand is dropped.
            const Lx = Clamp(Math.round(X + WindX * 1.6), 0, Nx - 1);
            const Lz = Clamp(Math.round(Z + WindZ * 1.6), 0, Nz - 1);
            const LeeIdx = Lz * Nx + Lx;
            const Shelter = Clamp((Heights[LeeIdx] - Heights[Idx]) / Math.max(1e-3, StepX), -1.0, 1.0);

            let Move = Clamp(Deflation, 0.0, 0.35);
            Move = Math.min(Move, Math.max(0.0, Heights[Idx] - SlabBottom - 0.3));
            if (Move <= 1e-7) continue;

            Heights[Idx] -= Move;
            Deflated += Move * CellArea;
            Heights[LeeIdx] += Move;
            Deposited += Move * CellArea;

            // Bedform: phase-shifted ripple with wavelength growing downwind.
            const Along = X * StepX * WindX + Z * StepZ * WindZ;
            const DunePhase = ((Along / Wavelength) % 1.0 + 1.0) % 1.0;
            const DuneProfile = Math.sin(DunePhase * Math.PI * 2.0);
            const DuneAmp = A.duneAmplitudeM * A.strength * SandHere * (0.35 + 0.65 * Clamp(Shelter + 0.5, 0, 1));
            const Displacement = DuneProfile * DuneAmp * 0.05;
            Heights[Idx] += Displacement;
            Heights[LeeIdx] -= Displacement * 0.5;
        }
    }
    return { deflatedVolume: Deflated, depositedVolume: Deposited };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                            8. GLACIAL ICE FLOW (Shallow-Ice Approximation, Glen n=3)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * H_{k+1} = H_k + dt_s * ( div( D grad s ) + b ),   D = C * H^{n+2} |grad s|^{n-1}
 *
 * The diffusion coefficient makes ice thicken in hollows and thin on ridges, which is exactly the
 * mechanism that carves cirques and broadens V-valleys into U-valleys. Erosion is driven by the
 * sliding velocity u_s ~ H |grad s|, and is concentrated where the ice is thick and fast.
 */
function ApplyGlacial(Nx, Nz, Heights, SlabBottom, SeaLevel, StepX, StepZ, Params, Climate, IterDtYears, Sim)
{
    const G = Params.glacial;
    if (!G.enabled || G.erosionK <= 0) return { iceVolume: 0, erodedVolume: 0 };

    const Count = Nx * Nz;
    const CellArea = StepX * StepZ;
    const N = G.slidingExponent;
    const RhoG = 910.0 * 9.81; // ice density * gravity

    // Equilibrium line altitude: where accumulation balances ablation for this climate.
    let MaxH = -Infinity, MinH = Infinity;
    for (let I = 0; I < Count; ++I) { if (Heights[I] > MaxH) MaxH = Heights[I]; if (Heights[I] < MinH) MinH = Heights[I]; }
    const ColdBias = Clamp((0.0 - Params.climate.meanTemperatureC) / 22.0, -1.0, 2.0);
    const ELA = Mix(MaxH, MinH, Clamp(0.35 - ColdBias * 0.30, 0.02, 0.95)) + G.elaOffsetM;

    const Ice = Sim.iceThickness;
    Ice.fill(0);

    const SubIters = 10;
    const Dt = 0.55;
    for (let It = 0; It < SubIters; ++It)
    {
        const NewIce = new Float32Array(Ice); // start from previous
        for (let Z = 1; Z < Nz - 1; ++Z)
        {
            for (let X = 1; X < Nx - 1; ++X)
            {
                const Idx = Z * Nx + X;
                const Surface = Heights[Idx] + Ice[Idx];
                const Hx = (Ice[Z * Nx + X + 1] - Ice[Z * Nx + X - 1]) / (2 * StepX);
                const Hz = (Ice[(Z + 1) * Nx + X] - Ice[(Z - 1) * Nx + X]) / (2 * StepZ);
                const SurfaceGrad = Math.hypot(
                    (Surface - (Heights[Z * Nx + X - 1] + Ice[Z * Nx + X - 1])) / StepX,
                    (Surface - (Heights[(Z - 1) * Nx + X] + Ice[(Z - 1) * Nx + X])) / StepZ
                );

                const H = Math.max(0.0, Ice[Idx]);
                const D = G.flowConstant * Math.pow(RhoG, 3) * 1e-16 * Math.pow(H, 5) * Math.pow(Math.max(1e-4, SurfaceGrad), 2);
                const Lap = (Ice[Z * Nx + X + 1] + Ice[Z * Nx + X - 1] + Ice[(Z + 1) * Nx + X] + Ice[(Z - 1) * Nx + X] - 4.0 * H);

                // Mass balance b: positive above the ELA, strongly negative below it.
                const Above = Heights[Idx] - ELA;
                const B = Above > 0
                    ? Math.min(2.2, G.massBalanceGrad * Above)
                    : -G.massBalanceGrad * Math.min(900.0, -Above) * 1.35;

                let Next = H + Dt * (D * Lap / (StepX * StepZ) * 1e4 + B);
                Next = Clamp(Next, 0.0, G.maxIceThicknessM);
                NewIce[Idx] = Next;
            }
        }
        for (let I = 0; I < Count; ++I) Ice[I] = NewIce[I];
    }

    // Erosion from sliding: u_s ~ H * |grad s|, erosion ~ u_s^N
    let IceVolume = 0, Eroded = 0;
    for (let Z = 1; Z < Nz - 1; ++Z)
    {
        for (let X = 1; X < Nx - 1; ++X)
        {
            const Idx = Z * Nx + X;
            const H = Ice[Idx];
            IceVolume += H * CellArea;
            if (H < 1.5) continue;

            const Sx = (Heights[Z * Nx + X + 1] - Heights[Z * Nx + X - 1]) / (2 * StepX);
            const Sz = (Heights[(Z + 1) * Nx + X] - Heights[(Z - 1) * Nx + X]) / (2 * StepZ);
            const Grad = Math.hypot(Sx, Sz);

            const Slide = H * Grad;
            const E = G.erosionK * Math.pow(Math.max(0, Slide), N) * 1e-3;
            let Cut = E * IterDtYears;
            // Never cut a trench deeper than the ice itself; clamps runaway cirque digging.
            Cut = Math.min(Cut, H * 0.22, Math.max(0.0, Heights[Idx] - SlabBottom - 0.4));
            if (Cut <= 1e-7) continue;

            Heights[Idx] -= Cut;
            Sim.erosionDepth[Idx] += Cut;
            Sim.erodedThisStep[Idx] += Cut;
            Eroded += Cut * CellArea;
        }
    }
    return { iceVolume: IceVolume, erodedVolume: Eroded, ela: ELA };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                   9. CHEMICAL WEATHERING (bedrock -> regolith, plus solute export)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Regolith is what the fluvial term actually detaches; chemical weathering is the rate limiter
 * that produces it. Temperature (Arrhenius-like exponential) and moisture drive the conversion,
 * carbonates respond fastest, and dissolved load is exported as solute.
 */
function ApplyChemicalWeathering(Nx, Nz, Heights, StepX, StepZ, Params, Climate, IterDtYears, Sim)
{
    const Ch = Params.chemical;
    if (!Ch.enabled || Ch.rate <= 0) return { soluteVolume: 0, regolithProduced: 0 };

    const Count = Nx * Nz;
    let SoluteVolume = 0, Produced = 0;
    const CellArea = StepX * StepZ;

    for (let I = 0; I < Count; ++I)
    {
        const T = Climate.temp[I];
        // Q10-style temperature dependence, normalised around 10 C.
        const TempFactor = Math.pow(2.0, (T - 10.0) / 10.0 * Ch.temperatureSensitivity);
        const Moisture = Clamp(Climate.rain[I], 0.0, 3.0);
        const Hard = Params.hardnessMap ? Clamp(Params.hardnessMap[I], 0, 1) : 0.5;
        const LithoBoost = 1.0 + Ch.carbonateBoost * (1.0 - Hard);

        const Rate = Ch.rate * 2.2e-5 * Clamp(TempFactor, 0.02, 12.0) * Moisture * LithoBoost;
        const Depth = Rate * IterDtYears * Mix(0.35, 1.0, Ch.solubility);
        if (Depth <= 0) continue;

        const Regolith = Sim.regolith[I];
        if (Regolith < 4.0)
        {
            const Grow = Math.min(Depth, 4.0 - Regolith);
            Sim.regolith[I] = Regolith + Grow;
            Produced += Grow * CellArea;
        }
        // Solute leaves the system (dissolved load) - recorded for texturing & accounting.
        const Solute = Depth * Ch.solubility * 0.55;
        Sim.soluteDepth[I] += Solute;
        Sim.erosionDepth[I] += Solute;
        SoluteVolume += Solute * CellArea;

        Heights[I] = Math.max(Heights[I] - Solute * 0.25, -1e9);
    }
    return { soluteVolume: SoluteVolume, regolithProduced: Produced };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                            10. FROST / SALT PITTING (micro-shattering)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Freeze-thaw shattering and salt crystallisation open small pits on exposed rock. Modelled as a
 * sparse Poisson field of fatigue sites (seeded) whose depth grows with cycle count and slope
 * exposure, with a soft radial falloff - so you get pocked cliff faces rather than uniform noise.
 */
function ApplyPitting(Nx, Nz, Heights, SlabBottom, StepX, StepZ, Params, Climate, IterDtYears, Sim, Random)
{
    const P = Params.pitting;
    if (!P.enabled || P.depthM <= 0) return { pittedVolume: 0 };

    const Count = Nx * Nz;
    const CellArea = StepX * StepZ;
    const SiteCount = Math.max(1, Math.round(Count * 0.012 * P.density));
    let Pitted = 0;

    for (let S = 0; S < SiteCount; ++S)
    {
        const Sx = Math.floor(Random() * Nx);
        const Sz = Math.floor(Random() * Nz);
        const Idx = Sz * Nx + Sx;

        const Cycles = Climate.freezeThaw[Idx];
        if (Cycles <= 0.5) continue;

        // Exposure: steep faces shed water and frost-shatter most.
        const Xm = Math.max(0, Sx - 1), Xp = Math.min(Nx - 1, Sx + 1);
        const Zm = Math.max(0, Sz - 1), Zp = Math.min(Nz - 1, Sz + 1);
        const Gx = (Heights[Sz * Nx + Xp] - Heights[Sz * Nx + Xm]) / ((Xp - Xm || 1) * StepX);
        const Gz = (Heights[Zp * Nx + Sx] - Heights[Zm * Nx + Sx]) / ((Zp - Zm || 1) * StepZ);
        const SlopeDeg = Math.atan(Math.hypot(Gx, Gz)) / Deg2Rad;
        const Exposure = Mix(1.0, Clamp(SlopeDeg / 55.0, 0.05, 1.4), P.exposureBias);

        const Fatigue = Clamp(Cycles / 60.0, 0.02, 1.6) * Exposure;
        const Depth = P.depthM * Fatigue * Math.min(1.0, IterDtYears / 8.0) * 0.28;
        if (Depth <= 1e-5) continue;

        const Radius = Math.max(1, Math.round(P.siteScaleM / Math.max(1e-3, StepX)));
        for (let Dz = -Radius; Dz <= Radius; ++Dz)
        {
            const Z2 = Sz + Dz;
            if (Z2 < 0 || Z2 >= Nz) continue;
            for (let Dx = -Radius; Dx <= Radius; ++Dx)
            {
                const X2 = Sx + Dx;
                if (X2 < 0 || X2 >= Nx) continue;
                const R = Math.hypot(Dx * StepX, Dz * StepZ) / Math.max(1e-3, P.siteScaleM);
                if (R > 1.0) continue;
                const Fall = (1.0 - R * R) ** 2;
                const NIdx = Z2 * Nx + X2;
                let Cut = Depth * Fall;
                Cut = Math.min(Cut, Math.max(0.0, Heights[NIdx] - SlabBottom - 0.3));
                if (Cut <= 0) continue;
                Heights[NIdx] -= Cut;
                Sim.erosionDepth[NIdx] += Cut;
                Sim.erodedThisStep[NIdx] += Cut;
                Pitted += Cut * CellArea;
            }
        }
    }
    return { pittedVolume: Pitted };
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                    STRATIGRAPHY: BEDDED HARDNESS FIELD (what carves real benches)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Differential erodibility by stratigraphic bed. This is the single most important control on
 * landform character: alternating hard caprock (low K) and soft shale (high K) produces the
 * stair-step cliff/bench profile of a canyon. Without it every landscape converges to the same
 * smooth, scale-invariant hill.
 *
 * With `jointSpacingM > 0` we also lay down joint-bounded blocks (columnar jointing / fracture
 * corridors), which is what makes basalt colonnades and granite tors instead of smooth walls.
 *
 * @returns {Float32Array} hardness in [0..1]: 0 = unweatherable, 1 = softest.
 */
export function BuildHardnessField(Nx, Nz, StepX, StepZ, Strata, Seed = 4029)
{
    const Count = Nx * Nz;
    const Out = new Float32Array(Count);
    if (!Strata || !Strata.enabled)
    {
        Out.fill(0.5);
        return Out;
    }

    const Beds = Math.max(2, Number(Strata.bedFrequency) || 8);
    const Dip = (Number(Strata.dipDeg) || 0) * Deg2Rad;
    const Strike = (Number(Strata.strikeDeg) || 0) * Deg2Rad;
    const Contrast = Clamp(Number(Strata.caprockContrast) ?? 0.7, 0, 1);
    const DipX = -Math.sin(Strike) * Math.tan(Dip);
    const DipZ = Math.cos(Strike) * Math.tan(Dip);
    const JointSpacing = Number(Strata.jointSpacingM) || 0;
    const JointStrength = Clamp(Number(Strata.jointStrength) ?? 0.5, 0, 1);
    const ReferenceRelief = Math.max(40, Number(Strata.referenceReliefM) || 220);

    for (let Z = 0; Z < Nz; ++Z)
    {
        const Wz = Z * StepZ;
        for (let X = 0; X < Nx; ++X)
        {
            const Wx = X * StepX;
            // Stratigraphic coordinate measured along bedding planes: tilted by dip.
            const Strat = (Wz + Wx * DipX + Wz * DipZ) / ReferenceRelief;
            const Phase = (Strat * Beds + 0.5) % 1.0;

            // Beds are thinner/softer in the middle of a package, with hard caps at top and base.
            const Cap = Smoothstep(0.0, 0.12, Phase) * (1.0 - Smoothstep(0.72, 1.0, Phase));
            let Hard = Mix(0.85, 0.12, Cap); // soft where the bed is thin, hard where capped

            // Gentle lateral variability so beds are not perfectly uniform (seeded, deterministic).
            const Var = FbmNoise2D(Wx * 0.0022, Wz * 0.0022, 3, Seed + 909);
            Hard = Clamp(Hard + (Var - 0.5) * 0.14 * Contrast, 0, 1);

            // Columnar jointing: fracture corridors erode faster than their bounding columns.
            if (JointSpacing > 0.5)
            {
                const Jx = Math.abs(((Wx / JointSpacing) % 1.0 + 1.0) % 1.0 - 0.5);
                const Jz = Math.abs(((Wz / JointSpacing) % 1.0 + 1.0) % 1.0 - 0.5);
                const Joint = 1.0 - Smoothstep(0.0, 0.10, Math.min(Jx, Jz));
                Hard = Clamp(Hard - Joint * JointStrength * 0.45, 0, 1);
            }
            Out[Z * Nx + X] = Clamp(Hard * Mix(0.5, 1.0, Contrast) + 0.5 * (1.0 - Contrast), 0, 1);
        }
    }
    return Out;
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                    STEPPABLE SIMULATION STATE (frame-friendly, resumable)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * A run is a small state machine so the studio can advance it across animation frames instead of
 * blocking the main thread for a multi-second bake. Each `SimulationStep()` performs one
 * geological iteration (fill -> route -> incise -> deposit -> waste -> wind -> ice -> chemistry).
 */
export class ErosionSimulationState
{
    /**
     * @param {object} Grid  { nx, nz, stepX, stepZ, heights, sdf2D?, seaLevel, slabBottom, rainMask?, imprintTexture?, referenceHeights? }
     * @param {object} ParamsIn  Partial erosion parameters.
     */
    constructor(Grid, ParamsIn = {})
    {
        this.params = NormalizeErosionParams(ParamsIn);
        this.nx = Grid.nx | 0;
        this.nz = Grid.nz | 0;
        this.count = this.nx * this.nz;
        this.stepX = Math.max(1e-3, Grid.stepX);
        this.stepZ = Math.max(1e-3, Grid.stepZ);
        this.seaLevel = Num(Grid.seaLevel, 0.0);
        this.slabBottom = Num(Grid.slabBottom, this.seaLevel - 6.5);
        this.cellArea = this.stepX * this.stepZ;

        this.heights = new Float32Array(Grid.heights);
        this.originalHeights = new Float32Array(Grid.heights);
        this.sdf2D = Grid.sdf2D || null;
        this.rainMask = Grid.rainMask || null;
        this.imprintTexture = Grid.imprintTexture || null;

        this.random = CreateRandom(this.params.seed);
        this.iteration = 0;
        this.totalIterations = this.params.iterations;
        this.dtYears = this.params.dtKyr * 1000.0;

        this.hardness = this.params.hardnessMap || BuildHardnessField(this.nx, this.nz, this.stepX, this.stepZ, this.params.strata, this.params.seed);

        this.erosionDepth = new Float32Array(this.count);
        this.depositionDepth = new Float32Array(this.count);
        this.erodedThisStep = new Float32Array(this.count);
        this.regolith = new Float32Array(this.count);
        this.soluteDepth = new Float32Array(this.count);
        this.fluxVol = new Float64Array(this.count);
        this.iceThickness = new Float32Array(this.count);
        this.isLake = new Uint8Array(this.count);
        this.lakeLevel = new Float32Array(this.count);
        this.flowAccum = new Float32Array(this.count);

        this.acc = {
            incisionVolume: 0, depositedVolume: 0, exportedVolume: 0, thermalVolume: 0,
            aeolianDeflated: 0, aeolianDeposited: 0, glacialEroded: 0, glacialIce: 0,
            soluteVolume: 0, pittedVolume: 0, maxIncision: 0, ela: null
        };
        this.done = false;
    }

    /** Runs one geological iteration. Returns a small progress record. */
    step()
    {
        if (this.done) return { iteration: this.iteration, total: this.totalIterations, done: true };

        const Nx = this.nx, Nz = this.nz, Count = this.count;
        const P = this.params;
        const Dt = this.dtYears;

        const Climate = BuildClimateField(Nx, Nz, this.heights, this.stepX, this.stepZ, P, this.iteration, this.rainMask);

        // Flow network: fill closed depressions, route steepest descent, accumulate drainage area.
        const Fill = PriorityFloodFill(Nx, Nz, this.heights, this.seaLevel);
        const Routing = ComputeFlowReceivers(Nx, Nz, Fill.filled, Fill.popRank);
        const Area = ComputeDrainageArea(Nx, Nz, Routing.recv, Fill.order, this.stepX, this.stepZ);

        // The ocean is an outlet, not a lake. Without this, every sea cell reads as "lake" and
        // the deposition term would try to fill the sea - the exact bug that made the reference
        // viewport flood. Lakes are closed basins *inside* the authored land mask, and must be
        // deeper than a noise pit to qualify.
        const LakeDepth = this.params.lakeDepthM;
        for (let I = 0; I < Count; ++I)
        {
            if ((this.sdf2D && this.sdf2D[I] > this.stepX * 1.1) ||
                !(Fill.filled[I] - this.heights[I] >= LakeDepth))
            {
                Fill.isLake[I] = 0;
            }
        }
        this.isLake = Fill.isLake;
        this.lakeLevel = Fill.filled;

        const FlowGrid = { recv: Routing.recv, recvDist: Routing.recvDist, area: Area, stepX: this.stepX, stepZ: this.stepZ };
        const StepCtx = {
            order: Fill.order, isLake: Fill.isLake, regolith: this.regolith,
            erosionDepth: this.erosionDepth, depositionDepth: this.depositionDepth,
            erodedThisStep: this.erodedThisStep, soluteDepth: this.soluteDepth,
            iceThickness: this.iceThickness, fluxVol: this.fluxVol
        };

        this.erodedThisStep.fill(0);

        // Params carries the hardness field for the incision term.
        if (!P.hardnessMap) P.hardnessMap = this.hardness;

        if (P.streamPower.enabled)
        {
            const R = ApplyStreamPower(Nx, Nz, this.heights, this.slabBottom, FlowGrid, StepCtx, P, Climate, Dt);
            this.acc.incisionVolume += R.incisionVolume;
            if (R.maxIncision > this.acc.maxIncision) this.acc.maxIncision = R.maxIncision;
        }
        if (P.deposition.enabled)
        {
            const R = ApplySedimentRouting(Nx, Nz, this.heights, this.slabBottom, this.seaLevel, FlowGrid, StepCtx, P, Dt);
            this.acc.depositedVolume += R.depositedVolume;
            this.acc.exportedVolume += R.exportedVolume;
        }
        if (P.thermal.enabled)
        {
            this.acc.thermalVolume += ApplyThermalErosion(Nx, Nz, this.heights, this.slabBottom, this.stepX, this.stepZ, P, Dt).movedVolume;
        }
        if (P.aeolian.enabled)
        {
            const R = ApplyAeolian(Nx, Nz, this.heights, this.slabBottom, this.seaLevel, this.stepX, this.stepZ, P, Climate, Dt);
            this.acc.aeolianDeflated += R.deflatedVolume;
            this.acc.aeolianDeposited += R.depositedVolume;
        }
        if (P.glacial.enabled)
        {
            const R = ApplyGlacial(Nx, Nz, this.heights, this.slabBottom, this.seaLevel, this.stepX, this.stepZ, P, Climate, Dt, StepCtx);
            this.acc.glacialEroded += R.erodedVolume;
            this.acc.glacialIce = R.iceVolume;
            if (R.ela !== undefined) this.acc.ela = R.ela;
        }
        if (P.chemical.enabled)
        {
            this.acc.soluteVolume += ApplyChemicalWeathering(Nx, Nz, this.heights, this.stepX, this.stepZ, P, Climate, Dt, StepCtx).soluteVolume;
        }
        if (P.pitting.enabled)
        {
            this.acc.pittedVolume += ApplyPitting(Nx, Nz, this.heights, this.slabBottom, this.stepX, this.stepZ, P, Climate, Dt, StepCtx, this.random).pittedVolume;
        }

        // Filters (elevation / slope gating) then hard safety clamps.
        this._applyFiltersAndClamp(StepCtx);

        this.iteration++;
        if (this.iteration >= this.totalIterations) this.done = true;
        return { iteration: this.iteration, total: this.totalIterations, done: this.done };
    }

    /** Elevation/slope filters + non-negotiable clamps (ocean floor, held coastline). */
    _applyFiltersAndClamp(StepCtx)
    {
        const Nx = this.nx, Nz = this.nz, Count = this.count;
        const P = this.params;
        const F = P.filters;
        const LandMask = this.sdf2D;
        const Cell = this.stepX * this.stepZ;

        for (let Z = 0; Z < Nz; ++Z)
        {
            for (let X = 0; X < Nx; ++X)
            {
                const I = Z * Nx + X;
                if (LandMask && LandMask[I] > this.stepX * 1.1) continue; // Outside the authored mask

                let H = this.heights[I];

                // Hard numeric guard: a non-finite height would poison the whole mesh, so a
                // process that misbehaves can only ever cost one cell, never the bake.
                if (!Number.isFinite(H))
                {
                    H = this.originalHeights[I];
                    if (!Number.isFinite(H)) H = this.slabBottom + 0.5;
                    this.numericRecoveries = (this.numericRecoveries || 0) + 1;
                }
                else if (H < this.slabBottom - 400.0)
                {
                    // Unphysical depth spike: clamp back to the thin-slab floor.
                    H = this.slabBottom + 0.3;
                }

                // Restoring eroded material is only allowed inside the filter band.
                if (H < F.minElev || H > F.maxElev) { this.heights[I] = H; continue; }

                // Absolute flood guard. Inside the authored mask, open ground can never go deeper
                // than maxLandDrownedM below sea level - so the ocean plane can never swallow the
                // landmass. Enclosed lake basins are exempt (they are what lakes ARE).
                if (!this.isLake[I])
                {
                    const DrownedFloor = this.seaLevel - P.maxLandDrownedM;
                    if (H < DrownedFloor) H = DrownedFloor;
                }

                if (P.preserveShoreline && LandMask)
                {
                    const Inland = Math.max(0.0, -LandMask[I]);
                    const ShoreW = Smoothstep(0.0, 90.0, Inland);
                    const Floor = Mix(this.slabBottom + 0.5, this.seaLevel + P.shoreLiftM, ShoreW * 0.25);
                    if (H < Floor) H = Floor;
                }
                if (H < this.slabBottom + 0.3) H = this.slabBottom + 0.3;
                this.heights[I] = H;
            }
        }
    }

    /** Final flow accumulation for texturing (one extra routing pass, cheap relative to the run). */
    finalizeFlowFields()
    {
        const Fill = PriorityFloodFill(this.nx, this.nz, this.heights, this.seaLevel);
        const Routing = ComputeFlowReceivers(this.nx, this.nz, Fill.filled, Fill.popRank);
        const Area = ComputeDrainageArea(this.nx, this.nz, Routing.recv, Fill.order, this.stepX, this.stepZ);
        for (let I = 0; I < this.count; ++I) this.flowAccum[I] = Area[I] / this.cellArea;
        const LakeDepth = this.params.lakeDepthM;
        for (let I = 0; I < this.count; ++I)
        {
            if ((this.sdf2D && this.sdf2D[I] > this.stepX * 1.1) ||
                !(Fill.filled[I] - this.heights[I] >= LakeDepth))
            {
                Fill.isLake[I] = 0;
            }
        }
        this.isLake = Fill.isLake;
        this.lakeLevel = Fill.filled;
    }

    /** Re-imprints authored micro-detail so brushwork survives the simulation. */
    applyImprint()
    {
        if (!this.imprintTexture) return;
        for (let I = 0; I < this.count; ++I) this.heights[I] += this.imprintTexture[I];
    }

    /** Final result object consumed by the layer stack / viewport. */
    buildResult()
    {
        let MeanErosion = 0, MeanDeposition = 0, MaxErosion = 0;
        for (let I = 0; I < this.count; ++I)
        {
            MeanErosion += this.erosionDepth[I];
            MeanDeposition += this.depositionDepth[I];
            if (this.erosionDepth[I] > MaxErosion) MaxErosion = this.erosionDepth[I];
        }
        MeanErosion /= this.count;
        MeanDeposition /= this.count;

        return {
            heights: this.heights,
            originalHeights: this.originalHeights,
            erosionDepth: this.erosionDepth,
            depositionDepth: this.depositionDepth,
            regolith: this.regolith,
            soluteDepth: this.soluteDepth,
            iceThickness: this.iceThickness,
            lakeMask: this.isLake,
            lakeLevel: this.lakeLevel,
            flowAccumCells: this.flowAccum,
            hardness: this.hardness,
            params: this.params,
            stats: {
                meanErosionM: MeanErosion,
                meanDepositionM: MeanDeposition,
                maxErosionM: MaxErosion,
                incisionVolumeM3: this.acc.incisionVolume,
                depositedVolumeM3: this.acc.depositedVolume,
                exportedVolumeM3: this.acc.exportedVolume,
                netVolumeM3: this.acc.incisionVolume - this.acc.depositedVolume - this.acc.exportedVolume,
                thermalVolumeM3: this.acc.thermalVolume,
                aeolianDeflatedM3: this.acc.aeolianDeflated,
                aeolianDepositedM3: this.acc.aeolianDeposited,
                glacialErodedM3: this.acc.glacialEroded,
                glacialIceM3: this.acc.glacialIce,
                soluteVolumeM3: this.acc.soluteVolume,
                pittedVolumeM3: this.acc.pittedVolume,
                equilibriumLineAltitudeM: this.acc.ela,
                numericRecoveries: this.numericRecoveries || 0,
                iterations: this.iteration,
                dtKyr: this.params.dtKyr,
                simulatedKyr: this.iteration * this.params.dtKyr
            }
        };
    }
}

/**
 * Synchronous entry point (used by tests, CLI and the headless tools). The interactive studio uses
 * `ErosionSimulationState.step()` across animation frames instead.
 */
export function RunErosionSimulation(Grid, ParamsIn = {}, Options = {})
{
    const Started = (typeof performance !== 'undefined' ? performance.now() : Date.now());
    const State = new ErosionSimulationState(Grid, ParamsIn);
    const Iterations = Math.min(State.totalIterations, Options.iterations || State.totalIterations);

    for (let I = 0; I < Iterations; ++I)
    {
        State.step();
        if (Options.onProgress) Options.onProgress((I + 1) / Iterations, `Erosion iteration ${I + 1}/${Iterations}`);
    }
    State.applyImprint();
    State.finalizeFlowFields();

    const Result = State.buildResult();
    Result.stats.elapsedMs = (typeof performance !== 'undefined' ? performance.now() : Date.now()) - Started;
    return Result;
}

/*------------------------------------------------------------------------------------------------------------------------------------------
                                        SHARED-WEATHER GROUPS (simulate N layers with ONE climate)
------------------------------------------------------------------------------------------------------------------------------------------*/
/**
 * Several erosion layers can share a single weather regime so they evolve under the *same*
 * rainfall/wind/season cycle - your "simulate 2 layers with the same weather/pattern/cycle".
 * The group key derives a deterministic climate mask; identical layers + identical mask produce
 * bit-identical results, which is exactly what you want when two terrains must match at a seam.
 */
export function MakeWeatherGroupKey(GroupId, Seed)
{
    return `${GroupId || 'solo'}::${Seed | 0}`;
}

export function BuildWeatherRegimeMask(Nx, Nz, GroupId, Seed, Intensity = 1.0, StepX = 1, StepZ = 1)
{
    const Mask = new Float32Array(Nx * Nz);
    const Salt = String(GroupId || 'solo').split('').reduce((A, C) => (Math.imul(A, 31) + C.charCodeAt(0)) | 0, 7);
    for (let Z = 0; Z < Nz; ++Z)
    {
        for (let X = 0; X < Nx; ++X)
        {
            const Wx = X * StepX, Wz = Z * StepZ;
            const N1 = FbmNoise2D(Wx * 0.0016, Wz * 0.0016, 4, (Seed ^ Salt) + 11);
            const Belt = 0.75 + 0.45 * Math.sin((Wx + Wz) * 0.0009 + (Salt % 7));
            Mask[Z * Nx + X] = Clamp(Mix(0.35, 1.65, N1) * Belt * Intensity, 0, 3);
        }
    }
    return Mask;
}

/** Convenience: does this parameter set enable any process that changes the surface? */
export function HasActiveProcesses(Params)
{
    const P = NormalizeErosionParams(Params);
    return P.streamPower.enabled || P.thermal.enabled || P.aeolian.enabled ||
           P.glacial.enabled || P.chemical.enabled || P.pitting.enabled;
}
