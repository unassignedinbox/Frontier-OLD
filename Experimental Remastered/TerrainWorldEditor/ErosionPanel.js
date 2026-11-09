/*==========================================================================================================================================
                                                        EROSIONPANEL.JS
==========================================================================================================================================*/
/**
 * Erosion Simulator inspector UI for the SDF Terrain Studio.
 *
 * This is the authoring surface for ErosionEngine.js: presets, the weather regime,
 * per-process toggles with physical units, shared-weather groups, the live bake progress
 * and the mass-balance readout that tells you whether the numbers are physically sane.
 */

import {
    MakeEl,
    MakePropertyGroup,
    MakeSelectRow,
    MakeSliderPillRow,
    ConstructGlyph
} from './StudioControls.js';

import {
    EROSION_PRESETS,
    EROSION_PRESET_OPTIONS,
    WEATHER_MODES,
    WEATHER_MODE_OPTIONS,
    ApplyWeatherMode,
    ClonePresetParams,
    GetErosionPreset,
    GetWeatherMode,
    MergeErosionParams
} from './Engine/ErosionPresets.js';

import { CLIMATE_PATTERNS, PROCESS_META } from './Engine/ErosionEngine.js';
import { CreateTerrainLayer, InvalidateErosionLayer } from './Engine/TerrainLayerStack.js';

const CLIMATE_PATTERN_OPTIONS = CLIMATE_PATTERNS.map(P => [P[0], P[1]]);

/** Small labelled number row that writes a nested parameter and reports live edits. */
function NumberRow(Label, Min, Max, Step, Value, Decimals, Unit, OnInput, OnCommit)
{
    return MakeSliderPillRow(Label, Min, Max, Step, Value, Decimals, Unit, OnInput, OnCommit);
}

/** Compact metric tile used for the mass-balance readout. */
function StatTile(Label, Value, Sub = '')
{
    return MakeEl('div', 'kpi-card', [
        MakeEl('span', 'kpi-label', Label),
        MakeEl('strong', 'kpi-val', Value),
        Sub ? MakeEl('span', 'kpi-sub', Sub) : null
    ]);
}

const FormatM3 = V =>
{
    if (!Number.isFinite(V)) return '—';
    const A = Math.abs(V);
    if (A >= 1e9) return `${(V / 1e9).toFixed(2)} km³`;
    if (A >= 1e6) return `${(V / 1e6).toFixed(1)} Mm³`;
    if (A >= 1e3) return `${(V / 1e3).toFixed(0)} k m³`;
    return `${V.toFixed(0)} m³`;
};

/**
 * Builds the whole Erosion tab into `Content`.
 *
 * @param {object} App      TerrainWorldStudioApp instance
 * @param {HTMLElement} Content  inspector body element
 * @param {object} Region   active terrain region
 */
