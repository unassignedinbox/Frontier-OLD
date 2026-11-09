/*==========================================================================================================================================
                                                       LOBBYWORKSPACECAPTURE.CPP
==========================================================================================================================================*/
// 🧩 Offscreen Vulkan/SwiftShader GPU + Frontier Editor UI capture of Studio Lobby & Proving Ground Slope Equilibrium:
//      01-EverestLobbySoloStartup.png                 — Default startup: 1 hero car on centre turntable, empty ring stands, full Editor UI
//      02-EverestLobbyThreeDrivers.png                — Dynamic join: 3/5 drivers (FALCON-1 + KESTREL + HARRIER)
//      03-EverestLobbyFullFiveDrivers.png             — Full 5/5 studio roster on turntable + 4 ring plinths
//      04-EverestLobbyCinematicTour.png               — Lobby-only cinematic camera tour shot framing
//      05-EverestProvingGroundTransition.png          — Live archive switch (Lobby -> ProvingGround) on the same RenderingExtension
//      06-EverestProvingGroundSlopeRampEquilibrium.png — 4-corner 12.68 deg slope equilibrium + brake hold on Proving Ground Jump ramp

#include "Engine/Interface/IconPresentation.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "EverestWorkspace.h"
#include "Exhibits/ControlCentre/Code/CpuRasterization.h"
#include "Frontier/Extensions/Physics/PhysicsExtension.h"
#include "Frontier/Extensions/Rendering/RenderingExtension.h"
#include "UI/Editor/ControlCentrePanel.h"

#include <imgui.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

constexpr int   CaptureWidth     = 1920;
constexpr int   CaptureHeight    = 1080;
constexpr ImU64 SceneTextureSlot = 0x4C4F4242595650ULL;

void Require(bool Condition, const std::string& Message)
{
    if (!Condition)
    {
        throw std::runtime_error(Message);
    }
}

} // namespace

