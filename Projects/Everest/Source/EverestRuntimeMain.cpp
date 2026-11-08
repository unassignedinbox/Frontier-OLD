/*==========================================================================================================================================
                                                        EVERESTRUNTIMEMAIN.CPP
==========================================================================================================================================*/
// 🧩 Standalone Vulkan Everest game runtime (FRONTIER_EDITOR == 0): Studio Lobby 1..5 roster, cinematic/transition cameras & driving.

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

#include "Engine/Diagnostics/BuildConfiguration.h"
#if FRONTIER_EDITOR
#error "EverestRuntimeMain must compile with FRONTIER_EDITOR == 0."
#endif

#include "Engine/Diagnostics/DiagnosticJournal.h"
#include "Engine/Interface/ContentDirectory.h"
#include "Engine/Interface/VulkanPresentation.h"
#include "Engine/Interface/WindowsApplication.h"
#include "Engine/PhysicalDynamics/Vehicle/DriverInputIntegrator.h"
#include "Engine/Scene/VehicleReadings.h"
#include "EverestWorkspace.h"
#include "Frontier/Extensions/Interface/ViewportTexture.h"
#include "Frontier/Extensions/Physics/PhysicsExtension.h"
#include "Frontier/Extensions/Rendering/RenderingExtension.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{

//------------------------------------------------------------------------------------------------------------------------
//                                                    PATH & VEHICLE HELPERS
//------------------------------------------------------------------------------------------------------------------------

std::filesystem::path LocateEverestProjectRoot()
{
    const std::filesystem::path CandidateA = Frontier::ResolveContentDirectory("Projects/Everest");
    if (std::filesystem::exists(CandidateA / "Everest.projectspace"))
    {
        return CandidateA;
    }
    const std::filesystem::path EngineDir  = Frontier::ResolveContentDirectory("EngineContent");
    const std::filesystem::path CandidateB = EngineDir.parent_path() / "Projects" / "Everest";
    if (std::filesystem::exists(CandidateB / "Everest.projectspace"))
    {
        return CandidateB;
    }
    return std::filesystem::current_path() / "Projects" / "Everest";
}

Frontier::VehicleSubjectKinematics SampleRuntimeVehicleSubject(const Frontier::WorldSpace& World, double AspectRatio)
{
    Frontier::VehicleSubjectKinematics Subject{};
    Subject.AspectRatio = std::max(0.25, AspectRatio);
    Subject.Available   = false;

    for (const Frontier::SceneRecordBase* Candidate : World.FindAllByType("Vehicle"))
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
            constexpr double DegToRad = 3.14159265358979323846 / 180.0;
            const double YawRad   = static_cast<double>(Configuration.Spatial.OrientationDegrees[2]) * DegToRad;
            const double PitchRad = static_cast<double>(Configuration.Spatial.OrientationDegrees[1]) * DegToRad;
            const double RollRad  = static_cast<double>(Configuration.Spatial.OrientationDegrees[0]) * DegToRad;
            const Frontier::Vehicle::Quat Orientation =
                (Frontier::Vehicle::Quat::AxisAngle({0.0f, 0.0f, 1.0f}, static_cast<float>(YawRad)) *
                 Frontier::Vehicle::Quat::AxisAngle({0.0f, 1.0f, 0.0f}, static_cast<float>(PitchRad)) *
                 Frontier::Vehicle::Quat::AxisAngle({1.0f, 0.0f, 0.0f}, static_cast<float>(RollRad))).Normalized();
            const Frontier::Vehicle::Vec3 Fwd  = Orientation.Rotate({1.0f, 0.0f, 0.0f});
            const Frontier::Vehicle::Vec3 Left = Orientation.Rotate({0.0f, 1.0f, 0.0f});
            const Frontier::Vehicle::Vec3 Up   = Orientation.Rotate({0.0f, 0.0f, 1.0f});
            const double BodyGroundOffset = Geometry.AuthoredGroundOffset();
            Subject.GroundFloorMetres     = static_cast<double>(Configuration.Spatial.PositionMetres[2]);
            Subject.PositionMetres        = {
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
        const auto OffsetHub = [&](const Frontier::Vehicle::Vec3& Offset)
        {
            return std::array<double, 3>{
                Subject.PositionMetres[0] + Subject.ForwardUnit[0] * Offset.x + Subject.LeftUnit[0] * Offset.y + Subject.UpUnit[0] * Offset.z,
                Subject.PositionMetres[1] + Subject.ForwardUnit[1] * Offset.x + Subject.LeftUnit[1] * Offset.y + Subject.UpUnit[1] * Offset.z,
                Subject.PositionMetres[2] + Subject.ForwardUnit[2] * Offset.x + Subject.LeftUnit[2] * Offset.y + Subject.UpUnit[2] * Offset.z,
            };
        };
        Subject.FrontLeftHubMetres = OffsetHub(ResolveBodyLocalHub(0u));
        Subject.RearRightHubMetres = OffsetHub(ResolveBodyLocalHub(3u));
        break;
    }

    return Subject;
}

