/*==========================================================================================================================================
                                                        EVERESTWORKSPACE.CPP
==========================================================================================================================================*/
// 🧩 Project Everest workspace implementation: EverestApplication runtime + #if FRONTIER_EDITOR docked authoring interface.

#include "EverestWorkspace.h"

#include "Engine/Diagnostics/LogCategories.h"
#include "Engine/Diagnostics/LogStream.h"
#include "Engine/Scene/VehicleReadings.h"

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
#include "Frontier/Extensions/Rendering/RenderingExtension.h"
#include "Frontier/Extensions/Physics/PhysicsExtension.h"
#include "Frontier/Extensions/Networking/NetworkingSession.h"
#include "Engine/Scene/WorldRegister.h"

#include "Engine/Interface/Controls/WorldVectorOverlay.h"
#include "Engine/Interface/ForceFieldVisualization/ForceFieldVisualization.h"
#include "Engine/Interface/InspectorControls.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/Interface/LogStreamPanelRegistration.h"
#include "Engine/Interface/NativeInspectors.h"
#include "Engine/Interface/ParticleEditor.h"
#include "Engine/Interface/ProjectContent.h"
#include "Engine/Interface/SceneDirectoryPanelRegistration.h"
#include "Engine/Interface/SceneInspectorPanel.h"
#include "Engine/Interface/ViewportChromeControls.h"
#include "Engine/PhysicalDynamics/ForceSourceCatalogue.h"
#include "Engine/Scene/SceneDirectory.h"
#include "Frontier/Engine/Geometry/ShapeArchive.h"
#include <imgui.h>
#include <imgui_internal.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Everest
{

namespace
{

uint32_t EnsureProjectLogCategory(std::string_view CategoryName)
{
    return Frontier::LogCategories().Register(CategoryName, Frontier::LogColour::Mint, true);
}

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
void AppendVehicleImpactSparks(Frontier::PhysicsExtension& Physics, std::vector<Frontier::SceneImpactSpark>& Pending)
{
    for (const Frontier::PhysicsImpactEvent& Event : Physics.DrainImpactEvents())
    {
        if (Event.Surface != Frontier::PhysicsImpactSurface::Metal && Event.Surface != Frontier::PhysicsImpactSurface::Concrete) continue;
        Frontier::SceneImpactSpark Spark;
        Spark.Surface = Event.Surface == Frontier::PhysicsImpactSurface::Metal ? Frontier::SceneImpactSurface::Metal
                                                                               : Frontier::SceneImpactSurface::Concrete;
        Spark.PointMetres = Event.PointMetres;
        Spark.Normal = Event.Normal;
        Spark.RelativeVelocityMps = Event.RelativeVelocityMps;
        Spark.ClosingSpeedMps = Event.ClosingSpeedMps;
        Spark.EventSeed = Event.EventSeed;
        Pending.push_back(Spark);
    }
    if (Pending.size() > 32u) Pending.erase(Pending.begin(), Pending.begin() + static_cast<std::ptrdiff_t>(Pending.size() - 32u));
}
#endif

} // namespace

//------------------------------------------------------------------------------------------------------------------------
//                                                    VIEWPORT CAMERA
//------------------------------------------------------------------------------------------------------------------------

namespace
{

constexpr double DegreesToRadian = 3.14159265358979323846 / 180.0;
constexpr float  NotchBandHeight = 18.0f;   // [px] the Control Centre notch tab's height (PanelExpansionTab, 18 px)

// 📝 The demo's Render entity readouts (InspectorPages.js Meta): renderer and grade, TLAS instances (or the clipmap edge),
//    surface cards and geometry clusters, published on the records' Outliner rows.
#if FRONTIER_EDITOR
// 📝 The geometry records' resolutions (what each stands for in the scene archive), copied whenever any of them changed.
void PublishGeometryResolutions(Frontier::WorldSpace& World, const SceneViewportReadout& Readout, uint64_t& PublishedGeneration)
{
    if (!Readout.Geometry || Readout.GeometryGeneration == PublishedGeneration) return;
    PublishedGeneration = Readout.GeometryGeneration;
    for (const Frontier::SceneRecordBase* Entry : World.FindAllByType("Geometry"))
    {
        auto* Geometry = dynamic_cast<Frontier::GeometryRecord*>(World.FindRecord(Entry->QueryToken()));
        if (Geometry == nullptr) continue;
        const auto Resolved = Readout.Geometry->find(Entry->QueryToken().Value);
        Geometry->AccessConfiguration().Resolved = Resolved != Readout.Geometry->end() ? Resolved->second : Frontier::GeometryConfiguration::Resolution{};
    }
}

void PublishRenderReadouts(Frontier::WorldSpace& World, const SceneViewportReadout& Readout)
{
    if (!Readout.Presenting) return;
    if (Readout.Environment && Readout.Environment->AtmosphereBake)
    {
        const auto& Bake = *Readout.Environment->AtmosphereBake;
        auto* Entry = dynamic_cast<Frontier::AtmosphereRecord*>(World.FindFirstByType("Atmosphere"));
        if (Entry) (void)Frontier::ConfirmAtmosphereBake(Entry->AccessConfiguration(),Entry->QueryToken().Value,Bake);
    }
    auto Publish = [&World](const char* Type, const std::string& Text) {
        Frontier::SceneRecordBase* Entry = World.FindFirstByType(Type);
        if (Entry == nullptr) return;
        Frontier::SceneDirectoryMetadata Meta = Frontier::SceneDirectory::Instance().QueryMetadata(Entry->QueryToken());
        if (Meta.LiveMetadata == Text) return;
        Meta.LiveMetadata = Text;
        (void)Frontier::SceneDirectory::Instance().ConfigureMetadata(Entry->QueryToken(), std::move(Meta));
    };
    auto* Pipeline = dynamic_cast<Frontier::RenderPipelineRecord*>(World.FindFirstByType("RenderPipeline"));
    if (Pipeline == nullptr) return;
    // 📝 The Render settings page's measurements: the refresh times and what the viewport ran with (not saved with the level).
    auto& Measured = Pipeline->AccessConfiguration().Measured;
    if (!Readout.Held && Readout.LatestMilliseconds > 0.0)
    {
        Measured.HistoryMs[Measured.RefreshCount % Measured.HistoryMs.size()] = static_cast<float>(Readout.LatestMilliseconds);
        Measured.HistoryCount = std::min<uint32_t>(Measured.HistoryCount + 1u, static_cast<uint32_t>(Measured.HistoryMs.size()));
        ++Measured.RefreshCount;
    }
    Measured.Presenting       = Readout.Presenting;
    Measured.RayTracing       = Readout.RayTracing;
    Measured.RefreshMs        = Readout.RefreshMilliseconds;
    Measured.LatestMs         = Readout.Held ? Measured.LatestMs : Readout.LatestMilliseconds;
    Measured.ScreenExtent     = Readout.ScreenExtent;
    Measured.TraceExtent      = Readout.TraceExtent;
    Measured.TlasInstances    = Readout.TlasInstances;
    Measured.ClipResolution   = Readout.ClipResolution;
    Measured.SurfaceCards     = Readout.SurfaceCards;
    Measured.GeometryClusters = Readout.GeometryClusters;
    Measured.SceneTriangles   = Readout.SceneTriangles;
    Measured.Adapter          = Readout.Adapter;
    const auto& Settings = Pipeline->QueryConfiguration();
    char Row[96];
    std::snprintf(Row, sizeof(Row), "%s \xC2\xB7 %u\xC3\x97%u \xC2\xB7 %.1f ms", Settings.AnalyticIllumination ? "Analytic" : (Readout.RayTracing ? "RTX" : "GTX"),
                  Readout.ScreenExtent[0], Readout.ScreenExtent[1], Readout.RefreshMilliseconds);
    Publish("RenderPipeline", Row);
}

#endif

// 📝 Unreal Engine 5 editor viewport constants (FEditorViewportClient, FCameraControllerConfig, ULevelEditorViewportSettings
//    defaults), in metres where Unreal counts centimetres.
constexpr double MouseSensitivity        = 0.2;      // [°/px] MouseSensitivty: look, orbit and the left-drag turn
constexpr double DragMetresPerPixel      = 0.01;     // [m/px] at speed 1: drags move 1 cm per pixel (Drag *= CameraSpeed)
constexpr double FlightAcceleration      = 200.0;    // [m/s²] MovementAccelerationRate 20000 cm/s²
constexpr double FlightDamping           = 10.0;     // [1/s] MovementVelocityDampingAmount: 20 m/s at speed 1
constexpr double FieldOfViewAcceleration = 1200.0;   // [°/s²] FOVAccelerationRate
constexpr double FieldOfViewDamping      = 10.0;     // [1/s] FOVVelocityDampingAmount, also the recoil rate
constexpr double WheelDollyMetres        = 0.96;     // [m] per notch: GetCameraSpeed(MouseScrollCameraSpeed 5) = 3 × 32 cm
constexpr double ShiftBoost              = 2.0;      // [-] CameraBoost while Shift is held
constexpr double ElevationLimit          = 89.5;     // [°] short of the pole, where a z-up look-at degenerates
constexpr double OrbitDollyRate          = 0.004;    // [1/px] Alt + right drag: pivot distance × e^(−rate · (Δx + Δy))
constexpr double OrbitTrackRate          = 0.0016;   // [1/px] Alt + middle drag: pivot distance × rate per pixel

} // namespace

std::array<double, 3> SceneViewportFlight::ResolveForward() const noexcept
{
    const double θ = Heading * DegreesToRadian;
    const double φ = Elevation * DegreesToRadian;
    return {std::cos(φ) * std::cos(θ), std::cos(φ) * std::sin(θ), std::sin(φ)};
}

// 📝 Screen right of the renderer's view (ConstructViewTransform: Right = Forward × Up, z up). The former orbit camera used
//    Up × Forward here, which is why A and D moved the wrong way.
std::array<double, 3> SceneViewportFlight::ResolveLateral() const noexcept
{
    const double θ = Heading * DegreesToRadian;
    return {std::sin(θ), -std::cos(θ), 0.0};
}

std::array<double, 3> SceneViewportFlight::ResolveTarget() const noexcept
{
    const std::array<double, 3> Forward = ResolveForward();
    return {Position[0] + Forward[0] * PivotDistance, Position[1] + Forward[1] * PivotDistance, Position[2] + Forward[2] * PivotDistance};
}

double SceneViewportFlight::ResolveFieldOfView() const noexcept
{
    return std::clamp(VerticalFieldOfView / DegreesToRadian + FieldOfViewOffset, 5.0, 140.0) * DegreesToRadian;
}

double SceneViewportFlight::ResolveSpeed(int Setting) noexcept
{
    constexpr std::array<double, 8> Speeds = {0.03125, 0.09375, 0.33, 1.0, 3.0, 8.0, 16.0, 32.0};
    return Speeds[static_cast<size_t>(std::clamp(Setting, 1, 8) - 1)];
}

void IntegrateSceneViewportFlight(SceneViewportFlight& Flight, const SceneViewportPointer& Pointer, double DeltaSeconds)
{
    const double Δt     = std::clamp(DeltaSeconds, 0.0, 0.1);
    const double Speed  = SceneViewportFlight::ResolveSpeed(Flight.SpeedSetting);
    const double Δx     = Pointer.MotionPixels[0];
    const double Δy     = Pointer.MotionPixels[1];
    const bool   Flying = Pointer.Active && Pointer.RightDown && !Pointer.LeftDown && !Pointer.AltDown;
    auto Translate = [&Flight](const std::array<double, 3>& Axis, double Metres) {
        for (int Component = 0; Component < 3; ++Component) Flight.Position[Component] += Axis[Component] * Metres;
    };
    auto Turn = [&Flight](double HeadingDegrees, double ElevationDegrees) {
        Flight.Heading   = std::remainder(Flight.Heading + HeadingDegrees, 360.0);
        Flight.Elevation = std::clamp(Flight.Elevation + ElevationDegrees, -ElevationLimit, ElevationLimit);
    };
    auto ResolveUpward = [&Flight]() {
        const std::array<double, 3> Forward = Flight.ResolveForward();
        const std::array<double, 3> Lateral = Flight.ResolveLateral();
        return std::array<double, 3>{Lateral[1] * Forward[2] - Lateral[2] * Forward[1], Lateral[2] * Forward[0] - Lateral[0] * Forward[2],
                                     Lateral[0] * Forward[1] - Lateral[1] * Forward[0]};
    };

    // ① drags (Unreal's ConvertMovementToDragRot and orbit modes): pointer right turns right, pointer up looks up
    if (Pointer.Active && Pointer.AltDown && (Pointer.LeftDown || Pointer.RightDown || Pointer.MiddleDown))
    {
        const std::array<double, 3> Pivot = Flight.ResolveTarget();
        if (Pointer.LeftDown)
        {
            // 📝 Alt + left: orbit the pivot.
            Turn(-Δx * MouseSensitivity, -Δy * MouseSensitivity);
        }
        else if (Pointer.RightDown)
        {
            // 📝 Alt + right: dolly toward the pivot (pointer right or down) or away from it.
            Flight.PivotDistance = std::clamp(Flight.PivotDistance * std::exp(-OrbitDollyRate * (Δx + Δy)), 0.25, 5000.0);
        }
        if (Pointer.MiddleDown && !Pointer.LeftDown && !Pointer.RightDown)
        {
            // 📝 Alt + middle: track the camera and its pivot with the pointer, in the view plane.
            const double Metres = OrbitTrackRate * Flight.PivotDistance;
            Translate(Flight.ResolveLateral(), Δx * Metres);
            Translate(ResolveUpward(), -Δy * Metres);
        }
        else
        {
            const std::array<double, 3> Forward = Flight.ResolveForward();
            for (int Component = 0; Component < 3; ++Component) Flight.Position[Component] = Pivot[Component] - Forward[Component] * Flight.PivotDistance;
        }
    }
    else if (Pointer.Active && (Pointer.MiddleDown || (Pointer.LeftDown && Pointer.RightDown)))
    {
        // 📝 Middle, or left + right: move right/left and up/down (world z) with the pointer, without turning.
        Translate(Flight.ResolveLateral(), Δx * DragMetresPerPixel * Speed);
        Translate({0.0, 0.0, 1.0}, -Δy * DragMetresPerPixel * Speed);
    }
    else if (Pointer.Active && Pointer.LeftDown)
    {
        // 📝 Left: forward/back over the ground plane (pointer up = forward) and turn left/right.
        const double θ = Flight.Heading * DegreesToRadian;
        Translate({std::cos(θ), std::sin(θ), 0.0}, -Δy * DragMetresPerPixel * Speed);
        Turn(-Δx * MouseSensitivity, 0.0);
    }
    else if (Flying)
    {
        // 📝 Right: look around in place.
        Turn(-Δx * MouseSensitivity, -Δy * MouseSensitivity);
    }

    // ② the wheel: camera speed while flying (whole notches), else one dolly step along the view per notch
    if (Flying || (Pointer.Hovered && !Pointer.Active))
    {
        Flight.WheelRemainder += Pointer.WheelNotches;
        const double Notches = std::trunc(Flight.WheelRemainder);
        Flight.WheelRemainder -= Notches;
        if (Flying)
        {
            Flight.SpeedSetting = std::clamp(Flight.SpeedSetting + static_cast<int>(Notches), 1, 8);
        }
        else if (Notches != 0.0)
        {
            const double Metres = Notches * WheelDollyMetres;
            Translate(Flight.ResolveForward(), Metres);
            Flight.PivotDistance = std::max(1.0, Flight.PivotDistance - Metres);
        }
    }
    else
    {
        Flight.WheelRemainder = 0.0;
    }

    // ③ flight: W/S along the view, D/A along screen right, E/Q along world z, R/F along the view's up; Shift doubles
    //    the speed. Velocity approaches Unreal's terminal speed (acceleration ÷ damping) with damping's time constant,
    //    independent of the tick length, and decays the same way once the keys or the right button are released.
    std::array<double, 3> Thrust = {0.0, 0.0, 0.0};
    if (Flying)
    {
        const std::array<double, 3> Forward = Flight.ResolveForward();
        const std::array<double, 3> Lateral = Flight.ResolveLateral();
        const std::array<double, 3> Upward  = ResolveUpward();
        const auto&                 Keys    = Pointer.FlightKeys;
        const double Along    = (Keys[0] ? 1.0 : 0.0) - (Keys[1] ? 1.0 : 0.0);
        const double Across   = (Keys[3] ? 1.0 : 0.0) - (Keys[2] ? 1.0 : 0.0);
        const double Vertical = (Keys[4] ? 1.0 : 0.0) - (Keys[5] ? 1.0 : 0.0);
        const double Local    = (Keys[6] ? 1.0 : 0.0) - (Keys[7] ? 1.0 : 0.0);
        for (int Component = 0; Component < 3; ++Component)
        {
            Thrust[Component] = Forward[Component] * Along + Lateral[Component] * Across + Upward[Component] * Local + (Component == 2 ? Vertical : 0.0);
        }
    }
    const double TerminalScale = FlightAcceleration / FlightDamping * Speed * (Pointer.ShiftDown ? ShiftBoost : 1.0);
    const double Decay         = std::exp(-FlightDamping * Δt);
    for (int Component = 0; Component < 3; ++Component)
    {
        const double Terminal       = Thrust[Component] * TerminalScale;
        const double Previous       = Flight.Velocity[Component];
        Flight.Velocity[Component]  = Terminal + (Previous - Terminal) * Decay;
        // 📝 Exact displacement of the exponential approach over the tick.
        Flight.Position[Component] += Terminal * Δt + (Previous - Terminal) * (1.0 - Decay) / FlightDamping;
        if (std::fabs(Flight.Velocity[Component]) < 1.0e-4 && Terminal == 0.0) Flight.Velocity[Component] = 0.0;
    }

    // ④ Z widens and C narrows the field of view while flying; it recoils to rest once the flight ends (Unreal's FOV recoil)
    if (Flying)
    {
        const double Impulse      = (Pointer.FlightKeys[8] ? 1.0 : 0.0) - (Pointer.FlightKeys[9] ? 1.0 : 0.0);
        const double TerminalRate = Impulse * FieldOfViewAcceleration / FieldOfViewDamping;
        Flight.FieldOfViewRate    = TerminalRate + (Flight.FieldOfViewRate - TerminalRate) * std::exp(-FieldOfViewDamping * Δt);
        const double RestDegrees  = Flight.VerticalFieldOfView / DegreesToRadian;
        Flight.FieldOfViewOffset  = std::clamp(Flight.FieldOfViewOffset + Flight.FieldOfViewRate * Δt, 5.0 - RestDegrees, 140.0 - RestDegrees);
    }
    else
    {
        Flight.FieldOfViewRate    = 0.0;
        Flight.FieldOfViewOffset *= std::exp(-FieldOfViewDamping * Δt);
        if (std::fabs(Flight.FieldOfViewOffset) < 0.01) Flight.FieldOfViewOffset = 0.0;
    }

    // ⑤ F frames the level's default view again (the viewport has no selection to focus); the speed setting stays
    if (Pointer.RecentrePress)
    {
        const int SpeedSetting = Flight.SpeedSetting;
        Flight                 = SceneViewportFlight{};
        Flight.SpeedSetting    = SpeedSetting;
    }
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    CONSTRUCTION & LIFECYCLE
//------------------------------------------------------------------------------------------------------------------------

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

EverestWorkspace::EverestWorkspace(Frontier::ControlCentrePanel& ControlCentre,
                                   std::filesystem::path         ProjectRootPath)
    : Centre(ControlCentre)
    , InspectorPanel(OutlinerPanel)
    , ProjectRoot(std::move(ProjectRootPath))
{
    InitializeWorld();

    // 📝 The scene's entries plus the one Render settings component (the whole renderer, owner's request) and the scene archive's
    //    geometry objects; neither the renderer nor the geometry is constructible (the archive decides what geometry exists).
    const std::vector<std::string> SceneTypes = {"RenderPipeline","Solar","OpticalFlare","Atmosphere","Clouds","Fog","Stars","Moons","Planet","ForceField","Wind",
                                                 "FluidPlume","Fracture","Particles","ForceSource","Geometry","Vehicle","Tyre"};
    OutlinerPanel.ConfigureVisibleTypes(SceneTypes);
    auto Types = QueryWorldSpace().EnumerateRegisteredTypes();
    std::erase_if(Types,[&](const auto& Entry)
    {
        // 📝 The localized forces are added by type, as the Construct dialog's Forces group (one tile per type), not as one record type.
        return (std::find(SceneTypes.begin(),SceneTypes.end(),Entry.first)==SceneTypes.end() && Entry.first!="LocalCloud" && Entry.first!="LocalFog") ||
               Entry.first=="RenderPipeline" || Entry.first=="Geometry" || Entry.first=="ForceSource";
    });
    OutlinerPanel.ConfigureConstructibleTypes(std::move(Types));
    // 📝 The Outliner's '+' and Ctrl+A open the Construct dialog: the engine shapes, then the record types above.
    OutlinerPanel.ConfigureConstructDialog(true);
    ConfigureConstructCatalogue();
    OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());

    if (Frontier::SceneRecordBase* Solar = QueryWorldSpace().FindFirstByType("Solar"))
    {
        OutlinerPanel.Select(Solar->QueryToken());
        OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    }

    Frontier::InspectorRuntime::Instance().RegisterSolarPlacementSolver(&Frontier::SolveSolarPlacement);
    Frontier::InspectorRuntime::Instance().RegisterLevelFlowSampler([this](const std::array<double, 3>& LevelMetres) { return SampleLevelFlow(LevelMetres); });

    Dockspace.ConfigureDockspaceKey("EverestWorkspaceDockspace");
    // 📝 No top toolbar (owner's request): only the Control Centre notch remains, in a band as tall as its tab.
    Dockspace.ConfigureWorkspaceInsets(NotchBandHeight, 0);
    // 📝 The Control Centre can outlive the workspace (a host may retire it either way round): the actions hold only a weak
    //    token of the workspace and do nothing once it is gone.
    const std::weak_ptr<bool> Token = Liveness;
    Centre.ConfigureQuickActions({
        {"Projects", Frontier::ControlCentreIconCategory::FolderOpen,
         [this, Token]() {
             if (!Token.expired() && EvaluateReturnToBrowser)
             {
                 EvaluateReturnToBrowser();
             }
         }},
        {"Render settings",Frontier::ControlCentreIconCategory::SlidersQuality,
         [this,Token]() { if (!Token.expired()) if (auto* Entry=QueryWorldSpace().FindFirstByType("RenderPipeline")) OutlinerPanel.Select(Entry->QueryToken()); }},
        {"Save level", Frontier::ControlCentreIconCategory::SaveDisk,
         [this, Token]() {
             if (!Token.expired())
             {
                 SaveActiveLevel();
             }
         }},
        {"Networking", Frontier::ControlCentreIconCategory::WirelessSignal,
         [this, Token]() {
             if (!Token.expired())
             {
                 ToggleNetworking();
             }
         },
         [this, Token]() { return !Token.expired() && QueryNetworkingEnabled(); }},
        {"Vehicle workbench", Frontier::ControlCentreIconCategory::SlidersQuality,
         [this, Token]() {
             if (!Token.expired()) (void)Dockspace.ConfigureOpen("VehicleWorkbench", !Dockspace.QueryOpen("VehicleWorkbench"));
         },
         [this, Token]() { return !Token.expired() && Dockspace.QueryOpen("VehicleWorkbench"); }},
    });

    auto OutlinerSpec=Frontier::MakeSceneDirectoryPanelSpecification(OutlinerPanel);
    auto InspectorSpec=Frontier::MakeSceneInspectorPanelSpecification(InspectorPanel);
    InspectorSpec.ConstructContent = [this]() {
        if (!Dockspace.QueryOpen("ForceFieldWorkspace"))
        {
            InspectorPanel.ConstructInterface();
            return;
        }
        ImGui::Dummy(ImVec2(1.0f, 18.0f));
        ImGui::TextDisabled("Force Field / Wind workspace");
        ImGui::TextWrapped("Record inspection is hosted beside the field canvas while that workspace is open.");
        if (ImGui::SmallButton("Close Force Field workspace")) (void)Dockspace.ConfigureOpen("ForceFieldWorkspace", false);
    };
    auto LogSpec=Frontier::MakeLogStreamPanelSpecification(LogPanel);
    Frontier::DockPanelSpecification ViewportSpec{"Viewport","Viewport",[this]() { ConstructViewportSurface(); },true,{},{},true};
    ViewportSpec.ConstructFooter=[this]() { ConstructViewportFooter(Frontier::InspectorRuntime::Instance().ResolveInterfaceScale()); };
    auto Footer=[this](std::vector<Frontier::ViewportStatusItem> Cells)
    {
        const float Scale=Frontier::InspectorRuntime::Instance().ResolveInterfaceScale();
        const auto Min=ImGui::GetCursorScreenPos();
        Frontier::ViewportChromeControls::ConstructStatusStrip(ImGui::GetWindowDrawList(),Frontier::EditorTypefaces::Resolve(),Min,
            {Min.x+ImGui::GetContentRegionAvail().x,Min.y+28*Scale},Cells,{},Scale);
        ImGui::Dummy({1,28*Scale});
    };
    OutlinerSpec.ConstructFooter=[this,Footer]() { Footer({{"SCENE",std::to_string(OutlinerPanel.QueryTotal()),IM_COL32(164,198,180,255),"Layers"},
        {"SELECTED",OutlinerPanel.QuerySelected().IsValid()?"1":"0",IM_COL32(170,190,208,255),"MousePointer"}}); };
    InspectorSpec.ConstructFooter=[this,Footer]()
    {
        const auto* Page=Frontier::InspectorRuntime::Instance().FindInspector(OutlinerPanel.QuerySelected());
        Footer({{"PARAMETERS",Page?"Editing":"No selection",IM_COL32(160,192,213,255),"Info"},
            {"REV",Page?std::to_string(Page->QueryRevision()):"—",IM_COL32(192,195,184,255),nullptr}});
    };
    LogSpec.ConstructFooter=[this,Footer]() { Footer({{"ENTRIES",std::to_string(LogPanel.QueryTotalCount()),IM_COL32(150,192,216,255),"Info"},
        {"WARN",std::to_string(LogPanel.QueryWarningsLastMinute()),IM_COL32(224,184,105,255),"TriangleAlert"},
        {"ERROR",std::to_string(LogPanel.QueryErrorsLastMinute()),IM_COL32(224,125,124,255),"CircleAlert"}}); };
    if (!Dockspace.Register(std::move(OutlinerSpec)) || !Dockspace.Register(std::move(ViewportSpec)) ||
        !Dockspace.Register(std::move(LogSpec)) || !Dockspace.Register(std::move(InspectorSpec)))
        throw std::runtime_error("EverestWorkspace dockspace registration refused.");

    (void)Dockspace.ConfigureStackedPanels("Viewport", "LogStream", 520.0f);
    (void)Dockspace.ConfigureColumns({
        {"Outliner", 0.200f},
        {"LogStream", 0.545f},
        {"Inspector", 0.255f},
    });

    // 📝 An opt-in dockable workbench: the existing field canvas/timeline and the selected ForceField, Wind or ForceSource inspector.
    Frontier::DockPanelSpecification ForceFieldWorkspaceSpec;
    ForceFieldWorkspaceSpec.Key              = "ForceFieldWorkspace";
    ForceFieldWorkspaceSpec.Caption          = "Force Field / Wind";
    ForceFieldWorkspaceSpec.ConstructContent = [this]() {
        ConstructForceFieldWorkspace(Frontier::InspectorRuntime::Instance().ResolveInterfaceScale());
    };
    ForceFieldWorkspaceSpec.Open            = false;
    ForceFieldWorkspaceSpec.WheelScrollOnly = true;
    ForceFieldWorkspaceSpec.Available = [this]() { return QueryWorldSpace().FindFirstByType("ForceField") != nullptr; };
    ForceFieldWorkspaceSpec.ConstructFooter = [this, Footer]() {
        const auto* FieldRecord = dynamic_cast<const Frontier::ForceFieldRecord*>(QueryWorldSpace().FindFirstByType("ForceField"));
        const auto* WindRecord  = dynamic_cast<const Frontier::WindRecord*>(QueryWorldSpace().FindFirstByType("Wind"));
        const size_t Sources    = QueryWorldSpace().FindAllByType("ForceSource").size();
        char         Clock[24];
        if (FieldRecord != nullptr) std::snprintf(Clock, sizeof(Clock), "%.1f s", FieldRecord->QueryConfiguration().TimelineSeconds);
        else std::snprintf(Clock, sizeof(Clock), "—");
        Footer({{"SOURCES", std::to_string(Sources), IM_COL32(0, 194, 168, 255), "Waves"},
                {"WIND", WindRecord != nullptr && WindRecord->QueryConfiguration().Enabled ? "On" : "Off / absent", IM_COL32(255, 138, 91, 255), "Wind"},
                {"CLOCK", Clock, IM_COL32(173, 181, 189, 255), nullptr}});
    };
    if (!Dockspace.Register(std::move(ForceFieldWorkspaceSpec)))
        throw std::runtime_error("EverestWorkspace Force Field workspace registration refused.");

    // 📝 The vehicle workbench puts component inspection, driver-aid quick controls and the live test loop in one dockable place.
    Frontier::DockPanelSpecification VehicleWorkbenchSpec;
    VehicleWorkbenchSpec.Key = "VehicleWorkbench";
    VehicleWorkbenchSpec.Caption = "Vehicle workbench";
    VehicleWorkbenchSpec.ConstructContent = [this]() {
        ConstructVehicleWorkbench(Frontier::InspectorRuntime::Instance().ResolveInterfaceScale());
    };
    VehicleWorkbenchSpec.Open = false;
    VehicleWorkbenchSpec.WheelScrollOnly = true;
    VehicleWorkbenchSpec.Available = [this]() { return !QueryWorldSpace().FindAllByType("Vehicle").empty(); };
    VehicleWorkbenchSpec.ConstructFooter = [this, Footer]() {
        const auto Vehicles = QueryWorldSpace().FindAllByType("Vehicle");
        Frontier::VehicleReading Reading{};
        const bool Live = !Vehicles.empty() && Frontier::QueryVehicleReading(Vehicles.front()->QueryToken(), Reading) && Reading.Simulating;
        char Speed[32];
        if (Live) std::snprintf(Speed, sizeof(Speed), "%.0f km/h", Reading.Telemetry.SpeedMetresPerSecond * 3.6f);
        else std::snprintf(Speed, sizeof(Speed), "Stopped");
        Footer({{"VEHICLES", std::to_string(Vehicles.size()), IM_COL32(152, 200, 181, 255), "Car"},
                {"TEST LOOP", Speed, Live ? IM_COL32(93, 215, 159, 255) : IM_COL32(165, 171, 182, 255), "Gauge"}});
    };
    if (!Dockspace.Register(std::move(VehicleWorkbenchSpec)))
        throw std::runtime_error("EverestWorkspace vehicle workbench registration refused.");

    ImGui::GetIO().IniFilename = nullptr;
    ObservedLayoutRevision     = Centre.QueryLayoutResetRevision();
}

#else

EverestWorkspace::EverestWorkspace(std::filesystem::path ProjectRootPath)
    : ProjectRoot(std::move(ProjectRootPath))
{
    InitializeWorld();
}

#endif

EverestWorkspace::~EverestWorkspace()
{
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
    ReleasePointerConstraint();
    if (Physics && Physics->QueryRunning()) Physics->End(QueryWorldSpace());
    Frontier::InspectorRuntime::Instance().ConfigureEnvironmentAccess({});
    Frontier::InspectorRuntime::Instance().RegisterLevelFlowSampler({});
    Frontier::InspectorRuntime::Instance().ConfigureLevelFile({});
#endif
}

void EverestWorkspace::InitializeWorld()
{
    const uint32_t ProjectCategory = EnsureProjectLogCategory("Everest");
    (void)EnsureProjectLogCategory("Physics");

    const std::filesystem::path ProjectSpacePath = ProjectRoot / "Everest.projectspace";
    if (std::filesystem::exists(ProjectSpacePath))
    {
        if (Application.Initialize(ProjectSpacePath, &Diagnostic))
        {
            ActiveLevelPath = QueryWorldSpace().QueryLoadedLevelPath();
#if FRONTIER_EDITOR
            Frontier::SceneDirectory::Instance().ConfigureActiveSceneHeader(
                ActiveLevelPath.stem().string(), "Frontier LevelSpace");
#endif
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log,
                                     "EverestWorkspace", "Loaded '%s' (%zu scene records).",
                                     ActiveLevelPath.filename().string().c_str(),
                                     QueryWorldSpace().QueryRecordCount());
        }
        else
        {
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Error, Frontier::LogVerbosity::Log,
                                     "EverestWorkspace", "Project startup failed: %s", Diagnostic.c_str());
        }
    }
    else
    {
        ActiveLevelPath = ProjectRoot / "Content" / "Levels" / "Default.levelspace";
        RegisterEverestSimulationTypes(QueryWorldSpace());
        if (std::filesystem::exists(ActiveLevelPath) && QueryWorldSpace().LoadLevelSpace(ActiveLevelPath, &Diagnostic))
        {
#if FRONTIER_EDITOR
            Frontier::SceneDirectory::Instance().ConfigureActiveSceneHeader(
                ActiveLevelPath.stem().string(), "Frontier LevelSpace");
#endif
        }
        else
        {
            Diagnostic = "Project container missing: " + ProjectSpacePath.string();
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Warning, Frontier::LogVerbosity::Log,
                                     "EverestWorkspace", "Initialized empty WorldSpace (%s).", Diagnostic.c_str());
        }
    }
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
    // 📝 Records with a file of their own (a Particles record's .particlespace) keep it beside this level.
    Frontier::InspectorRuntime::Instance().ConfigureLevelFile(ActiveLevelPath);
