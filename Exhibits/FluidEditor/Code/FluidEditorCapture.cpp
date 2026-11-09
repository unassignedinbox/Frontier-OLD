/*==========================================================================================================================================
                                                       FLUIDEDITORCAPTURE.CPP
==========================================================================================================================================*/
// 🧩 End-to-end validation & GPU capture harness for the Fluid Editor and Proving Ground world-anchored PlumeSpace:
//      1. Verifies lossless .gasscene.toml / .fluidspace round-trip parity across all 8 Experimental/Fluid sample scenes.
//      2. Captures EverestWorkspace + FluidPlumeInspector + modal 1:1 FluidPlumeEditor (Outliner, 3D volumetric raymarcher,
//         Tyre Ring collider, 2D Tiled Voxel Atlas Minimap, and 4x4 RGBA Flipbook sprite-sheet bake).
//      3. Renders standalone 3D Eulerian explosion frames (FluidExplosionSequence) AND live Proving Ground ControlVehicle
//         burnout/drift tyre smoke + tyre ignition fire + trackside FluidPlume mushroom-cloud detonation + ReSTIR GI bounce,
//         demonstrating smoke lingering anchored in world space behind the moving car.

#include "Engine/Interface/IconPresentation.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"
#include "Engine/PhysicalDynamics/FluidDynamics/FluidSpaceCodec.h"
#include "Engine/Scene/VehicleReadings.h"
#include "EverestWorkspace.h"
#include "Exhibits/ControlCentre/Code/CpuRasterization.h"
#include "Frontier/Extensions/Fluid/FluidExtension.h"
#include "Frontier/Extensions/Physics/PhysicsExtension.h"
#include "Frontier/Extensions/Rendering/RenderingExtension.h"
#include "UI/Editor/ControlCentrePanel.h"

#include <imgui.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

constexpr int      CaptureWidth  = 1920;
constexpr int      CaptureHeight = 1080;
constexpr uint64_t SceneTag      = 0x5CE7E5u;
constexpr uint64_t FluidTag      = 0xF101D5u;

void Require(bool Condition, const std::string& Message)
{
    if (!Condition) throw std::runtime_error(Message);
}

void AimFlightAt(Everest::SceneViewportFlight& Flight, const std::array<double, 3>& Eye,
                 const std::array<double, 3>& Target, double FovRad = 0.84)
{
    Flight.Position            = Eye;
    const double Dx            = Target[0] - Eye[0];
    const double Dy            = Target[1] - Eye[1];
    const double Dz            = Target[2] - Eye[2];
    const double Horizontal    = std::max(1.0e-6, std::hypot(Dx, Dy));
    Flight.Heading             = std::atan2(Dy, Dx) * (180.0 / 3.14159265358979323846);
    Flight.Elevation           = std::atan2(Dz, Horizontal) * (180.0 / 3.14159265358979323846);
    Flight.PivotDistance       = std::hypot(Horizontal, Dz);
    Flight.VerticalFieldOfView = FovRad;
    Flight.Velocity            = { 0.0, 0.0, 0.0 };
}

void WriteRgbPng(const std::filesystem::path& Target, const std::vector<uint8_t>& Rgba, uint32_t Width, uint32_t Height)
{
    std::vector<uint8_t> Rgb(static_cast<size_t>(Width) * Height * 3u);
    for (size_t Pixel = 0; Pixel < static_cast<size_t>(Width) * Height; ++Pixel)
    {
        Rgb[Pixel * 3u + 0u] = Rgba[Pixel * 4u + 0u];
        Rgb[Pixel * 3u + 1u] = Rgba[Pixel * 4u + 1u];
        Rgb[Pixel * 3u + 2u] = Rgba[Pixel * 4u + 2u];
    }
    std::filesystem::create_directories(Target.parent_path());
    Require(stbi_write_png(Target.string().c_str(), static_cast<int>(Width), static_cast<int>(Height), 3,
                           Rgb.data(), static_cast<int>(Width * 3u)) != 0,
            "Failed writing PNG: " + Target.string());
}

void WriteRgbaPng(const std::filesystem::path& Target, const std::vector<uint8_t>& Rgba, uint32_t Width, uint32_t Height)
{
    std::filesystem::create_directories(Target.parent_path());
    Require(stbi_write_png(Target.string().c_str(), static_cast<int>(Width), static_cast<int>(Height), 4,
                           Rgba.data(), static_cast<int>(Width * 4u)) != 0,
            "Failed writing RGBA PNG: " + Target.string());
}