void PublishGeometryResolutions(Frontier::WorldSpace& World, const std::shared_ptr<const Frontier::GeometryResolutions>& Resolutions)
{
    if (!Resolutions)
    {
        return;
    }
    for (const auto& [TokenValue, Resolved] : *Resolutions)
    {
        if (auto* Geometry = dynamic_cast<Frontier::GeometryRecord*>(World.FindRecord(Frontier::RecordToken{TokenValue})))
        {
            Geometry->AccessConfiguration().Resolved = Resolved;
        }
    }
}

void StartCoursePhysics(Frontier::PhysicsExtension& Physics, Frontier::WorldSpace& World)
{
    if (Physics.QueryRunning())
    {
        Physics.End(World);
    }
    Frontier::PhysicsExtensionSettings Settings{};
    Settings.WriteRegister = false;
    Settings.Threaded      = true;
    Settings.StepHz        = 240.0;
    for (const Frontier::SceneRecordBase* Candidate : World.FindAllByType("Vehicle"))
    {
        const auto* VehicleRec = dynamic_cast<const Frontier::VehicleRecord*>(Candidate);
        if (VehicleRec != nullptr && VehicleRec->QueryConfiguration().Visible && VehicleRec->QueryConfiguration().Physics)
        {
            const auto& Config      = VehicleRec->QueryConfiguration();
            Settings.Threaded       = Config.ThreadedPhysics;
            Settings.StepHz         = Config.PhysicsStepHz > 0.0f ? static_cast<double>(Config.PhysicsStepHz) : 240.0;
            Settings.CollisionSteps = std::clamp(Config.CollisionSteps, 1, 8);
            Settings.SolverSubsteps = std::clamp(Config.SolverSubsteps, 1, 8);
            break;
        }
    }
    (void)Physics.Begin(World, Settings);
}

} // namespace

//------------------------------------------------------------------------------------------------------------------------
//                                                    RUNTIME ENTRY POINT
//------------------------------------------------------------------------------------------------------------------------

