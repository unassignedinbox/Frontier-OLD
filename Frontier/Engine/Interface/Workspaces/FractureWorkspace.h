/*==========================================================================================================================================
                                                             FRACTUREWORKSPACE.H
==========================================================================================================================================*/
// 🧩 Fracture Workspace as a C++ UI shell: the Remastered 1:1 visuals (top bar with construct tabs, scene outliner,
//    solid & solver preset library, viewport bar + shard stage + transport + fracture timeline, Overview / Physics /
//    Bonds / SDF inspector, solver matrix dialog, log drawer, status bar) over local view state only. The staged
//    FractureSpec copy mirrors the prototype's recipe fields (Experimental Remastered/FractureEditor); value controls
//    edit that copy and log keyed PARAM lines, and nothing else. Every command that would mutate the scene or reach
//    the engines (add-node, step, strike, bake, export, help, diagnostics, focus, maximize, preset spawn) is an
//    explicit NOOP: it raises a toast, appends a warm log line, and changes nothing. Play/pause, speed, explode,
//    reset, timeline scrub, tools, camera, lens, document tabs, pipeline select, matrix solver pick, and filters are
//    silent local view state. Timecode and frame read honest frozen values; only the shard census, bond counts, SDF
//    footprint, solar ephemeris, and receipt figures are live, because they
//    derive from shell-local closed-form estimators. The workspace makes no engine, shader, or renderer calls; the
//    stage shards, stress slices, and SDF bricks are static decor, honestly labelled in the viewport footer.

#pragma once

#include "Frontier/Engine/Interface/Workspaces/AssetBrowserWorkspace.h"

#include <imgui.h>

#include <deque>
#include <map>
#include <string>