#endif
    SynchronizeActiveLevelState(true, false);
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    SESSION & LEVEL TRANSITIONS
//------------------------------------------------------------------------------------------------------------------------

bool EverestWorkspace::QueryLobbyActive() const noexcept
{
    return QueryWorldSpace().QueryEnvironmentArchive() == "Lobby";
}

Frontier::VehicleSubjectKinematics EverestWorkspace::ResolveVehicleKinematics(double AspectRatio) const
{
    Frontier::VehicleSubjectKinematics Subject{};
    Subject.AspectRatio = std::max(0.25, AspectRatio);
    Subject.Available   = false;

    for (const Frontier::SceneRecordBase* Candidate : QueryWorldSpace().FindAllByType("Vehicle"))
    {
        const auto* VehicleRec = dynamic_cast<const Frontier::VehicleRecord*>(Candidate);
        if (VehicleRec == nullptr || !VehicleRec->QueryConfiguration().Visible)
        {
            continue;
        }
        Subject.Available = true;
        const Frontier::VehicleConfiguration& Configuration = VehicleRec->QueryConfiguration();
        const Frontier::Vehicle::VehicleGeometry& Geometry = Configuration.Specification.Geometry;
        const Frontier::Vehicle::VehicleSolverConfiguration Solver =
            Frontier::Vehicle::ResolveSolverConfiguration(Configuration.Specification);

        Frontier::VehicleReading Reading;
        if (Frontier::QueryVehicleReading(Candidate->QueryToken(), Reading) && Reading.Simulating)
        {
            const Frontier::Vehicle::ChassisKinematics& Chassis = Reading.Telemetry.Chassis;
            const Frontier::Vehicle::Vec3               Fwd     = Chassis.Orientation.Rotate({1.0f, 0.0f, 0.0f});
            const Frontier::Vehicle::Vec3               Left    = Chassis.Orientation.Rotate({0.0f, 1.0f, 0.0f});
            const Frontier::Vehicle::Vec3               Up      = Chassis.Orientation.Rotate({0.0f, 0.0f, 1.0f});
            Subject.ForwardUnit          = {Fwd.x, Fwd.y, Fwd.z};
            Subject.LeftUnit             = {Left.x, Left.y, Left.z};
            Subject.UpUnit               = {Up.x, Up.y, Up.z};
            Subject.VelocityMetresPerSec = {Chassis.LinearVelocity.x, Chassis.LinearVelocity.y, Chassis.LinearVelocity.z};
            Subject.SpeedMetresPerSecond = std::sqrt(Subject.VelocityMetresPerSec[0] * Subject.VelocityMetresPerSec[0] +
                                                     Subject.VelocityMetresPerSec[1] * Subject.VelocityMetresPerSec[1] +
                                                     Subject.VelocityMetresPerSec[2] * Subject.VelocityMetresPerSec[2]);
            Subject.GroundFloorMetres    = static_cast<double>(Configuration.Spatial.PositionMetres[2]);
            if (Reading.VisualValid)
            {
                Subject.PositionMetres     = {Reading.BodyTransform[12], Reading.BodyTransform[13], Reading.BodyTransform[14]};
                Subject.FrontLeftHubMetres = {Reading.WheelTransforms[0][12], Reading.WheelTransforms[0][13], Reading.WheelTransforms[0][14]};
                Subject.RearRightHubMetres = {Reading.WheelTransforms[3][12], Reading.WheelTransforms[3][13], Reading.WheelTransforms[3][14]};
                return Subject;
            }
            const double ChassisToBodyZ = Geometry.CoMAuthoredZ();
            Subject.PositionMetres = {
                static_cast<double>(Chassis.Position.x) - static_cast<double>(Up.x) * ChassisToBodyZ,
                static_cast<double>(Chassis.Position.y) - static_cast<double>(Up.y) * ChassisToBodyZ,
                static_cast<double>(Chassis.Position.z) - static_cast<double>(Up.z) * ChassisToBodyZ,
            };
        }
        else
        {
            const double YawRad   = static_cast<double>(Configuration.Spatial.OrientationDegrees[2]) * DegreesToRadian;
            const double PitchRad = static_cast<double>(Configuration.Spatial.OrientationDegrees[1]) * DegreesToRadian;
            const double RollRad  = static_cast<double>(Configuration.Spatial.OrientationDegrees[0]) * DegreesToRadian;
            const Frontier::Vehicle::Quat Orientation =
                (Frontier::Vehicle::Quat::AxisAngle({0.0f, 0.0f, 1.0f}, static_cast<float>(YawRad)) *
                 Frontier::Vehicle::Quat::AxisAngle({0.0f, 1.0f, 0.0f}, static_cast<float>(PitchRad)) *
                 Frontier::Vehicle::Quat::AxisAngle({1.0f, 0.0f, 0.0f}, static_cast<float>(RollRad))).Normalized();
            const Frontier::Vehicle::Vec3 Fwd  = Orientation.Rotate({1.0f, 0.0f, 0.0f});
            const Frontier::Vehicle::Vec3 Left = Orientation.Rotate({0.0f, 1.0f, 0.0f});
            const Frontier::Vehicle::Vec3 Up   = Orientation.Rotate({0.0f, 0.0f, 1.0f});
            const double BodyGroundOffset = Geometry.AuthoredGroundOffset();
            Subject.GroundFloorMetres     = static_cast<double>(Configuration.Spatial.PositionMetres[2]);
            Subject.PositionMetres             = {
                static_cast<double>(Configuration.Spatial.PositionMetres[0]) + Up.x * BodyGroundOffset,
                static_cast<double>(Configuration.Spatial.PositionMetres[1]) + Up.y * BodyGroundOffset,
                static_cast<double>(Configuration.Spatial.PositionMetres[2]) + Up.z * BodyGroundOffset,
            };
            Subject.ForwardUnit = {Fwd.x, Fwd.y, Fwd.z};
            Subject.LeftUnit    = {Left.x, Left.y, Left.z};
            Subject.UpUnit      = {Up.x, Up.y, Up.z};
        }

        const auto ResolveBodyLocalHub = [&](uint32_t Corner)
        {
            Frontier::Vehicle::Vec3 Offset = Frontier::Vehicle::ResolveRestingHubLocal(Solver, Corner);
            Offset.z += Geometry.CoMAuthoredZ(); // solver hub is CoM-local; camera subject position is authored-body-local
            return Offset;
        };
        const auto TransformWheelOffset = [&](const Frontier::Vehicle::Vec3& Offset)
        {
            return std::array<double, 3>{
                Subject.PositionMetres[0] + Subject.ForwardUnit[0] * Offset.x + Subject.LeftUnit[0] * Offset.y + Subject.UpUnit[0] * Offset.z,
                Subject.PositionMetres[1] + Subject.ForwardUnit[1] * Offset.x + Subject.LeftUnit[1] * Offset.y + Subject.UpUnit[1] * Offset.z,
                Subject.PositionMetres[2] + Subject.ForwardUnit[2] * Offset.x + Subject.LeftUnit[2] * Offset.y + Subject.UpUnit[2] * Offset.z,
            };
        };
        Subject.FrontLeftHubMetres = TransformWheelOffset(ResolveBodyLocalHub(0u));
        Subject.RearRightHubMetres = TransformWheelOffset(ResolveBodyLocalHub(3u));
        break;
    }

    return Subject;
}

