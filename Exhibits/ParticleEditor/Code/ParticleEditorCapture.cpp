/*==========================================================================================================================================
                                                      PARTICLEEDITORCAPTURE.CPP
==========================================================================================================================================*/
// 🧩 The Particle editor in the Everest workspace, captured offscreen: a Particles record constructed and selected, its editor
//    opened through the Inspector runtime, the GPU particle engine on an own device (read back into the captured image), and
//    the editor walked through as a user would (Force Field page with the overlay, Wind, attractor, Lightning, black-hole lensing,
//    Derez cube shards, the preset library and wind arrows). Every capture checks the relevant viewport telemetry.

#include "Engine/Interface/IconPresentation.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/Interface/ParticleEditor.h"
#include "Engine/Interface/ParticleEditor/ParticleEditorWorkspace.h"
#include "Engine/Interface/SceneDirectoryPanel.h"
#include "Engine/Scene/SceneDirectory.h"
#include "Frontier/Extensions/Particle/ParticleExtension.h"
#include "EverestWorkspace.h"
#include "Exhibits/ControlCentre/Code/CpuRasterization.h"
#include "UI/Editor/ControlCentrePanel.h"

#include <imgui.h>
#include <stb_image_write.h>

#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

constexpr int      CaptureWidth  = 1920;
constexpr int      CaptureHeight = 1080;
constexpr uint64_t ParticleTag   = 0x9A27C1u;   // [-] ImTextureID the capture resolves to the read-back particle image

void Require(bool Condition, const std::string& Message)
{
    if (!Condition) throw std::runtime_error(Message);
}

size_t CountDifferentPixels(const std::vector<uint8_t>& Left, const std::vector<uint8_t>& Right)
{
    if (Left.size() != Right.size() || Left.size() % 4u != 0u) return 0;
    size_t Changed = 0;
    for (size_t Pixel = 0; Pixel < Left.size(); Pixel += 4u)
    {
        const int Difference = std::abs(static_cast<int>(Left[Pixel]) - static_cast<int>(Right[Pixel])) +
                               std::abs(static_cast<int>(Left[Pixel + 1u]) - static_cast<int>(Right[Pixel + 1u])) +
                               std::abs(static_cast<int>(Left[Pixel + 2u]) - static_cast<int>(Right[Pixel + 2u]));
        if (Difference > 12) ++Changed;
    }
    return Changed;
}

} // namespace