namespace Frontier
{

struct EditorTypefaces;

//------------------------------------------------------------------------------------------------------------------------
//                                                    CATALOGUE
//------------------------------------------------------------------------------------------------------------------------

struct FractureWorkspaceMaterial
{
    const char* Id              = nullptr;
    const char* Label           = nullptr; // 🧩 e.g. "Reinforced Concrete".
    const char* ShortLabel      = nullptr; // 🧩 e.g. "Concrete".
    const char* FractureCategory = nullptr; // 🧩 bulk-brittle / plate-brittle / fibrous / ductile.
    double      Density         = 2350.0;  // kg/m³
    double      Gc              = 140.0;   // J/m²
    double      YoungModulusGPa = 30.0;
    double      PoissonRatio    = 0.20;
    double      AnisotropyRatio = 1.15;
    uint32_t    Exterior        = 0x8D908BFF;
};

struct FractureWorkspaceSolver
{
    const char* Id        = nullptr;
    const char* Name      = nullptr;
    const char* Lineage   = nullptr;
    const char* Cpu       = nullptr; // 🧩 e.g. "0.25–1.25 ms".
    const char* Realtime  = nullptr;
    const char* Baked     = nullptr;
    const char* Mechanism = nullptr;
};

struct FractureWorkspacePreset
{
    const char* Category = nullptr; // 🧩 monolith / curtain_pane / boulder / timber_beam / cube / cone / cylinder.
    const char* Material = nullptr;
    const char* Solver   = nullptr;
    const char* Group    = nullptr; // 🧩 Engineering / Brittle / Stone & Timber / Voronoi / Ductile.
    const char* Title    = nullptr;
    const char* Blurb    = nullptr;
};

[[nodiscard]] int                              QueryFractureMaterialCount() noexcept; // 🧩 Always 6.
[[nodiscard]] const FractureWorkspaceMaterial& QueryFractureMaterial(int Index) noexcept;
[[nodiscard]] int                              QueryFractureMaterialById(const char* Id) noexcept;
[[nodiscard]] int                              QueryFractureSolverCount() noexcept; // 🧩 Always 6.
[[nodiscard]] const FractureWorkspaceSolver&   QueryFractureSolver(int Index) noexcept;
[[nodiscard]] int                              QueryFractureSolverById(const char* Id) noexcept;
[[nodiscard]] int                              QueryFracturePresetCount() noexcept; // 🧩 Always 8.
[[nodiscard]] const FractureWorkspacePreset&   QueryFracturePreset(int Index) noexcept;

//------------------------------------------------------------------------------------------------------------------------
//                                                    RECIPE & RECEIPT
//------------------------------------------------------------------------------------------------------------------------

// 📝 Shell-local mirror of the prototype's fracture Spec (units in comments match the control unit map).
struct FractureSpec
{
    int    Solver = 1; // 🧩 Index into the solver catalogue.
    int    Material = 0;
    double Energy   = 4800.0; // J
    int    Seed     = 4177;
    int    Ceiling  = 96;     // pcs
    double MinimumSize = 0.04; // m
    double GrainAngleDeg = 0.0; // °
    double BondStrengthScale = 1.0; // ×
    bool   PieceSdf = true;
    int    SdfResolution = 64; // 🧩 32 / 64 / 128.
    double ImpactPoint[3]  = { 0.08, 0.18, 0.44 }; // m
    double ImpactNormal[3] = { 0.0, 0.0, -1.0 };
};

// 📝 Closed-form partition receipt: deterministic in (Spec, material, nucleus count), no engine calls.
struct FractureReceipt
{
    int    FragmentCount   = 0;
    int    BondCount       = 0;
    int    IntactBondCount = 0;
    double TotalCrackArea  = 0.0; // m²
    double VolumeDriftRatio = 0.0;
    double SpentSurfaceEnergy = 0.0; // J
    double BudgetPercent   = 0.0;
    double RayleighSpeedMps = 0.0;
    double WaveHazard      = 0.0;
    double StepMilliseconds = 0.0;
    bool   WatertightManifold = true;
};

struct FractureConstructSeed
{
    const char* Id           = nullptr;
    const char* Title        = nullptr;
    const char* SolidCategory = nullptr; // 🧩 pillar / glazing / pane / erratic / beam / cube / spire / drum.
    const char* Pipeline     = nullptr;  // 🧩 realtime / baked / hybrid.
    bool        BakedResident = false;
    int         PrimaryNucleusCells = 0;
    FractureSpec Spec;
};

[[nodiscard]] int                          QueryFractureConstructSeedCount() noexcept; // 🧩 Always 7.
[[nodiscard]] const FractureConstructSeed& QueryFractureConstructSeed(int Index) noexcept;
[[nodiscard]] FractureSpec                 QueryFractureConstructSeedSpec(int Index) noexcept;

struct FractureWorkspaceConstruct
{
    std::string Title;
    std::string SolidCategory;
    std::string Pipeline; // 🧩 realtime / baked / hybrid.
    bool        BakedResident = false;
    int         PrimaryNucleusCells = 0;
    FractureSpec Spec;
    FractureReceipt Receipt; // 🧩 Recomputed from Spec whenever the recipe edits.
};

void RecomputeFractureReceipt(FractureWorkspaceConstruct& Construct) noexcept;
[[nodiscard]] double QueryFractureSdfMegabytes(int FragmentCount, int Resolution, bool Enabled) noexcept;

struct FractureWorkspaceLogEntry
{
    std::string Channel;
    std::string Message;
    std::string Detail;
};

//------------------------------------------------------------------------------------------------------------------------
//                                                    CELESTIAL STATE
//------------------------------------------------------------------------------------------------------------------------

struct FractureSunParams
{
    double AngularDiameterDeg = 0.533; // °
    double TintRgb[3] = { 1.0, 0.96, 0.90 };
    double SkyGainMultiplier   = 1.00; // ×
    double DirectBeamMultiplier = 1.15; // ×
    double LocalHours = 15.65; // h
    bool   AnimateCycle = false;
    double CycleSpeed  = 1.0; // ×
    double LatitudeDeg  = -26.19; // °
    double LongitudeDeg = 28.32;  // °
    int    Month = 10;
    int    Day   = 31;
};

struct FractureSunSolved
{
    double ElevationDeg = 33.8;
    double AzimuthDeg   = 0.0;
    double AirMass      = 1.79;
    double SunriseHour  = 0.0;
    double SolarNoonHour = 12.0;
    double SunsetHour   = 0.0;
    char   PhaseLabel[32] = "Daylight";
};

void EvaluateFractureSun(const FractureSunParams& Params, FractureSunSolved& Solved) noexcept;

struct FractureAtmosParams
{
    double GroundRadiusKm    = 6360.0;
    double AtmosphereHeightKm = 100.0;
    double RayleighMultiplier = 1.0; // ×
    double RayleighScaleHeightKm = 8.0;
    double MieMultiplier = 1.0; // ×
    double MieAbsorptionMultiplier = 1.1; // ×
    double MieScaleHeightKm = 1.2;
    double MiePhaseG = 0.80;
    double OzoneMultiplier = 1.0; // ×
    double GroundAlbedo = 0.14;
    double TwilightGlow = 0.62;
    double HorizonWhiteLine = 0.24;
    bool   CivilTwilightEnabled = true;
    double SkyBrightness = 1.05; // ×
    bool   BakedFetchEnabled = true;
    int    PreviewMode = 0; // 🧩 0 altitude, 1 panorama, 2 transmittance, 3 sphere.
    int    BakedRevision = 1;
};

struct FractureCloudSlab
{
    bool   Enabled   = true;
    bool   FollowWind = true;
    double Coverage  = 0.56; // 0..1
    double BaseHeightM   = 900.0;
    double ThicknessM    = 1150.0;
    double CeilingHeightM = 3800.0;
    double DensityScale = 1.05;
    double FeatureScale = 1.0; // ×
    double PhaseG = 0.36;
    int    Morphology = 0; // 🧩 0 cumulus … 5 altocumulus.
    double AnvilAmount = 0.42;
    double WindXMps = 6.5;
    double WindYMps = 2.2;
    double ShadowStrength = 0.68;
};

struct FractureCloudLocal
{
    bool   Enabled   = true;
    bool   FollowWind = true;
    double Coverage  = 0.68; // 0..1
    double DensityScale = 1.25;
    double FeatureScale = 1.35; // ×
    double PhaseG = 0.40;
    double CentreX = 1.65;  // m
    double CentreY = 2.05;  // m
    double CentreZ = -1.10; // m
    double HalfExtentX = 1.45; // m
    double HalfExtentY = 0.58; // m
    double HalfExtentZ = 1.25; // m
    double WindXMps = 4.0;
    double WindYMps = 1.5;
};

struct FractureCloudsParams
{
    int               ActiveScope = 0; // 🧩 0 global, 1 local.
    FractureCloudSlab Global;
    FractureCloudLocal Local;
};

//------------------------------------------------------------------------------------------------------------------------
//                                                    STATE
//------------------------------------------------------------------------------------------------------------------------

struct FractureWorkspaceState
{
    enum
    {
        SelectedConstruct0 = 0,
        SelectedConstruct1,
        SelectedConstruct2,
        SelectedConstruct3,
        SelectedConstruct4,
        SelectedConstruct5,
        SelectedConstruct6,
        SelectedSun,
        SelectedAtmosphere,
        SelectedCloudsGlobal,
        SelectedCloudsLocal
    };