void EverestWorkspace::ApplyVehicleCameraSolution(const Frontier::VehicleCameraSolution& Solution)
{
    ActiveNearClipMetres = Solution.NearClipMetres;
    ActiveVeilOpacity    = Solution.VeilOpacity;
    LastAppliedStudioEye = Solution.PositionMetres;

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
    Flight.Position            = Solution.PositionMetres;
    Flight.VerticalFieldOfView = Solution.VerticalFieldOfViewRadians;
    Flight.FieldOfViewOffset   = 0.0;
    Flight.FieldOfViewRate     = 0.0;
    Flight.Velocity            = {0.0, 0.0, 0.0};
    Navigation.Up              = Solution.UpUnit;

    const double Dx            = Solution.TargetMetres[0] - Solution.PositionMetres[0];
    const double Dy            = Solution.TargetMetres[1] - Solution.PositionMetres[1];
    const double Dz            = Solution.TargetMetres[2] - Solution.PositionMetres[2];
    const double Planar        = std::hypot(Dx, Dy);
    const double Distance      = std::max(0.05, std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz));
    Flight.PivotDistance       = Distance;
    Flight.Heading             = std::atan2(Dy, Dx) / DegreesToRadian;
    Flight.Elevation           = std::clamp(std::atan2(Dz, std::max(1.0e-6, Planar)) / DegreesToRadian, -89.5, 89.5);
#endif
}

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
void EverestWorkspace::CaptureEditorCameraState() noexcept
{
    if (StudioCameraEngaged)
    {
        return;
    }

    SavedEditorFlight           = Flight;
    SavedEditorNavigation       = Navigation;
    SavedEditorCameraReady      = true;
}

void EverestWorkspace::RestoreEditorCameraState() noexcept
{
    if (!SavedEditorCameraReady)
    {
        return;
    }

    Flight                     = SavedEditorFlight;
    Navigation                 = SavedEditorNavigation;
    SavedEditorCameraReady     = false;
}
#endif

void EverestWorkspace::ConfigureGameCameraMode(bool Enabled)
{
    if (Enabled == StudioCameraEngaged)
    {
        return;
    }

    if (Enabled)
    {
        double Aspect = 16.0 / 9.0;
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        if (LastReadout.ScreenExtent[1] > 0u)
        {
            Aspect = static_cast<double>(LastReadout.ScreenExtent[0]) / static_cast<double>(LastReadout.ScreenExtent[1]);
        }
#endif
        const auto Subject = ResolveVehicleKinematics(Aspect);
        if (!Subject.Available && !QueryLobbyActive())
        {
            return;
        }
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        CaptureEditorCameraState();
#endif
        StudioCameraEngaged = true;
        const auto Solution = VehicleCamera.Advance(Subject, Frontier::VehicleCameraInput{}, 0.0);
        ApplyVehicleCameraSolution(Solution);
    }
    else
    {
        StudioCameraEngaged  = false;
        ActiveNearClipMetres = 0.05;
        ActiveVeilOpacity    = 0.0;
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        RestoreEditorCameraState();
#endif
    }
}

void EverestWorkspace::SynchronizeActiveLevelState(bool ResetOutdoorEditorCamera, bool StartDrivingSimulation)
{
    const bool InLobby = QueryLobbyActive();
    if (InLobby)
    {
        (void)Lobby.SynchronizeWorldVisibility(QueryWorldSpace());
        VehicleCamera.ConfigureEnvironment(true, ResolveVehicleKinematics());
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        if (!StudioCameraEngaged)
        {
            CaptureEditorCameraState();
        }
#endif
        StudioCameraEngaged = true;
        ApplyVehicleCameraSolution(VehicleCamera.QueryLastSolution());
    }
    else
    {
        const auto InitialSubject = ResolveVehicleKinematics();
        VehicleCamera.ConfigureEnvironment(false, InitialSubject);
        const bool EnterVehicleCamera = StartDrivingSimulation && InitialSubject.Available;
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        const bool LeavingVehicleCamera = StudioCameraEngaged && !EnterVehicleCamera;
        if (EnterVehicleCamera && !StudioCameraEngaged)
        {
            CaptureEditorCameraState();
        }
#endif
        StudioCameraEngaged  = EnterVehicleCamera;
        ActiveNearClipMetres = StudioCameraEngaged ? VehicleCamera.QueryLastSolution().NearClipMetres : 0.05;
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        if (StudioCameraEngaged)
        {
            ApplyVehicleCameraSolution(VehicleCamera.QueryLastSolution());
        }
        else if (ResetOutdoorEditorCamera)
        {
            Flight                     = SceneViewportFlight{};
            Navigation                 = SceneViewportNavigation{};
            SavedEditorCameraReady     = false;
        }
        else if (LeavingVehicleCamera)
        {
            // A level reload may stage a custom editor pose; do not restore a stale pre-transition snapshot.
            SavedEditorCameraReady = false;
        }
        if (StartDrivingSimulation && (!Physics || !Physics->QueryRunning()))
        {
            if (PresentSceneViewport && PublishedGeometryGeneration == 0)
            {
                PendingAutoSimulation = true;
            }
            else
            {
                PendingAutoSimulation = false;
                ToggleSimulation();
            }
        }
#else
        (void)ResetOutdoorEditorCamera;
        (void)StartDrivingSimulation;
#endif
    }
}

void EverestWorkspace::CheckExternalLevelReload()
{
    const std::filesystem::path Loaded = QueryWorldSpace().QueryLoadedLevelPath();
    if (Loaded.empty() || Loaded == ActiveLevelPath)
    {
        return;
    }
    ActiveLevelPath = Loaded;
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
    PublishedGeometryGeneration = 0;
    Frontier::SceneDirectory::Instance().ConfigureActiveSceneHeader(ActiveLevelPath.stem().string(), "Frontier LevelSpace");
    Frontier::InspectorRuntime::Instance().ConfigureLevelFile(ActiveLevelPath);
    OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    // Reset Flight to standard outdoor scene camera only if the caller has not already staged a custom Flight pose.
    const double DriftFromStudio = std::hypot(Flight.Position[0] - LastAppliedStudioEye[0],
                                              std::hypot(Flight.Position[1] - LastAppliedStudioEye[1],
                                                         Flight.Position[2] - LastAppliedStudioEye[2]));
    const bool CallerLeftStudioPose = StudioCameraEngaged && (DriftFromStudio < 1.0e-3);
    SynchronizeActiveLevelState(CallerLeftStudioPose, false);
#else
    SynchronizeActiveLevelState(false, false);
#endif
}

bool EverestWorkspace::OpenLevel(const std::filesystem::path& LevelPath, bool StartDrivingSimulation)
{
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
    if (Physics && Physics->QueryRunning())
    {
        Physics->End(QueryWorldSpace());
        DriverControls = Frontier::Vehicle::DriverInputIntegrator{};
    }
    PendingImpactSparks.clear();
    Frontier::ClearVehicleReadings();
#endif

    std::filesystem::path ResolvedPath = LevelPath;
    if (ResolvedPath.is_relative() && !std::filesystem::exists(ResolvedPath))
    {
        ResolvedPath = ProjectRoot / ResolvedPath;
    }

    std::string LoadDiag;
    if (!QueryWorldSpace().LoadLevelSpace(ResolvedPath, &LoadDiag))
    {
        Diagnostic = LoadDiag;
        Frontier::WriteLogFormat(EnsureProjectLogCategory("Everest"), Frontier::LogSeverity::Error, Frontier::LogVerbosity::Log,
                                 "EverestWorkspace", "OpenLevel failed for '%s': %s",
                                 ResolvedPath.string().c_str(), LoadDiag.c_str());
        return false;
    }

    ActiveLevelPath = QueryWorldSpace().QueryLoadedLevelPath();
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
    PublishedGeometryGeneration = 0;
    PendingAutoSimulation       = false;
    PendingAlbedo.clear();
    Frontier::SceneDirectory::Instance().ConfigureActiveSceneHeader(ActiveLevelPath.stem().string(), "Frontier LevelSpace");
    Frontier::InspectorRuntime::Instance().ConfigureLevelFile(ActiveLevelPath);
    OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    if (Frontier::SceneRecordBase* Solar = QueryWorldSpace().FindFirstByType("Solar"))
    {
        OutlinerPanel.Select(Solar->QueryToken());
    }
#endif

    SynchronizeActiveLevelState(true, StartDrivingSimulation);
    Frontier::WriteLogFormat(EnsureProjectLogCategory("Everest"), Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log,
                             "EverestWorkspace", "Opened level '%s' (archive '%s', %zu records).",
                             ActiveLevelPath.filename().string().c_str(),
                             QueryWorldSpace().QueryEnvironmentArchive().c_str(),
                             QueryWorldSpace().QueryRecordCount());
    return true;
}

void EverestWorkspace::BeginLevelTransition(const std::filesystem::path& LevelPath, bool StartDrivingSimulation)
{
    PendingTransitionLevel = LevelPath;
    PendingTransitionDrive = StartDrivingSimulation;
    VehicleCamera.BeginRouteTransition(0.48, 0.58);
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    RUNTIME SIMULATION
//------------------------------------------------------------------------------------------------------------------------

void EverestWorkspace::IntegrateSimulation(double DeltaSeconds)
{
    CheckExternalLevelReload();

    const float ClampedDt = static_cast<float>(std::clamp(DeltaSeconds, 0.0, 0.25));
    ElapsedSeconds += static_cast<double>(ClampedDt);

    Lobby.Advance(QueryWorldSpace(), static_cast<double>(ClampedDt));
    Application.StepFrame(ClampedDt);
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    EDITOR INTERFACE
//------------------------------------------------------------------------------------------------------------------------

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

bool EverestWorkspace::ConfigurePlatform() noexcept
{
    Dockspace.ConfigureDetachedWindows(Centre.QueryDetachedWindows());
    return Dockspace.ConfigurePlatform();
}

void EverestWorkspace::ServiceDynamicSceneRequests()
{
    const uint32_t ProjectCategory = EnsureProjectLogCategory("Everest");
    bool           Mutated         = false;

    if (OutlinerPanel.ConsumeConstructDialogRequest()) ConstructDialog.Open();
    while (const auto DuplicateToken = OutlinerPanel.ConsumeDuplicateRequest())
    {
        const std::string           Original = Frontier::SceneDirectory::Instance().QueryName(*DuplicateToken);
        const Frontier::RecordToken Created  = DuplicateGeometryRecord(*DuplicateToken);
        if (!Created.IsValid())
        {
            AnnounceViewportNotice("Only scene objects duplicate");
            continue;
        }
        OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
        OutlinerPanel.Select(Created);
        Mutated = true;
        Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log, "EverestWorkspace",
                                 "Duplicated '%s' as '%s' (token #%llu): its copies come from the reserve.", Original.c_str(),
                                 Frontier::SceneDirectory::Instance().QueryName(Created).c_str(), static_cast<unsigned long long>(Created.Value));
    }
    while (const auto Renamed = OutlinerPanel.ConsumeRenameRequest())
    {
        if (Frontier::SceneRecordBase* Record = QueryWorldSpace().FindRecord(Renamed->first))
        {
            const std::string Former = Record->QueryRecordName();
            Record->AssignRecordName(Renamed->second);
            Mutated = true;
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log, "EverestWorkspace",
                                     "Renamed '%s' to '%s'.", Former.c_str(), Renamed->second.c_str());
        }
    }

    while (const auto RequestedType = OutlinerPanel.ConsumeConstructRequest())
    {
        Frontier::RecordToken ParentToken{};
        if (*RequestedType == "OpticalFlare")
        {
            if (Frontier::SceneRecordBase* Solar = QueryWorldSpace().FindFirstByType("Solar"))
            {
                ParentToken = Solar->QueryToken();
            }
        }
        const Frontier::RecordToken Created = QueryWorldSpace().ConstructRecord(*RequestedType, {}, ParentToken);
        if (Created.IsValid())
        {
            OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
            OutlinerPanel.Select(Created);
            Mutated = true;
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log,
                                     "EverestWorkspace", "Added record '%s' (type '%s', token #%llu).",
                                     Frontier::SceneDirectory::Instance().QueryName(Created).c_str(),
                                     RequestedType->c_str(),
                                     static_cast<unsigned long long>(Created.Value));
        }
    }

    while (const auto DeleteToken = OutlinerPanel.ConsumeDeleteRequest())
    {
        const std::string OldName = Frontier::SceneDirectory::Instance().QueryName(*DeleteToken);
        if (QueryWorldSpace().DestroyRecord(*DeleteToken))
        {
            Mutated = true;
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log,
                                     "EverestWorkspace", "Removed record '%s' (token #%llu).",
                                     OldName.c_str(),
                                     static_cast<unsigned long long>(DeleteToken->Value));
        }
    }

    if (Mutated)
    {
        OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    }
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    SCENE OBJECTS
//------------------------------------------------------------------------------------------------------------------------

void EverestWorkspace::ConfigureConstructCatalogue()
{
    std::vector<Frontier::ConstructCatalogueEntry> Catalogue;
    // ① the engine shapes: EngineContent/ShapeArchives/*.toml
    for (const Frontier::ShapeArchive& Shape : Frontier::ReadShapeArchives(Frontier::QueryEngineContentDirectory() / "ShapeArchives"))
    {
        Frontier::ConstructCatalogueEntry Entry;
        Entry.Identifier = Shape.Name;
        Entry.Caption    = Shape.Name;
        Entry.Glyph      = Shape.GlyphName.empty() ? "Box" : Shape.GlyphName;
        Entry.Group      = "Geometry";
        Entry.Summary    = Shape.Description;
        Entry.Shape      = true;
        Entry.Size       = Shape.Size;
        Entry.Albedo     = {Shape.Surface[0], Shape.Surface[1], Shape.Surface[2]};
        Catalogue.push_back(std::move(Entry));
    }
    // ② the record types the level can add, under the Project Zero editor's categories
    struct TypeLook
    {
        const char* Type;
        const char* Group;
        const char* Glyph;
    };
    static constexpr TypeLook Looks[] = {
        {"Solar", "Environment", "Sun"},      {"OpticalFlare", "Environment", "Aperture"}, {"Atmosphere", "Environment", "Sunrise"},
        {"Stars", "Environment", "Sparkles"}, {"Moons", "Environment", "Moon"},          {"Planet", "Environment", "Orbit"},
        {"Clouds", "Weather", "CloudFog"},    {"LocalCloud", "Weather", "CloudFog"},     {"Fog", "Weather", "Droplets"},
        {"LocalFog", "Weather", "Droplets"},  {"Wind", "Weather", "Wind"},               {"ForceField", "Weather", "Waves"},
        {"FluidPlume", "Solvers", "Flame"},    {"Fracture", "Solvers", "Hammer"},          {"Particles", "Solvers", "Sparkles"},
        {"Vehicle", "Geometry", "Car"},       {"Tyre", "Geometry", "CircleDot"},
    };
    for (const auto& [Type, Label] : OutlinerPanel.QueryConstructibleTypes())
    {
        Frontier::ConstructCatalogueEntry Entry;
        Entry.Identifier = Type;
        const size_t Divider = Label.find("\xC2\xB7 ");
        Entry.Caption    = Divider == std::string::npos ? Label : Label.substr(Divider + 3);
        Entry.Group      = "Environment";
        Entry.Glyph      = "Box";
        for (const TypeLook& Look : Looks)
            if (Type == Look.Type)
            {
                Entry.Group = Look.Group;
                Entry.Glyph = Look.Glyph;
            }
        Entry.Summary = Label + ".";
        Catalogue.push_back(std::move(Entry));
    }
    // ③ the localized forces under the Force Field, one tile per type in the library's order (flows, accelerations, damping)
    for (const Frontier::ForceSourceDescriptor& Descriptor : Frontier::EnumerateForceSourceDescriptors())
    {
        Frontier::ConstructCatalogueEntry Entry;
        Entry.Identifier = std::string("ForceSource:") + Descriptor.Identifier;
        Entry.Caption    = Descriptor.Name;
        Entry.Glyph      = Descriptor.Glyph;
        Entry.Group      = "Forces";
        Entry.Summary    = std::string(Frontier::ResolveForceContributionDescriptor(Descriptor.Contribution).Label) + " \xC2\xB7 " + Descriptor.Blurb;
        Catalogue.push_back(std::move(Entry));
    }
    ConstructDialog.ConfigureCatalogue(std::move(Catalogue));
}

std::string EverestWorkspace::ResolveUniqueName(const std::string& Stem) const
{
    const auto Taken = [this](const std::string& Name) {
        for (const auto& Row : OutlinerPanel.QueryRows())
            if (Row.Name == Name) return true;
        return false;
    };
    if (!Taken(Stem)) return Stem;
    for (int Index = 2;; ++Index)
    {
        const std::string Name = Stem + " " + std::to_string(Index);
        if (!Taken(Name)) return Name;
    }
}

std::array<double, 3> EverestWorkspace::ResolveGroundTarget() const
{
    // 📝 Where the view meets the ground (z = 0), 4 to 40 m ahead; looking level or up, 12 m ahead on the ground.
    const std::array<double, 3> Forward = Flight.ResolveForward();
    double                      Reach   = 12.0;
    if (Forward[2] < -0.02) Reach = std::clamp(-Flight.Position[2] / Forward[2], 4.0, 40.0);
    const double Level = std::max(1.0e-6, std::hypot(Forward[0], Forward[1]));
    if (Forward[2] < -0.02) return {Flight.Position[0] + Forward[0] * Reach, Flight.Position[1] + Forward[1] * Reach, 0.0};
    return {Flight.Position[0] + Forward[0] / Level * Reach, Flight.Position[1] + Forward[1] / Level * Reach, 0.0};
}