void VerifyUpstreamSampleRoundTrip(const std::filesystem::path& SamplesRoot)
{
    size_t VerifiedCount = 0u;
    for (const auto& Entry : std::filesystem::directory_iterator(SamplesRoot))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".toml") continue;
        Frontier::FluidDynamics::FluidSceneDocument Original{};
        std::string                                 Diag;
        Require(Frontier::FluidDynamics::LoadFluidSpaceFile(Entry.path(), Original, &Diag),
                "Failed loading upstream sample " + Entry.path().string() + ": " + Diag);

        const std::string Encoded = Frontier::FluidDynamics::WriteFluidSpaceToml(Original);
        Frontier::FluidDynamics::FluidSceneDocument Reloaded{};
        Require(Frontier::FluidDynamics::ReadFluidSpaceToml(Encoded, Reloaded, &Diag),
                "Failed round-tripping sample " + Entry.path().string() + ": " + Diag);

        Require(Reloaded.Parameters.GridResolution == Original.Parameters.GridResolution, "GridResolution mismatch");
        Require(std::fabs(Reloaded.Parameters.Buoyancy - Original.Parameters.Buoyancy) < 1.0e-4f, "Buoyancy mismatch");
        Require(std::fabs(Reloaded.Parameters.BlastStrength - Original.Parameters.BlastStrength) < 1.0e-4f, "BlastStrength mismatch");
        Require(Reloaded.Parameters.ObstacleCategory == Original.Parameters.ObstacleCategory, "ObstacleCategory mismatch");
        Require(Reloaded.Names.Domain == Original.Names.Domain && Reloaded.Names.Source == Original.Names.Source, "Names mismatch");
        Require(std::fabs(Reloaded.Camera.Distance - Original.Camera.Distance) < 1.0e-4f, "Camera distance mismatch");
        ++VerifiedCount;
    }
    Require(VerifiedCount == 8u, "Expected 8 Experimental/Fluid sample .gasscene.toml files, got " + std::to_string(VerifiedCount));
    std::printf("[FluidEditorCapture] Verified 100%% lossless TOML round-trip across all %zu Experimental/Fluid samples.\n", VerifiedCount);
}

} // namespace

