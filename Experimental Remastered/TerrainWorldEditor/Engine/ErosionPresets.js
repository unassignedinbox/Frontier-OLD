/*==========================================================================================================================================
                                                         EROSIONPRESETS.JS
==========================================================================================================================================*/
/**
 * Geological erosion presets for the SDF Terrain Studio.
 *
 * These are the same *landscapes* as the reference studio (arena/68c84c0d-frontier), but every
 * parameter here is a measurable quantity (mm/yr, C, mm/yr rain, repose angle) rather than a
 * 0..1 cosmetic dial. Where the reference exposed `erosionStrength` and `sedimentCapacity`, this
 * exposes bedrock erodibility K, m/n exponents, transport capacity and talus angle - which is
 * what actually determines whether you get a gorge or a smooth hill.
 *
 * Each preset also declares a `landform` block so the studio can rebuild a matching base stack
 * (multifractal / ridged / voronoi / billow) before erosion runs.
 */

const Base = {
    iterations: 36,
    dtKyr: 12.0,
    seed: 4029,
    upliftMmYr: 0.0000,
    streamPower: { enabled: true, erodibility: 0.55, m: 0.5, n: 1.0, minSlope: 0.0025, hardRockResistance: 0.45, regolithFactor: 5.5 },
    deposition: { enabled: true, capacity: 0.5, fillRate: 0.45, lakeFill: 0.85, maxAggradationM: 4.0 },
    thermal: { enabled: true, talusDeg: 33, rate: 0.55, passes: 2 },
    aeolian: { enabled: false, strength: 0, saltationLengthM: 42, duneWavelengthM: 155, duneAmplitudeM: 7.5, deflationHardness: 0.5 },
    glacial: { enabled: false, elaOffsetM: -18, massBalanceGrad: 0.0075, flowConstant: 0.55, erosionK: 0.42, maxIceThicknessM: 420, slidingExponent: 2.0 },
    chemical: { enabled: false, rate: 0.35, temperatureSensitivity: 0.6, solubility: 0.45, carbonateBoost: 0.35 },
    pitting: { enabled: false, density: 0.45, depthM: 1.6, siteScaleM: 26, exposureBias: 0.55 },
    climate: {
        rainMmYr: 1150, pattern: 'orographic', windAzimuthDeg: 38, windSpeed: 0.45,
        meanTemperatureC: 11, lapseRateCPerKm: 6.2, seasonalAmplitude: 0.28,
        seasonPeriodIters: 9, freezeThawCyclesPerYr: 55
    },
    strata: { enabled: false, bedFrequency: 9, dipDeg: 4, strikeDeg: 25, caprockContrast: 0.75, jointSpacingM: 0, jointStrength: 0.5 },
    imprint: { amplitudeM: 3.0, scaleM: 13.0 },
    preserveShoreline: true,
    shoreLiftM: 0.35
};

/** Deep merge that keeps arrays/typed arrays by reference. */
function Merge(...Sources)
{
    const Out = {};
    for (const Src of Sources)
    {
        if (!Src) continue;
        for (const [K, V] of Object.entries(Src))
        {
            if (V && typeof V === 'object' && !Array.isArray(V) && !ArrayBuffer.isView(V))
            {
                Out[K] = Merge(Out[K] && typeof Out[K] === 'object' ? Out[K] : {}, V);
            }
            else
            {
                Out[K] = V;
            }
        }
    }
    return Out;
}

const Preset = (Id, Name, Subtitle, Params, Landform, Notes) => ({
    id: Id,
    name: Name,
    subtitle: Subtitle,
    params: Merge(Base, Params),
    landform: Landform || { type: 'multifractal', amplitude: 150, scale: 2.4 },
    notes: Notes || ''
});