void EverestWorkspace::ServiceConstructOrder(const Frontier::ConstructOrder& Order)
{
    const uint32_t ProjectCategory = EnsureProjectLogCategory("Everest");
    const std::string Name = ResolveUniqueName(Order.Name);
    if (Order.Entry.Identifier.rfind("ForceSource:", 0) == 0)
    {
        // 📝 A localized force: a ForceSource record under the Force Field, with its type's defaults, where the view looks.
        const Frontier::RecordToken Created = ConstructForceRecord(Order.Entry.Identifier.substr(12), Name, Order.Visible);
        if (Created.IsValid())
            Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log, "EverestWorkspace",
                                     "Constructed '%s' (force %s, token #%llu).", Name.c_str(), Order.Entry.Identifier.c_str() + 12,
                                     static_cast<unsigned long long>(Created.Value));
        return;
    }
    if (!Order.Entry.Shape)
    {
        // 📝 A record type: as the Outliner's add menu, with the dialog's name and visibility.
        Frontier::RecordToken ParentToken{};
        if (Order.Entry.Identifier == "OpticalFlare")
            if (Frontier::SceneRecordBase* Solar = QueryWorldSpace().FindFirstByType("Solar")) ParentToken = Solar->QueryToken();
        if (Order.Entry.Identifier == "Tyre")
        {
            const Frontier::RecordToken CurrentSelected = OutlinerPanel.QuerySelected();
            if (dynamic_cast<Frontier::VehicleRecord*>(QueryWorldSpace().FindRecord(CurrentSelected)) != nullptr)
                ParentToken = CurrentSelected;
            else if (Frontier::SceneRecordBase* FirstVehicle = QueryWorldSpace().FindFirstByType("Vehicle"))
                ParentToken = FirstVehicle->QueryToken();
        }
        const Frontier::RecordToken Created = QueryWorldSpace().ConstructRecord(Order.Entry.Identifier, Name, ParentToken);
        if (!Created.IsValid()) return;
        if (Order.Entry.Identifier == "Vehicle")
        {
            if (auto* VehicleRec = dynamic_cast<Frontier::VehicleRecord*>(QueryWorldSpace().FindRecord(Created)))
            {
                Frontier::VehicleConfiguration& VehicleConfig = VehicleRec->AccessConfiguration();
                const std::array<double, 3>     Ground        = ResolveGroundTarget();
                VehicleConfig.Visible                         = Order.Visible;
                VehicleConfig.Spatial.PositionMetres[0]       = static_cast<float>(Ground[0]);
                VehicleConfig.Spatial.PositionMetres[1]       = static_cast<float>(Ground[1]);
                VehicleConfig.Spatial.PositionMetres[2]       = 0.0f;
                VehicleConfig.Spatial.OrientationDegrees[2]   = static_cast<float>(std::round(Flight.Heading / 15.0) * 15.0);
                VehicleConfig.AdvanceRevision();
            }
            static constexpr const char* CornerTags[4] = { "FL", "FR", "RL", "RR" };
            for (uint32_t Corner = 0u; Corner < 4u; ++Corner)
            {
                const std::string           TyreName  = Name + " Tyre " + CornerTags[Corner];
                const Frontier::RecordToken TyreToken = QueryWorldSpace().ConstructRecord("Tyre", TyreName, Created);
                if (auto* TyreRec = dynamic_cast<Frontier::TyreRecord*>(QueryWorldSpace().FindRecord(TyreToken)))
                {
                    Frontier::TyreConfiguration& Fit = TyreRec->AccessConfiguration();
                    Fit.Corner                       = Corner;
                    Fit.Enclosed                     = true;
                    Fit.Compound                     = "Medium";
                    Fit.ColdPressure                 = 110000.0f;
                    Fit.AdvanceRevision();
                }
            }
        }
        if (!Order.Visible) (void)Frontier::SceneDirectory::Instance().ConfigureVisible(Created, false);
        OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
        OutlinerPanel.Select(Created);
        Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log, "EverestWorkspace",
                                 "Constructed '%s' (type '%s', token #%llu).", Name.c_str(), Order.Entry.Identifier.c_str(),
                                 static_cast<unsigned long long>(Created.Value));
        return;
    }
    // 📝 An engine shape: a Geometry record that names the shape and claims no archive occupants, on the ground where the view
    //    looks and resting on it (the shapes span −0.5 … 0.5 of their bounding box).
    const Frontier::RecordToken Created = QueryWorldSpace().ConstructRecord("Geometry", Name);
    auto* Record = dynamic_cast<Frontier::GeometryRecord*>(QueryWorldSpace().FindRecord(Created));
    if (Record == nullptr) return;
    Frontier::GeometryConfiguration& Configuration = Record->AccessConfiguration();
    Configuration.OccupantStems.clear();
    Configuration.ClaimsArchive = false;
    Configuration.ShapeName     = Order.Entry.Identifier;
    Configuration.GroupName     = "Constructed";
    Configuration.GlyphName     = Order.Entry.Glyph;
    Configuration.Visible       = Order.Visible;
    const std::array<double, 3> Ground = ResolveGroundTarget();
    for (size_t Axis = 0; Axis < 3; ++Axis) Configuration.Spatial.Scale[Axis] = Order.Size;
    Configuration.Spatial.PositionMetres[0] = static_cast<float>(Ground[0]);
    Configuration.Spatial.PositionMetres[1] = static_cast<float>(Ground[1]);
    Configuration.Spatial.PositionMetres[2] = static_cast<float>(0.5 * Order.Entry.Size[2] * Order.Size);
    Configuration.Spatial.OrientationDegrees[2] = static_cast<float>(std::round(Flight.Heading / 15.0) * 15.0);
    Configuration.AdvanceRevision();
    Record->ConfigureEditorPresentation(Configuration.GlyphName.c_str(), Configuration.GroupName.c_str(), "Geometry", "");
    // 📝 A colour of its own waits until the viewport has bound the shape (its material family is known then).
    if (Order.Surfaced) PendingAlbedo[Created.Value] = Order.Albedo;
    OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    OutlinerPanel.Select(Created);
    Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log, "EverestWorkspace",
                             "Constructed '%s' (engine shape %s \xC3\x97 %.2f, token #%llu) at (%.1f, %.1f).", Name.c_str(),
                             Order.Entry.Identifier.c_str(), Order.Size, static_cast<unsigned long long>(Created.Value), Ground[0], Ground[1]);
}

void EverestWorkspace::ApplyPendingSurfaces()
{
    for (auto Cursor = PendingAlbedo.begin(); Cursor != PendingAlbedo.end();)
    {
        auto* Record = dynamic_cast<Frontier::GeometryRecord*>(QueryWorldSpace().FindRecord(Frontier::RecordToken{Cursor->first}));
        if (Record == nullptr)
        {
            Cursor = PendingAlbedo.erase(Cursor);
            continue;
        }
        Frontier::GeometryConfiguration& Configuration = Record->AccessConfiguration();
        const auto&                      Resolved      = Configuration.Resolved;
        if (!Resolved.Bound || Resolved.Families.empty() || Resolved.FamilyChannels.empty())
        {
            ++Cursor;
            continue;
        }
        Frontier::SurfaceSlotConfiguration Slot;
        Slot.Family   = Resolved.Families.front();
        Slot.Channels = Resolved.FamilyChannels.front();
        for (size_t Channel = 0; Channel < 3; ++Channel) Slot.Channels[Channel] = Cursor->second[Channel];
        Configuration.SurfaceSlots.push_back(Slot);
        Configuration.AdvanceRevision();
        Cursor = PendingAlbedo.erase(Cursor);
    }
}

Frontier::RecordToken EverestWorkspace::DuplicateGeometryRecord(Frontier::RecordToken Token)
{
    auto* Source = dynamic_cast<Frontier::GeometryRecord*>(QueryWorldSpace().FindRecord(Token));
    if (Source == nullptr) return {};
    Frontier::GeometryConfiguration Copy = Source->QueryConfiguration();
    // 📝 Beside the original, one width and a metre along X; a duplicate lays out reserve copies of the same shapes (it never
    //    claims the archive's occupants, so removing it leaves the original alone).
    const auto&  Resolved = Source->QueryConfiguration().Resolved;
    const double Width    = Resolved.Bound ? static_cast<double>(Resolved.BoundsMaximum[0] - Resolved.BoundsMinimum[0]) : 1.0;
    Copy.ClaimsArchive    = false;
    Copy.Resolved         = {};
    Copy.Locked           = false;
    Copy.Spatial.PositionMetres[0] += static_cast<float>(std::max(2.0, Width + 1.0));
    const Frontier::RecordToken Created = QueryWorldSpace().ConstructRecord("Geometry", ResolveUniqueName(Source->QueryRecordName() + " copy"));
    auto* Target = dynamic_cast<Frontier::GeometryRecord*>(QueryWorldSpace().FindRecord(Created));
    if (Target == nullptr) return {};
    Target->AccessConfiguration() = std::move(Copy);
    Target->AccessConfiguration().AdvanceRevision();
    Target->ConfigureEditorPresentation(Target->QueryConfiguration().GlyphName.c_str(), Target->QueryConfiguration().GroupName.c_str(), "Geometry", "");
    return Created;
}

Frontier::GeometryConfiguration* EverestWorkspace::ResolveEditedGeometry()
{
    // 📝 The selected scene object, while it is shown, unlocked and bound (its transform has handles then).
    auto* Record = dynamic_cast<Frontier::GeometryRecord*>(QueryWorldSpace().FindRecord(OutlinerPanel.QuerySelected()));
    if (Record == nullptr) return nullptr;
    Frontier::GeometryConfiguration& Configuration = Record->AccessConfiguration();
    if (Configuration.Locked || !Configuration.Visible || !Configuration.Resolved.Bound || Configuration.Resolved.Shown == 0) return nullptr;
    return &Configuration;
}

Frontier::PlacementView EverestWorkspace::ResolvePlacementView(const ImVec2& StageMin, const ImVec2& StageExtent) const
{
    return Frontier::PlacementView::Resolve(Flight.Position, Flight.ResolveForward(), Navigation.Up, Flight.ResolveFieldOfView(), StageMin, StageExtent);
}

void EverestWorkspace::ServiceSceneObjectKeys()
{
    // 📝 The Project Zero editor's keys: Ctrl+A Construct, Ctrl+D duplicate, Del remove, F2 rename; never while a field takes the
    //    keys, the camera flies or the dialog is up.
    const ImGuiIO& IO = ImGui::GetIO();
    // 📝 Nor while a modal editor (the Particle editor) has the keys: Delete there must not remove the record behind it.
    if (IO.WantTextInput || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ConstructDialog.QueryOpen() || ImGui::GetTopMostPopupModal() != nullptr) return;
    const Frontier::RecordToken Selected = OutlinerPanel.QuerySelected();
    if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) ConstructDialog.Open();
    else if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && Selected.IsValid()) OutlinerPanel.RequestDuplicateRecord(Selected);
    else if (!IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && Selected.IsValid() &&
             Frontier::SceneDirectory::Instance().QueryMetadata(Selected).RemovableInEditor)
        OutlinerPanel.RequestDeleteRecord(Selected);
    else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && Selected.IsValid()) OutlinerPanel.BeginRename(Selected);
}

void EverestWorkspace::ConstructInterface(double DeltaSeconds)
{
    IntegrateSimulation(DeltaSeconds);
    if (Physics && Physics->QueryRunning())
    {
        IntegrateDrivingControls(DeltaSeconds);
        (void)Physics->Step(QueryWorldSpace(), DeltaSeconds);
        AppendVehicleImpactSparks(*Physics, PendingImpactSparks);
        if (!PresentSceneViewport && StudioCameraEngaged)
        {
            IntegrateChaseCamera(DeltaSeconds);
        }
    }
    // 📝 The replication session advances over the level while the Networking toggle enables it (silent otherwise).
    if (Network) (void)Network->Advance(QueryWorldSpace());
    ViewportDeltaSeconds = DeltaSeconds;
    AdvanceFieldTimeline(DeltaSeconds);

    if (ObservedLayoutRevision != Centre.QueryLayoutResetRevision())
    {
        ObservedLayoutRevision = Centre.QueryLayoutResetRevision();
        Dockspace.ResetLayout();
        (void)Dockspace.ConfigureOpen("ForceFieldWorkspace", false);
        (void)Dockspace.ConfigureOpen("VehicleWorkbench", false);
        Field.CameraPreview = false;
        (void)Dockspace.ConfigureStackedPanels("Viewport", "LogStream", 520.0f);
        (void)Dockspace.ConfigureColumns({
            {"Outliner", 0.200f},
            {"LogStream", 0.545f},
            {"Inspector", 0.255f},
        });
    }

    ServiceDynamicSceneRequests();

    Frontier::EnvironmentEditAccess Access;
    Access.ViewExtent    = LastReadout.ScreenExtent;
    Access.Rendering = LastReadout.Environment;
    Access.CameraReady   = true;
    Access.CameraEye     = Flight.Position;
    Access.CameraForward = Flight.ResolveForward();
    if (auto* Entry = dynamic_cast<Frontier::SolarRecord*>(QueryWorldSpace().FindFirstByType("Solar")))
        Access.Solar = &Entry->AccessConfiguration();
    if (auto* Entry = dynamic_cast<Frontier::AtmosphereRecord*>(QueryWorldSpace().FindFirstByType("Atmosphere")))
        Access.Atmosphere = &Entry->AccessConfiguration();
    if (auto* Entry = dynamic_cast<Frontier::ForceFieldRecord*>(QueryWorldSpace().FindFirstByType("ForceField")))
        Access.Forces = &Entry->AccessConfiguration();
    if (auto* Entry = dynamic_cast<Frontier::WindRecord*>(QueryWorldSpace().FindFirstByType("Wind")))
        Access.Wind = &Entry->AccessConfiguration();
    if (auto* Entry = dynamic_cast<Frontier::LightningRecord*>(QueryWorldSpace().FindFirstByType("Lightning")))
        Access.Lightning = &Entry->AccessConfiguration();
    for (auto* Entry : QueryWorldSpace().FindAllByType("Clouds"))
    {
        auto* Cloud = dynamic_cast<Frontier::CloudRecord*>(QueryWorldSpace().FindRecord(Entry->QueryToken()));
        if (Cloud == nullptr) continue;
        auto& Destination = Cloud->QueryConfiguration().LocalVolume ? Access.LocalCloud : Access.Clouds;
        if (Destination == nullptr) Destination = &Cloud->AccessConfiguration();
    }
    for (auto* Entry : QueryWorldSpace().FindAllByType("Fog"))
    {
        auto* Fog = dynamic_cast<Frontier::FogRecord*>(QueryWorldSpace().FindRecord(Entry->QueryToken()));
        if (Fog == nullptr) continue;
        auto& Destination = Fog->QueryConfiguration().LocalVolume ? Access.LocalFog : Access.Fog;
        if (Destination == nullptr) Destination = &Fog->AccessConfiguration();
    }

    // 📝 The day clock (the demo's AdvanceCelestial) moves the level's sun before anything reads it this frame.
    if (auto* Solar = dynamic_cast<Frontier::SolarRecord*>(QueryWorldSpace().FindFirstByType("Solar")))
    {
        (void)Frontier::AdvanceSolarDay(Solar->AccessConfiguration(), DeltaSeconds);
        Frontier::InspectorRuntime::Instance().PublishSolarElevation(Solar->QueryConfiguration().ElevationDegrees);
    }
    for (const auto* Entry : QueryWorldSpace().FindAllByType("Planet"))
    {
        if (auto* Planet = dynamic_cast<Frontier::PlanetRecord*>(QueryWorldSpace().FindRecord(Entry->QueryToken())))
        {
            const auto Body = Planet->QueryConfiguration().Body;
            if (Body < Access.Planets.size() && Access.Planets[Body] == nullptr)
                Access.Planets[Body] = &Planet->AccessConfiguration();
        }
    }
    if (Access.Solar != nullptr)
    try
    {
        Frontier::GlobalIllumination::EnvironmentSettings Settings;
        Settings.SkyDate = Access.Solar->CalendarDate;
        Settings.Latitude = Access.Solar->LatitudeDegrees;
        Settings.Longitude = Access.Solar->LongitudeDegrees;
        Settings.TimeZone = Access.Solar->UtcOffsetHours;
        Settings.SunTime = Access.Solar->LocalSolarHours;
        const auto Solution = Frontier::GlobalIllumination::SolveCelestial(Settings);
        Access.CelestialReady = true;
        Access.LunarPlacement = {Solution.Moon.Azimuth, Solution.Moon.Elevation, Solution.MoonPhase};
        for (size_t Body = 0; Body < Access.PlanetPlacements.size(); ++Body)
            Access.PlanetPlacements[Body] = {Solution.Planets[Body].Azimuth, Solution.Planets[Body].Elevation};
        for (size_t Body=0;Body<Access.Planets.size();++Body)
            if (auto* Planet=Access.Planets[Body]; Planet && Planet->FollowSky)
            {
                const float Azimuth=float(std::fmod(Solution.Planets[Body].Azimuth+540,360)-180);
                const float Elevation=float(Solution.Planets[Body].Elevation);
                if(Planet->AzimuthDegrees!=Azimuth || Planet->ElevationDegrees!=Elevation)
                {
                    Planet->AzimuthDegrees=Azimuth;Planet->ElevationDegrees=Elevation;Planet->AdvanceRevision();
                }
            }
        for (const auto* Entry : QueryWorldSpace().FindAllByType("Moons"))
        {
            auto* Moons = dynamic_cast<Frontier::MoonRecord*>(QueryWorldSpace().FindRecord(Entry->QueryToken()));
            if (Moons == nullptr) continue;
            auto& Configuration = Moons->AccessConfiguration();
            bool Changed = false;
            for (auto& Moon : Configuration.Instances)
            {
                if (!Moon.FollowSky) continue;
                const float Azimuth = static_cast<float>(std::fmod(Solution.Moon.Azimuth + 540.0, 360.0) - 180.0);
                const float Elevation = static_cast<float>(Solution.Moon.Elevation);
                const float Age = static_cast<float>(Solution.MoonPhase);
                Changed |= Moon.AzimuthDegrees != Azimuth || Moon.ElevationDegrees != Elevation || Moon.PhaseDays != Age;
                Moon.AzimuthDegrees = Azimuth;
                Moon.ElevationDegrees = Elevation;
                Moon.PhaseDays = Age;
            }
            if (Changed) Configuration.AdvanceRevision();
        }
    }
    catch (const std::exception&)
    {
        Access.CelestialReady = false;
    }
    PublishFieldReadings(Access, DeltaSeconds);
    Frontier::InspectorRuntime::Instance().ConfigureEnvironmentAccess(Access);
    ServiceFieldActions();
    // 📝 A page's "Open … ↗" selects the first record of the type it names.
    if (const std::string Requested = Frontier::InspectorRuntime::Instance().ConsumeSelectionRequest(); !Requested.empty())
    {
        Frontier::SceneRecordBase* Target = QueryWorldSpace().FindFirstByType(Requested);
        if (Requested == "LocalCloud" || Requested == "LocalFog")
        {
            for (auto* Candidate : QueryWorldSpace().FindAllByType(Requested == "LocalCloud" ? "Clouds" : "Fog"))
            {
                const auto* Cloud = dynamic_cast<const Frontier::CloudRecord*>(Candidate);
                const auto* Fog   = dynamic_cast<const Frontier::FogRecord*>(Candidate);
                if ((Cloud != nullptr && Cloud->QueryConfiguration().LocalVolume) ||
                    (Fog != nullptr && Fog->QueryConfiguration().LocalVolume))
                {
                    Target = QueryWorldSpace().FindRecord(Candidate->QueryToken());
                    break;
                }
            }
        }
        if (Target != nullptr)
        {
            OutlinerPanel.Select(Target->QueryToken());
        }
    }
    OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    LogPanel.Synchronize(Frontier::LogStore(), Frontier::LogCategories(), DeltaSeconds);

    // 📝 The readout's frame figures: a 0.9 / 0.1 smoothed tick and the last 240 ticks for the 1 % low (IlluminationHost.js).
    if (DeltaSeconds > 0.0)
    {
        FrameSeconds = FrameSeconds <= 0.0 ? DeltaSeconds : FrameSeconds * 0.9 + DeltaSeconds * 0.1;
        FrameDurations[FrameDurationCursor] = static_cast<float>(DeltaSeconds * 1000.0);
        FrameDurationCursor                 = (FrameDurationCursor + 1) % FrameDurations.size();
    }
    NoticeSeconds = std::max(0.0, NoticeSeconds - std::max(0.0, DeltaSeconds));

    const float Scale = Centre.QueryAppearance().QueryApplied().InterfaceScale / 100.0f;
    Frontier::InspectorRuntime::Instance().ConfigureInterfaceScale(Scale);
    // 📝 Ctrl+S saves the level (the save button left with the top toolbar; the Control Centre drawer carries it too).
    const ImGuiIO& IO = ImGui::GetIO();
    if (IO.KeyCtrl && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        SaveActiveLevel();
    }
    // 📝 An open editor window (the Particle editor) owns the keys: nothing behind it is removed or toggled.
    const bool EditorOpen = Frontier::InspectorRuntime::Instance().QueryActiveEditorToken().IsValid();
    if (!EditorOpen) ServiceSceneObjectKeys();
    if (!EditorOpen && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_V,false)) ToggleViewportOverlay(2);
    if (Overlays.Fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape,false)) { Overlays.Fullscreen=false; NoticeSeconds=0; NoticeText.clear(); }
    std::array<float,3> Focus;
    if (Frontier::InspectorRuntime::Instance().ConsumeSkyFocus(Focus))
    {
        Flight.Heading=Focus[0];Flight.Elevation=Focus[1];Flight.VerticalFieldOfView=std::clamp(double(Focus[2])*2.4,3.0,110.0)*3.141592653589793/180;
        Flight.Velocity={0,0,0};Navigation.Mode=SceneCameraMode::Free;Navigation.Up={0,0,1};
    }
    if (Overlays.Fullscreen) ConstructFullscreenViewport(Scale);
    else Dockspace.ConstructInterface(Scale);
    Frontier::InspectorRuntime::Instance().ConstructActiveEditor();
    if (const auto Order = ConstructDialog.Construct(Scale)) ServiceConstructOrder(*Order);
}