int main(int ArgumentCount, char** Arguments)
{
    try
    {
        Require(ArgumentCount == 5,
                "Usage: LobbyWorkspaceCapture <EngineContent> <Projects/Everest> <ShaderArchives> <OutputDirectory>");
        const std::filesystem::path ContentDir = std::filesystem::absolute(Arguments[1]);
        const std::filesystem::path EverestDir = std::filesystem::absolute(Arguments[2]);
        const std::filesystem::path ShaderDir  = std::filesystem::absolute(Arguments[3]);
        const std::filesystem::path OutputDir  = std::filesystem::absolute(Arguments[4]);
        std::filesystem::create_directories(OutputDir);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& Io    = ImGui::GetIO();
        Io.IniFilename = nullptr;
        Io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        Io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth  = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        Frontier::IconPresentation::Attach(ContentDir / "Icons");

        const std::filesystem::path ScratchPrefs = OutputDir / "LobbyCapturePrefs.projectspace";
        std::filesystem::remove(ScratchPrefs);

        Frontier::ActivationOwnership Ownership(
            new Frontier::ControlCentrePanel(ContentDir / "FontArchives", ScratchPrefs, EverestDir / "Everest.projectspace"));
        auto* Centre = static_cast<Frontier::ControlCentrePanel*>(Ownership.get());
        Require(Centre->Initialize() && Centre->Activate(), "ControlCentrePanel initialization failed");

        Everest::EverestWorkspace     Workspace(*Centre, EverestDir);
        Require(Workspace.QueryLobbyActive(), "EverestWorkspace must boot into Studio Lobby by default");
        Frontier::RenderingExtension  Bridge(EverestDir, ShaderDir, nullptr);
        std::vector<uint8_t>          ViewportPixels;
        std::array<uint32_t, 2>       ViewportExtent = {0u, 0u};
        std::string                   LastStatus;

        // Quality tier 1 (medium) converges rapidly on SwiftShader while keeping full ReSTIR GI + reflections + studio luminaires.
        const auto ConfigureMediumQuality = [&]()
        {
            if (auto* Pipeline = dynamic_cast<Frontier::RenderPipelineRecord*>(Workspace.QueryWorldSpace().FindFirstByType("RenderPipeline")))
            {
                Pipeline->AccessConfiguration().QualityTier = 1;
                Pipeline->AccessConfiguration().AdvanceRevision();
            }
        };
        ConfigureMediumQuality();

        Workspace.PresentSceneViewport = [&](const Frontier::SceneViewportRequest& Request)
        {
            Frontier::SceneViewportImage         Image{};
            const Frontier::SceneViewportRefresh Outcome = Bridge.Render(Request, Workspace.QueryWorldSpace(), Workspace.QueryElapsedSeconds());
            Image.Status                                 = Outcome.Status;
            Image.Readout                                = Outcome.Readout;
            LastStatus                                   = Outcome.Status;
            if (Outcome.View != VK_NULL_HANDLE && Bridge.AccessViewport() != nullptr && Bridge.AccessViewport()->ReadPresented(ViewportPixels))
            {
                ViewportExtent = Outcome.Extent;
                Image.Texture  = static_cast<uint64_t>(SceneTextureSlot);
            }
            return Image;
        };

        const auto RenderFrames = [&](int Count, double StepSeconds = 1.0 / 30.0)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                Centre->PrepareInterface();
                Io.DisplaySize = ImVec2(static_cast<float>(CaptureWidth), static_cast<float>(CaptureHeight));
                Io.DeltaTime   = static_cast<float>(StepSeconds);
                ImGui::NewFrame();
                (void)Centre->Integrate(StepSeconds);
                (void)Workspace.ConfigurePlatform();
                Workspace.ConstructInterface(StepSeconds);
                Centre->ConstructInterface();
                ImGui::Render();
                FrontierExhibit::AcknowledgeTextures();
            }
        };

        const auto WriteCapture = [&](const std::filesystem::path& FilePath)
        {
            Require(!ViewportPixels.empty() && ViewportExtent[0] > 0u && ViewportExtent[1] > 0u,
                    "SceneViewport produced no pixels: " + LastStatus);
            const ImDrawData*          DrawData = ImGui::GetDrawData();
            std::vector<unsigned char> Canvas(static_cast<size_t>(CaptureWidth) * CaptureHeight * 3u, 14u);
            const std::function<FrontierExhibit::Image(ImTextureID)> ResolveTexture = [&](ImTextureID Texture)
            {
                if (static_cast<ImU64>(Texture) == SceneTextureSlot)
                {
                    return FrontierExhibit::Image{ViewportPixels.data(), static_cast<int>(ViewportExtent[0]), static_cast<int>(ViewportExtent[1]), 4};
                }
                return FrontierExhibit::Image{};
            };
            for (const ImDrawList* List : DrawData->CmdLists)
            {
                FrontierExhibit::RasterizeCommands(List, Canvas.data(), CaptureWidth, CaptureHeight, DrawData->DisplayPos,
                                                   DrawData->FramebufferScale, nullptr, &ResolveTexture);
            }
            Require(stbi_write_png(FilePath.string().c_str(), CaptureWidth, CaptureHeight, 3, Canvas.data(), CaptureWidth * 3) != 0,
                    "Failed writing PNG: " + FilePath.string());
            std::cout << "[LobbyWorkspaceCapture] Wrote " << FilePath.filename().string() << " (" << LastStatus << ")\n";
        };

        // 01: Solo hero car on centre turntable (settled front 3/4 studio Orbit, 1/5 drivers, full Editor UI).
        if (auto* HeroRec = Workspace.QueryWorldSpace().FindFirstByType("Vehicle"))
        {
            Workspace.QueryOutliner().Select(HeroRec->QueryToken());
        }
        Workspace.AccessVehicleCamera().ConfigureRigImmediate(Frontier::VehicleCameraRig::Orbit, Frontier::VehicleSubjectKinematics{});
        RenderFrames(6);
        WriteCapture(OutputDir / "01-EverestLobbySoloStartup.png");

        // 02: Dynamically join 2 guest drivers (3/5 total) — KESTREL (NE) and MIRAGE (NW) appear on their plinths!
        // Pull back slightly within studio bounds so hero + ring stands read clearly in one frame.
        (void)Workspace.AccessLobbySession().JoinGuestDriver(Workspace.QueryWorldSpace());
        (void)Workspace.AccessLobbySession().JoinGuestDriver(Workspace.QueryWorldSpace());
        {
            Frontier::VehicleCameraInput ZoomOut{};
            ZoomOut.WheelNotches = -3.0;
            (void)Workspace.AccessVehicleCamera().Advance(Frontier::VehicleSubjectKinematics{}, ZoomOut, 0.05);
        }
        RenderFrames(5);
        WriteCapture(OutputDir / "02-EverestLobbyThreeDrivers.png");

        // 03: Join remaining 2 drivers (5/5 full studio roster) and frame high studio overview (Shot 6 angle).
        (void)Workspace.AccessLobbySession().JoinGuestDriver(Workspace.QueryWorldSpace());
        (void)Workspace.AccessLobbySession().JoinGuestDriver(Workspace.QueryWorldSpace());
        Workspace.AccessVehicleCamera().ConfigureEnvironment(true);
        // Advance past 7.0 s dolly + 60/10 s idle directly into Shot 5 (high studio softbox & 5-plinth overview)!
        for (int Tick = 0; Tick < 175; ++Tick)
        {
            (void)Workspace.AccessVehicleCamera().Advance(Frontier::VehicleSubjectKinematics{}, Frontier::VehicleCameraInput{}, 0.1);
        }
        // Advance through Shots 0..4 into Shot 5 (overview of all 5 stands) where VeilOpacity == 0.0!
        while (!(Workspace.AccessVehicleCamera().QueryLastSolution().CinematicActive &&
                 Workspace.AccessVehicleCamera().QueryLastSolution().CinematicShotIndex == 5u &&
                 Workspace.AccessVehicleCamera().QueryLastSolution().VeilOpacity < 0.01))
        {
            (void)Workspace.AccessVehicleCamera().Advance(Frontier::VehicleSubjectKinematics{}, Frontier::VehicleCameraInput{}, 0.2);
        }
        RenderFrames(5, 1.0 / 60.0);
        WriteCapture(OutputDir / "03-EverestLobbyFullFiveDrivers.png");

        // 04: Lobby Cinematic Camera close-up shot (Shot 1 — Front-left tyre & hub / flank showcase with solo hero car).
        Workspace.AccessLobbySession().ResetSolo(Workspace.QueryWorldSpace());
        Workspace.AccessVehicleCamera().ConfigureEnvironment(true);
        while (!(Workspace.AccessVehicleCamera().QueryLastSolution().CinematicActive &&
                 Workspace.AccessVehicleCamera().QueryLastSolution().CinematicShotIndex == 1u &&
                 Workspace.AccessVehicleCamera().QueryLastSolution().VeilOpacity < 0.01))
        {
            (void)Workspace.AccessVehicleCamera().Advance(Frontier::VehicleSubjectKinematics{}, Frontier::VehicleCameraInput{}, 0.2);
        }
        RenderFrames(5, 1.0 / 60.0);
        WriteCapture(OutputDir / "04-EverestLobbyCinematicTour.png");

        // 05: Live transition from Studio Lobby -> Proving Grounds on the SAME RenderingExtension instance + live driving!
        Require(Workspace.OpenLevel(EverestDir / "Content/Levels/ProvingGroundVehicle.levelspace", true),
                "Failed switching from Lobby to ProvingGroundVehicle.levelspace");
        ConfigureMediumQuality();
        RenderFrames(6, 1.0 / 30.0);
        WriteCapture(OutputDir / "05-EverestProvingGroundTransition.png");

        // 06: Proving Ground TerrainEquilibrium station 1 ("Slope Ramp (12.7 deg)") — 4-corner slope alignment + brake hold!
        if (Workspace.AccessPhysics() && Workspace.AccessPhysics()->QueryRunning()) Workspace.ToggleSimulation();
        Require(Workspace.AccessLobbySession().DeployProvingGroundStation(Workspace.QueryWorldSpace(), 1u),
                "Failed deploying Proving Ground Slope Ramp station");
        Workspace.ToggleSimulation();
        // Frame a 3/4 side-profile orbit view so the 12.68 deg uphill ramp angle and wheel contact read clearly!
        Workspace.AccessVehicleCamera().TransitionToRig(Frontier::VehicleCameraRig::Orbit);
        for (int Step = 0; Step < 25; ++Step)
        {
            Frontier::VehicleCameraInput SidePan{};
            SidePan.PointerDeltaPixels = {Step == 0 ? 220.0 : 0.0, 0.0};
            SidePan.PrimaryDrag        = (Step == 0);
            (void)Workspace.AccessVehicleCamera().Advance(Frontier::VehicleSubjectKinematics{}, SidePan, 0.1);
        }
        RenderFrames(10, 1.0 / 30.0);
        WriteCapture(OutputDir / "06-EverestProvingGroundSlopeRampEquilibrium.png");
        if (Workspace.AccessPhysics() && Workspace.AccessPhysics()->QueryRunning()) Workspace.ToggleSimulation();

        std::filesystem::remove(ScratchPrefs);
        Ownership.reset();
        FrontierExhibit::AcknowledgeTextures();
        ImGui::GetIO().BackendFlags = 0;
        ImGui::DestroyContext();
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "[LobbyWorkspaceCapture] FAILED: " << Error.what() << "\n";
        return 1;
    }
}