export const EROSION_PRESETS = [
    Preset('icelandic-highlands', 'Icelandic Glacial Highlands',
        'Glacial troughs, frost-shattered palagonite, moss-covered lava & black-sand outwash',
        {
            upliftMmYr: 0.0100,
            climate: { rainMmYr: 1650, pattern: 'coastal', meanTemperatureC: 2.5, windSpeed: 0.62, windAzimuthDeg: -24, freezeThawCyclesPerYr: 120, seasonalAmplitude: 0.35 },
            streamPower: { erodibility: 0.46, m: 0.5, n: 1.0, hardRockResistance: 0.42 },
            thermal: { talusDeg: 36, rate: 0.62 },
            glacial: { enabled: true, erosionK: 0.46, elaOffsetM: -40, maxIceThicknessM: 380 },
            pitting: { enabled: true, density: 0.6, depthM: 2.1, siteScaleM: 22 },
            chemical: { enabled: true, rate: 0.28, solubility: 0.52 },
            strata: { enabled: true, bedFrequency: 7, dipDeg: 2, strikeDeg: -10, caprockContrast: 0.62 }
        },
        { type: 'ridged', amplitude: 168, scale: 2.5, sharpness: 0.72, octaves: 6 },
        'Cold, wet, repeatedly glaciated: ice does the excavating, frost does the detailing.'),

    Preset('himalayan-karakoram', 'Himalayan Karakoram',
        'Asymmetric monsoon-flank massifs, dendritic snow aretes & U-shaped glacier gorges',
        {
            upliftMmYr: 0.1375,
            climate: { rainMmYr: 850, pattern: 'orographic', meanTemperatureC: -2.0, lapseRateCPerKm: 6.6, windSpeed: 0.55, windAzimuthDeg: -25, freezeThawCyclesPerYr: 160, seasonalAmplitude: 0.3 },
            streamPower: { erodibility: 0.42, m: 0.5, n: 1.0, hardRockResistance: 0.72 },
            thermal: { talusDeg: 38, rate: 0.68 },
            glacial: { enabled: true, erosionK: 0.58, elaOffsetM: -10, flowConstant: 0.62, maxIceThicknessM: 620, slidingExponent: 2.4 },
            pitting: { enabled: true, density: 0.75, depthM: 2.4, siteScaleM: 18 },
            chemical: { enabled: true, rate: 0.2, solubility: 0.35 },
            imprint: { amplitudeM: 4.2, scaleM: 11 }
        },
        { type: 'ridged', amplitude: 225, scale: 2.9, sharpness: 0.82, octaves: 7, azimuth: -25 },
        'Fast uplift + strong orographic gradient = the highest relief in the set.'),

    Preset('layered-plateau', 'Stratified Canyon Plateau',
        'Utah-style hard sandstone caprock & soft shale benches cut by rain and wash',
        {
            upliftMmYr: 0.0312,
            climate: { rainMmYr: 320, pattern: 'arid', meanTemperatureC: 15.0, windSpeed: 0.48, windAzimuthDeg: 40, freezeThawCyclesPerYr: 90, seasonalAmplitude: 0.22 },
            streamPower: { erodibility: 0.58, m: 0.55, n: 1.05, hardRockResistance: 0.55 },
            deposition: { enabled: true, capacity: 0.62, fillRate: 0.55, maxAggradationM: 6.5 },
            thermal: { talusDeg: 31, rate: 0.72 },
            pitting: { enabled: true, density: 0.5, depthM: 1.8, siteScaleM: 30 },
            aeolian: { enabled: true, strength: 0.34, saltationLengthM: 55, duneWavelengthM: 210, duneAmplitudeM: 6, deflationHardness: 0.65 },
            strata: { enabled: true, bedFrequency: 11, dipDeg: 6, strikeDeg: 24, caprockContrast: 0.86, jointSpacingM: 220, jointStrength: 0.65 }
        },
        { type: 'voronoi', amplitude: 142, scale: 3.0, variant: 0, sharpness: 0.7 },
        'The benches come from hardness contrast between beds, not from the wind. This is what the reference could not do with a soft-heightmap droplet scheme.'),

    Preset('alpine-divide', 'Tectonic Alpine Divide',
        'Folded thrust ridge, snow-dusted summits, alluvial fans & hanging valleys',
        {
            upliftMmYr: 0.0813,
            climate: { rainMmYr: 1450, pattern: 'orographic', meanTemperatureC: 6.0, windSpeed: 0.45, windAzimuthDeg: 15, freezeThawCyclesPerYr: 95, seasonalAmplitude: 0.32 },
            streamPower: { erodibility: 0.5, m: 0.5, n: 1.0, hardRockResistance: 0.5 },
            deposition: { capacity: 0.55, fillRate: 0.5, maxAggradationM: 5.0 },
            thermal: { talusDeg: 35, rate: 0.6 },
            glacial: { enabled: true, erosionK: 0.4, elaOffsetM: 40, maxIceThicknessM: 300 },
            pitting: { enabled: true, density: 0.45, depthM: 1.5 },
            chemical: { enabled: true, rate: 0.3 }
        },
        { type: 'ridged', amplitude: 176, scale: 2.6, sharpness: 0.74, azimuth: 12 },
        'Balanced fluvial + glacial: the classic horn/arete/alluvial-fan package.'),

    Preset('fault-escarpment', 'Fault Escarpment & Basin',
        'Straight fault-line scarp, triangular facets, bajada & closed basin',
        {
            upliftMmYr: 0.0563,
            climate: { rainMmYr: 240, pattern: 'arid', meanTemperatureC: 17.0, windSpeed: 0.5, windAzimuthDeg: 70, seasonalAmplitude: 0.2 },
            streamPower: { erodibility: 0.6, m: 0.5, n: 1.0, hardRockResistance: 0.4 },
            deposition: { capacity: 0.72, fillRate: 0.72, lakeFill: 0.95, maxAggradationM: 9.0 },
            thermal: { talusDeg: 33, rate: 0.8 },
            aeolian: { enabled: true, strength: 0.28, duneAmplitudeM: 4.5, duneWavelengthM: 180 },
            strata: { enabled: true, bedFrequency: 8, dipDeg: 22, strikeDeg: 8, caprockContrast: 0.7, jointSpacingM: 300, jointStrength: 0.6 }
        },
        { type: 'ridged', amplitude: 155, scale: 2.2, sharpness: 0.8, azimuth: 8 },
        'Low rain + high talus = facets stay sharp; basin fills with alluvium (good deposition test).'),

    Preset('volcanic-caldera', 'Volcanic Caldera & Domes',
        'Collapsed caldera rim, lava domes, radiating barrancas & crater lake',
        {
            upliftMmYr: 0.0063,
            climate: { rainMmYr: 620, pattern: 'altitudeBands', meanTemperatureC: 13.0, windSpeed: 0.42, windAzimuthDeg: 120, seasonalAmplitude: 0.24 },
            streamPower: { erodibility: 0.62, m: 0.5, n: 1.0, hardRockResistance: 0.35, regolithFactor: 7.0 },
            deposition: { capacity: 0.55, lakeFill: 0.98, maxAggradationM: 4.0 },
            thermal: { talusDeg: 34, rate: 0.62 },
            chemical: { enabled: true, rate: 0.5, solubility: 0.68, carbonateBoost: 0.2 },
            pitting: { enabled: true, density: 0.35, depthM: 1.2 }
        },
        { type: 'voronoi', amplitude: 150, scale: 3.2, variant: 2, sharpness: 0.66 },
        'Radial drainage on a cone is a great test of the flow router.'),

    Preset('badlands-domes', 'Badlands Domes',
        'Finely rilled shale badlands, popcorn domes & dense dendritic rills',
        {
            upliftMmYr: 0.0187,
            climate: { rainMmYr: 420, pattern: 'stormTracks', meanTemperatureC: 12.0, windSpeed: 0.5, windAzimuthDeg: 30, seasonalAmplitude: 0.4 },
            streamPower: { erodibility: 0.82, m: 0.52, n: 1.1, hardRockResistance: 0.2, regolithFactor: 9.0, minSlope: 0.001 },
            deposition: { capacity: 0.6, fillRate: 0.6, maxAggradationM: 3.0 },
            thermal: { talusDeg: 28, rate: 0.85, passes: 3 },
            pitting: { enabled: true, density: 0.85, depthM: 1.0, siteScaleM: 12 },
            chemical: { enabled: true, rate: 0.55, solubility: 0.6 }
        },
        { type: 'billow', amplitude: 78, scale: 3.6, octaves: 5, duneMix: 0.1 },
        'Very high K on soft rock: dense first-order rills appear within ~20 iterations.'),

    Preset('coastal-massif', 'Coastal Massif',
        'Sheer sea cliffs, wave-cut platform, coastal monsoon & deep lateritic soil',
        {
            upliftMmYr: 0.0250,
            climate: { rainMmYr: 2100, pattern: 'coastal', meanTemperatureC: 22.0, windSpeed: 0.55, windAzimuthDeg: 95, freezeThawCyclesPerYr: 2, seasonalAmplitude: 0.3 },
            streamPower: { erodibility: 0.55, m: 0.5, n: 1.0, hardRockResistance: 0.35, regolithFactor: 11.0 },
            deposition: { capacity: 0.5 },
            chemical: { enabled: true, rate: 0.85, temperatureSensitivity: 0.85, solubility: 0.8, carbonateBoost: 0.5 },
            thermal: { talusDeg: 40, rate: 0.5 },
            strata: { enabled: true, bedFrequency: 6, dipDeg: 12, strikeDeg: 95, caprockContrast: 0.55 }
        },
        { type: 'multifractal', amplitude: 185, scale: 2.2, gain: 0.55 },
        'Hot + wet + deep regolith = rounded, convex-up divides (humid geomorphology).'),

    Preset('grand-canyon', 'Grand Canyon Gorge',
        'Vishnu inner gorge, Redwall benches & a through-flowing trunk river',
        {
            upliftMmYr: 0.0250,
            climate: { rainMmYr: 380, pattern: 'arid', meanTemperatureC: 16.0, windSpeed: 0.38, windAzimuthDeg: 36, freezeThawCyclesPerYr: 60, seasonalAmplitude: 0.2 },
            streamPower: { erodibility: 0.62, m: 0.55, n: 1.05, hardRockResistance: 0.68, minSlope: 0.0015 },
            deposition: { enabled: true, capacity: 0.86, fillRate: 0.4, maxAggradationM: 3.0 },
            thermal: { talusDeg: 29, rate: 0.78, passes: 3 },
            pitting: { enabled: true, density: 0.4, depthM: 1.4 },
            strata: { enabled: true, bedFrequency: 13, dipDeg: 3, strikeDeg: 10, caprockContrast: 0.95, jointSpacingM: 180, jointStrength: 0.8 }
        },
        { type: 'voronoi', amplitude: 190, scale: 3.4, variant: 0, sharpness: 0.78 },
        'Extreme bed contrast (0.95) is what makes the stair-step profile; drop it and you get a smooth V.'),

    Preset('studlagil-basalt', 'Studlagil Basalt Canyon',
        'Hexagonal basalt colonnades, glacial meltwater & turquoise plunge pools',
        {
            upliftMmYr: 0.0075,
            climate: { rainMmYr: 1250, pattern: 'coastal', meanTemperatureC: 3.0, windSpeed: 0.48, windAzimuthDeg: -30, freezeThawCyclesPerYr: 130 },
            streamPower: { erodibility: 0.44, m: 0.5, n: 1.0, hardRockResistance: 0.75 },
            thermal: { talusDeg: 39, rate: 0.5 },
            glacial: { enabled: true, erosionK: 0.38, elaOffsetM: 10, maxIceThicknessM: 250 },
            pitting: { enabled: true, density: 0.55, depthM: 1.9, siteScaleM: 15 },
            strata: { enabled: true, bedFrequency: 16, dipDeg: 2, strikeDeg: -8, caprockContrast: 0.5 }
        },
        { type: 'voronoi', amplitude: 148, scale: 3.6, variant: 1, sharpness: 0.75 },
        'High-frequency regular beds + a single trunk channel reproduce columnar walls.'),

    Preset('fjadrargljufur-palagonite', 'Fjadrargljufur Canyon',
        'Serpentine palagonite gorge, moss benches & meltwater meander',
        {
            upliftMmYr: 0.0063,
            climate: { rainMmYr: 1780, pattern: 'coastal', meanTemperatureC: 3.5, windSpeed: 0.44, windAzimuthDeg: -30, freezeThawCyclesPerYr: 115 },
            streamPower: { erodibility: 0.56, m: 0.5, n: 1.0, hardRockResistance: 0.45 },
            thermal: { talusDeg: 35, rate: 0.66 },
            glacial: { enabled: true, erosionK: 0.42, elaOffsetM: -20, maxIceThicknessM: 300 },
            pitting: { enabled: true, density: 0.6, depthM: 2.0 },
            chemical: { enabled: true, rate: 0.35, solubility: 0.5 }
        },
        { type: 'ridged', amplitude: 142, scale: 2.4, sharpness: 0.66 },
        'Sinuous gorge: pair with a river spline layer to lock the meander path early.'),

    Preset('namib-erg-dunes', 'Namib Erg Dunes',
        'Transverse dune field, sharp crests, star dunes at junctions & interdune corridors',
        {
            upliftMmYr: 0.0000,
            climate: { rainMmYr: 25, pattern: 'arid', meanTemperatureC: 19.0, windSpeed: 0.85, windAzimuthDeg: 5, freezeThawCyclesPerYr: 10, seasonalAmplitude: 0.15 },
            streamPower: { enabled: false, erodibility: 0.3 },
            deposition: { enabled: false },
            thermal: { talusDeg: 32, rate: 0.9, passes: 3 },
            aeolian: { enabled: true, strength: 0.95, saltationLengthM: 190, duneWavelengthM: 240, duneAmplitudeM: 22, deflationHardness: 0.15 },
            chemical: { enabled: false }
        },
        { type: 'billow', amplitude: 42, scale: 3.4, octaves: 5, duneMix: 0.55, azimuth: 5 },
        'Pure wind end-member: fluvial off entirely, so every form is saltation.'),

    Preset('sahara-tassili-erg', 'Sahara Tassili Erg',
        'Wind-scoured sandstone plateau, yardang corridors & encroaching sand sheets',
        {
            upliftMmYr: 0.0025,
            climate: { rainMmYr: 15, pattern: 'arid', meanTemperatureC: 26.0, windSpeed: 0.8, windAzimuthDeg: 300, freezeThawCyclesPerYr: 35, seasonalAmplitude: 0.1 },
            streamPower: { erodibility: 0.35, m: 0.5, n: 1.0, hardRockResistance: 0.6 },
            deposition: { enabled: true, capacity: 0.4, fillRate: 0.3 },
            thermal: { talusDeg: 30, rate: 0.7 },
            aeolian: { enabled: true, strength: 0.8, saltationLengthM: 240, duneWavelengthM: 320, duneAmplitudeM: 16, deflationHardness: 0.55 },
            pitting: { enabled: true, density: 0.4, depthM: 1.1 },
            strata: { enabled: true, bedFrequency: 5, dipDeg: 1, strikeDeg: 300, caprockContrast: 0.8, jointSpacingM: 420, jointStrength: 0.7 }
        },
        { type: 'ridged', amplitude: 120, scale: 2.6, sharpness: 0.6, azimuth: 300 },
        'Yardangs form because windward faces are softer (deflationHardness) than the caprock.'),

    Preset('rugged-desert-crags', 'Rugged Desert Crags',
        'Fractured granite spires, tors & boulder-strewn pediments',
        {
            upliftMmYr: 0.0125,
            climate: { rainMmYr: 180, pattern: 'arid', meanTemperatureC: 18.0, windSpeed: 0.55, windAzimuthDeg: 45, freezeThawCyclesPerYr: 70 },
            streamPower: { erodibility: 0.5, m: 0.5, n: 1.05, hardRockResistance: 0.8, minSlope: 0.01 },
            thermal: { talusDeg: 36, rate: 0.85 },
            pitting: { enabled: true, density: 1.0, depthM: 3.0, siteScaleM: 20, exposureBias: 0.8 },
            strata: { enabled: true, bedFrequency: 4, dipDeg: 70, strikeDeg: 40, caprockContrast: 0.6, jointSpacingM: 90, jointStrength: 0.9 }
        },
        { type: 'voronoi', amplitude: 128, scale: 3.8, variant: 1, sharpness: 0.85 },
        'Steep beds + strong joints + aggressive pitting = spires rather than smooth hills.'),

    Preset('desert-oasis-erg', 'Desert Oasis Basin',
        'Blown sand sheet, artesian spring oasis & evaporite flats',
        {
            upliftMmYr: 0.0025,
            climate: { rainMmYr: 60, pattern: 'arid', meanTemperatureC: 23.0, windSpeed: 0.7, windAzimuthDeg: 65, freezeThawCyclesPerYr: 25 },
            streamPower: { erodibility: 0.4, m: 0.5, n: 1.0, hardRockResistance: 0.4 },
            deposition: { enabled: true, capacity: 0.5, fillRate: 0.5, lakeFill: 1.0, maxAggradationM: 5.0 },
            thermal: { talusDeg: 31, rate: 0.7 },
            aeolian: { enabled: true, strength: 0.7, saltationLengthM: 140, duneWavelengthM: 200, duneAmplitudeM: 12, deflationHardness: 0.3 },
            chemical: { enabled: true, rate: 0.4, solubility: 0.75, carbonateBoost: 0.9 },
            strata: { enabled: true, bedFrequency: 6, dipDeg: 2, strikeDeg: 65, caprockContrast: 0.5 }
        },
        { type: 'billow', amplitude: 62, scale: 3.2, octaves: 5, duneMix: 0.35 },
        'Closed basin: lakeFill = 1.0 makes the depression trap its sediment instead of flooding.'),

    Preset('icelandic-green-highlands', 'Icelandic Green Highlands',
        'Moss-covered palagonite ridges, meltwater braidplains & low-relief highlands',
        {
            upliftMmYr: 0.0050,
            climate: { rainMmYr: 2200, pattern: 'coastal', meanTemperatureC: 4.0, windSpeed: 0.6, windAzimuthDeg: -20, freezeThawCyclesPerYr: 100 },
            streamPower: { erodibility: 0.62, m: 0.5, n: 1.0, hardRockResistance: 0.3, regolithFactor: 8.0 },
            deposition: { capacity: 0.6, fillRate: 0.6, maxAggradationM: 5.5 },
            thermal: { talusDeg: 33, rate: 0.7 },
            glacial: { enabled: true, erosionK: 0.3, elaOffsetM: 60, maxIceThicknessM: 220 },
            chemical: { enabled: true, rate: 0.6, solubility: 0.7 },
            pitting: { enabled: true, density: 0.5, depthM: 1.4 }
        },
        { type: 'multifractal', amplitude: 96, scale: 2.6, gain: 0.5 },
        'Wet + soft rock + moderate ice: broad, rounded, heavily vegetated terrain.'),

    Preset('lunar-mare-craters', 'Lunar Mare & Craters',
        'Airless surface: zero fluvial, zero wind; only mass wasting and impact pitting',
        {
            upliftMmYr: 0.0000,
            climate: { rainMmYr: 0, pattern: 'arid', meanTemperatureC: -20.0, windSpeed: 0, freezeThawCyclesPerYr: 0, seasonalAmplitude: 0.0 },
            streamPower: { enabled: false, erodibility: 0.2 },
            deposition: { enabled: false },
            thermal: { enabled: true, talusDeg: 26, rate: 0.25, passes: 1 },
            aeolian: { enabled: false },
            pitting: { enabled: true, density: 1.0, depthM: 4.5, siteScaleM: 55, exposureBias: 0.1 },
            chemical: { enabled: false },
            imprint: { amplitudeM: 5.0, scaleM: 9 }
        },
        { type: 'voronoi', amplitude: 110, scale: 3.0, variant: 2, sharpness: 0.55 },
        'Useful control case: if anything other than pitting changes the surface, a process leaked.'),

    Preset('yardang-wind-corridor', 'Yardang Wind Corridor',
        'Aligned yardang hulls, sand-streaked corridors & bedrock flutes',
        {
            upliftMmYr: 0.0037,
            climate: { rainMmYr: 40, pattern: 'stormTracks', meanTemperatureC: 20.0, windSpeed: 0.95, windAzimuthDeg: 20, freezeThawCyclesPerYr: 40 },
            streamPower: { erodibility: 0.42, m: 0.5, n: 1.0, hardRockResistance: 0.55 },
            deposition: { enabled: true, capacity: 0.35, fillRate: 0.35 },
            thermal: { talusDeg: 30, rate: 0.65 },
            aeolian: { enabled: true, strength: 1.0, saltationLengthM: 280, duneWavelengthM: 420, duneAmplitudeM: 10, deflationHardness: 0.72 },
            strata: { enabled: true, bedFrequency: 9, dipDeg: 8, strikeDeg: 20, caprockContrast: 0.85, jointSpacingM: 260, jointStrength: 0.55 }
        },
        { type: 'ridged', amplitude: 108, scale: 2.4, sharpness: 0.7, azimuth: 20 },
        'Wind-aligned rock: hardness contrast + only ~40 mm/yr rain keeps the rain from smoothing it.'),

    Preset('glacial-lake-watershed', 'Glacial Lake Watershed',
        'Over-deepened cirques, moraine dams & a large proglacial lake chain',
        {
            upliftMmYr: 0.0375,
            climate: { rainMmYr: 1500, pattern: 'orographic', meanTemperatureC: 1.0, windSpeed: 0.4, windAzimuthDeg: 60, freezeThawCyclesPerYr: 150 },
            streamPower: { erodibility: 0.5, m: 0.5, n: 1.0, hardRockResistance: 0.5 },
            deposition: { enabled: true, capacity: 0.85, fillRate: 0.75, lakeFill: 1.0, maxAggradationM: 8.0 },
            thermal: { talusDeg: 34, rate: 0.65 },
            glacial: { enabled: true, erosionK: 0.68, elaOffsetM: -60, flowConstant: 0.7, maxIceThicknessM: 520 },
            pitting: { enabled: true, density: 0.7, depthM: 2.2 }
        },
        { type: 'ridged', amplitude: 160, scale: 2.7, sharpness: 0.7 },
        'The best preset for verifying that lakes trap sediment instead of flooding the viewport.')
];