void EverestWorkspace::SaveActiveLevel()
{
    // 📝 The level file the workspace opened (Default.levelspace when it started from the project's own default). A running
    //    simulation stops first, so the level keeps the authored transforms, not the simulated ones.
    if (Physics && Physics->QueryRunning()) ToggleSimulation();
    std::string                 SaveDiag;
    const uint32_t              ProjectCategory = EnsureProjectLogCategory("Everest");
    const std::filesystem::path TargetPath      = ActiveLevelPath.empty()
                                                      ? (ProjectRoot / "Content" / "Levels" / "Default.levelspace")
                                                      : ActiveLevelPath;
    if (QueryWorldSpace().SaveLevelSpace(TargetPath, &SaveDiag))
    {
        ActiveLevelPath = TargetPath;
#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)
        Frontier::InspectorRuntime::Instance().ConfigureLevelFile(ActiveLevelPath);
        (void)Frontier::ParticleEditor::SaveSpace();
#endif
        Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Info, Frontier::LogVerbosity::Log,
                                 "EverestWorkspace", "Saved '%s' (%zu records).", TargetPath.filename().string().c_str(),
                                 QueryWorldSpace().QueryRecordCount());
        Frontier::WorldRegister Register;
        Register.Survey(QueryWorldSpace());
        (void)Register.WriteMarkdown(Frontier::WorldRegister::ResolveDocumentPath(TargetPath));
        AnnounceViewportNotice("Saved " + TargetPath.filename().string());
    }
    else
    {
        Diagnostic = SaveDiag;
        Frontier::WriteLogFormat(ProjectCategory, Frontier::LogSeverity::Error, Frontier::LogVerbosity::Log,
                                 "EverestWorkspace", "Save failed: %s", SaveDiag.c_str());
    }
}

void EverestWorkspace::IntegrateViewportCamera(bool Hovered, bool Active, double DeltaSeconds, bool LeftClaimed)
{
    // 📝 Unreal Engine's perspective viewport navigation (IntegrateSceneViewportFlight); this reads one tick of ImGui input.
    //    Called right after the viewport's invisible button, so SetItemKeyOwner claims keys for that button.
    const ImGuiIO&       IO = ImGui::GetIO();
    SceneViewportPointer Pointer;
    Pointer.Hovered      = Hovered;
    Pointer.Active       = Active;
    // 📝 A press on a transform handle belongs to the handle: the camera leaves the left button alone until the release.
    Pointer.LeftDown     = ImGui::IsMouseDown(ImGuiMouseButton_Left) && !LeftClaimed;
    Pointer.RightDown    = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    Pointer.MiddleDown   = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    Pointer.AltDown      = IO.KeyAlt;
    Pointer.ControlDown  = IO.KeyCtrl;
    Pointer.ShiftDown    = IO.KeyShift;
    Pointer.MotionPixels = {static_cast<double>(IO.MouseDelta.x), static_cast<double>(IO.MouseDelta.y)};
    Pointer.WheelNotches = (Hovered || Active) ? static_cast<double>(IO.MouseWheel) : 0.0;

    // 📝 Unreal's keys per flight axis: forward, back, left, right, up and down (world z), up and down (view), widen, narrow.
    static constexpr std::array<std::array<ImGuiKey, 3>, 10> FlightKeyTable = {{
        {ImGuiKey_W, ImGuiKey_UpArrow, ImGuiKey_Keypad8},
        {ImGuiKey_S, ImGuiKey_DownArrow, ImGuiKey_Keypad2},
        {ImGuiKey_A, ImGuiKey_LeftArrow, ImGuiKey_Keypad4},
        {ImGuiKey_D, ImGuiKey_RightArrow, ImGuiKey_Keypad6},
        {ImGuiKey_E, ImGuiKey_PageUp, ImGuiKey_Keypad9},
        {ImGuiKey_Q, ImGuiKey_PageDown, ImGuiKey_Keypad7},
        {ImGuiKey_R, ImGuiKey_None, ImGuiKey_None},
        {ImGuiKey_F, ImGuiKey_None, ImGuiKey_None},
        {ImGuiKey_Z, ImGuiKey_Keypad1, ImGuiKey_None},
        {ImGuiKey_C, ImGuiKey_Keypad3, ImGuiKey_None},
    }};
    // 📝 The wheel never scrolls the panel under the viewport; during a camera drag the viewport also owns the flight keys,
    //    so keyboard navigation leaves the arrows and page keys alone.
    (void)ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool Dragging = Active && (Pointer.LeftDown || Pointer.RightDown || Pointer.MiddleDown);
    for (size_t Axis = 0; Axis < FlightKeyTable.size(); ++Axis)
    {
        for (const ImGuiKey Key : FlightKeyTable[Axis])
        {
            if (Key == ImGuiKey_None) continue;
            if (Dragging) (void)ImGui::SetItemKeyOwner(Key);
            Pointer.FlightKeys[Axis] = Pointer.FlightKeys[Axis] || ImGui::IsKeyDown(Key);
        }
    }
    Pointer.RecentrePress = Hovered && !Pointer.RightDown && ImGui::IsKeyPressed(ImGuiKey_F, false);

    // ⚠️ The first tick after the lock may carry the platform's recentring jump; its motion is dropped.
    if (ConstraintSettleTicks > 0)
    {
        Pointer.MotionPixels = {0.0, 0.0};
        --ConstraintSettleTicks;
    }
    // 📝 Unreal hides and holds the pointer during camera drags and puts it back where the drag began.
    const ImGuiViewport* WindowViewport = ImGui::GetWindowViewport();
    void* const          PlatformWindow = WindowViewport != nullptr ? WindowViewport->PlatformHandle : nullptr;
    if (Dragging && ConstrainedWindow == nullptr && ConstrainPointer && PlatformWindow != nullptr)
    {
        ConstrainPointer(PlatformWindow, true);
        ConstrainedWindow     = PlatformWindow;
        ConstraintSettleTicks = 1;
    }
    else if (!Dragging && ConstrainedWindow != nullptr)
    {
        ReleasePointerConstraint();
    }

    const auto Subject=ResolveCameraSubject();
    const uint64_t Selected=OutlinerPanel.QuerySelected().Value;
    if(!StudioCameraEngaged && Selected!=Navigation.Selected && Navigation.Mode!=SceneCameraMode::Free && Navigation.Mode!=SceneCameraMode::Space)
        ConfigureSceneCamera(Flight,Navigation,Navigation.Mode,Subject);
    Navigation.Selected=Selected;

    const bool WindowFocused = Hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool KeysAvailable = WindowFocused && !IO.WantTextInput && !IO.KeyCtrl && !IO.KeyAlt && !Pointer.RightDown &&
                               !ConstructDialog.QueryOpen() && ImGui::GetTopMostPopupModal() == nullptr;
    const double Aspect      = LastReadout.ScreenExtent[1] > 0u
                                   ? static_cast<double>(LastReadout.ScreenExtent[0]) / static_cast<double>(LastReadout.ScreenExtent[1])
                                   : 16.0 / 9.0;
    const auto VehicleSubject = ResolveVehicleKinematics(Aspect);
    const bool HasGameCamera  = QueryLobbyActive() || VehicleSubject.Available;

    if (WindowFocused && !IO.WantTextInput && !ConstructDialog.QueryOpen() && ImGui::GetTopMostPopupModal() == nullptr &&
        HasGameCamera && ImGui::IsKeyPressed(ImGuiKey_F8, false))
    {
        ConfigureGameCameraMode(!StudioCameraEngaged);
        AnnounceViewportNotice(StudioCameraEngaged
                                   ? ("Vehicle camera: " + std::string(Frontier::QueryVehicleCameraRigLabel(VehicleCamera.QueryRig())) +
                                      " (F8 Editor \xC2\xB7 C cycle \xC2\xB7 M HMI)")
                                   : "Editor camera (F8 Vehicle camera)");
    }

    if (KeysAvailable && HasGameCamera)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_M, false))
        {
            const auto TargetRig = (StudioCameraEngaged && VehicleCamera.QueryRig() == Frontier::VehicleCameraRig::Hmi)
                                       ? Frontier::VehicleCameraRig::Cockpit
                                       : Frontier::VehicleCameraRig::Hmi;
            if (!StudioCameraEngaged) ConfigureGameCameraMode(true);
            VehicleCamera.TransitionToRig(TargetRig);
            AnnounceViewportNotice("Vehicle camera: " + std::string(Frontier::QueryVehicleCameraRigLabel(TargetRig)) + " (M)");
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_C, false))
        {
            if (!StudioCameraEngaged)
            {
                ConfigureGameCameraMode(true);
            }
            else
            {
                VehicleCamera.CycleRig(IO.KeyShift);
            }
            AnnounceViewportNotice((QueryLobbyActive() ? "Studio vehicle camera: " : "Vehicle camera: ") +
                                   std::string(Frontier::QueryVehicleCameraRigLabel(VehicleCamera.QueryRig())) + " (C)");
        }
    }

    if (QueryLobbyActive() && KeysAvailable)
    {
        for (int SlotKey = 0; SlotKey < 5; ++SlotKey)
        {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + SlotKey), false))
            {
                Lobby.ConfigureTargetDriverCount(QueryWorldSpace(), static_cast<uint32_t>(SlotKey + 1));
                OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
                AnnounceViewportNotice(std::to_string(Lobby.QueryActiveVehicleCount()) + " / 5 Lobby vehicles active");
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !VehicleCamera.QueryRouteTransitionActive())
        {
            BeginLevelTransition(ProjectRoot / "Content" / "Levels" / "ProvingGroundVehicle.levelspace", true);
        }
    }

    if (StudioCameraEngaged && Active && Pointer.RightDown)
    {
        ConfigureGameCameraMode(false);
        AnnounceViewportNotice("Editor camera (F8 Vehicle camera)");
    }

    if (StudioCameraEngaged && HasGameCamera)
    {
        Frontier::VehicleCameraInput CameraInput{};
        CameraInput.PointerDeltaPixels = Pointer.MotionPixels;
        CameraInput.WheelNotches       = Pointer.WheelNotches;
        CameraInput.PrimaryDrag        = Active && Pointer.LeftDown;
        CameraInput.AnyActivity        = CameraInput.PrimaryDrag || std::abs(CameraInput.WheelNotches) > 0.0;

        const auto Solution = VehicleCamera.Advance(VehicleSubject, CameraInput, DeltaSeconds);
        ApplyVehicleCameraSolution(Solution);
        if (VehicleCamera.ConsumeRouteSwitchPoint() && !PendingTransitionLevel.empty())
        {
            const std::filesystem::path Destination = std::exchange(PendingTransitionLevel, {});
            (void)OpenLevel(Destination, PendingTransitionDrive);
        }
        return;
    }

    IntegrateSceneCamera(Flight,Navigation,Pointer,Subject,DeltaSeconds);

    if (VehicleCamera.QueryRouteTransitionActive())
    {
        const auto FadeSolution = VehicleCamera.Advance(VehicleSubject, Frontier::VehicleCameraInput{}, DeltaSeconds);
        ActiveVeilOpacity       = FadeSolution.VeilOpacity;
        if (VehicleCamera.ConsumeRouteSwitchPoint() && !PendingTransitionLevel.empty())
        {
            const std::filesystem::path Destination = std::exchange(PendingTransitionLevel, {});
            (void)OpenLevel(Destination, PendingTransitionDrive);
        }
    }
    else
    {
        ActiveVeilOpacity = 0.0;
    }
}

void EverestWorkspace::ReleasePointerConstraint()
{
    if (ConstrainedWindow != nullptr && ConstrainPointer) ConstrainPointer(ConstrainedWindow, false);
    ConstrainedWindow = nullptr;
}

void EverestWorkspace::ConstructViewportSurface()
{
    // 📝 The demo's viewport column (index.html #viewport-column): the viewport bar above the stage; on the stage the
    //    presented image, then the overlays (bring-up, failure, distance slice, readout, notice).
    const float  Scale = Centre.QueryAppearance().QueryApplied().InterfaceScale / 100.0f;
    const ImVec2 Avail = ImGui::GetContentRegionAvail();
    const float  BarH  = Frontier::ViewportChromeControls::BarHeight * Scale;
    const float  W     = std::max(64.0f, Avail.x);
    const float  H     = std::max(64.0f + BarH, Avail.y);
    const ImVec2 Min   = ImGui::GetCursorScreenPos();
    const ImVec2 Max(Min.x + W, Min.y + H);
    const ImVec2 StageMin(Min.x, Min.y + BarH);
    if (!Dockspace.QueryOpen("ForceFieldWorkspace")) Field.CameraPreview = false;

    // 📝 When the dockable Force Field workspace is open, its own canvas hosts the slice and timeline; keep this viewport on the scene.
    if (Field.Shown && !Dockspace.QueryOpen("ForceFieldWorkspace"))
    {
        ReleasePointerConstraint();
        ConstructFieldViewport(StageMin, Max, Scale);
        ConstructViewportBar(Min, ImVec2(Max.x, StageMin.y), Scale);
        return;
    }

    ImGui::SetCursorScreenPos(StageMin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##SceneViewportCanvas", ImVec2(W, H - BarH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool StageHovered = ImGui::IsItemHovered();
    const bool StageActive  = ImGui::IsItemActive();
    const ImGuiIO& IO = ImGui::GetIO();

    // 📝 The selected scene object's transform handles: a left press on one drags it, a press elsewhere that is released
    //    without moving picks the object under the pointer (Project Zero's viewport selection).
    const ImVec2                  StageExtent(W, H - BarH);
    const Frontier::PlacementView View = ResolvePlacementView(StageMin, StageExtent);
    Frontier::GeometryConfiguration* EditedGeometry = StudioCameraEngaged ? nullptr : ResolveEditedGeometry();
    Frontier::VehicleConfiguration* EditedVehicle  = nullptr;
    if (auto* SelectedVehicle = dynamic_cast<Frontier::VehicleRecord*>(QueryWorldSpace().FindRecord(OutlinerPanel.QuerySelected())))
    {
        Frontier::VehicleConfiguration& Configuration = SelectedVehicle->AccessConfiguration();
        // 📝 Physics owns the live vehicle pose while running. Author the spawn transform only with simulation stopped;
        //    applying gizmo deltas during a live step would be overwritten by the next physics snapshot.
        if (Configuration.Visible && (!Physics || !Physics->QueryRunning())) EditedVehicle = &Configuration;
    }
    Frontier::SpatialConfiguration* EditedSpatial = EditedGeometry != nullptr ? &EditedGeometry->Spatial
                                                         : EditedVehicle != nullptr ? &EditedVehicle->Spatial
                                                                                    : nullptr;
    if (StageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !IO.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        GizmoPress  = EditedSpatial != nullptr && Gizmo.Probe(View, GizmoMode, *EditedSpatial, IO.MousePos, Scale) >= 0;
        PickPending = !GizmoPress && !StudioCameraEngaged;
        PickPress   = IO.MousePos;
    }
    IntegrateViewportCamera(StageHovered, StageActive, ViewportDeltaSeconds, GizmoPress || Gizmo.QueryEngaged());

    // 📝 The demo's overlay keys, while the viewport has the pointer or the focus and no text field takes the keys; W, E and R
    //    choose the transform handles (Unreal's keys) while the camera is in Editor mode and not flying.
    const bool ModalEditor = ImGui::GetTopMostPopupModal() != nullptr;
    if ((StageHovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) && !IO.WantTextInput && !StageActive && !ModalEditor)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) ToggleViewportOverlay(0);
        if (IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_S, false)) ToggleSimulation();
    }
    if ((StageHovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) && !IO.WantTextInput && !IO.KeyCtrl &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ModalEditor && (!StudioCameraEngaged || EditedVehicle != nullptr))
    {
        const std::array<std::pair<ImGuiKey, Frontier::PlacementGizmoMode>, 3> ModeKeys = {{{ImGuiKey_W, Frontier::PlacementGizmoMode::Move},
                                                                                            {ImGuiKey_E, Frontier::PlacementGizmoMode::Rotate},
                                                                                            {ImGuiKey_R, Frontier::PlacementGizmoMode::Scale}}};
        for (const auto& [Key, Mode] : ModeKeys)
        {
            if (!ImGui::IsKeyPressed(Key, false) || GizmoMode == Mode) continue;
            GizmoMode = Mode;
            AnnounceViewportNotice(Mode == Frontier::PlacementGizmoMode::Move ? "Move (W)" : Mode == Frontier::PlacementGizmoMode::Rotate ? "Rotate (E)" : "Scale (R)");
        }
    }

    ImDrawList* DrawList = ImGui::GetWindowDrawList();
    DrawList->PushClipRect(Min, Max, true);

    // 📝 The host renders the level's scene through its records; the schematic stands in until an image exists.
    SceneViewportImage Image;
    if (PresentSceneViewport)
    {
        SceneViewportRequest Request;
        Request.PanelWidth          = W;
        Request.PanelHeight         = H - BarH;
        Request.PixelDensity        = ImGui::GetWindowViewport() != nullptr ? ImGui::GetWindowViewport()->FramebufferScale.y : 1.0f;
        Request.CameraPosition      = Flight.Position;
        Request.CameraTarget        = Flight.ResolveTarget();
        Request.VerticalFieldOfView = Flight.ResolveFieldOfView();
        Request.CameraNear          = ActiveNearClipMetres;
        Request.DeltaSeconds        = ViewportDeltaSeconds;
        Request.CameraUp = Navigation.Up;
        Request.CameraMotionBlur = StudioCameraEngaged ? VehicleCamera.QueryMotionBlur() : EditorCameraMotionBlur;
        Request.PickSerial          = PickSerial;
        Request.PickPoint           = PickPoint;
        Request.ImpactSparks        = std::exchange(PendingImpactSparks, {});
        Image = PresentSceneViewport(Request);
        LastReadout = Image.Readout;
        // 📝 GPU picking answers a click one interface tick later, from the image the click was made on; a click on the sky or on
        //    nothing a record owns leaves the selection as it is.
        if (LastReadout.Pick.Serial != 0 && LastReadout.Pick.Serial == PickSerial && PickApplied != PickSerial)
        {
            PickApplied = PickSerial;
            if (LastReadout.Pick.Record)
            {
                OutlinerPanel.Select(Frontier::RecordToken{*LastReadout.Pick.Record});
                OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
            }
            else if (!LastReadout.Pick.Failure.empty())
            {
                Frontier::WriteLogFormat(EnsureProjectLogCategory("Everest"), Frontier::LogSeverity::Warning, Frontier::LogVerbosity::Log,
                                         "SceneViewport", "Viewport pick: %s", LastReadout.Pick.Failure.c_str());
            }
        }
        PublishGeometryResolutions(QueryWorldSpace(), LastReadout, PublishedGeometryGeneration);
        if (PendingAutoSimulation && PublishedGeometryGeneration > 0)
        {
            PendingAutoSimulation = false;
            if (!Physics || !Physics->QueryRunning()) ToggleSimulation();
        }
        ApplyPendingSurfaces();
        PublishRenderReadouts(QueryWorldSpace(), LastReadout);
    }
    if (Image.Texture != 0)
    {
        DrawList->AddImage(static_cast<ImTextureID>(Image.Texture), StageMin, Max);
        const auto* FieldRecord = dynamic_cast<const Frontier::ForceFieldRecord*>(QueryWorldSpace().FindFirstByType("ForceField"));
        if (FieldRecord && FieldRecord->QueryConfiguration().Visualize && !Overlays.Fullscreen && LastReadout.Environment)
        {
            Frontier::WorldVectorView Projection;
            Projection.Eye = Flight.Position;
            Projection.Forward = Flight.ResolveForward();
            const auto& F=Projection.Forward; const auto& U=Navigation.Up;
            Projection.Right={F[1]*U[2]-F[2]*U[1],F[2]*U[0]-F[0]*U[2],F[0]*U[1]-F[1]*U[0]};
            const double Length=std::max(1e-8,std::hypot(Projection.Right[0],Projection.Right[1],Projection.Right[2]));
            for (double& Axis:Projection.Right) Axis/=Length;
            const auto& R=Projection.Right;
            Projection.Up={R[1]*F[2]-R[2]*F[1],R[2]*F[0]-R[0]*F[2],R[0]*F[1]-R[1]*F[0]};
            Projection.VerticalFieldOfView = Flight.ResolveFieldOfView();
            Frontier::WorldVectorOverlay::Construct(DrawList,StageMin,{W,H-BarH},Projection,
                LastReadout.Environment->Vectors,FieldRecord->QueryConfiguration().ArrowSeconds,Scale);
            DrawList->AddText({StageMin.x + 12 * Scale,StageMin.y + 12 * Scale},IM_COL32(160,220,236,230),
                "Atmospheric flow / m/s / X-ray vectors");
        }
        // 📝 The Force Field's Scene overlay: the slice in this camera's perspective over the image.
        if (!Overlays.Fullscreen) ConstructFieldOverlay(StageMin, Max, Scale);
    }
    else
    {
        ConstructViewportSchematic(DrawList, StageMin, Max);
    }
    if (EditedSpatial != nullptr && Image.Texture != 0)
    {
        const Frontier::PlacementGizmoOutcome Outcome = Gizmo.Construct(DrawList, View, GizmoMode, *EditedSpatial, IO.KeyCtrl, GizmoPress, Scale);
        if (Outcome.Changed)
        {
            if (EditedGeometry != nullptr) EditedGeometry->AdvanceRevision();
            else if (EditedVehicle != nullptr) EditedVehicle->AdvanceRevision();
        }
        if (Outcome.Engaged && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            Gizmo.Cancel(*EditedSpatial);
            if (EditedGeometry != nullptr) EditedGeometry->AdvanceRevision();
            else if (EditedVehicle != nullptr) EditedVehicle->AdvanceRevision();
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        const float Travel = std::hypot(IO.MousePos.x - PickPress.x, IO.MousePos.y - PickPress.y);
        // 📝 A click asks the viewport which record is drawn under the pointer (GPU picking against the shown triangles).
        if (PickPending && Travel < 5.0f * Scale && LastReadout.Presenting && StageExtent.x > 0.0f && StageExtent.y > 0.0f)
        {
            ++PickSerial;
            PickPoint = {static_cast<double>((IO.MousePos.x - StageMin.x) / StageExtent.x), static_cast<double>((IO.MousePos.y - StageMin.y) / StageExtent.y)};
        }
        PickPending = false;
        GizmoPress  = false;
    }
    ConstructViewportOverlays(StageMin, Max, Image, Scale);
    DrawList->PopClipRect();

    ConstructViewportBar(Min, ImVec2(Max.x, StageMin.y), Scale);
}