int main(int ArgumentCount, char** Arguments)
{
    using namespace Frontier;
    try
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Require(ArgumentCount >= 5, "usage: ParticleEditorCapture <EngineContent> <Projects/Everest> <ShaderRoot> <OutDir> [Only]");
        const std::filesystem::path ContentDir  = std::filesystem::absolute(Arguments[1]);
        const std::filesystem::path EverestRoot = std::filesystem::absolute(Arguments[2]);
        const std::filesystem::path ShaderRoot  = std::filesystem::absolute(Arguments[3]);
        const std::filesystem::path OutputDir   = std::filesystem::absolute(Arguments[4]);
        const std::string           Only        = ArgumentCount > 5 ? Arguments[5] : "";
        std::filesystem::create_directories(OutputDir);

        ImGui::CreateContext();
        ImGuiIO& Io    = ImGui::GetIO();
        Io.DisplaySize = ImVec2(static_cast<float>(CaptureWidth), static_cast<float>(CaptureHeight));
        Io.DeltaTime   = 1.0f / 30.0f;
        Io.IniFilename = nullptr;
        Io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;   // the dockspace's, before the first refresh
        Io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth  = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
        IconPresentation::Attach(ContentDir / "Icons");
        const std::filesystem::path Preferences = OutputDir / "Personal.projectspace";
        std::filesystem::remove(Preferences);
        ActivationOwnership Ownership(new ControlCentrePanel(ContentDir / "FontArchives", Preferences, EverestRoot / "Everest.projectspace"));
        auto* Centre = static_cast<ControlCentrePanel*>(Ownership.get());
        Require(Centre->Initialize() && Centre->Activate(), "ControlCentrePanel initialization failed");

        Everest::EverestWorkspace       Workspace(*Centre, EverestRoot);
        Require(Workspace.OpenLevel(EverestRoot / "Content/Levels/Default.levelspace"), "Failed to open Default.levelspace");
        Frontier::ParticleExtension Bridge(ShaderRoot, nullptr);
        std::vector<uint8_t>             ParticlePixels;
        std::array<uint32_t, 2>          ParticleExtent = { 0, 0 };
        bool                             ReadNext       = false;
        std::vector<double>              RefreshTimes;
        // 📝 The presenter the editor application registers, here on an own device; the image is read back only for the tick a
        //    capture rasterizes.
        InspectorRuntime::Instance().RegisterParticleViewportPresenter([&](const ParticleViewportRequest& Request) {
            Frontier::ParticleViewportOutcome Outcome = Bridge.Render(Request);
            ParticleViewportImage            Image   = std::move(Outcome.Image);
            if (Outcome.View != VK_NULL_HANDLE)
            {
                RefreshTimes.push_back(Image.RefreshMilliseconds);
                Image.Texture = ParticleTag;
                if (ReadNext)
                {
                    std::string Diagnostic;
                    Require(Bridge.AccessSequence()->ReadPresentedImage(ParticlePixels, &Diagnostic), "particle image readback: " + Diagnostic);
                    ParticleExtent = Image.Extent;
                }
            }
            return Image;
        });

        // 📝 A Particles record as the Construct dialog adds one, selected, and its editor opened as the Inspector's button does.
        WorldSpace&       World = Workspace.QueryWorldSpace();
        const RecordToken Token = World.ConstructRecord("Particles", "GPU Particles");
        Require(Token.IsValid(), "the Particles record could not be constructed");
        Workspace.QueryOutliner().Select(Token);
        Require(InspectorRuntime::Instance().RequestEditorOpen(Token), "the Particles record has no editor");

        ImVec2     Pointer(-FLT_MAX, -FLT_MAX);
        const auto Tick = [&](int Count) {
            for (int Index = 0; Index < Count; ++Index)
            {
                Io.MousePos = Pointer;
                ReadNext    = Index + 1 == Count;
                Centre->PrepareInterface();
                ImGui::NewFrame();
                (void)Centre->Integrate(1.0 / 30.0);
                (void)Workspace.ConfigurePlatform();
                Workspace.ConstructInterface(1.0 / 30.0);
                Centre->ConstructInterface();
                ImGui::Render();
                FrontierExhibit::AcknowledgeTextures();
            }
            ReadNext = false;
        };
        Tick(1);
        ParticleEditorWorkspace* Editor = ParticleEditor::AccessWorkspace();
        Require(Editor != nullptr && InspectorRuntime::Instance().QueryActiveEditorToken() == Token, "the Particle editor did not open");
        Require(Editor->QueryDiagnostic().empty(), Editor->QueryDiagnostic());
        std::printf("[ParticleEditorCapture] editor open: %zu presets, %zu effects, %zu fields\n", Editor->QueryLibrary().Presets.size(),
                    Editor->AccessSpace().Effects.size(), Editor->AccessSpace().Fields.size());

        std::ofstream Ledger(OutputDir / "Captures.tsv");
        Ledger << "capture\tselected\teffects\talive\tfields\toverlay\tlattice\tlightning_segments\tlensing_holes\theat_shimmers\tshard_raster_effects\tshard_particle_capacity\tmean_ms\textent\n";
        const auto WriteCapture = [&](const std::string& Name) {
            const ParticleViewportImage& Image = Editor->QueryImage();
            Require(Image.Failure.empty(), Name + ": " + Image.Failure);
            Require(Image.Texture == ParticleTag && !ParticlePixels.empty(), Name + ": the viewport has no particle image");
            uint32_t Alive = 0;
            for (const ParticleEffectCensus& Census : Image.Census) Alive += Census.Alive;
            const ImDrawData*          Submitted = ImGui::GetDrawData();
            std::vector<unsigned char> Pixels(static_cast<size_t>(CaptureWidth) * CaptureHeight * 3u, 14u);
            const std::function<FrontierExhibit::Image(ImTextureID)> Resolve = [&](ImTextureID Texture) {
                return Texture == ParticleTag
                           ? FrontierExhibit::Image{ ParticlePixels.data(), static_cast<int>(ParticleExtent[0]), static_cast<int>(ParticleExtent[1]), 4 }
                           : FrontierExhibit::Image{};
            };
            for (const ImDrawList* List : Submitted->CmdLists)
                FrontierExhibit::RasterizeCommands(List, Pixels.data(), CaptureWidth, CaptureHeight, Submitted->DisplayPos, Submitted->FramebufferScale, nullptr,
                                                   &Resolve);
            const std::filesystem::path Target = OutputDir / (Name + ".png");
            Require(stbi_write_png(Target.string().c_str(), CaptureWidth, CaptureHeight, 3, Pixels.data(), CaptureWidth * 3) != 0, "PNG write failed");
            const double Mean = RefreshTimes.empty() ? 0.0 : std::accumulate(RefreshTimes.begin(), RefreshTimes.end(), 0.0) / RefreshTimes.size();
            std::printf("[ParticleEditorCapture] %s: %s selected, %zu effects, %u alive, overlay %s, lattice %s, lightning %u seg, lens %u, shimmer %u, shard pass %u/%u slots, %u x %u image, %.1f ms mean refresh\n",
                        Target.string().c_str(), Editor->AccessSpace().Selected.c_str(), Editor->AccessSpace().Effects.size(), Alive,
                        Editor->Overlay.Settings.Enabled ? "on" : "off", Image.Lattice ? "read" : "none", Image.LightningSegmentCount,
                        Image.LensingHoleCount, Image.HeatShimmerCount, Image.ShardRasterEffectCount, Image.ShardRasterParticleCapacity,
                        ParticleExtent[0], ParticleExtent[1], Mean);
            Ledger << Name << '\t' << Editor->AccessSpace().Selected << '\t' << Editor->AccessSpace().Effects.size() << '\t' << Alive << '\t'
                  << Editor->AccessSpace().Fields.size() << '\t' << (Editor->Overlay.Settings.Enabled ? "on" : "off") << '\t'
                  << (Image.Lattice ? "read" : "none") << '\t' << Image.LightningSegmentCount << '\t' << Image.LensingHoleCount << '\t'
                  << Image.HeatShimmerCount << '\t' << Image.ShardRasterEffectCount << '\t' << Image.ShardRasterParticleCapacity << '\t' << Mean << '\t'
                  << ParticleExtent[0] << 'x' << ParticleExtent[1] << '\n';
            RefreshTimes.clear();
            return Alive;
        };
        const auto Wanted = [&](const std::string& Name) { return Only.empty() || Name.find(Only) != std::string::npos; };
        const auto ClearEffects = [&] {
            ParticleSpace& Space = Editor->AccessSpace();
            while (!Space.Effects.empty()) (void)Space.RemoveEffect(Space.Effects.back().Token);
        };
        const auto AliveFor = [&](const std::string& EffectToken) {
            for (const ParticleEffectCensus& Census : Editor->QueryImage().Census)
                if (Census.Token == EffectToken) return Census.Alive;
            return 0u;
        };

        int Captured = 0;
        // ① The editor as it opens: the default scene (sparks, leaves, atoms, chemicals) after 2.5 s, the Force Field page and the
        //    Force Field editor's visualization over the particles (flow speed, read back from the GPU wind lattice).
        Tick(74);
        if (Wanted("EditorOpening"))
        {
            const uint32_t Alive = WriteCapture("EditorOpening");
            Require(Alive > 500 && Editor->QueryImage().Lattice != nullptr, "EditorOpening: too few particles or no wind lattice");
            ++Captured;
        }
        // ② An effect's page: the first effect selected in the Outliner (its origin cross turns amber).
        Editor->Perform("select:" + Editor->AccessSpace().Effects.front().Token);
        Tick(12);
        if (Wanted("EffectPage"))
        {
            WriteCapture("EffectPage");
            ++Captured;
        }
        // ③ The Wind guest's page.
        Editor->Perform("select:Wind");
        Tick(8);
        if (Wanted("WindPage"))
        {
            WriteCapture("WindPage");
            ++Captured;
        }
        // ④ A localized force: an attractor at the domain centre, its page, and the overlay showing acceleration.
        Require(Editor->Perform("add-force:attract"), "the attractor could not be added");
        Editor->Perform("contribution:accelerate");
        Tick(36);
        if (Wanted("AttractorPage"))
        {
            WriteCapture("AttractorPage");
            ++Captured;
        }
        // ⑤ Lightning: its page and a strike (the bolt is lit for a few ticks).
        Editor->Perform("contribution:flow");
        Editor->Perform("select:Lightning");
        Editor->AccessSpace().Lightning.Automatic = false;
        Tick(4);
        Editor->Perform("strike");
        Tick(3);
        if (Wanted("LightningPage"))
        {
            Require(Editor->QueryImage().LightningSegmentCount > 0, "LightningPage: the strike produced no GPU segments");
            WriteCapture("LightningPage");
            ++Captured;
        }
        // ⑥ The preset library (52 presets in 14 groups).
        Editor->Perform("library:on");
        Tick(3);
        if (Wanted("PresetLibrary"))
        {
            Require(Editor->QueryLibraryShown(), "PresetLibrary: the library did not open");
            WriteCapture("PresetLibrary");
            ++Captured;
        }
        // ⑦ A preset from the library: the tornado, selected, the camera closer.
        const size_t Before = Editor->AccessSpace().Effects.size();
        Require(Editor->Perform("add:tornado"), "the tornado preset could not be added");
        Require(Editor->AccessSpace().Effects.size() == Before + 1 && !Editor->QueryLibraryShown(), "the tornado did not join the scene");
        Editor->Perform("select:" + Editor->AccessSpace().Effects.back().Token);
        Editor->Perform("camera:0.4,0.22,13");
        Tick(90);
        if (Wanted("TornadoPreset"))
        {
            WriteCapture("TornadoPreset");
            ++Captured;
        }
        // ⑧ The prototype's wind arrows (the lattice drawn on the GPU) with the overlay off.
        Editor->Perform("overlay:off");
        Editor->AccessSpace().Wind.ShowArrows = true;
        Editor->Perform("select:Wind");
        Editor->Perform("camera:0.6,0.55,15");
        Tick(20);
        if (Wanted("WindArrows"))
        {
            WriteCapture("WindArrows");
            ++Captured;
        }
        // ⑨ Derez runtime path: the transition preset releases surface cubes and dispatches its procedural shard raster pass.
        ClearEffects();
        ParticleSpace& Space = Editor->AccessSpace();
        Space.Playing = true;
        Space.TimeScale = 1.0;
        Space.Wind.ShowArrows = false;
        Editor->Perform("overlay:off");
        Editor->Perform("camera:0.45,0.35,5.5");
        Tick(1);
        const std::vector<uint8_t> DerezBaseline = ParticlePixels;
        Require(!DerezBaseline.empty(), "DerezCube: the no-effect baseline image is empty");
        Require(Editor->Perform("add:derez-cube"), "the Derez cube preset could not be added");
        const ParticleEffectInstance* Derez = Space.ResolveEffect(Space.Effects.back().Token);
        Require(Derez != nullptr && Derez->Parameters.Appearance == ParticleAppearance::Cube && Derez->Parameters.Transition &&
                    Derez->Parameters.Transition->Fragment,
                "Derez cube is missing its cube or shard transition path");
        const std::string DerezToken = Derez->Token;
        Editor->Perform("select:" + DerezToken);
        Tick(93);   // hold completes, cubes reach the floor, and recent shard bursts remain visible
        if (Wanted("DerezCube"))
        {
            Require(AliveFor(DerezToken) > 0, "DerezCube: no transition particles reached the GPU census");
            Require(Editor->QueryImage().ShardRasterEffectCount > 0 && Editor->QueryImage().ShardRasterParticleCapacity > 0,
                    "DerezCube: the procedural shard raster pass had no draw instances");
            Require(CountDifferentPixels(ParticlePixels, DerezBaseline) > 16, "DerezCube: the GPU image did not change from its empty baseline");
            WriteCapture("DerezCubeTransition");
            ++Captured;
        }

        // ⑩ Black-hole runtime path: the native preset must reach the acceleration and screen-space lensing dispatches.
        ClearEffects();
        Editor->Perform("camera:0.0,0.3,7.0");
        Tick(1);
        const std::vector<uint8_t> BlackHoleBaseline = ParticlePixels;
        Require(!BlackHoleBaseline.empty(), "BlackHole: the no-effect baseline image is empty");
        Require(Editor->Perform("add:black-hole"), "the Black hole preset could not be added");
        const ParticleEffectInstance* BlackHole = Space.ResolveEffect(Space.Effects.back().Token);
        Require(BlackHole != nullptr && BlackHole->Parameters.Attractor && BlackHole->Parameters.Lensing,
                "Black hole is missing its attractor or lensing payload");
        const std::string BlackHoleToken = BlackHole->Token;
        Editor->Perform("select:" + BlackHoleToken);
        Tick(84);
        if (Wanted("BlackHole"))
        {
            Require(AliveFor(BlackHoleToken) > 0, "BlackHole: no accretion particles reached the GPU census");
            Require(Editor->QueryImage().LensingHoleCount > 0, "BlackHole: no screen-space lens reached the GPU pass");
            Require(CountDifferentPixels(ParticlePixels, BlackHoleBaseline) > 16, "BlackHole: the GPU image did not change from its empty baseline");
            WriteCapture("BlackHoleLensing");
            ++Captured;
        }

        // ⑪ Closing: the editor's Close button returns to the workspace.
        InspectorRuntime::Instance().CloseActiveEditor();
        Tick(2);
        Require(!InspectorRuntime::Instance().QueryActiveEditorToken().IsValid(), "the editor did not close");

        std::printf("[ParticleEditorCapture] %d captures on %s\n", Captured, Editor->QueryImage().Adapter.c_str());
        InspectorRuntime::Instance().RegisterParticleViewportPresenter({});
        ImGui::DestroyContext();
        return Captured > 0 ? 0 : 1;
    }
    catch (const std::exception& Failure)
    {
        std::fprintf(stderr, "[ParticleEditorCapture] %s\n", Failure.what());
        return 1;
    }
}