int main(int, char**)
{
    const std::filesystem::path ProjectRoot    = LocateEverestProjectRoot();
    const std::filesystem::path ShaderArchives = Frontier::ResolveContentDirectory("EngineContent/ShaderArchives");

    Frontier::VulkanPresentation*                 Presentation = nullptr;
    GLFWwindow*                                   MainWindow   = nullptr;
    std::unique_ptr<Everest::EverestWorkspace>    Workspace;
    std::unique_ptr<Frontier::RenderingExtension> SceneRenderer;
    std::unique_ptr<Frontier::PhysicsExtension>   Physics;
    Frontier::ViewportTexture                     SceneTexture;
    Frontier::Vehicle::DriverInputIntegrator      DriverInput{};
    bool                                          HudHidden               = false;
    std::filesystem::path                         PendingRoutePath{};
    bool                                          PendingRouteDrive       = false;
    bool                                          PendingCoursePhysicsBoot = false;
    uint64_t                                      PublishedGeometryGen    = 0u;

    Frontier::ApplicationWindowCallbacks Callbacks{};
    Callbacks.Title          = "Everest — Studio Lobby";
    Callbacks.SceneRendering = true;

    Callbacks.InitializeVulkan = [&](Frontier::VulkanPresentation& ReadyPresentation)
    {
        Presentation = &ReadyPresentation;
    };

    Callbacks.Initialize = [&](GLFWwindow* ReadyWindow)
    {
        MainWindow = ReadyWindow;
        Workspace  = std::make_unique<Everest::EverestWorkspace>(ProjectRoot);
        Physics    = std::make_unique<Frontier::PhysicsExtension>();
    };

    Callbacks.ConstructInterface = [&](bool MainVisible)
    {
        if (!MainVisible || !Workspace)
        {
            return;
        }

        const ImGuiIO&      IO                 = ImGui::GetIO();
        const double        FrameDeltaSeconds  = std::max(0.0, static_cast<double>(IO.DeltaTime));
        const double        DeltaSeconds       = std::clamp(FrameDeltaSeconds, 1.0 / 240.0, 0.1);
        std::vector<Frontier::SceneImpactSpark> ImpactSparks;
        const ImGuiViewport* Viewport    = ImGui::GetMainViewport();
        const float         Width        = std::max(64.0f, Viewport->Size.x);
        const float         Height       = std::max(64.0f, Viewport->Size.y);
        const double        Aspect       = static_cast<double>(Width) / static_cast<double>(Height);

        Workspace->IntegrateSimulation(DeltaSeconds);

        const bool InLobby = Workspace->QueryLobbyActive();
        auto&      Camera  = Workspace->AccessVehicleCamera();
        auto&      Lobby   = Workspace->AccessLobbySession();
        auto&      World   = Workspace->QueryWorldSpace();

        if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H, false))
        {
            HudHidden = !HudHidden;
        }

        // ① Lobby controls vs outdoor Proving Grounds driving controls.
        if (InLobby)
        {
            if (!IO.WantTextInput)
            {
                for (int SlotKey = 0; SlotKey < 5; ++SlotKey)
                {
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + SlotKey), false))
                    {
                        Lobby.ConfigureTargetDriverCount(World, static_cast<uint32_t>(SlotKey + 1));
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !Camera.QueryRouteTransitionActive())
                {
                    PendingRoutePath  = ProjectRoot / "Content" / "Levels" / "ProvingGroundVehicle.levelspace";
                    PendingRouteDrive = true;
                    Camera.BeginRouteTransition(0.48, 0.58);
                }
            }
        }
        else if (Physics && Physics->QueryRunning())
        {
            DriverInput.ForwardThrottleKey(ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow));
            DriverInput.ForwardBrakeKey(ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow));
            DriverInput.ForwardSteerLeftKey(ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow));
            DriverInput.ForwardSteerRightKey(ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow));
            DriverInput.ForwardHandbrakeKey(ImGui::IsKeyDown(ImGuiKey_Space));
            if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            {
                Physics->RequestVehicleReset();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !Camera.QueryRouteTransitionActive())
            {
                PendingRoutePath  = ProjectRoot / "Content" / "Levels" / "Lobby.levelspace";
                PendingRouteDrive = false;
                Camera.BeginRouteTransition(0.48, 0.58);
            }

            const auto Command = DriverInput.Advance(static_cast<float>(DeltaSeconds));
            Frontier::PhysicsVehicleInput VehicleInput{};
            VehicleInput.Throttle    = Command.Drive.Throttle;
            VehicleInput.Brake       = Command.Drive.Brake;
            VehicleInput.Steer       = Command.Drive.Steer;
            VehicleInput.Handbrake   = Command.Drive.Handbrake;
            VehicleInput.ClutchPedal = (!IO.WantTextInput && ImGui::IsKeyDown(ImGuiKey_LeftShift)) ? 1.0f : 0.0f;
            Physics->AssignVehicleInput(VehicleInput);
            (void)Physics->Step(World, DeltaSeconds);
            for (const Frontier::PhysicsImpactEvent& Event : Physics->DrainImpactEvents())
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
                ImpactSparks.push_back(Spark);
            }
        }

        // ② Advance VehicleHMI + Studio Lobby cinematic + transition camera.
        Frontier::VehicleCameraInput CameraInput{};
        if (!IO.WantCaptureMouse)
        {
            CameraInput.PointerDeltaPixels = {static_cast<double>(IO.MouseDelta.x), static_cast<double>(IO.MouseDelta.y)};
            CameraInput.WheelNotches       = static_cast<double>(IO.MouseWheel);
            CameraInput.PrimaryDrag        = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        }
        if (!IO.WantTextInput)
        {
            CameraInput.CycleNextRig     = !IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false);
            CameraInput.CyclePreviousRig = IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false);
            if (ImGui::IsKeyPressed(ImGuiKey_M, false))
            {
                Camera.TransitionToRig(Camera.QueryRig() == Frontier::VehicleCameraRig::Hmi ? Frontier::VehicleCameraRig::Cockpit
                                                                                            : Frontier::VehicleCameraRig::Hmi);
            }
        }
        CameraInput.AnyActivity = CameraInput.PrimaryDrag || std::abs(CameraInput.WheelNotches) > 0.0;

        const auto Subject  = SampleRuntimeVehicleSubject(World, Aspect);
        const auto Solution = Camera.Advance(Subject, CameraInput, DeltaSeconds);

        if (Camera.ConsumeRouteSwitchPoint() && !PendingRoutePath.empty())
        {
            if (Physics && Physics->QueryRunning())
            {
                Physics->End(World);
                DriverInput = Frontier::Vehicle::DriverInputIntegrator{};
            }
            const std::filesystem::path TargetPath = std::exchange(PendingRoutePath, {});
            if (Workspace->OpenLevel(TargetPath, false))
            {
                PublishedGeometryGen     = 0u;
                PendingCoursePhysicsBoot = PendingRouteDrive;
                if (MainWindow != nullptr)
                {
                    glfwSetWindowTitle(MainWindow, Workspace->QueryLobbyActive() ? "Everest — Studio Lobby" : "Everest — Proving Grounds");
                }
            }
        }

        // ③ Render fullscreen Vulkan SceneViewport image.
        if (!SceneRenderer && Presentation != nullptr)
        {
            const Frontier::VulkanDeviceAccess Device = Presentation->QueryDeviceAccess();
            SceneRenderer = std::make_unique<Frontier::RenderingExtension>(ProjectRoot, ShaderArchives, &Device);
        }

        uint64_t TextureHandle = 0u;
        Frontier::SceneViewportRefresh RenderResult{};
        if (SceneRenderer)
        {
            Frontier::SceneViewportRequest Request{};
            Request.PanelWidth          = Width;
            Request.PanelHeight         = Height;
            Request.PixelDensity        = Viewport->FramebufferScale.y > 0.0f ? Viewport->FramebufferScale.y : 1.0f;
            Request.CameraPosition      = Solution.PositionMetres;
            Request.CameraTarget        = Solution.TargetMetres;
            Request.CameraUp            = Solution.UpUnit;
            Request.VerticalFieldOfView = Solution.VerticalFieldOfViewRadians;
            Request.CameraNear          = Solution.NearClipMetres;
            Request.DeltaSeconds        = DeltaSeconds;
            Request.CameraMotionBlur    = Workspace->QueryVehicleCamera().QueryMotionBlur();
            Request.ImpactSparks        = std::move(ImpactSparks);
            RenderResult = SceneRenderer->Render(Request, World, Workspace->QueryElapsedSeconds());
            TextureHandle = SceneTexture.Resolve(RenderResult.View);
            if (RenderResult.Readout.Geometry && RenderResult.Readout.GeometryGeneration != PublishedGeometryGen)
            {
                PublishGeometryResolutions(World, RenderResult.Readout.Geometry);
                PublishedGeometryGen = RenderResult.Readout.GeometryGeneration;
            }
            if (PendingCoursePhysicsBoot && Physics && RenderResult.Readout.Geometry)
            {
                StartCoursePhysics(*Physics, World);
                PendingCoursePhysicsBoot = false;
            }
        }

        // ④ Fullscreen canvas + clean standalone game HUD + fade veil.
        ImGui::SetNextWindowPos(Viewport->Pos);
        ImGui::SetNextWindowSize(Viewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##EverestRuntimeCanvas", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImDrawList*  DrawList = ImGui::GetWindowDrawList();
        const ImVec2 StageMin = Viewport->Pos;
        const ImVec2 StageMax(StageMin.x + Width, StageMin.y + Height);
        if (TextureHandle != 0u)
        {
            DrawList->AddImage(static_cast<ImTextureID>(TextureHandle), StageMin, StageMax);
        }
        else
        {
            DrawList->AddRectFilled(StageMin, StageMax, IM_COL32(10, 14, 20, 255));
        }

        if (!HudHidden)
        {
            if (Workspace->QueryLobbyActive())
            {
                const ImVec2 CardMin(StageMin.x + 20.0f, StageMin.y + 20.0f);
                const ImVec2 CardMax(CardMin.x + 310.0f, CardMin.y + 240.0f);
                DrawList->AddRectFilled(CardMin, CardMax, IM_COL32(12, 16, 22, 218), 10.0f);
                DrawList->AddRect(CardMin, CardMax, IM_COL32(96, 122, 152, 96), 10.0f);

                const uint32_t ActiveDrivers = Lobby.QueryActiveVehicleCount();
                char           TitleLine[96];
                if (Solution.CinematicActive)
                {
                    std::snprintf(TitleLine, sizeof(TitleLine), "EVEREST STUDIO LOBBY  \xC2\xB7  %u/5  \xC2\xB7  SHOT %u/6",
                                  ActiveDrivers, Solution.CinematicShotIndex + 1u);
                }
                else
                {
                    std::snprintf(TitleLine, sizeof(TitleLine), "EVEREST STUDIO LOBBY  \xC2\xB7  %u/5 DRIVERS", ActiveDrivers);
                }
                DrawList->AddText(ImVec2(CardMin.x + 14.0f, CardMin.y + 12.0f), IM_COL32(230, 238, 248, 245), TitleLine);

                const auto& Presets = Everest::QueryLobbyPresetCallsigns();
                for (uint32_t Slot = 0u; Slot < 5u; ++Slot)
                {
                    const auto* Entry = Lobby.QuerySession().FindParticipantInSlot(Slot);
                    char        Line[96];
                    ImU32       Colour = IM_COL32(120, 132, 148, 175);
                    if (Entry != nullptr)
                    {
                        std::snprintf(Line, sizeof(Line), "P%u  %-8s  \xC2\xB7  %s%s",
                                      Slot + 1u, Entry->Callsign.c_str(), Entry->RankBadge.c_str(), Entry->LocalDriver ? "  [HOST]" : "");
                        Colour = Entry->LocalDriver ? IM_COL32(142, 220, 178, 245) : IM_COL32(188, 212, 238, 235);
                    }
                    else
                    {
                        std::snprintf(Line, sizeof(Line), "P%u  Stand vacant  (%s)", Slot + 1u, std::string(Presets[Slot].Callsign).c_str());
                    }
                    DrawList->AddText(ImVec2(CardMin.x + 14.0f, CardMin.y + 38.0f + static_cast<float>(Slot) * 22.0f), Colour, Line);
                }

                ImGui::SetCursorScreenPos(ImVec2(CardMin.x + 12.0f, CardMin.y + 154.0f));
                ImGui::BeginDisabled(ActiveDrivers >= 5u);
                if (ImGui::Button("+ Join##RuntimeJoin")) (void)Lobby.JoinGuestDriver(World);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(ActiveDrivers <= 1u);
                if (ImGui::Button("- Leave##RuntimeLeave")) (void)Lobby.LeaveLastGuest(World);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Orbit")) Camera.TransitionToRig(Frontier::VehicleCameraRig::Orbit);
                ImGui::SameLine();
                if (ImGui::Button("Cabin")) Camera.TransitionToRig(Frontier::VehicleCameraRig::Cockpit);
                ImGui::SameLine();
                if (ImGui::Button("HMI")) Camera.TransitionToRig(Frontier::VehicleCameraRig::Hmi);

                ImGui::SetCursorScreenPos(ImVec2(CardMin.x + 12.0f, CardMin.y + 194.0f));
                if (ImGui::Button("Enter Proving Grounds  (Enter)", ImVec2(286.0f, 34.0f)) && !Camera.QueryRouteTransitionActive())
                {
                    PendingRoutePath  = ProjectRoot / "Content" / "Levels" / "ProvingGroundVehicle.levelspace";
                    PendingRouteDrive = true;
                    Camera.BeginRouteTransition(0.48, 0.58);
                }
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(StageMin.x + 20.0f, StageMin.y + 20.0f));
                if (ImGui::Button("Studio Lobby (L)##RuntimeBack") && !Camera.QueryRouteTransitionActive())
                {
                    PendingRoutePath  = ProjectRoot / "Content" / "Levels" / "Lobby.levelspace";
                    PendingRouteDrive = false;
                    Camera.BeginRouteTransition(0.48, 0.58);
                }
                ImGui::SameLine();
                const std::string CamText = "Camera: " + std::string(Frontier::QueryVehicleCameraRigLabel(Camera.QueryRig())) + " (C)";
                if (ImGui::Button(CamText.c_str()))
                {
                    Camera.CycleRig(false);
                }
                static constexpr std::array<const char*, 5> StationButtons = {
                    "Straight##RtSt0", "Slope Ramp 12.7\xC2\xB0##RtSt1", "Skidpad##RtSt2", "Slalom##RtSt3", "Hangar##RtSt4"};
                for (uint32_t StationIdx = 0u; StationIdx < StationButtons.size(); ++StationIdx)
                {
                    ImGui::SameLine();
                    if (ImGui::Button(StationButtons[StationIdx]) && Physics)
                    {
                        if (Physics->QueryRunning()) Physics->End(World);
                        if (Lobby.DeployProvingGroundStation(World, StationIdx))
                        {
                            DriverInput = Frontier::Vehicle::DriverInputIntegrator{};
                            StartCoursePhysics(*Physics, World);
                        }
                    }
                }
                ImGui::SameLine();
                char SpeedText[80];
                std::snprintf(SpeedText, sizeof(SpeedText), "%.0f km/h  |  WASD/Space/Shift Drive  |  R Reset", Subject.SpeedMetresPerSecond * 3.6);
                ImGui::TextUnformatted(SpeedText);
            }
        }

        if (Solution.VeilOpacity > 0.002)
        {
            const int Alpha = std::clamp(static_cast<int>(std::lround(Solution.VeilOpacity * 255.0)), 0, 255);
            DrawList->AddRectFilled(StageMin, StageMax, IM_COL32(6, 8, 12, Alpha));
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    };

    Callbacks.Finalize = [&]()
    {
        if (Presentation != nullptr)
        {
            Presentation->AwaitCompletion();
        }
        if (Physics && Workspace && Physics->QueryRunning())
        {
            Physics->End(Workspace->QueryWorldSpace());
        }
        SceneTexture.Retire();
        SceneRenderer.reset();
        Physics.reset();
        Workspace.reset();
    };

    return Frontier::RunWindowsApplication(Callbacks);
}