SceneCameraSubject EverestWorkspace::ResolveCameraSubject() const
{
    SceneCameraSubject Subject;
    const auto* Inspector=Frontier::InspectorRuntime::Instance().FindInspector(OutlinerPanel.QuerySelected());
    Frontier::InspectorViewTarget Target;
    if (Inspector && Inspector->QueryViewTarget(Target))
    {
        Subject.Available=true;Subject.Celestial=Target.Celestial;Subject.Position=Target.Position;Subject.Radius=Target.Radius;
        Subject.Azimuth=Target.Azimuth;Subject.Elevation=Target.Elevation;Subject.Diameter=Target.Diameter;
    }
    return Subject;
}

void EverestWorkspace::ConstructViewportBar(const ImVec2& Min,const ImVec2& Max,float Scale)
{
    using Frontier::ViewportChromeControls;
    const auto Faces=Frontier::EditorTypefaces::Resolve();
    auto* Ink=ImGui::GetWindowDrawList();
    ViewportChromeControls::ConstructBarBackground(Ink,Min,Max,Scale);
    Ink->PushClipRect(Min,Max,true);
    if (Field.Shown && !Dockspace.QueryOpen("ForceFieldWorkspace"))
    {
        ConstructFieldBar(Min,Max,Scale);
        Ink->PopClipRect();
        return;
    }
    const float Height=Max.y-Min.y, Button=34*Scale, Gap=8*Scale;
    float X=Min.x+12*Scale;
    const bool HasGameCamera = QueryLobbyActive() || ResolveVehicleKinematics().Available;
    if (HasGameCamera)
    {
        static constexpr const char* CameraDomainLabels[] = {"Editor", "Vehicle"};
        int DomainIndex = StudioCameraEngaged ? 1 : 0;
        if (ViewportChromeControls::ConstructSegmentedSwitch("##CameraSource", Faces, {X, Min.y + (Height - Button) * .5f},
                                                             {124 * Scale, Button}, CameraDomainLabels, 2, DomainIndex, nullptr, nullptr, Scale))
        {
            ConfigureGameCameraMode(DomainIndex == 1);
            AnnounceViewportNotice(StudioCameraEngaged
                                       ? ("Vehicle camera: " + std::string(Frontier::QueryVehicleCameraRigLabel(VehicleCamera.QueryRig())) +
                                          " (F8 Editor \xC2\xB7 C cycle \xC2\xB7 M HMI)")
                                       : "Editor camera (F8 Vehicle camera)");
        }
        X += 124 * Scale + Gap;
    }

    if (StudioCameraEngaged && HasGameCamera)
    {
        static constexpr std::array<const char*, 8> OutdoorRigNames = {
            "Chase", "Orbit", "Cockpit", "HMI Tablet", "Passenger", "Hood", "Tyre FL", "Tyre RL"};
        static constexpr std::array<const char*, 3> StudioRigNames = {"Orbit", "Cockpit", "HMI Tablet"};
        static constexpr std::array<Frontier::VehicleCameraRig, 3> StudioRigValues = {
            Frontier::VehicleCameraRig::Orbit, Frontier::VehicleCameraRig::Cockpit, Frontier::VehicleCameraRig::Hmi};

        const float SelectW = std::min(168 * Scale, (Max.x - Min.x) * .30f);
        if (QueryLobbyActive())
        {
            int SelectedRig = 0;
            for (size_t Idx = 0; Idx < StudioRigValues.size(); ++Idx)
                if (StudioRigValues[Idx] == VehicleCamera.QueryRig()) SelectedRig = static_cast<int>(Idx);
            if (ViewportChromeControls::ConstructSelectField("##GameCameraRig", Faces, {X, Min.y + (Height - Button) * .5f},
                                                             {SelectW, Button}, StudioRigNames.data(), 3, SelectedRig, Scale))
            {
                VehicleCamera.TransitionToRig(StudioRigValues[static_cast<size_t>(std::clamp(SelectedRig, 0, 2))]);
            }
        }
        else
        {
            int SelectedRig = static_cast<int>(VehicleCamera.QueryRig());
            if (ViewportChromeControls::ConstructSelectField("##GameCameraRig", Faces, {X, Min.y + (Height - Button) * .5f},
                                                             {SelectW, Button}, OutdoorRigNames.data(), 8, SelectedRig, Scale))
            {
                VehicleCamera.TransitionToRig(static_cast<Frontier::VehicleCameraRig>(std::clamp(SelectedRig, 0, 7)));
            }
        }
        X += SelectW + Gap;
        const bool OnHmi = VehicleCamera.QueryRig() == Frontier::VehicleCameraRig::Hmi;
        if (ViewportChromeControls::ConstructIconButton("##HmiFocus", {X, Min.y + (Height - Button) * .5f}, Button, "Focus", OnHmi, true,
                                                        "Focus HMI tablet / Cockpit (M)"))
        {
            VehicleCamera.TransitionToRig(OnHmi ? Frontier::VehicleCameraRig::Cockpit : Frontier::VehicleCameraRig::Hmi);
        }
        X += Button + Gap;
        if (ViewportChromeControls::ConstructIconButton("##ResetGameRig", {X, Min.y + (Height - Button) * .5f}, Button, "RotateCcw", false, true,
                                                        "Reset active Vehicle camera rig"))
        {
            VehicleCamera.ConfigureRigImmediate(
                QueryLobbyActive() ? Frontier::VehicleCameraRig::Orbit : VehicleCamera.QueryRig(), ResolveVehicleKinematics());
            ApplyVehicleCameraSolution(VehicleCamera.QueryLastSolution());
        }
        X += Button + Gap;
    }
    else
    {
        const float SelectW = std::min((HasGameCamera ? 176 : 210) * Scale, (Max.x - Min.x) * .36f);
        int Mode = int(Navigation.Mode);
        if (ViewportChromeControls::ConstructSelectField("##CameraType", Faces, {X, Min.y + (Height - Button) * .5f}, {SelectW, Button},
                                                         SceneCameraNames.data(), int(SceneCameraNames.size()), Mode, Scale))
        {
            const auto Subject = ResolveCameraSubject();
            ConfigureSceneCamera(Flight, Navigation, SceneCameraMode(Mode), Subject);
            if ((Mode == 0 || Mode == 1) && !Subject.Available)
                AnnounceViewportNotice("Select a scene object, local volume, field region or celestial body to target");
        }
        X += SelectW + Gap;
        if (ViewportChromeControls::ConstructIconButton("##FrameSelection", {X, Min.y + (Height - Button) * .5f}, Button, "Focus", false, true,
                                                        "Frame selected (F)"))
            ConfigureSceneCamera(Flight, Navigation, Navigation.Mode, ResolveCameraSubject(), true);
        X += Button + Gap;
        if (ViewportChromeControls::ConstructIconButton("##HomeCamera", {X, Min.y + (Height - Button) * .5f}, Button, "RotateCcw", false, true,
                                                        "Return to the scene camera"))
        {
            Flight = SceneViewportFlight{};
            Navigation = SceneViewportNavigation{};
        }
        X += Button + Gap;
        // 📝 Outdoor levels can start in Editor-camera mode, where the Game-camera rig picker is otherwise hidden.
        //    Keep the authored HMI tablet view one click away rather than inventing a per-level enable flag.
        if (HasGameCamera && ViewportChromeControls::ConstructIconButton("##QuickHmiRig", {X, Min.y + (Height - Button) * .5f}, Button,
                                                                         "Focus", VehicleCamera.QueryRig() == Frontier::VehicleCameraRig::Hmi,
                                                                         true, "Open the vehicle HMI tablet camera"))
        {
            ConfigureGameCameraMode(true);
            VehicleCamera.TransitionToRig(Frontier::VehicleCameraRig::Hmi);
            AnnounceViewportNotice("HMI Tablet camera (M)");
        }
        if (HasGameCamera) X += Button + Gap;
    }
    // 📝 Camera effects stay attached to the active camera: exactly Off / Camera / Full-scene blur choices plus a bounded seeded
    //    millimetre-scale shake component on the driven vehicle rigs.
    const float EffectsRightReserve = 12.0f * Scale + 3.0f * Button + 4.0f * Gap + 132.0f * Scale;
    if (X + Button + Gap < Max.x - EffectsRightReserve &&
        ViewportChromeControls::ConstructIconButton("##CameraEffects", {X, Min.y + (Height - Button) * .5f}, Button, "SlidersHorizontal", false, true,
                                                     "Camera effects: motion blur and subtle vehicle shake"))
    {
        ImGui::OpenPopup("##CameraEffectsPopup");
    }
    if (X + Button + Gap < Max.x - EffectsRightReserve) X += Button + Gap;
    if (ImGui::BeginPopup("##CameraEffectsPopup"))
    {
        Frontier::CameraMotionBlurComponent& Blur = StudioCameraEngaged ? VehicleCamera.AccessMotionBlur() : EditorCameraMotionBlur;
        static constexpr const char* BlurModes[] = {"Off", "Camera only", "Full scene (camera + objects)"};
        int BlurMode = std::clamp(static_cast<int>(Blur.Mode), 0, 2);
        if (ImGui::Combo("Motion blur", &BlurMode, BlurModes, 3)) Blur.Mode = static_cast<Frontier::CameraMotionBlurMode>(BlurMode);
        float BlurStrength = Blur.Strength;
        if (ImGui::SliderFloat("Blur strength", &BlurStrength, 0.0f, 1.0f, "%.2f")) Blur.Strength = BlurStrength;
        ImGui::TextDisabled("Blur applies in the normal GI view; inspection and analytic views stay clean.");

        ImGui::Separator();
        if (HasGameCamera)
        {
            ImGui::TextUnformatted("Vehicle camera shake");
            Frontier::CameraShakeSettings& Shake = VehicleCamera.AccessCameraShake().AccessSettings();
            ImGui::Checkbox("Enabled##VehicleCameraShake", &Shake.Enabled);
            ImGui::BeginDisabled(!Shake.Enabled);
            float AmplitudeMillimetres = Shake.AmplitudeMetres * 1000.0f;
            if (ImGui::SliderFloat("Amplitude", &AmplitudeMillimetres, 0.0f, 6.0f, "%.1f mm"))
                Shake.AmplitudeMetres = AmplitudeMillimetres * 0.001f;
            ImGui::SliderFloat("Frequency", &Shake.FrequencyHz, 2.0f, 30.0f, "%.1f Hz");
            ImGui::SliderFloat("Attack", &Shake.AttackSeconds, 0.01f, 0.25f, "%.2f s");
            ImGui::SliderFloat("Release", &Shake.ReleaseSeconds, 0.05f, 0.75f, "%.2f s");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Seeded response; default offset is capped at 6 mm.");
        }
        else
        {
            ImGui::TextDisabled("Vehicle shake appears when a vehicle camera is available.");
        }
        ImGui::EndPopup();
    }

    // 📝 Debug views (Render settings › Debug views): the demo's inspection views and the analytic illumination as a view of its own.
    if (auto* Pipeline=dynamic_cast<Frontier::RenderPipelineRecord*>(QueryWorldSpace().FindFirstByType("RenderPipeline")))
    {
        using Frontier::RenderInspectionModes;
        static const auto ViewNames=[]
        {
            std::array<const char*,RenderInspectionModes::ViewCount> Names{};
            for (size_t Index=0;Index<Names.size();++Index) Names[Index]=RenderInspectionModes::Views[Index].Name;
            return Names;
        }();
        auto& Settings=Pipeline->AccessConfiguration();
        int View=Settings.AnalyticIllumination?RenderInspectionModes::AnalyticView:std::clamp(Settings.InspectionModeIndex,0,14);
        const float Wide=std::min(236*Scale,(Max.x-Min.x)*.30f);
        if (X+Wide<Max.x-12*Scale-3*Button-4*Gap-132*Scale &&
            ViewportChromeControls::ConstructSelectField("##DebugView",Faces,{X,Min.y+(Height-Button)*.5f},{Wide,Button},ViewNames.data(),int(ViewNames.size()),View,Scale))
        {
            Settings.AnalyticIllumination=View==RenderInspectionModes::AnalyticView;
            if (!Settings.AnalyticIllumination) Settings.InspectionModeIndex=View;
            Settings.AdvanceRevision();
        }
    }
    const float Right=Max.x-12*Scale;
    // 📝 Scene | Field swaps this viewport unless the dockable Force Field workspace already owns the slice.
    static constexpr const char* SceneFieldLabels[]={"Scene","Field"};
    const ImVec2 ViewModePos(Right-2*Button-2*Gap-132*Scale,Min.y+(Height-Button)*.5f);
    if (!Dockspace.QueryOpen("ForceFieldWorkspace"))
    {
        int Shown = Field.Shown ? 1 : 0;
        if (ViewportChromeControls::ConstructSegmentedSwitch("##SceneField",Faces,ViewModePos,{132*Scale,Button},SceneFieldLabels,2,Shown,nullptr,nullptr,Scale))
            Field.Shown=Shown==1;
    }
    else
    {
        Ink->AddRectFilled(ViewModePos, ImVec2(ViewModePos.x+132*Scale,ViewModePos.y+Button), IM_COL32(0,194,168,24), 10*Scale);
        Ink->AddRect(ViewModePos, ImVec2(ViewModePos.x+132*Scale,ViewModePos.y+Button), IM_COL32(0,194,168,90), 10*Scale, 0, Scale);
        Ink->AddText(Faces.Medium, 10.0f*Scale, ImVec2(ViewModePos.x+12*Scale,ViewModePos.y+10*Scale), IM_COL32(140,225,206,255), "FIELD WORKSPACE");
    }
    const bool Simulating=Physics && Physics->QueryRunning();
    if (ViewportChromeControls::ConstructIconButton("##Simulate",{Right-3*Button-3*Gap-132*Scale,Min.y+(Height-Button)*.5f},Button,Simulating?"Pause":"Play",Simulating,true,
        Simulating?"Stop simulating and restore the objects (Alt+S)":"Simulate: objects with Physics on fall and collide (Alt+S)")) ToggleSimulation();
    if (ViewportChromeControls::ConstructIconButton("##Readout",{Right-2*Button-Gap,Min.y+(Height-Button)*.5f},Button,"Activity",Overlays.ReadoutVisible,true,"Detailed readout (H)")) ToggleViewportOverlay(0);
    if (ViewportChromeControls::ConstructIconButton("##Fullscreen",{Right-Button,Min.y+(Height-Button)*.5f},Button,"ScanCorners",Overlays.Fullscreen,true,"Fullscreen viewport (V), Escape restores panels")) ToggleViewportOverlay(2);
    Ink->PopClipRect();
}