export function BuildErosionInspector(App, Content, Region)
{
    const ErosionLayers = Region.stack.filter(L => L.type === 'erosion');
    const ActiveLayer = Region.stack.find(L => L.id === App.activeLayerId && L.type === 'erosion') || ErosionLayers[ErosionLayers.length - 1] || null;

    /*--------------------------------------------------------------------------------------------------------------
                                           1. HEADER / ADD / BAKE CONTROLS
    --------------------------------------------------------------------------------------------------------------*/
    const Head = MakePropertyGroup('Erosion Simulator', 'Activity', `${ErosionLayers.length} EROSION LAYERS`, true,
        'Non-destructive geological bake. Each erosion layer reads the surface produced below it, evolves it under a physical weather regime, and hands the result up the stack.');

    const BtnRow = MakeEl('div', 'studio-btn-row');

    const BtnAdd = MakeEl('button', 'button export-button', '+ Add Erosion Bake');
    BtnAdd.type = 'button';
    BtnAdd.title = 'Append a new erosion layer with the current preset';
    BtnAdd.addEventListener('click', () => App.addErosionLayer('alpine-divide'));

    const BtnBake = MakeEl('button', 'button', '⟳ Re-Bake Active');
    BtnBake.type = 'button';
    BtnBake.title = 'Force a full re-simulation of the active erosion layer';
    BtnBake.addEventListener('click', () =>
    {
        if (!ActiveLayer) return;
        InvalidateErosionLayer(ActiveLayer, 'manual');
        ActiveLayer.bakeVersion = 0;
        App.triggerErosionRebake(Region, ActiveLayer);
    });

    const BtnClear = MakeEl('button', 'button', '✕ Remove');
    BtnClear.type = 'button';
    BtnClear.title = 'Delete the active erosion layer (geometry returns to the un-eroded surface)';
    BtnClear.addEventListener('click', () =>
    {
        if (!ActiveLayer) return;
        App.removeErosionLayer(Region, ActiveLayer);
    });

    BtnRow.append(BtnAdd, BtnBake, BtnClear);
    Head.Body.appendChild(BtnRow);

    // Bake progress / status line
    const Status = MakeEl('div', 'erosion-status', App.erosionStatusText || 'Idle');
    Status.id = 'erosion-status-line';
    Head.Body.appendChild(Status);

    Content.appendChild(Head.Group);

    if (!ActiveLayer)
    {
        Head.Body.appendChild(MakeEl('p', 'property-hint',
            'No erosion layer on this terrain yet. Add one to simulate stream-power incision, sediment deposition, talus, wind, ice, chemical weathering and frost pitting.'));
        return;
    }

    const P = ActiveLayer.params || {};
    const Commit = (Reason = 'params') =>
    {
        InvalidateErosionLayer(ActiveLayer, Reason);
        App.refreshChrome();
    };
    const Live = (Reason = 'params') =>
    {
        InvalidateErosionLayer(ActiveLayer, Reason);
        App.scheduleErosionRebake(Region, ActiveLayer);
    };

    /*--------------------------------------------------------------------------------------------------------------
                                           2. PRESET & WEATHER REGIME
    --------------------------------------------------------------------------------------------------------------*/
    const PresetGroup = MakePropertyGroup('Landscape Preset', 'Mountain', ActiveLayer.presetId.toUpperCase(), true,
        'Presets are complete physical parameter sets. Changing one replaces the process parameters (geometry, falloff and stack position are preserved).');

    const Preset = GetErosionPreset(ActiveLayer.presetId);
    if (Preset)
    {
        PresetGroup.Body.appendChild(MakeEl('p', 'property-hint', `${Preset.name} — ${Preset.subtitle}`));
        if (Preset.notes) PresetGroup.Body.appendChild(MakeEl('p', 'property-hint', Preset.notes));
    }

    PresetGroup.Body.appendChild(MakeSelectRow('Preset', EROSION_PRESET_OPTIONS, ActiveLayer.presetId, Val =>
    {
        const Params = ClonePresetParams(Val);
        if (!Params) return;
        ActiveLayer.presetId = Val;
        ActiveLayer.params = ApplyWeatherMode(Params, ActiveLayer.weatherMode);
        ActiveLayer.name = GetErosionPreset(Val)?.name || ActiveLayer.name;
        Commit('preset');
        App.evaluateAndRefresh(Region);
    }));

    PresetGroup.Body.appendChild(MakeSelectRow('Weather Mode', WEATHER_MODE_OPTIONS, ActiveLayer.weatherMode, Val =>
    {
        // Weather mode re-applies on top of a clean preset so modes are not cumulative.
        const Base = ClonePresetParams(ActiveLayer.presetId) || {};
        ActiveLayer.weatherMode = Val;
        ActiveLayer.params = ApplyWeatherMode(MergeErosionParams(Base), Val);
        Commit('weather');
        App.evaluateAndRefresh(Region);
    }));

    const Mode = GetWeatherMode(ActiveLayer.weatherMode);
    if (Mode) PresetGroup.Body.appendChild(MakeEl('p', 'property-hint', Mode.subtitle));

    PresetGroup.Body.appendChild(NumberRow('Iterations', 1, 200, 1, P.iterations ?? 36, 0, '×', V =>
    {
        ActiveLayer.params.iterations = V;
        Live('iteration');
    }, () => App.evaluateAndRefresh(Region)));

    PresetGroup.Body.appendChild(NumberRow('Time Step', 0.5, 120, 0.5, P.dtKyr ?? 12, 1, 'kyr', V =>
    {
        ActiveLayer.params.dtKyr = V;
        Live('dt');
    }, () => App.evaluateAndRefresh(Region)));

    const SimKyr = (P.iterations ?? 36) * (P.dtKyr ?? 12);
    PresetGroup.Body.appendChild(MakeEl('p', 'property-hint',
        `Geological span: ${SimKyr.toFixed(0)} kyr per bake (a compressed history — every rate is real, the clock is not).`));

    PresetGroup.Body.appendChild(NumberRow('Tectonic Uplift', 0, 0.2, 0.002, P.upliftMmYr ?? 0, 3, 'mm/yr', V =>
    {
        ActiveLayer.params.upliftMmYr = V;
        Live('uplift');
    }, () => App.evaluateAndRefresh(Region)));

    PresetGroup.Body.appendChild(NumberRow('Seed', 1, 99999, 1, P.seed ?? 4029, 0, '', V =>
    {
        ActiveLayer.params.seed = V | 0;
        Live('seed');
    }, () => App.evaluateAndRefresh(Region)));

    Content.appendChild(PresetGroup.Group);

    /*--------------------------------------------------------------------------------------------------------------
                                           3. SHARED WEATHER GROUPS
    --------------------------------------------------------------------------------------------------------------*/
    const GroupGroup = MakePropertyGroup('Shared Weather Group', 'Planet', ActiveLayer.shareMode.toUpperCase(), true,
        'Layers in the same group evolve under one identical climate — the same rainfall, wind and season phase — so two adjacent terrains stay geologically consistent at their seam.');

    GroupGroup.Body.appendChild(MakeSelectRow('Sharing Mode', [
        ['Independent — own weather per layer', 'independent'],
        ['Group — shared climate field', 'group'],
        ['Uniform — shared climate + identical time steps', 'uniform']
    ], ActiveLayer.shareMode, Val =>
    {
        ActiveLayer.shareMode = Val;
        Commit('shareMode');
        App.evaluateAndRefresh(Region);
    }));

    const GroupInput = MakeEl('input', 'text-input');
    GroupInput.type = 'text';
    GroupInput.value = ActiveLayer.weatherGroup || '';
    GroupInput.placeholder = 'e.g. northern-continent';
    GroupInput.setAttribute('aria-label', 'Shared weather group id');
    const GroupRow = MakeEl('div', 'property-row', [MakeEl('label', 'property-label', 'Weather Group ID'), GroupInput]);
    GroupInput.addEventListener('change', () =>
    {
        ActiveLayer.weatherGroup = GroupInput.value.trim() || null;
        Commit('group');
        App.evaluateAndRefresh(Region);
    });
    GroupGroup.Body.appendChild(GroupRow);

    // Show which other layers share this group so the intent is visible, not guessed.
    const GroupId = ActiveLayer.weatherGroup;
    if (GroupId)
    {
        const Members = [];
        for (const R of App.world.regions)
        {
            for (const L of R.stack)
            {
                if (L.type === 'erosion' && L.weatherGroup === GroupId) Members.push(`${R.name} · ${L.name}`);
            }
        }
        GroupGroup.Body.appendChild(MakeEl('p', 'property-hint', `Group members (${Members.length}): ${Members.join(' · ')}`));
    }
    Content.appendChild(GroupGroup.Group);

    /*--------------------------------------------------------------------------------------------------------------
                                           4. CLIMATE
    --------------------------------------------------------------------------------------------------------------*/
    const Climate = MakePropertyGroup('Climate & Weather Regime', 'Sun', (P.climate?.pattern || 'orographic').toUpperCase(), false,
        'Rainfall drives stream power; wind drives saltation; temperature controls frost, ice and chemical weathering.');

    const C = P.climate || {};
    Climate.Body.appendChild(MakeSelectRow('Rainfall Pattern', CLIMATE_PATTERN_OPTIONS, C.pattern || 'orographic', Val =>
    {
        ActiveLayer.params.climate.pattern = Val;
        Live('climate');
    }));

    Climate.Body.appendChild(NumberRow('Rainfall', 0, 4000, 10, C.rainMmYr ?? 1150, 0, 'mm/yr', V =>
    {
        ActiveLayer.params.climate.rainMmYr = V;
        Live('rain');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Mean Temperature', -30, 40, 0.5, C.meanTemperatureC ?? 11, 1, '°C', V =>
    {
        ActiveLayer.params.climate.meanTemperatureC = V;
        Live('temp');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Lapse Rate', 0, 10, 0.1, C.lapseRateCPerKm ?? 6.2, 1, '°C/km', V =>
    {
        ActiveLayer.params.climate.lapseRateCPerKm = V;
        Live('lapse');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Wind Speed', 0, 1, 0.01, C.windSpeed ?? 0.45, 2, '', V =>
    {
        ActiveLayer.params.climate.windSpeed = V;
        Live('wind');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Wind Azimuth', 0, 360, 1, C.windAzimuthDeg ?? 38, 0, '°', V =>
    {
        ActiveLayer.params.climate.windAzimuthDeg = V;
        Live('azimuth');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Seasonal Swing', 0, 1, 0.01, C.seasonalAmplitude ?? 0.28, 2, '', V =>
    {
        ActiveLayer.params.climate.seasonalAmplitude = V;
        Live('season');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Season Length', 2, 40, 1, C.seasonPeriodIters ?? 9, 0, 'iter', V =>
    {
        ActiveLayer.params.climate.seasonPeriodIters = V;
        Live('season');
    }, () => App.evaluateAndRefresh(Region)));

    Climate.Body.appendChild(NumberRow('Freeze–Thaw', 0, 300, 5, C.freezeThawCyclesPerYr ?? 55, 0, 'cyc/yr', V =>
    {
        ActiveLayer.params.climate.freezeThawCyclesPerYr = V;
        Live('frost');
    }, () => App.evaluateAndRefresh(Region)));

    Content.appendChild(Climate.Group);

    /*--------------------------------------------------------------------------------------------------------------
                                           5. PROCESSES (each independently toggleable)
    --------------------------------------------------------------------------------------------------------------*/
    const ProcessDefs = [
        {
            key: 'streamPower', group: 'Fluvial Incision', open: true,
            hint: 'dz/dt = U − K·A^m·S^n, solved implicitly downstream→upstream. Produces the drainage network, valleys and knickpoints.',
            rows: [
                ['Erodibility K', 0, 1, 0.01, P.streamPower?.erodibility ?? 0.55, 2, '', V => { ActiveLayer.params.streamPower.erodibility = V; }],
                ['Area Exponent m', 0, 1, 0.01, P.streamPower?.m ?? 0.5, 2, '', V => { ActiveLayer.params.streamPower.m = V; }],
                ['Slope Exponent n', 0.5, 2, 0.05, P.streamPower?.n ?? 1, 2, '', V => { ActiveLayer.params.streamPower.n = V; }],
                ['Rock Resistance', 0, 1, 0.01, P.streamPower?.hardRockResistance ?? 0.45, 2, '', V => { ActiveLayer.params.streamPower.hardRockResistance = V; }],
                ['Regolith Boost', 1, 30, 0.5, P.streamPower?.regolithFactor ?? 5.5, 1, '×', V => { ActiveLayer.params.streamPower.regolithFactor = V; }]
            ]
        },
        {
            key: 'deposition', group: 'Sediment Routing', open: false,
            hint: 'Transport-limited: excess load aggrades the bed, deficits incise. Lakes trap sediment and build deltas.',
            rows: [
                ['Transport Capacity', 0, 1, 0.01, P.deposition?.capacity ?? 0.5, 2, '', V => { ActiveLayer.params.deposition.capacity = V; }],
                ['Fill Rate', 0, 1, 0.01, P.deposition?.fillRate ?? 0.45, 2, '', V => { ActiveLayer.params.deposition.fillRate = V; }],
                ['Lake Trapping', 0, 1, 0.01, P.deposition?.lakeFill ?? 0.85, 2, '', V => { ActiveLayer.params.deposition.lakeFill = V; }],
                ['Max Aggradation', 0.05, 40, 0.05, P.deposition?.maxAggradationM ?? 4, 2, 'm', V => { ActiveLayer.params.deposition.maxAggradationM = V; }]
            ]
        },
        {
            key: 'thermal', group: 'Thermal Talus (Mass Wasting)', open: false,
            hint: 'Mass-conserving scree transport above the critical repose angle. Softens over-steep cliffs into talus aprons.',
            rows: [
                ['Repose Angle', 12, 60, 0.5, P.thermal?.talusDeg ?? 33, 1, '°', V => { ActiveLayer.params.thermal.talusDeg = V; }],
                ['Efficiency', 0, 1, 0.01, P.thermal?.rate ?? 0.55, 2, '', V => { ActiveLayer.params.thermal.rate = V; }],
                ['Passes', 1, 6, 1, P.thermal?.passes ?? 2, 0, '×', V => { ActiveLayer.params.thermal.passes = V | 0; }]
            ]
        },
        {
            key: 'aeolian', group: 'Aeolian Saltation (Wind)', open: false,
            hint: 'Windward deflation, lee deposition and transverse dune bedforms. Best on dry sand; poor at carving canyons.',
            rows: [
                ['Strength', 0, 1, 0.01, P.aeolian?.strength ?? 0.35, 2, '', V => { ActiveLayer.params.aeolian.strength = V; }],
                ['Saltation Length', 2, 400, 1, P.aeolian?.saltationLengthM ?? 42, 0, 'm', V => { ActiveLayer.params.aeolian.saltationLengthM = V; }],
                ['Dune Wavelength', 12, 1200, 5, P.aeolian?.duneWavelengthM ?? 155, 0, 'm', V => { ActiveLayer.params.aeolian.duneWavelengthM = V; }],
                ['Dune Height', 0, 60, 0.5, P.aeolian?.duneAmplitudeM ?? 7.5, 1, 'm', V => { ActiveLayer.params.aeolian.duneAmplitudeM = V; }]
            ]
        },
        {
            key: 'glacial', group: 'Glacial Ice Flow', open: false,
            hint: 'Shallow-ice diffusion with Glen n=3 creep; erosion scales with sliding velocity, so it widens V-valleys into U-valleys and digs cirques.',
            rows: [
                ['Ice Erosion K', 0, 1, 0.01, P.glacial?.erosionK ?? 0.42, 2, '', V => { ActiveLayer.params.glacial.erosionK = V; }],
                ['ELA Offset', -300, 300, 5, P.glacial?.elaOffsetM ?? -18, 0, 'm', V => { ActiveLayer.params.glacial.elaOffsetM = V; }],
                ['Mass Balance Grad', 0.001, 0.03, 0.0005, P.glacial?.massBalanceGrad ?? 0.0075, 4, '', V => { ActiveLayer.params.glacial.massBalanceGrad = V; }],
                ['Max Ice Thickness', 20, 2000, 10, P.glacial?.maxIceThicknessM ?? 420, 0, 'm', V => { ActiveLayer.params.glacial.maxIceThicknessM = V; }]
            ]
        },
        {
            key: 'chemical', group: 'Chemical Weathering', open: false,
            hint: 'Bedrock → regolith conversion with an Arrhenius-like temperature response, plus dissolved-load export.',
            rows: [
                ['Rate', 0, 1, 0.01, P.chemical?.rate ?? 0.35, 2, '', V => { ActiveLayer.params.chemical.rate = V; }],
                ['Temperature Sensitivity', 0, 1, 0.01, P.chemical?.temperatureSensitivity ?? 0.6, 2, '', V => { ActiveLayer.params.chemical.temperatureSensitivity = V; }],
                ['Solubility', 0, 1, 0.01, P.chemical?.solubility ?? 0.45, 2, '', V => { ActiveLayer.params.chemical.solubility = V; }]
            ]
        },
        {
            key: 'pitting', group: 'Frost & Salt Pitting', open: false,
            hint: 'Stochastic shattering of exposed faces on freeze–thaw fatigue sites. Produces pocked cliff faces, not uniform noise.',
            rows: [
                ['Site Density', 0, 1, 0.01, P.pitting?.density ?? 0.45, 2, '', V => { ActiveLayer.params.pitting.density = V; }],
                ['Pit Depth', 0.05, 8, 0.05, P.pitting?.depthM ?? 1.6, 2, 'm', V => { ActiveLayer.params.pitting.depthM = V; }],
                ['Site Scale', 2, 200, 1, P.pitting?.siteScaleM ?? 26, 0, 'm', V => { ActiveLayer.params.pitting.siteScaleM = V; }]
            ]
        }
    ];

    for (const Def of ProcessDefs)
    {
        const Enabled = P[Def.key]?.enabled !== false && P[Def.key]?.enabled !== undefined ? P[Def.key].enabled : !!P[Def.key]?.enabled;
        const Meta = PROCESS_META[Def.key] || { label: Def.group, badge: '' };
        const Grp = MakePropertyGroup(Def.group, 'Sliders', Enabled ? 'ON' : 'OFF', Def.open, Def.hint);

        // Enable switch
        const SwitchWrap = MakeEl('div', 'property-row');
        SwitchWrap.appendChild(MakeEl('label', 'property-label', 'Process Enabled'));
        const SwitchLabel = MakeEl('label', 'switch');
        const Chk = MakeEl('input');
        Chk.type = 'checkbox';
        Chk.checked = Enabled;
        Chk.setAttribute('aria-label', `Enable ${Def.group}`);
        Chk.addEventListener('change', () =>
        {
            if (!ActiveLayer.params[Def.key]) ActiveLayer.params[Def.key] = {};
            const Proc = ActiveLayer.params[Def.key];
            Proc.enabled = Chk.checked;

            // Turning a process on when its amplitude is zero would look broken (nothing happens),
            // so seed a usable default. This is the minimum "it visibly does something" value.
            if (Chk.checked)
            {
                const Minimums = {
                    aeolian: ['strength', 0.40],
                    glacial: ['erosionK', 0.40],
                    chemical: ['rate', 0.35],
                    pitting: ['density', 0.45],
                    thermal: ['rate', 0.45]
                };
                const Min = Minimums[Def.key];
                if (Min && !(Number(Proc[Min[0]]) >= 0.05)) Proc[Min[0]] = Min[1];
            }
            Commit('toggle');
            App.evaluateAndRefresh(Region);
        });
        SwitchLabel.append(Chk, MakeEl('span'));
        SwitchWrap.appendChild(SwitchLabel);
        Grp.Body.appendChild(SwitchWrap);

        for (const [Label, Min, Max, Step, Value, Decimals, Unit, Setter] of Def.rows)
        {
            Grp.Body.appendChild(NumberRow(Label, Min, Max, Step, Value, Decimals, Unit, V =>
            {
                Setter(V);
                Live('process');
            }, () => App.evaluateAndRefresh(Region)));
        }
        Content.appendChild(Grp.Group);
    }

    /*--------------------------------------------------------------------------------------------------------------
                                           6. STRATIGRAPHY / BED HARDNESS
    --------------------------------------------------------------------------------------------------------------*/
    const Strata = P.strata || { enabled: false };
    const StrataGroup = MakePropertyGroup('Stratigraphy & Bed Hardness', 'Layers', Strata.enabled ? 'ON' : 'OFF', false,
        'Alternating hard caprock and soft beds is what produces canyon benches, mesas and columnar cliffs. This is the biggest single control on landform character.');

    const StrataSwitchRow = MakeEl('div', 'property-row');
    StrataSwitchRow.appendChild(MakeEl('label', 'property-label', 'Bedded Hardness'));
    const StrataSwitch = MakeEl('label', 'switch');
    const StrataChk = MakeEl('input');
    StrataChk.type = 'checkbox';
    StrataChk.checked = !!Strata.enabled;
    StrataChk.addEventListener('change', () =>
    {
        if (!ActiveLayer.params.strata) ActiveLayer.params.strata = {};
        ActiveLayer.params.strata.enabled = StrataChk.checked;
        Commit('strata');
        App.evaluateAndRefresh(Region);
    });
    StrataSwitch.append(StrataChk, MakeEl('span'));
    StrataSwitchRow.appendChild(StrataSwitch);
    StrataGroup.Body.appendChild(StrataSwitchRow);

    StrataGroup.Body.appendChild(NumberRow('Bed Frequency', 2, 24, 1, Strata.bedFrequency ?? 9, 0, 'beds', V =>
    {
        ActiveLayer.params.strata.bedFrequency = V; Live('strata');
    }, () => App.evaluateAndRefresh(Region)));

    StrataGroup.Body.appendChild(NumberRow('Dip', 0, 60, 0.5, Strata.dipDeg ?? 4, 1, '°', V =>
    {
        ActiveLayer.params.strata.dipDeg = V; Live('strata');
    }, () => App.evaluateAndRefresh(Region)));

    StrataGroup.Body.appendChild(NumberRow('Strike', 0, 360, 1, Strata.strikeDeg ?? 25, 0, '°', V =>
    {
        ActiveLayer.params.strata.strikeDeg = V; Live('strata');
    }, () => App.evaluateAndRefresh(Region)));

    StrataGroup.Body.appendChild(NumberRow('Caprock Contrast', 0, 1, 0.01, Strata.caprockContrast ?? 0.75, 2, '', V =>
    {
        ActiveLayer.params.strata.caprockContrast = V; Live('strata');
    }, () => App.evaluateAndRefresh(Region)));

    StrataGroup.Body.appendChild(NumberRow('Joint Spacing', 0, 500, 5, Strata.jointSpacingM ?? 0, 0, 'm', V =>
    {
        ActiveLayer.params.strata.jointSpacingM = V; Live('strata');
    }, () => App.evaluateAndRefresh(Region)));

    Content.appendChild(StrataGroup.Group);

    /*--------------------------------------------------------------------------------------------------------------
                                           7. MASS BALANCE & DIAGNOSTICS
    --------------------------------------------------------------------------------------------------------------*/
    const Stats = ActiveLayer.lastStats;
    const Balance = MakePropertyGroup('Mass Balance & Diagnostics', 'Activity', Stats ? `${Stats.iterations} ITER` : 'NOT BAKED', true,
        'Physical accounting for the last bake. Eroded volume should be split between deposition and export — nothing may appear from nowhere.');

    if (Stats)
    {
        const Trio = MakeEl('div', 'kpi-trio', [
            StatTile('Simulated Span', `${Math.round(Stats.simulatedKyr || 0)} kyr`, `${Stats.iterations} × ${Stats.dtKyr} kyr`),
            StatTile('Mean Erosion', `${(Stats.meanErosionM || 0).toFixed(2)} m`, `peak ${(Stats.maxErosionM || 0).toFixed(1)} m`),
            StatTile('Mean Deposition', `${(Stats.meanDepositionM || 0).toFixed(2)} m`, 'aggradation')
        ]);
        Balance.Body.appendChild(Trio);

        const Rows = [
            ['Rock incised', FormatM3(Stats.incisionVolumeM3)],
            ['Sediment deposited', FormatM3(Stats.depositedVolumeM3)],
            ['Exported off-map', FormatM3(Stats.exportedVolumeM3)],
            ['Talus moved', FormatM3(Stats.thermalVolumeM3)],
            ['Ice eroded', FormatM3(Stats.glacialErodedM3)],
            ['Chemical solute', FormatM3(Stats.soluteVolumeM3)],
            ['Frost pitting', FormatM3(Stats.pittedVolumeM3)],
            ['Wind (deflated)', FormatM3(Stats.aeolianDeflatedM3)]
        ];
        for (const [Label, Value] of Rows)
        {
            const Row = MakeEl('div', 'property-row');
            Row.append(MakeEl('label', 'property-label', Label), MakeEl('span', 'property-readout', Value));
            Balance.Body.appendChild(Row);
        }

        if (Number.isFinite(Stats.equilibriumLineAltitudeM))
        {
            Balance.Body.appendChild(MakeEl('p', 'property-hint', `Equilibrium line altitude: ${Math.round(Stats.equilibriumLineAltitudeM)} m`));
        }
        if (Stats.numericRecoveries)
        {
            Balance.Body.appendChild(MakeEl('p', 'property-hint', `Numeric guard recovered ${Stats.numericRecoveries} cells.`));
        }
    }
    else
    {
        Balance.Body.appendChild(MakeEl('p', 'property-hint', 'Bake the layer to see mass balance.'));
    }
    Content.appendChild(Balance.Group);

    /*--------------------------------------------------------------------------------------------------------------
                                           8. FLOOD / WATER GUARD
    --------------------------------------------------------------------------------------------------------------*/
    const Water = MakePropertyGroup('Water & Flood Guard', 'Water', 'ON', false,
        'Controls how erosion interacts with the sea. The guard is what stops a lowered valley from drowning the landmass under the ocean plane.');
    Content.appendChild(Water.Group);

    Water.Body.appendChild(NumberRow('Max Land Drowned', 0, 20, 0.1, P.maxLandDrownedM ?? 2.5, 1, 'm', V =>
    {
        ActiveLayer.params.maxLandDrownedM = V; Live('flood');
    }, () => App.evaluateAndRefresh(Region)));

    Water.Body.appendChild(NumberRow('Lake Depth Threshold', 0, 20, 0.1, P.lakeDepthM ?? 1.2, 1, 'm', V =>
    {
        ActiveLayer.params.lakeDepthM = V; Live('lake');
    }, () => App.evaluateAndRefresh(Region)));

    const OceanRow = MakeEl('div', 'property-row');
    OceanRow.appendChild(MakeEl('label', 'property-label', 'Ocean Shore Feather'));
    const OceanSwitch = MakeEl('label', 'switch');
    const OceanChk = MakeEl('input');
    OceanChk.type = 'checkbox';
    OceanChk.checked = App.view3D.oceanShoreFade !== false;
    OceanChk.addEventListener('change', () =>
    {
        App.view3D.oceanShoreFade = OceanChk.checked;
        App.view3D._waterCacheKey = null;
    });
    OceanSwitch.append(OceanChk, MakeEl('span'));
    OceanRow.appendChild(OceanSwitch);
    Water.Body.appendChild(OceanRow);

    Water.Body.appendChild(MakeEl('p', 'property-hint',
        'Lakes and trunk rivers are drawn from the simulated hydrology. They are separate from the ocean sheet, so raising sea level can never flood a valley that is above it.'));
}