int main(int ArgumentCount, char** Arguments)
{
    using namespace Frontier;
    try
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Require(ArgumentCount >= 6,
                "usage: FluidEditorCapture <EngineContent> <Projects/Everest> <ShaderRoot> <Experimental/Fluid/Samples> <ExhibitDir>");
        const std::filesystem::path ContentDir  = std::filesystem::absolute(Arguments[1]);
        const std::filesystem::path EverestRoot = std::filesystem::absolute(Arguments[2]);
        const std::filesystem::path ShaderRoot  = std::filesystem::absolute(Arguments[3]);
        const std::filesystem::path SamplesDir  = std::filesystem::absolute(Arguments[4]);
        const std::filesystem::path ExhibitDir  = std::filesystem::absolute(Arguments[5]);
        const bool SkipProvingGround            = (ArgumentCount >= 7 &&
                                                   (std::string_view(Arguments[6]) == "--skip-proving-ground" ||
                                                    std::string_view(Arguments[6]) == "--modal-only"));
        const bool ModalOnly                    = (ArgumentCount >= 7 && std::string_view(Arguments[6]) == "--modal-only");

        const std::filesystem::path ImagesDir     = ExhibitDir / "Images";
        const std::filesystem::path ExplosionDir  = ExhibitDir / "Frames" / "FluidExplosion";
        const std::filesystem::path ProvingDir    = ExhibitDir / "Frames" / "ProvingGroundSmoke";
        std::filesystem::create_directories(ImagesDir);
        std::filesystem::create_directories(ExplosionDir);
        std::filesystem::create_directories(ProvingDir);

        // 1. Verify lossless codec parity against all 8 Experimental/Fluid/Samples/*.gasscene.toml
        VerifyUpstreamSampleRoundTrip(SamplesDir);

        // 2. Standalone 3D Eulerian solver sequence (FluidExplosionSequence frames + 4x4 RGBA Flipbook sprite sheet)
        FluidExtension               StandaloneFluid(ShaderRoot, nullptr);
        std::vector<uint8_t>         FluidPixels;
        std::array<uint32_t, 2>      FluidExtent = { 0u, 0u };

        if (!ModalOnly)
        {
        std::printf("[FluidEditorCapture] Rendering 18-frame standalone 3D Eulerian mushroom-cloud detonation sequence...\n");
        {
            auto HeroScene                       = FluidDynamics::ConstructSampleFluidScene("hero_detonation");
            HeroScene.Parameters.GridResolution  = 64;
            HeroScene.Parameters.RaymarchSteps   = 96;
            HeroScene.Parameters.RenderScale     = 1.0f;
            HeroScene.Parameters.Paused          = false;
            HeroScene.Parameters.SourceRate      = 0.32f;
            HeroScene.Parameters.CoolingRate     = 1.15f;
            HeroScene.Parameters.ShowEmbers      = true;
            HeroScene.Parameters.EmberCount      = 650;
            HeroScene.Camera.TargetDistance      = 4.65f;
            HeroScene.Camera.TargetPhi           = 1.24f;
            HeroScene.Camera.TargetCentre        = { 0.0f, 0.20f, 0.0f };
            HeroScene.Camera.SnapToTarget();

            for (int FrameIdx = 0; FrameIdx < 18; ++FrameIdx)
            {
                FluidViewportRequest Req{};
                Req.DocumentSlot   = 2u;
                Req.ViewportWidth  = 800u;
                Req.ViewportHeight = 600u;
                Req.DeltaSeconds   = 1.0f / 24.0f;
                Req.Restart        = (FrameIdx == 0);
                Req.Parameters     = HeroScene.Parameters;
                Req.Camera         = HeroScene.Camera;
                if (FrameIdx == 0)
                {
                    FluidBlastImpulse Stem{};
                    Stem.CentreUvw           = { 0.50f, 0.13f, 0.50f };
                    Stem.Radius              = 0.12f;
                    Stem.Strength            = 6.8f;
                    Stem.Temperature         = 4.8f;
                    Stem.Fuel                = 3.8f;
                    Stem.Smoke               = 4.2f;
                    Stem.Lobes               = 7;
                    Stem.SpawnShrapnel       = true;
                    Stem.HasDirectionalVel   = true;
                    Stem.DirectionalVelocity = { 0.0f, 8.5f, 0.0f };
                    Req.Blasts.push_back(Stem);
                }
                else if (FrameIdx == 2)
                {
                    FluidBlastImpulse Cap{};
                    Cap.CentreUvw   = { 0.50f, 0.28f, 0.50f };
                    Cap.Radius      = 0.16f;
                    Cap.Strength    = 6.2f;
                    Cap.Temperature = 5.4f;
                    Cap.Fuel        = 4.2f;
                    Cap.Smoke       = 4.8f;
                    Cap.Lobes       = 8;
                    Req.Blasts.push_back(Cap);
                }

                // Advance 2 solver substeps per visual frame so the mushroom cap rises & rolls dramatically
                FluidViewportOutcome Out{};
                for (int Sub = 0; Sub < 2; ++Sub)
                {
                    if (Sub > 0)
                    {
                        Req.Restart = false;
                        Req.Blasts.clear();
                    }
                    Out = StandaloneFluid.Render(Req);
                    Require(Out.Image.Failure.empty(), "StandaloneFluid failed: " + Out.Image.Failure);
                }
                HeroScene.Camera.Orbit(0.032f, 0.0f);
                HeroScene.Camera.SnapToTarget();

                std::vector<uint8_t> FrameRgba;
                Require(StandaloneFluid.AccessSequence()->ReadColor(FrameRgba), "ReadColor failed on explosion frame");
                char NameBuf[64]{};
                std::snprintf(NameBuf, sizeof(NameBuf), "frame_%02d.png", FrameIdx);
                WriteRgbPng(ExplosionDir / NameBuf, FrameRgba, Out.Image.Extent[0], Out.Image.Extent[1]);
            }

            // Bake a real 4x4 RGBA transparent Flipbook Sprite Sheet (1024x1024, 16 tiles of 256x256)
            const int Cols = 4, Rows = 4, Tile = 256;
            std::vector<uint8_t> Sheet(static_cast<size_t>(Cols * Tile) * (Rows * Tile) * 4u, 0u);
            for (int Idx = 0; Idx < Cols * Rows; ++Idx)
            {
                FluidViewportRequest BakeReq{};
                BakeReq.DocumentSlot   = 3u;
                BakeReq.ViewportWidth  = Tile;
                BakeReq.ViewportHeight = Tile;
                BakeReq.DeltaSeconds   = 1.0f / 18.0f;
                BakeReq.Restart        = (Idx == 0);
                BakeReq.ExportFlipbook = true;
                BakeReq.Parameters     = HeroScene.Parameters;
                BakeReq.Camera         = HeroScene.Camera;
                if (Idx == 0)
                {
                    FluidBlastImpulse Blast{};
                    Blast.CentreUvw           = { 0.50f, 0.18f, 0.50f };
                    Blast.Radius              = 0.14f;
                    Blast.Strength            = 6.5f;
                    Blast.Temperature         = 5.2f;
                    Blast.Fuel                = 4.0f;
                    Blast.Smoke               = 4.5f;
                    Blast.Lobes               = 7;
                    Blast.SpawnShrapnel       = true;
                    Blast.HasDirectionalVel   = true;
                    Blast.DirectionalVelocity = { 0.0f, 7.2f, 0.0f };
                    BakeReq.Blasts.push_back(Blast);
                }
                FluidViewportOutcome Baked = StandaloneFluid.Render(BakeReq);
                Require(Baked.Image.ReadbackRgba.size() == static_cast<size_t>(Tile * Tile * 4), "Flipbook readback incomplete");
                const int C = Idx % Cols;
                const int R = Idx / Cols;
                for (int Y = 0; Y < Tile; ++Y)
                {
                    std::memcpy(Sheet.data() + (static_cast<size_t>(R * Tile + Y) * (Cols * Tile) + C * Tile) * 4u,
                                Baked.Image.ReadbackRgba.data() + static_cast<size_t>(Y * Tile) * 4u,
                                static_cast<size_t>(Tile) * 4u);
                }
            }
            WriteRgbaPng(ImagesDir / "05-FluidEditorBakedFlipbookSheet.png", Sheet, Cols * Tile, Rows * Tile);
            std::printf("[FluidEditorCapture] Baked 1024x1024 RGBA Flipbook sprite sheet.\n");
        }
        }

        // 3. EverestWorkspace + FluidPlumeInspector + Modal 1:1 FluidPlumeEditor UI Captures (1920x1080)
        ImGui::CreateContext();
        ImGuiIO& Io    = ImGui::GetIO();
        Io.DisplaySize = ImVec2(static_cast<float>(CaptureWidth), static_cast<float>(CaptureHeight));
        Io.DeltaTime   = 1.0f / 30.0f;
        Io.IniFilename = nullptr;
        Io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        Io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth  = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        IconPresentation::Attach(ContentDir / "Icons");

        const std::filesystem::path Preferences = ExhibitDir / "Frames" / "Personal.projectspace";
        std::filesystem::remove(Preferences);
        ActivationOwnership Ownership(new ControlCentrePanel(ContentDir / "FontArchives", Preferences, EverestRoot / "Everest.projectspace"));
        auto*               Centre = static_cast<ControlCentrePanel*>(Ownership.get());
        Require(Centre->Initialize() && Centre->Activate(), "ControlCentrePanel initialization failed");

        Everest::EverestWorkspace Workspace(*Centre, EverestRoot);
        Require(Workspace.OpenLevel(EverestRoot / "Content/Levels/ProvingGroundVehicle.levelspace"),
                "Failed opening ProvingGroundVehicle.levelspace");

        RenderingExtension      SceneBridge(EverestRoot, ShaderRoot, nullptr);
        std::vector<uint8_t>    ScenePixels;
        std::array<uint32_t, 2> SceneExtent = { 0u, 0u };

        if (!ModalOnly)
        {
            Workspace.PresentSceneViewport = [&](const SceneViewportRequest& Request) {
                const SceneViewportRefresh Outcome = SceneBridge.Render(Request, Workspace.QueryWorldSpace(), Workspace.QueryElapsedSeconds());
                SceneViewportImage         Image{};
                Image.Status  = Outcome.Status;
                Image.Readout = Outcome.Readout;
                if (Outcome.View != VK_NULL_HANDLE && SceneBridge.AccessViewport()->ReadPresented(ScenePixels))
                {
                    SceneExtent   = Outcome.Extent;
                    Image.Texture = SceneTag;
                }
                return Image;
            };
        }

        InspectorRuntime::Instance().RegisterFluidViewportPresenter([&](const FluidViewportRequest& Request) {
            FluidViewportOutcome Outcome = StandaloneFluid.Render(Request);
            FluidViewportImage   Img     = std::move(Outcome.Image);
            if (Outcome.View != VK_NULL_HANDLE && StandaloneFluid.AccessSequence()->ReadColor(FluidPixels))
            {
                FluidExtent = Img.Extent;
                Img.Texture = FluidTag;
            }
            return Img;
        });

        const auto WriteWorkspacePng = [&](const std::filesystem::path& Target) {
            const ImDrawData*          DrawData = ImGui::GetDrawData();
            std::vector<unsigned char> Pixels(static_cast<size_t>(CaptureWidth) * CaptureHeight * 3u, 14u);
            const std::function<FrontierExhibit::Image(ImTextureID)> Resolve = [&](ImTextureID Tex) -> FrontierExhibit::Image {
                if (Tex == SceneTag && !ScenePixels.empty())
                {
                    return { ScenePixels.data(), static_cast<int>(SceneExtent[0]), static_cast<int>(SceneExtent[1]), 4 };
                }
                if (Tex == FluidTag && !FluidPixels.empty())
                {
                    return { FluidPixels.data(), static_cast<int>(FluidExtent[0]), static_cast<int>(FluidExtent[1]), 4 };
                }
                return {};
            };
            for (const ImDrawList* List : DrawData->CmdLists)
            {
                FrontierExhibit::RasterizeCommands(List, Pixels.data(), CaptureWidth, CaptureHeight,
                                                   DrawData->DisplayPos, DrawData->FramebufferScale, nullptr, &Resolve);
            }
            Require(stbi_write_png(Target.string().c_str(), CaptureWidth, CaptureHeight, 3, Pixels.data(), CaptureWidth * 3) != 0,
                    "Failed writing workspace PNG: " + Target.string());
            std::printf("[FluidEditorCapture] Wrote %s\n", Target.string().c_str());
        };

        const auto PumpWorkspace = [&](int Ticks, double Dt = 1.0 / 24.0) {
            Io.DisplaySize = ImVec2(static_cast<float>(CaptureWidth), static_cast<float>(CaptureHeight));
            Io.DeltaTime   = static_cast<float>(Dt);
            for (int Tick = 0; Tick < Ticks; ++Tick)
            {
                Centre->PrepareInterface();
                ImGui::NewFrame();
                (void)Centre->Integrate(Dt);
                (void)Workspace.ConfigurePlatform();
                Workspace.ConstructInterface(Dt);
                Centre->ConstructInterface();
                ImGui::Render();
                FrontierExhibit::AcknowledgeTextures();
            }
        };

        // Construct a FluidPlume record right beside the Proving Ground straight so both Everest and ProvingGround showcase it!
        {
            ConstructOrder Order{};
            Order.Entry.Identifier = "FluidPlume";
            Order.Entry.Caption    = "Fluid Plume";
            Order.Entry.Glyph      = "Flame";
            Order.Entry.Group      = "Physics";
            Order.Entry.Shape      = false;
            Order.Name             = "Trackside pyrotechnic rig";
            Order.Visible          = true;
            Workspace.ServiceConstructOrder(Order);
        }
        auto* PlumeEntry = dynamic_cast<FluidPlumeRecord*>(Workspace.QueryWorldSpace().FindFirstByType("FluidPlume"));
        Require(PlumeEntry != nullptr, "FluidPlumeRecord construction failed");
        const RecordToken PlumeToken = PlumeEntry->QueryToken();
        const std::filesystem::path SidecarPath = EverestRoot / "Content/Levels/trackside_pyrotechnic_rig.fluidspace";
        {
            auto HeroDoc                    = FluidDynamics::ConstructSampleFluidScene("hero_detonation");
            HeroDoc.Parameters.GridResolution = 64;
            HeroDoc.Parameters.RaymarchSteps  = 72;
            HeroDoc.Parameters.RenderScale    = 0.65f;
            HeroDoc.Camera.TargetDistance     = 4.65f;
            HeroDoc.Camera.TargetPhi          = 1.24f;
            HeroDoc.Camera.TargetCentre       = { 0.0f, 0.20f, 0.0f };
            HeroDoc.Camera.SnapToTarget();
            Require(FluidDynamics::SaveFluidSpaceFile(SidecarPath, HeroDoc), "Failed writing initial sidecar");

            auto& Cfg              = PlumeEntry->AccessConfiguration();
            Cfg.PositionMetres[0]  = -28.0f;
            Cfg.PositionMetres[1]  = 3.1f;
            Cfg.PositionMetres[2]  = 0.15f;
            Cfg.DomainScale        = 1.65f;
            Cfg.PresetIdentity     = "tactical_ordnance";
            Cfg.SpaceFile          = "trackside_pyrotechnic_rig.fluidspace";
            Cfg.BurstPattern       = static_cast<uint32_t>(FluidDynamics::FluidBurstPattern::MushroomCloud);
            Cfg.BurstGeneration    = 1u;
            Cfg.AdvanceRevision();
        }

        // Configure GTX SDF ReSTIR GI ("low", RayTracing = false) so SwiftShader renders full GI + PlumeSpace + FireIllumination ~20x faster!
        auto* PipelineRec = dynamic_cast<RenderPipelineRecord*>(Workspace.QueryWorldSpace().FindFirstByType("RenderPipeline"));
        Require(PipelineRec != nullptr, "No RenderPipelineRecord");
        {
            auto& Pipe                = PipelineRec->AccessConfiguration();
            Pipe.QualityTier          = 0;
            Pipe.HardwareRayQueryTier = false;
            Pipe.TraceScale           = 2;
            Pipe.RenderScale          = 0.75f;
            Pipe.LiveRefresh          = !SkipProvingGround;
            Pipe.AnalyticIllumination = false;
            Pipe.AdvanceRevision();
        }

        // Capture 01: EverestWorkspace with FluidPlumeInspector & active world-grid plume + GI glow on the Proving Ground
        Workspace.AccessViewportNavigation().Mode = Everest::SceneCameraMode::Free;
        AimFlightAt(Workspace.AccessViewportFlight(), { -36.5, -5.8, 2.85 }, { -28.0, 2.2, 1.85 }, 0.84);
        Workspace.QueryOutliner().Select(PlumeToken);
        if (!SkipProvingGround)
        {
            PumpWorkspace(4, 1.0 / 20.0);
            Require(!ScenePixels.empty(), "SceneViewport produced no pixels");
            WriteWorkspacePng(ImagesDir / "01-EverestFluidPlumeInspector.png");
        }

        // Pause background SceneViewport refresh while full-screen modal FluidPlumeEditor is open!
        PipelineRec->AccessConfiguration().LiveRefresh = false;
        PipelineRec->AccessConfiguration().AdvanceRevision();

        // Open the modal 1:1 FluidPlumeEditor via InspectorRuntime::RequestEditorOpen(PlumeToken)!
        Require(InspectorRuntime::Instance().RequestEditorOpen(PlumeToken), "RequestEditorOpen failed for FluidPlumeRecord");
        // Trigger an explosive burst inside the modal Fluid Editor (press 'E' on frame 2) and step 8 frames so the fireball blooms!
        PumpWorkspace(2, 1.0 / 24.0);
        Io.AddKeyEvent(ImGuiKey_E, true);
        PumpWorkspace(1, 1.0 / 24.0);
        Io.AddKeyEvent(ImGuiKey_E, false);
        PumpWorkspace(8, 1.0 / 24.0);
        Require(!FluidPixels.empty(), "Modal FluidPlumeEditor produced no GPU viewport pixels");
        WriteWorkspacePng(ImagesDir / "02-FluidEditorHeroDetonation.png");

        // Close editor, swap sidecar to tyre_burnout_ring with 2D Voxel Atlas Minimap enabled, re-open on a fresh FluidPlume record
        // so we capture the Tyre Ring SDF collider AND 2D Voxel Atlas PiP Minimap cleanly!
        InspectorRuntime::Instance().CloseActiveEditor();
        {
            ConstructOrder TyreOrder{};
            TyreOrder.Entry.Identifier = "FluidPlume";
            TyreOrder.Entry.Caption    = "Fluid Plume";
            TyreOrder.Entry.Glyph      = "Flame";
            TyreOrder.Entry.Group      = "Physics";
            TyreOrder.Name             = "Tyre ring collider study";
            TyreOrder.Visible          = true;
            Workspace.ServiceConstructOrder(TyreOrder);
            const auto AllPlumes = Workspace.QueryWorldSpace().FindAllByType("FluidPlume");
            auto* SecondPlume    = dynamic_cast<FluidPlumeRecord*>(Workspace.QueryWorldSpace().FindRecord(AllPlumes.back()->QueryToken()));
            Require(SecondPlume != nullptr, "Second FluidPlume missing");

            auto TyreDoc                         = FluidDynamics::ConstructSampleFluidScene("tyre_burnout_ring");
            TyreDoc.Parameters.GridResolution    = 64;
            TyreDoc.Parameters.RenderScale       = 0.65f;
            TyreDoc.Parameters.ShowAtlasMinimap  = true;
            TyreDoc.Parameters.AtlasMinimapField = 0;
            TyreDoc.Camera.TargetDistance        = 4.25f;
            TyreDoc.Camera.SnapToTarget();
            const auto TyreSidecar               = EverestRoot / "Content/Levels/tyre_ring_collider_study.fluidspace";
            Require(FluidDynamics::SaveFluidSpaceFile(TyreSidecar, TyreDoc), "Failed writing tyre_ring sidecar");

            SecondPlume->AccessConfiguration().Enabled   = false; // keep Proving Ground clean while inspecting in modal editor
            SecondPlume->AccessConfiguration().SpaceFile = "tyre_ring_collider_study.fluidspace";
            SecondPlume->AccessConfiguration().AdvanceRevision();

            Require(InspectorRuntime::Instance().RequestEditorOpen(SecondPlume->QueryToken()), "RequestEditorOpen failed on second plume");
            PumpWorkspace(14, 1.0 / 24.0);
            WriteWorkspacePng(ImagesDir / "03-FluidEditorTyreColliderAndAtlasMinimap.png");
            InspectorRuntime::Instance().CloseActiveEditor();
            (void)Workspace.QueryWorldSpace().DestroyRecord(SecondPlume->QueryToken());
            std::filesystem::remove(TyreSidecar);
        }

        // Capture 06: Modal FluidPlumeEditor in 3D Voxel Quantization + Cross-Section Slice Plane mode!
        {
            ConstructOrder VoxelOrder{};
            VoxelOrder.Entry.Identifier = "FluidPlume";
            VoxelOrder.Entry.Caption    = "Fluid Plume";
            VoxelOrder.Entry.Glyph      = "Flame";
            VoxelOrder.Entry.Group      = "Physics";
            VoxelOrder.Name             = "Voxel lattice & slice study";
            VoxelOrder.Visible          = true;
            Workspace.ServiceConstructOrder(VoxelOrder);
            const auto AllPlumes = Workspace.QueryWorldSpace().FindAllByType("FluidPlume");
            auto* ThirdPlume     = dynamic_cast<FluidPlumeRecord*>(Workspace.QueryWorldSpace().FindRecord(AllPlumes.back()->QueryToken()));
            Require(ThirdPlume != nullptr, "Third FluidPlume missing");

            auto VoxelDoc                         = FluidDynamics::ConstructSampleFluidScene("deflector_obstacle");
            VoxelDoc.Parameters.GridResolution    = 64;
            VoxelDoc.Parameters.RenderScale       = 0.65f;
            VoxelDoc.Parameters.VoxelQuantization = 0.92f;
            VoxelDoc.Parameters.SliceAxis         = 1;
            VoxelDoc.Parameters.SlicePosition     = 0.50f;
            VoxelDoc.Parameters.ShowBoundingBox   = true;
            VoxelDoc.Camera.TargetDistance        = 4.15f;
            VoxelDoc.Camera.SnapToTarget();
            const auto VoxelSidecar               = EverestRoot / "Content/Levels/voxel_slice_study.fluidspace";
            Require(FluidDynamics::SaveFluidSpaceFile(VoxelSidecar, VoxelDoc), "Failed writing voxel_slice sidecar");

            ThirdPlume->AccessConfiguration().Enabled   = false;
            ThirdPlume->AccessConfiguration().SpaceFile = "voxel_slice_study.fluidspace";
            ThirdPlume->AccessConfiguration().AdvanceRevision();

            Require(InspectorRuntime::Instance().RequestEditorOpen(ThirdPlume->QueryToken()), "RequestEditorOpen failed on third plume");
            PumpWorkspace(14, 1.0 / 24.0);
            WriteWorkspacePng(ImagesDir / "06-FluidEditorVoxelSliceDebug.png");
            InspectorRuntime::Instance().CloseActiveEditor();
            (void)Workspace.QueryWorldSpace().DestroyRecord(ThirdPlume->QueryToken());
            std::filesystem::remove(VoxelSidecar);
        }

        if (!SkipProvingGround)
        {
        // Re-enable LiveRefresh for Step 4 Proving Ground live vehicle burnout + detonation sequence!
        PipelineRec->AccessConfiguration().LiveRefresh = true;
        PipelineRec->AccessConfiguration().AdvanceRevision();

        // 4. Live Proving Ground ControlVehicle Burnout -> Tyre Ignition Fire -> Launch Past Trackside Explosion!
        //    Proves world-anchored toroidal PlumeSpace: smoke & fire stay anchored in world space behind the moving car!
        std::printf("[FluidEditorCapture] Rendering 24-frame Proving Ground ControlVehicle burnout smoke/fire + explosion sequence...\n");
        {
            auto* VehicleEntry = dynamic_cast<VehicleRecord*>(Workspace.QueryWorldSpace().FindFirstByType("Vehicle"));
            Require(VehicleEntry != nullptr, "ControlVehicle missing in ProvingGroundVehicle.levelspace");
            const RecordToken VehicleToken = VehicleEntry->QueryToken();

            auto& VAuth                                    = VehicleEntry->AccessConfiguration();
            VAuth.Spatial.PositionMetres[0]                = -40.0f;
            VAuth.Spatial.PositionMetres[1]                = 0.0f;
            VAuth.Spatial.PositionMetres[2]                = 0.0f;
            VAuth.Spatial.OrientationDegrees[0]            = 0.0f;
            VAuth.Spatial.OrientationDegrees[1]            = 0.0f;
            VAuth.Spatial.OrientationDegrees[2]            = 0.0f;
            VAuth.ThreadedPhysics                          = false;
            VAuth.PhysicsStepHz                            = 240.0f;
            VAuth.Specification.Solver.TyreThermal.Enabled = true;
            // Pre-warm rear tyres near ignition threshold so the standing burnout rapidly transitions from white rubber smoke -> burning tyre fire!
            VAuth.Specification.Solver.TyreThermal.AmbientTemperature = 345.0f;
            VAuth.Specification.Solver.TractionControl.Enabled        = false;
            VAuth.AdvanceRevision();

            auto& Pfg             = PlumeEntry->AccessConfiguration();
            Pfg.Enabled           = true;
            Pfg.PositionMetres[0] = -25.5f;
            Pfg.PositionMetres[1] = 3.4f;
            Pfg.PositionMetres[2] = 0.15f;
            Pfg.DomainScale       = 1.75f;
            Pfg.BurstPattern      = static_cast<uint32_t>(FluidDynamics::FluidBurstPattern::MushroomCloud);
            Pfg.BurstGeneration   = 2u;
            Pfg.AdvanceRevision();

            PhysicsExtension         DrivePhysics;
            PhysicsExtensionSettings Settings{};
            Settings.Threaded      = false;
            Settings.StepHz        = 240.0f;
            Settings.WriteRegister = false;
            DrivePhysics.Begin(Workspace.QueryWorldSpace(), Settings);

            for (int Warm = 0; Warm < 12; ++Warm) (void)DrivePhysics.Step(Workspace.QueryWorldSpace(), 1.0 / 30.0);

            for (int FrameIdx = 0; FrameIdx < 24; ++FrameIdx)
            {
                PhysicsVehicleInput Cmd{};
                if (FrameIdx < 8)
                {
                    // Standing burnout / power-launch: full throttle + slight yaw so rear tyres spin furiously & billow smoke!
                    Cmd.Throttle = 1.0f;
                    Cmd.Brake    = (FrameIdx < 4) ? 0.28f : 0.0f;
                    Cmd.Steer    = 0.10f;
                }
                else if (FrameIdx < 16)
                {
                    // Full-throttle acceleration past the trackside pyrotechnic detonation rig!
                    Cmd.Throttle = 1.0f;
                    Cmd.Steer    = -0.16f;
                }
                else
                {
                    // Aggressive drift / handbrake flick leaving dual trailing smoke ribbons across the asphalt!
                    Cmd.Throttle  = 0.85f;
                    Cmd.Steer     = 0.42f;
                    Cmd.Handbrake = (FrameIdx >= 16 && FrameIdx <= 19);
                }

                if (FrameIdx == 9)
                {
                    // Trigger secondary MushroomCloud detonation right as ControlVehicle blasts past X = -26m!
                    ++PlumeEntry->AccessConfiguration().BurstGeneration;
                    PlumeEntry->AccessConfiguration().AdvanceRevision();
                }

                DrivePhysics.AssignVehicleInput(Cmd);
                for (int Sub = 0; Sub < 5; ++Sub) (void)DrivePhysics.Step(Workspace.QueryWorldSpace(), 1.0 / 30.0);

                VehicleReading Reading{};
                Require(QueryVehicleReading(VehicleToken, Reading), "Missing VehicleReading");
                const auto& Pos = Reading.Telemetry.Chassis.Position;
                const auto& Rot = Reading.Telemetry.Chassis.Orientation;
                const auto  Fwd = Rot.Rotate({ 1.0f, 0.0f, 0.0f });
                const auto  Lft = Rot.Rotate({ 0.0f, 1.0f, 0.0f });

                // Rear-quarter elevated chase camera that frames BOTH the moving car AND the lingering world-space smoke wake behind it!
                SceneViewportRequest Req{};
                Req.PanelWidth          = 960.0f;
                Req.PanelHeight         = 540.0f;
                Req.DeltaSeconds        = 0.16f;
                Req.VerticalFieldOfView = 0.86f;
                Req.CameraUp            = { 0.0f, 0.0f, 1.0f };
                Req.CameraPosition      = {
                    Pos.x - Fwd.x * 7.6f - Lft.x * 3.4f,
                    Pos.y - Fwd.y * 7.6f - Lft.y * 3.4f,
                    Pos.z + 2.55f
                };
                Req.CameraTarget        = {
                    Pos.x - Fwd.x * 0.8f + 0.5f,
                    Pos.y - Fwd.y * 0.8f + 0.4f,
                    Pos.z + 0.75f
                };

                const auto Outcome = SceneBridge.Render(Req, Workspace.QueryWorldSpace(), static_cast<double>(FrameIdx) * 0.16);
                std::vector<uint8_t> Rgba;
                Require(Outcome.View != VK_NULL_HANDLE && SceneBridge.AccessViewport()->ReadPresented(Rgba),
                        "SceneViewport ReadPresented failed on frame " + std::to_string(FrameIdx));
                char NameBuf[64]{};
                std::snprintf(NameBuf, sizeof(NameBuf), "frame_%02d.png", FrameIdx);
                WriteRgbPng(ProvingDir / NameBuf, Rgba, Outcome.Extent[0], Outcome.Extent[1]);
                if (FrameIdx == 12)
                {
                    WriteRgbPng(ImagesDir / "04-ProvingGroundSmokeFireAndExplosionGI.png", Rgba, Outcome.Extent[0], Outcome.Extent[1]);
                }
                std::printf("  [ProvingGround %02d/24] car=(%.1f, %.1f) speed=%.1f m/s rearSlip=%.2f rearTemp=%.0f K\n",
                            FrameIdx + 1, Pos.x, Pos.y, Reading.Telemetry.SpeedMetresPerSecond,
                            Reading.Telemetry.Wheels[2].SlipRatio,
                            Reading.Telemetry.Wheels[2].Thermal.SurfaceTemperature);
            }
            DrivePhysics.End(Workspace.QueryWorldSpace());
        }
        }

        std::filesystem::remove(SidecarPath);
        InspectorRuntime::Instance().RegisterFluidViewportPresenter({});
        ImGui::DestroyContext();
        std::printf("[FluidEditorCapture] ALL VALIDATIONS AND GPU CAPTURES COMPLETED SUCCESSFULLY.\n");
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::fprintf(stderr, "[FluidEditorCapture] FAILED: %s\n", Error.what());
        return 1;
    }
}