/** Ordered list suitable for UI dropdowns. */
export const EROSION_PRESET_OPTIONS = EROSION_PRESETS.map(P => [P.name, P.id]);

export function GetErosionPreset(Id)
{
    return EROSION_PRESETS.find(P => P.id === Id) || null;
}

/** Deep-copies a preset's params so callers can mutate safely. */
export function ClonePresetParams(Id)
{
    const P = GetErosionPreset(Id);
    if (!P) return null;
    return JSON.parse(JSON.stringify(P.params));
}

/** Environment/experiment modes: exactly the reference's emitter modes, re-expressed as process toggles. */
export const WEATHER_MODES = [
    { id: 'all-weather', label: 'All Weather', subtitle: 'Rain, wind, frost and mass wasting together', apply: {} },
    { id: 'sky-rain', label: 'Sky Rain', subtitle: 'Fluvial incision + sediment routing only', apply: { aeolian: { enabled: false }, glacial: { enabled: false }, pitting: { enabled: false }, thermal: { enabled: true, rate: 0.35 } } },
    { id: 'glacial-blizzard', label: 'Glacial Blizzard', subtitle: 'Ice flow + frost pitting + light meltwater', apply: { glacial: { enabled: true }, pitting: { enabled: true, density: 0.8 }, streamPower: { erodibility: 0.35 } } },
    { id: 'aeolian-wind', label: 'Aeolian Wind', subtitle: 'Saltation and dune building only', apply: { streamPower: { enabled: false }, deposition: { enabled: false }, aeolian: { enabled: true, strength: 0.9 }, glacial: { enabled: false } } },
    { id: 'rockfall-avalanche', label: 'Rockfall & Avalanche', subtitle: 'Talus-driven mass wasting, no water', apply: { streamPower: { enabled: false }, deposition: { enabled: false }, thermal: { enabled: true, rate: 0.95, passes: 4 }, aeolian: { enabled: false } } },
    { id: 'storm-cell', label: 'Storm Cell', subtitle: 'Extreme rainfall, high sediment export', apply: { climate: { rainMmYr: 3200, pattern: 'stormTracks' }, streamPower: { erodibility: 0.78 }, deposition: { capacity: 0.7 } } }
];

export function GetWeatherMode(Id)
{
    return WEATHER_MODES.find(M => M.id === Id) || null;
}

/**
 * Applies a weather mode to a parameter object, returning a NEW object.
 * Kept separate from the preset so a user can take "Icelandic Highlands" and run it
 * as "rockfall only" without losing the underlying geological parameters.
 */
export function ApplyWeatherMode(Params, ModeId)
{
    const Mode = GetWeatherMode(ModeId);
    const Copy = JSON.parse(JSON.stringify(Params || {}));
    if (!Mode) return Copy;
    return Merge(Copy, Mode.apply);
}

/** The same presets as `[label, id]` pairs for a compact select row. */
export const WEATHER_MODE_OPTIONS = WEATHER_MODES.map(M => [M.label, M.id]);

export { Base as BASE_EROSION_PARAMS, Merge as MergeErosionParams };