    enum
    {
        TabOverview = 0,
        TabPhysics,
        TabBonds,
        TabSdf
    };

    enum
    {
        FilterAll = 0,
        FilterSolids,
        FilterSky,
        FilterBaked
    };

    int Selected     = SelectedConstruct0;
    int InspectorTab = TabOverview;

    FractureWorkspaceConstruct Constructs[7];
    int                        ActiveConstruct = 0;

    FractureSunParams   Sun;
    FractureSunSolved   SunSolved;
    FractureAtmosParams Atmos;
    FractureCloudsParams Clouds;

    double TimelineActive = 0.28; // s
    double TimelineDuration = 4.0; // s
    bool   TimelinePlaying = false;
    double PlaybackRate = 1.0; // ×
    double ExplodeAmount = 0.08;

    int Camera = 0; // 🧩 0 Perspective, 1 Front, 2 Side, 3 Top (local, silent).
    int Tool   = 0; // 🧩 0 Strike, 1 Orbit, 2 Gizmo (local, silent).
    int Lens   = 0; // 🧩 0 PBR, 1 Stress, 2 Bonds, 3 SDF (local, silent).

    bool MatrixOpen = false;
    bool LogOpen    = false;

    enum
    {
        CardOvReceipt = 0,
        CardOvMetrics,
        CardOvPipeline,
        CardPhMaterial,
        CardPhBudget,
        CardBoBonds,
        CardBoLocus,
        CardSdfAtlas,
        CardSdfArchive,
        CardSunArc,
        CardSunDisc,
        CardSunGain,
        CardSunMap,
        CardAtLut,
        CardAtProfiles,
        CardAtExtent,
        CardAtSpecs,
        CardClScope,
        CardClMask,
        CardClLocal,
        CardClSlab,
        CardClWind,
        CardCount
    };
    bool OpenCards[CardCount] = {};

    std::string OutlinerQuery;
    int         OutlinerFilter = FilterAll;

    bool StrikeNoopRaised = false; // 🧩 Latched by validation: Strike/Bake stay explicit NOOPs.
    bool BakeNoopRaised   = false;

    std::deque<FractureWorkspaceLogEntry> Log;
    std::map<std::string, ImVec4>         Rects; // 🧩 Named hit rects refreshed every frame for scripted validation.
};

void        ConstructFractureWorkspaceState(FractureWorkspaceState& State);
void        AppendFractureWorkspaceLog(FractureWorkspaceState& State, const char* Channel, const char* Message,
                                       const char* Detail = nullptr);
void        NoopFractureWorkspaceCommand(FractureWorkspaceState& State, const char* Channel, const char* Message, const char* ToastText);
void        LogFractureWorkspaceValue(FractureWorkspaceState& State, const char* Channel, const char* Label, const char* Shown);
void        SelectFractureWorkspaceNode(FractureWorkspaceState& State, int Selection) noexcept; // 🧩 Silent: maps node → tab.
void        RecordFractureWorkspaceRect(FractureWorkspaceState& State, const char* Name, ImVec2 Min, ImVec2 Max);
[[nodiscard]] bool QueryFractureWorkspaceRect(const FractureWorkspaceState& State, const char* Name, ImVec2& Min, ImVec2& Max) noexcept;
void        EncodeFractureToml(const FractureWorkspaceState& State, int Construct, char* Text, size_t TextSize) noexcept;

void ApplyFractureWorkspaceLink(FractureWorkspaceState& State, AssetBrowserLink* Link); // 🧩 fracture[-mat|-solver]/<id>.
void ConstructFractureWorkspace(FractureWorkspaceState& State, const EditorTypefaces& Faces, ImVec2 Size,
                                AssetBrowserLink* Link = nullptr);

class ControlColumn;
void ConstructFractureWorkspaceInspector(FractureWorkspaceState& State, const EditorTypefaces& Faces, ControlColumn& Column);

} // namespace Frontier