void EverestWorkspace::ConstructViewportOverlays(const ImVec2& StageMin, const ImVec2& StageMax, const SceneViewportImage& Image, float Scale)
{
    using Frontier::ViewportChromeControls;
    const Frontier::EditorTypefaces Faces    = Frontier::EditorTypefaces::Resolve();
    ImDrawList*                     DrawList = ImGui::GetWindowDrawList();
    const SceneViewportReadout&     Readout  = Image.Readout;

    // ① #failure, or #bring-up while the first image is still to come
    if (!Readout.Failure.empty())
    {
        ViewportChromeControls::ConstructFailurePanel(DrawList, Faces, StageMin, StageMax, Readout.Failure, Scale);
    }
    else if (PresentSceneViewport && Image.Texture == 0)
    {
        ViewportChromeControls::ConstructBringUpCard(DrawList, Faces, StageMin, StageMax, "Opening viewport",
                                                     Image.Status.empty() ? "Starting" : Image.Status.c_str(), 0.0f, Scale);
    }
    const bool Overlaid = !Overlays.Fullscreen && Image.Texture != 0;
    if (PresentSceneViewport)
    {
        ConstructSessionViewportOverlay(StageMin, StageMax, Scale);
    }

    // ③ #hud: the readout lines of the demo's PresentMeasurements, with this viewport's camera keys
    if (Overlaid && Overlays.ReadoutVisible)
    {
        std::vector<float> Durations;
        for (const float Duration : FrameDurations)
            if (Duration > 0.0f) Durations.push_back(Duration);
        std::sort(Durations.begin(), Durations.end());
        const float Slowest = Durations.empty() ? 0.0f : Durations[std::min(Durations.size() - 1, static_cast<size_t>(0.99 * static_cast<double>(Durations.size())))];
        char        Low[16];
        if (Slowest > 0.0f) std::snprintf(Low, sizeof(Low), "%.0f", 1000.0 / static_cast<double>(Slowest));
        else std::snprintf(Low, sizeof(Low), "\xE2\x80\x93");
        char First[256];
        std::snprintf(First, sizeof(First), "%.0f fps \xC2\xB7 %.1f ms \xC2\xB7 1%% low %s fps \xC2\xB7 %u\xC3\x97%u \xC2\xB7 trace %u\xC3\x97%u \xC2\xB7 %s",
                      1.0 / std::max(FrameSeconds, 1.0e-4), FrameSeconds * 1000.0, Low, Readout.ScreenExtent[0], Readout.ScreenExtent[1],
                      Readout.TraceExtent[0], Readout.TraceExtent[1], Readout.RendererTag.c_str());
        std::vector<std::string> Lines;
        Lines.emplace_back(First);
        if (!Readout.AccelerationLine.empty()) Lines.push_back(Readout.AccelerationLine);
        char Camera[160];
        std::snprintf(Camera, sizeof(Camera), "Camera  speed %d (%g\xC3\x97) \xC2\xB7 field of view %.0f\xC2\xB0", Flight.SpeedSetting,
                      SceneViewportFlight::ResolveSpeed(Flight.SpeedSetting), Flight.ResolveFieldOfView() * 57.29577951308232);
        Lines.emplace_back(Camera);
        Lines.emplace_back("RMB look  RMB+W/A/S/D fly  E/Q up\xC2\xB7" "down  RMB+wheel speed  Shift \xC3\x97" "2  Orbit selected / F frame selected");
        Lines.emplace_back("H readout  V fullscreen  Esc restore panels");
        if (ViewportChromeControls::ConstructReadoutOverlay("##Readout", Faces, StageMin, StageMax, Lines, "Hide the readout (H)", Scale))
        {
            ToggleViewportOverlay(0);
        }
    }

    // ④ #overlay-notice: transient lines (camera speed, clean view)
    if (AnnouncedSpeedSetting != Flight.SpeedSetting)
    {
        AnnouncedSpeedSetting = Flight.SpeedSetting;
        char Speed[64];
        std::snprintf(Speed, sizeof(Speed), "Camera speed %d (%g\xC3\x97)", Flight.SpeedSetting, SceneViewportFlight::ResolveSpeed(Flight.SpeedSetting));
        AnnounceViewportNotice(Speed);
    }
    if (NoticeSeconds > 0.0 && !NoticeText.empty())
    {
        ViewportChromeControls::ConstructOverlayNotice(DrawList, Faces, StageMin, StageMax, NoticeText.c_str(), Scale);
    }

    // ⑤ Cinematic / route transition fade veil across the viewport stage.
    if (ActiveVeilOpacity > 0.002)
    {
        const int Alpha = std::clamp(static_cast<int>(std::lround(ActiveVeilOpacity * 255.0)), 0, 255);
        DrawList->AddRectFilled(StageMin, StageMax, IM_COL32(6, 8, 12, Alpha));
    }
}

void EverestWorkspace::ConstructSessionViewportOverlay(const ImVec2& StageMin, const ImVec2& StageMax, float Scale)
{
    if (Overlays.Fullscreen)
    {
        return;
    }

    const float StageWidth  = StageMax.x - StageMin.x;
    const float StageHeight = StageMax.y - StageMin.y;
    if (StageWidth < 260.0f * Scale || StageHeight < 180.0f * Scale)
    {
        return;
    }

    ImDrawList* DrawList = ImGui::GetWindowDrawList();

    // Outdoor driving strip (Proving Grounds): quick return to Studio Lobby + Game/Editor mode + 8-rig VehicleHMI selector + live speed.
    if (!QueryLobbyActive())
    {
        const auto Subject = ResolveVehicleKinematics();
        if (!Subject.Available || (!StudioCameraEngaged && !QueryVehicleDrivingActive()))
        {
            return;
        }
        const ImVec2 StripMin(StageMin.x + 12.0f * Scale, StageMin.y + 12.0f * Scale);
        ImGui::SetCursorScreenPos(StripMin);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * Scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * Scale, 5.0f * Scale));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(18, 24, 34, 220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(34, 48, 68, 240));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(48, 68, 96, 255));
        if (ImGui::Button("Studio Lobby (L)##ReturnToLobby") && !VehicleCamera.QueryRouteTransitionActive())
        {
            BeginLevelTransition(ProjectRoot / "Content" / "Levels" / "Lobby.levelspace", false);
        }
        ImGui::SameLine(0.0f, 6.0f * Scale);
        const std::string ModeLabel = StudioCameraEngaged ? "Vehicle Cam (F8)##ToggleCamMode" : "Editor Cam (F8)##ToggleCamMode";
        if (ImGui::Button(ModeLabel.c_str()))
        {
            ConfigureGameCameraMode(!StudioCameraEngaged);
        }
        ImGui::SameLine(0.0f, 6.0f * Scale);
        const std::string RigLabel = "Rig: " + std::string(Frontier::QueryVehicleCameraRigLabel(VehicleCamera.QueryRig())) + " (C)##DriveRig";
        if (ImGui::Button(RigLabel.c_str()))
        {
            if (!StudioCameraEngaged) ConfigureGameCameraMode(true);
            else VehicleCamera.CycleRig(false);
        }
        ImGui::SameLine(0.0f, 5.0f * Scale);
        if (ImGui::Button("HMI (M)##QuickHmi"))
        {
            const auto TargetRig = (StudioCameraEngaged && VehicleCamera.QueryRig() == Frontier::VehicleCameraRig::Hmi)
                                       ? Frontier::VehicleCameraRig::Cockpit
                                       : Frontier::VehicleCameraRig::Hmi;
            if (!StudioCameraEngaged) ConfigureGameCameraMode(true);
            VehicleCamera.TransitionToRig(TargetRig);
        }
        static constexpr std::array<const char*, 2> QuickStations = {"Straight##PgSt0", "Slope Ramp 12.7\xC2\xB0##PgSt1"};
        for (uint32_t StationIdx = 0u; StationIdx < QuickStations.size(); ++StationIdx)
        {
            ImGui::SameLine(0.0f, 5.0f * Scale);
            if (ImGui::Button(QuickStations[StationIdx]))
            {
                const bool WasSimulating = Physics && Physics->QueryRunning();
                if (WasSimulating) Physics->End(QueryWorldSpace());
                if (Lobby.DeployProvingGroundStation(QueryWorldSpace(), StationIdx) && WasSimulating)
                {
                    ToggleSimulation();
                }
            }
        }
        if (QueryVehicleDrivingActive())
        {
            ImGui::SameLine(0.0f, 6.0f * Scale);
            char SpeedBadge[48];
            std::snprintf(SpeedBadge, sizeof(SpeedBadge), "%.0f km/h", Subject.SpeedMetresPerSecond * 3.6);
            ImGui::BeginDisabled();
            ImGui::Button(SpeedBadge);
            ImGui::EndDisabled();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        return;
    }

    // Studio Lobby session card: 1..5 dynamic roster stands, rig transition camera pills, and Proving Grounds launch.
    const float  CardWidth  = std::min(292.0f * Scale, StageWidth - 24.0f * Scale);
    const float  CardHeight = 228.0f * Scale;
    const ImVec2 CardMin(StageMin.x + 12.0f * Scale, StageMin.y + 12.0f * Scale);
    const ImVec2 CardMax(CardMin.x + CardWidth, CardMin.y + CardHeight);

    DrawList->AddRectFilled(CardMin, CardMax, IM_COL32(12, 16, 22, 214), 10.0f * Scale);
    DrawList->AddRect(CardMin, CardMax, IM_COL32(96, 122, 152, 92), 10.0f * Scale, 0, 1.0f);

    const auto&    SessionRoster = Lobby.QuerySession();
    const uint32_t ActiveCount   = Lobby.QueryActiveVehicleCount();
    const auto&    CameraFrame   = VehicleCamera.QueryLastSolution();

    char HeaderLine[96];
    if (CameraFrame.CinematicActive)
    {
        std::snprintf(HeaderLine, sizeof(HeaderLine), "STUDIO LOBBY  \xC2\xB7  %u/5  \xC2\xB7  TOUR %u/6",
                      ActiveCount, CameraFrame.CinematicShotIndex + 1u);
    }
    else if (CameraFrame.EntranceDollyActive)
    {
        std::snprintf(HeaderLine, sizeof(HeaderLine), "STUDIO LOBBY  \xC2\xB7  %u/5  \xC2\xB7  ENTRANCE", ActiveCount);
    }
    else if (CameraFrame.TransitionActive)
    {
        std::snprintf(HeaderLine, sizeof(HeaderLine), "STUDIO LOBBY  \xC2\xB7  %u/5  \xC2\xB7  TRANSITION", ActiveCount);
    }
    else
    {
        std::snprintf(HeaderLine, sizeof(HeaderLine), "STUDIO LOBBY  \xC2\xB7  %u/5 DRIVERS", ActiveCount);
    }
    DrawList->AddText(ImVec2(CardMin.x + 12.0f * Scale, CardMin.y + 10.0f * Scale), IM_COL32(226, 234, 244, 245), HeaderLine);

    const auto& Presets = QueryLobbyPresetCallsigns();
    for (uint32_t Slot = 0u; Slot < 5u; ++Slot)
    {
        const float                         RowY        = CardMin.y + (34.0f + static_cast<float>(Slot) * 21.0f) * Scale;
        const Frontier::SessionParticipant* Participant = SessionRoster.FindParticipantInSlot(Slot);
        char                                RowText[96];
        ImU32                               RowTint = IM_COL32(118, 130, 146, 175);
        if (Participant != nullptr)
        {
            std::snprintf(RowText, sizeof(RowText), "P%u  %-8s  \xC2\xB7  %s%s",
                          Slot + 1u,
                          Participant->Callsign.c_str(),
                          Participant->RankBadge.c_str(),
                          Participant->LocalDriver ? "  [HOST]" : "");
            RowTint = Participant->LocalDriver ? IM_COL32(142, 220, 178, 245) : IM_COL32(188, 212, 238, 235);
        }
        else
        {
            std::snprintf(RowText, sizeof(RowText), "P%u  Stand vacant  (%s)", Slot + 1u, std::string(Presets[Slot].Callsign).c_str());
        }
        DrawList->AddText(ImVec2(CardMin.x + 12.0f * Scale, RowY), RowTint, RowText);
    }

    ImGui::SetCursorScreenPos(ImVec2(CardMin.x + 10.0f * Scale, CardMin.y + 144.0f * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * Scale, 3.5f * Scale));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(26, 34, 46, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(42, 58, 80, 245));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(58, 82, 112, 255));

    ImGui::BeginDisabled(ActiveCount >= 5u);
    if (ImGui::Button("+ Join##LobbyJoin"))
    {
        (void)Lobby.JoinGuestDriver(QueryWorldSpace());
        OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 5.0f * Scale);
    ImGui::BeginDisabled(ActiveCount <= 1u);
    if (ImGui::Button("- Leave##LobbyLeave"))
    {
        (void)Lobby.LeaveLastGuest(QueryWorldSpace());
        OutlinerPanel.Synchronize(Frontier::SceneDirectory::Instance());
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 8.0f * Scale);
    static constexpr std::array<std::pair<Frontier::VehicleCameraRig, const char*>, 3> StudioRigs = {{
        {Frontier::VehicleCameraRig::Orbit, "Orbit"},
        {Frontier::VehicleCameraRig::Cockpit, "Cabin"},
        {Frontier::VehicleCameraRig::Hmi, "HMI"},
    }};
    for (size_t RigIdx = 0u; RigIdx < StudioRigs.size(); ++RigIdx)
    {
        if (RigIdx > 0u) ImGui::SameLine(0.0f, 4.0f * Scale);
        const bool SelectedRig = StudioCameraEngaged && VehicleCamera.QueryRig() == StudioRigs[RigIdx].first;
        if (SelectedRig) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(52, 92, 138, 245));
        if (ImGui::Button(StudioRigs[RigIdx].second))
        {
            if (!StudioCameraEngaged)
            {
                ConfigureGameCameraMode(true);
            }
            VehicleCamera.TransitionToRig(StudioRigs[RigIdx].first);
        }
        if (SelectedRig) ImGui::PopStyleColor();
    }

    ImGui::SetCursorScreenPos(ImVec2(CardMin.x + 10.0f * Scale, CardMin.y + 182.0f * Scale));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(36, 108, 78, 238));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(48, 136, 98, 250));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(62, 162, 116, 255));
    if (ImGui::Button("Enter Proving Grounds  (L)##EnterCourse", ImVec2(CardWidth - 20.0f * Scale, 32.0f * Scale)) &&
        !VehicleCamera.QueryRouteTransitionActive())
    {
        BeginLevelTransition(ProjectRoot / "Content" / "Levels" / "ProvingGroundVehicle.levelspace", true);
    }
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(2);
}

void EverestWorkspace::ConstructVehicleWorkbench(float Scale)
{
    const std::vector<const Frontier::SceneRecordBase*> Vehicles = QueryWorldSpace().FindAllByType("Vehicle");
    if (Vehicles.empty())
    {
        ImGui::TextDisabled("VEHICLE / WORKBENCH");
        ImGui::TextWrapped("Construct a Vehicle record to author its components, driver aids and proving-ground test flow.");
        return;
    }

    Frontier::VehicleRecord* VehicleEntry = nullptr;
    const Frontier::RecordToken Selected = OutlinerPanel.QuerySelected();
    if (auto* Candidate = dynamic_cast<Frontier::VehicleRecord*>(QueryWorldSpace().FindRecord(Selected))) VehicleEntry = Candidate;
    if (VehicleEntry == nullptr) VehicleEntry = dynamic_cast<Frontier::VehicleRecord*>(QueryWorldSpace().FindRecord(Vehicles.front()->QueryToken()));
    if (VehicleEntry == nullptr) return;

    for (const Frontier::SceneRecordBase* Candidate : Vehicles)
    {
        const bool IsSelected = Candidate->QueryToken() == VehicleEntry->QueryToken();
        if (ImGui::Selectable(Candidate->QueryRecordName().c_str(), IsSelected, 0, ImVec2(0.0f, 24.0f * Scale)))
        {
            VehicleEntry = dynamic_cast<Frontier::VehicleRecord*>(QueryWorldSpace().FindRecord(Candidate->QueryToken()));
            OutlinerPanel.Select(Candidate->QueryToken());
        }
    }
    if (VehicleEntry == nullptr) return;

    Frontier::VehicleConfiguration& Configuration = VehicleEntry->AccessConfiguration();
    Frontier::Vehicle::VehicleSolverConfiguration& Solver = Configuration.Specification.Solver;
    const bool SimulationRunning = Physics && Physics->QueryRunning();
    bool Changed = false;

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.56f, 0.84f, 0.70f, 1.0f), "%s", VehicleEntry->QueryRecordName().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Inspect vehicle")) OutlinerPanel.Select(VehicleEntry->QueryToken());

    if (ImGui::BeginTable("##VehicleWorkbenchComponents", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Specification", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("POWERTRAIN");
        ImGui::Text("%s", Solver.Induction == Frontier::Vehicle::InductionType::NaturallyAspirated ? "Naturally aspirated" :
                           Solver.Induction == Frontier::Vehicle::InductionType::Turbocharged ? "Turbocharged" : "Supercharged");
        ImGui::Text("%.1f L  /  %.0f rpm redline", Solver.Intake.Displacement_L, Solver.Engine.RedlineRPM);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("DRIVELINE");
        ImGui::Text("%zu forward gears  /  %.2f final drive", Frontier::Vehicle::QueryForwardRatios(Configuration.Specification).size(),
                    Solver.Transmission.FinalDriveRatio);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("CHASSIS & SUSPENSION");
        ImGui::Text("%.0f kg  /  %.0f%% front", Configuration.Specification.Geometry.Mass,
                    Configuration.Specification.Geometry.FrontWeightFraction * 100.0f);
        ImGui::Text("%s carcass  /  equilibrium %s", Frontier::Vehicle::TyreCarcassName(Solver.Carcass),
                    Configuration.EquilibriumPlacement ? "on" : "off");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("TYRES & DRIVER AIDS");
        ImGui::Text("ABS %s  /  traction %s", Solver.Abs.Enabled ? "on" : "off", Solver.TractionControl.Enabled ? "on" : "off");
        ImGui::TextUnformatted("Four-corner tyre set");
        ImGui::EndTable();
    }
    static constexpr const char* TyreCorners[] = { "Front left", "Front right", "Rear left", "Rear right" };
    bool FoundTyreRecord = false;
    for (const Frontier::SceneRecordBase* Candidate : QueryWorldSpace().FindAllByType("Tyre"))
    {
        const auto* TyreEntry = dynamic_cast<const Frontier::TyreRecord*>(Candidate);
        if (TyreEntry == nullptr || Candidate->QueryParentToken() != VehicleEntry->QueryToken()) continue;
        FoundTyreRecord = true;
        const Frontier::TyreConfiguration& Fit = TyreEntry->QueryConfiguration();
        const char* Corner = TyreCorners[Fit.Corner & 3u];
        const Frontier::TyreCompound& Compound = Frontier::ResolveTyreCompound(Fit.Compound);
        ImGui::PushID(static_cast<int>(Candidate->QueryToken().Value & 0x7fffffff));
        ImGui::Text("%s  ·  %s  ·  %.0f kPa  ·  %s", Corner, Compound.Name, Fit.ColdPressure / 1000.0f, Fit.Enclosed ? "fitted" : "detached");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open tyre inspector")) OutlinerPanel.Select(Candidate->QueryToken());
        ImGui::PopID();
    }
    if (!FoundTyreRecord) ImGui::TextDisabled("No individual Tyre records — using the vehicle's built-in four-corner setup.");
    ImGui::Separator();
    ImGui::TextDisabled("QUICK CONFIGURATION  ·  [units as shown]");
    ImGui::BeginDisabled(SimulationRunning);
    Changed |= ImGui::Checkbox("Visible in scene", &Configuration.Visible);
    ImGui::SameLine();
    Changed |= ImGui::Checkbox("Physics enabled", &Configuration.Physics);
    Changed |= ImGui::Checkbox("Equilibrium placement", &Configuration.EquilibriumPlacement);
    Changed |= ImGui::Checkbox("Physics thread", &Configuration.ThreadedPhysics);
    ImGui::EndDisabled();
    Changed |= ImGui::Checkbox("Possess for driving", &Configuration.Possessed);
    ImGui::SameLine();
    Changed |= ImGui::Checkbox("Chase camera", &Configuration.ChaseCamera);
    ImGui::BeginDisabled(SimulationRunning);
    Changed |= ImGui::Checkbox("ABS", &Solver.Abs.Enabled);
    ImGui::SameLine();
    Changed |= ImGui::Checkbox("Traction control", &Solver.TractionControl.Enabled);
    static constexpr const char* CarcassNames[] = { "Spring", "XPBD lattice", "3D FEM" };
    int Carcass = static_cast<int>(Solver.Carcass);
    if (ImGui::Combo("Tyre carcass", &Carcass, CarcassNames, 3))
    {
        Solver.Carcass = static_cast<Frontier::Vehicle::TyreCarcassCategory>(Carcass);
        Changed = true;
    }
    ImGui::EndDisabled();
    if (Changed) Configuration.AdvanceRevision();

    ImGui::Separator();
    ImGui::TextDisabled("TEST FLOW");
    if (ImGui::Button(SimulationRunning ? "Stop simulation" : "Start simulation", ImVec2(142.0f * Scale, 30.0f * Scale))) ToggleSimulation();
    ImGui::SameLine();
    ImGui::BeginDisabled(!SimulationRunning);
    const bool ResetPressed = ImGui::Button("Reset to start  (R)", ImVec2(150.0f * Scale, 30.0f * Scale));
    ImGui::EndDisabled();
    if (ResetPressed && SimulationRunning) Physics->RequestVehicleReset();
    ImGui::SameLine();
    if (ImGui::Button("Open Proving Ground", ImVec2(160.0f * Scale, 30.0f * Scale)) && !VehicleCamera.QueryRouteTransitionActive())
        BeginLevelTransition(ProjectRoot / "Content" / "Levels" / "ProvingGroundVehicle.levelspace", true);
    ImGui::TextWrapped("Drive: W/S throttle and brake · A/D steer · Space handbrake · R reset · C camera rig · M HMI · Alt+S stop.");

    Frontier::VehicleReading Reading{};
    if (Frontier::QueryVehicleReading(VehicleEntry->QueryToken(), Reading) && Reading.Simulating)
    {
        const Frontier::Vehicle::VehicleTelemetry& Telemetry = Reading.Telemetry;
        char SpeedText[24], RpmText[24], GearText[24], BoostText[24], ContactsText[24], AbsText[24];
        std::snprintf(SpeedText, sizeof(SpeedText), "%.0f", Telemetry.SpeedMetresPerSecond * 3.6f);
        std::snprintf(RpmText, sizeof(RpmText), "%.0f", Telemetry.EngineRPM);
        std::snprintf(GearText, sizeof(GearText), "%s%d", Telemetry.GearIndex < 0 ? "R" : "", std::abs(Telemetry.GearIndex));
        std::snprintf(BoostText, sizeof(BoostText), "%.2f", Telemetry.BoostBar);
        std::snprintf(ContactsText, sizeof(ContactsText), "%u/4", Telemetry.WheelsInContact);
        uint32_t AbsCount = 0;
        for (const Frontier::Vehicle::WheelTelemetry& Wheel : Telemetry.Wheels) if (Wheel.AbsActive) ++AbsCount;
        std::snprintf(AbsText, sizeof(AbsText), "%u active", AbsCount);
        ImGui::Text("LIVE  ·  %s km/h  ·  %s rpm  ·  gear %s  ·  %s bar  ·  %s wheels  ·  ABS %s", SpeedText, RpmText, GearText, BoostText,
                    ContactsText, AbsText);
        ImGui::Text("Throttle %.0f%%  ·  brake %.0f%%  ·  steer %.0f%%  ·  %llu fixed steps", Reading.Throttle[0] * 100.0f,
                    Reading.Throttle[1] * 100.0f, Reading.Throttle[2] * 100.0f, static_cast<unsigned long long>(Reading.Steps));
    }
    else
    {
        ImGui::TextDisabled("Live telemetry appears here while this vehicle is simulating.");
    }
}

void EverestWorkspace::ToggleSimulation()
{
    PendingAutoSimulation = false;
    if (!Physics) Physics = std::make_unique<Frontier::PhysicsExtension>();
    if (Physics->QueryRunning())
    {
        Physics->End(QueryWorldSpace());
        DriverControls = Frontier::Vehicle::DriverInputIntegrator{};
        AnnounceViewportNotice("Simulation stopped \xC2\xB7 objects restored");
        return;
    }
    Frontier::PhysicsExtensionSettings Settings{};
    Settings.WriteRegister = true;
    const char* CarcassLabel = nullptr;
    for (const Frontier::SceneRecordBase* Candidate : QueryWorldSpace().FindAllByType("Vehicle"))
    {
        const auto* VehicleRec = dynamic_cast<const Frontier::VehicleRecord*>(Candidate);
        if (VehicleRec == nullptr || !VehicleRec->QueryConfiguration().Visible || !VehicleRec->QueryConfiguration().Physics) continue;
        const Frontier::VehicleConfiguration& Config = VehicleRec->QueryConfiguration();
        Settings.Threaded       = Config.ThreadedPhysics;
        Settings.StepHz         = Config.PhysicsStepHz > 0.0f ? static_cast<double>(Config.PhysicsStepHz) : 240.0;
        Settings.CollisionSteps = std::clamp(Config.CollisionSteps, 1, 8);
        Settings.SolverSubsteps = std::clamp(Config.SolverSubsteps, 1, 8);
        CarcassLabel            = Frontier::Vehicle::TyreCarcassName(Config.Specification.Solver.Carcass);
        break;
    }
    DriverControls = Frontier::Vehicle::DriverInputIntegrator{};
    const size_t Moving = Physics->Begin(QueryWorldSpace(), Settings);
    const auto&  Tally  = Physics->QueryTally();
    char         Text[220];
    if (Tally.Vehicles > 0)
    {
        for (const Frontier::SceneRecordBase* Candidate : QueryWorldSpace().FindAllByType("Vehicle"))
        {
            const auto* VehicleRec = dynamic_cast<const Frontier::VehicleRecord*>(Candidate);
            if (VehicleRec != nullptr && VehicleRec->QueryConfiguration().Visible && VehicleRec->QueryConfiguration().Physics &&
                VehicleRec->QueryConfiguration().Possessed && VehicleRec->QueryConfiguration().ChaseCamera)
            {
                ConfigureGameCameraMode(true);
                break;
            }
        }
        std::snprintf(Text, sizeof(Text),
                      "Driving (%s \xC2\xB7 %.0f Hz %s) \xC2\xB7 W/S/A/D/Space drive \xC2\xB7 C rigs \xC2\xB7 M HMI \xC2\xB7 F8 Editor cam \xC2\xB7 Alt+S stop",
                      CarcassLabel ? CarcassLabel : "Spring", Tally.StepHz, Tally.Threaded ? "PhysicsThread" : "inline");
    }
    else if (Moving > 0)
    {
        std::snprintf(Text, sizeof(Text), "Simulating \xC2\xB7 %zu moving, %zu colliders, %zu static fractures, %zu groups held (Alt+S stops)",
                      Moving, Tally.Fixed, Tally.Fracture, Tally.Held);
    }
    else
    {
        std::snprintf(Text, sizeof(Text), "Simulating \xC2\xB7 no single object has Physics on (%zu colliders, %zu static fractures) \xC2\xB7 Alt+S stops",
                      Tally.Fixed, Tally.Fracture);
    }
    AnnounceViewportNotice(Text);
}

void EverestWorkspace::ToggleNetworking()
{
    if (!Network)
    {
        Network = std::make_unique<Frontier::NetworkingSession>();
        Network->ConfigureLocalPeer("Everest", Frontier::NetworkRole::Host);
    }
    Network->ConfigureEnabled(!Network->QueryEnabled());
    if (Network->QueryEnabled()) Network->Greet();
    AnnounceViewportNotice(Network->QueryReading());
}

bool EverestWorkspace::QueryNetworkingEnabled() const noexcept
{
    return Network && Network->QueryEnabled();
}

bool EverestWorkspace::QueryVehicleDrivingActive() const
{
    if (!Physics || !Physics->QueryRunning()) return false;
    for (const Frontier::SceneRecordBase* Candidate : const_cast<EverestWorkspace*>(this)->QueryWorldSpace().FindAllByType("Vehicle"))
    {
        const auto* VehicleRec = dynamic_cast<const Frontier::VehicleRecord*>(Candidate);
        if (VehicleRec != nullptr && VehicleRec->QueryConfiguration().Visible && VehicleRec->QueryConfiguration().Physics &&
            VehicleRec->QueryConfiguration().Possessed)
            return true;
    }
    return false;
}

void EverestWorkspace::IntegrateDrivingControls(double DeltaSeconds)
{
    if (!QueryVehicleDrivingActive()) return;
    const ImGuiIO& IO      = ImGui::GetIO();
    const bool     Blocked = IO.WantTextInput || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ConstructDialog.QueryOpen() ||
                         ImGui::GetTopMostPopupModal() != nullptr;
    if (!Blocked && !IO.KeyCtrl && !IO.KeyAlt)
    {
        const bool WasdDriving = StudioCameraEngaged;
        DriverControls.ForwardThrottleKey((WasdDriving && ImGui::IsKeyDown(ImGuiKey_W)) || ImGui::IsKeyDown(ImGuiKey_UpArrow));
        DriverControls.ForwardBrakeKey((WasdDriving && ImGui::IsKeyDown(ImGuiKey_S)) || ImGui::IsKeyDown(ImGuiKey_DownArrow));
        DriverControls.ForwardSteerLeftKey((WasdDriving && ImGui::IsKeyDown(ImGuiKey_A)) || ImGui::IsKeyDown(ImGuiKey_LeftArrow));
        DriverControls.ForwardSteerRightKey((WasdDriving && ImGui::IsKeyDown(ImGuiKey_D)) || ImGui::IsKeyDown(ImGuiKey_RightArrow));
        DriverControls.ForwardHandbrakeKey(ImGui::IsKeyDown(ImGuiKey_Space));
        if (WasdDriving && ImGui::IsKeyPressed(ImGuiKey_R, false))
        {
            Physics->RequestVehicleReset();
            AnnounceViewportNotice("Vehicle reset to start");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !VehicleCamera.QueryRouteTransitionActive())
        {
            BeginLevelTransition(ProjectRoot / "Content" / "Levels" / "Lobby.levelspace", false);
        }
    }
    else
    {
        DriverControls.ForwardThrottleKey(false);
        DriverControls.ForwardBrakeKey(false);
        DriverControls.ForwardSteerLeftKey(false);
        DriverControls.ForwardSteerRightKey(false);
        DriverControls.ForwardHandbrakeKey(false);
    }

    const Frontier::Vehicle::DriverCommand Command = DriverControls.Advance(static_cast<float>(std::clamp(DeltaSeconds, 0.0, 0.1)));
    Frontier::PhysicsVehicleInput          Input{};
    Input.Throttle    = Command.Drive.Throttle;
    Input.Brake       = Command.Drive.Brake;
    Input.Steer       = Command.Drive.Steer;
    Input.Handbrake   = Command.Drive.Handbrake;
    Input.ClutchPedal = (!ImGui::GetIO().WantTextInput && ImGui::IsKeyDown(ImGuiKey_LeftShift) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) ? 1.0f : 0.0f;
    Physics->AssignVehicleInput(Input);
}

void EverestWorkspace::IntegrateChaseCamera(double DeltaSeconds)
{
    if (!Physics || !Physics->QueryRunning() || ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;
    for (const Frontier::SceneRecordBase* Candidate : QueryWorldSpace().FindAllByType("Vehicle"))
    {
        const auto* VehicleRec = dynamic_cast<const Frontier::VehicleRecord*>(Candidate);
        if (VehicleRec == nullptr || !VehicleRec->QueryConfiguration().Visible || !VehicleRec->QueryConfiguration().Physics ||
            !VehicleRec->QueryConfiguration().Possessed || !VehicleRec->QueryConfiguration().ChaseCamera)
            continue;
        Frontier::VehicleReading Reading;
        if (!Frontier::QueryVehicleReading(Candidate->QueryToken(), Reading) || !Reading.Simulating) continue;

        const ImGuiIO&               IO = ImGui::GetIO();
        Frontier::VehicleCameraInput CameraInput{};
        if (!IO.WantCaptureMouse)
        {
            CameraInput.PointerDeltaPixels = {static_cast<double>(IO.MouseDelta.x), static_cast<double>(IO.MouseDelta.y)};
            CameraInput.WheelNotches       = static_cast<double>(IO.MouseWheel);
            CameraInput.PrimaryDrag        = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        }
        const double Aspect = LastReadout.ScreenExtent[1] > 0u
                                  ? static_cast<double>(LastReadout.ScreenExtent[0]) / static_cast<double>(LastReadout.ScreenExtent[1])
                                  : 16.0 / 9.0;
        const auto Solution = VehicleCamera.Advance(ResolveVehicleKinematics(Aspect), CameraInput, DeltaSeconds);
        ApplyVehicleCameraSolution(Solution);
        break;
    }
}

void EverestWorkspace::ToggleViewportOverlay(int Overlay)
{
    if (Overlay==2)
    {
        Overlays.Fullscreen=!Overlays.Fullscreen;
        if(Overlays.Fullscreen) AnnounceViewportNotice("Fullscreen viewport · V or Escape restores panels");
        else { NoticeSeconds=0; NoticeText.clear(); }
    }
    else Overlays.ReadoutVisible=!Overlays.ReadoutVisible;
}

void EverestWorkspace::ConstructFullscreenViewport(float Scale)
{
    const auto* Main=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Main->Pos);
    ImGui::SetNextWindowSize(Main->Size);
    ImGui::SetNextWindowViewport(Main->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
    ImGui::Begin("##ViewportFullscreen",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings);
    if (Dockspace.QueryDockspace()) ImGui::DockSpace(Dockspace.QueryDockspace(),{0,0},ImGuiDockNodeFlags_KeepAliveOnly);
    const auto Origin=ImGui::GetCursorScreenPos();
    const auto Extent=ImGui::GetContentRegionAvail();
    ImGui::BeginChild("FullscreenContent",{Extent.x,Extent.y-28*Scale},ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    ConstructViewportSurface();
    ImGui::EndChild();
    ImGui::SetCursorScreenPos({Origin.x,Origin.y+Extent.y-28*Scale});
    ConstructViewportFooter(Scale);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void EverestWorkspace::AnnounceViewportNotice(std::string Text)
{
    NoticeText    = std::move(Text);
    NoticeSeconds = 2.4;
}

void EverestWorkspace::ConstructViewportFooter(float Scale)
{
    const auto Min=ImGui::GetCursorScreenPos();
    const ImVec2 Max(Min.x+ImGui::GetContentRegionAvail().x,Min.y+28*Scale);
    const double Fps=FrameSeconds>0 ? 1/FrameSeconds : 0;
    const ImU32 Health=Fps>=55?IM_COL32(130,206,158,255):Fps>=25?IM_COL32(222,183,103,255):IM_COL32(227,121,120,255);
    char Rate[32],Time[32],Sun[32];
    std::snprintf(Rate,sizeof(Rate),"%.0f",Fps);
    std::snprintf(Time,sizeof(Time),"%.1f ms",LastReadout.RefreshMilliseconds);
    float Elevation=0;
    if(const auto* Entry=dynamic_cast<const Frontier::SolarRecord*>(QueryWorldSpace().FindFirstByType("Solar"))) Elevation=Entry->QueryConfiguration().ElevationDegrees;
    std::snprintf(Sun,sizeof(Sun),"%.1f°",double(Elevation));
    const std::vector<Frontier::ViewportStatusItem> Cells = {{"FPS",Rate,Health,Fps>=55?"Activity":"TriangleAlert"},
        {"RENDER",LastReadout.Presenting?Time:"—",IM_COL32(164,192,212,255),"Info"},
        {"TRIANGLES",LastReadout.Presenting?std::to_string(LastReadout.SceneTriangles):"—",IM_COL32(176,195,206,255),"Box"},
        {"SUN",Sun,IM_COL32(228,195,126,255),"Sun"}};
    Frontier::ViewportChromeControls::ConstructStatusStrip(ImGui::GetWindowDrawList(),Frontier::EditorTypefaces::Resolve(),Min,Max,Cells,{},Scale);
    ImGui::Dummy({1,28*Scale});
}

void EverestWorkspace::ConstructViewportSchematic(ImDrawList* DrawList, const ImVec2& Min, const ImVec2& Max)
{
    const Frontier::InspectorPalette Palette = Frontier::InspectorPalette::Resolve();
    const float W = Max.x - Min.x;
    const float H = Max.y - Min.y;

    float SunElevation = 38.0f;
    float SunAzimuth   = 132.0f;
    if (const auto* SolarRec = dynamic_cast<const Frontier::SolarRecord*>(QueryWorldSpace().FindFirstByType("Solar")))
    {
        SunElevation = SolarRec->QueryConfiguration().ElevationDegrees;
        SunAzimuth   = SolarRec->QueryConfiguration().AzimuthDegrees;
    }

    const float DayFactor  = std::clamp((SunElevation + 6.0f) / 55.0f, 0.05f, 1.0f);
    const ImU32 ZenithCol  = IM_COL32(static_cast<int>(14 + 28 * DayFactor),
                                      static_cast<int>(22 + 54 * DayFactor),
                                      static_cast<int>(38 + 92 * DayFactor), 255);
    const ImU32 HorizonCol = IM_COL32(static_cast<int>(28 + 86 * DayFactor),
                                      static_cast<int>(38 + 96 * DayFactor),
                                      static_cast<int>(54 + 108 * DayFactor), 255);
    const ImU32 GroundNear = IM_COL32(19, 22, 27, 255);
    const ImU32 GroundFar  = IM_COL32(28, 33, 41, 255);

    const float HorizonY = Min.y + H * 0.56f;
    DrawList->AddRectFilledMultiColor(Min, ImVec2(Max.x, HorizonY), ZenithCol, ZenithCol, HorizonCol, HorizonCol);
    DrawList->AddRectFilledMultiColor(ImVec2(Min.x, HorizonY), Max, GroundFar, GroundFar, GroundNear, GroundNear);
    DrawList->AddLine(ImVec2(Min.x, HorizonY), ImVec2(Max.x, HorizonY),
                      Frontier::ModulateSpectrumAlpha(Palette.TextSecondary, 0.35f), 1.0f);

    // Analytical directional sun disc above the horizon.
    if (SunElevation > -2.0f)
    {
        const float AzNorm = std::fmod(SunAzimuth, 360.0f) / 360.0f;
        const float SunX   = Min.x + W * (0.18f + 0.64f * AzNorm);
        const float SunY   = HorizonY - (std::clamp(SunElevation, 0.0f, 90.0f) / 90.0f) * (HorizonY - Min.y - 36.0f);
        DrawList->AddCircleFilled(ImVec2(SunX, SunY), 22.0f, IM_COL32(255, 222, 148, 38), 32);
        DrawList->AddCircleFilled(ImVec2(SunX, SunY), 8.5f, IM_COL32(255, 244, 214, 235), 24);
    }

    // Perspective metric reference plane (clean general-purpose 3D viewport floor grid).
    const float  VanishX  = (Min.x + Max.x) * 0.5f;
    const ImU32  GridLine = IM_COL32(130, 145, 168, 34);
    const ImU32  AxisX    = IM_COL32(214, 92, 92, 130);
    const ImU32  AxisZ    = IM_COL32(88, 152, 224, 130);
    for (int LineIdx = -12; LineIdx <= 12; ++LineIdx)
    {
        const float TopOffset    = static_cast<float>(LineIdx) * (W * 0.014f);
        const float BottomOffset = static_cast<float>(LineIdx) * (W * 0.095f);
        const ImU32 Tint         = (LineIdx == 0) ? AxisZ : GridLine;
        DrawList->AddLine(ImVec2(VanishX + TopOffset, HorizonY),
                          ImVec2(VanishX + BottomOffset, Max.y),
                          Tint, LineIdx == 0 ? 1.5f : 1.0f);
    }
    for (int Row = 1; Row <= 10; ++Row)
    {
        const float T  = static_cast<float>(Row) / 10.0f;
        const float Y  = HorizonY + (T * T) * (Max.y - HorizonY);
        const ImU32 C  = (Row == 4) ? AxisX : GridLine;
        DrawList->AddLine(ImVec2(Min.x, Y), ImVec2(Max.x, Y), C, Row == 4 ? 1.4f : 1.0f);
    }

}

#endif // FRONTIER_EDITOR && FRONTIER_WITH_INTERFACE_SDK

} // namespace Everest
