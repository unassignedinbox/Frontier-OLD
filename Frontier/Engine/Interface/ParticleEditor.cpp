/*==========================================================================================================================================
                                                        PARTICLEEDITOR.CPP
==========================================================================================================================================*/
// 🧩 The Particle editor window: a title bar with the live readout, the space's file and Close, and the editor's columns below;
//    the record's .particlespace is attached when the window opens and written back when it closes.

#include "Engine/Interface/ParticleEditor.h"

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

#include "Engine/Interface/ContentDirectory.h"
#include "Engine/Interface/InspectorControls.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/Interface/ParticleEditor/ParticleEditorWorkspace.h"
#include "Engine/Interface/ProjectContent.h"
#include "Engine/Scene/SceneDirectory.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace Frontier
{

namespace
{

// 📝 The library beside the executable (EngineContent/ParticleArchives), whatever the working folder: an editor opened through
//    Windows' Open with starts in the document's folder.
std::filesystem::path ResolveLibraryFile()
{
    const std::filesystem::path Relative = std::filesystem::path("ParticleArchives") / "ParticleLibrary.toml";
    const std::filesystem::path Engine   = QueryEngineContentDirectory();
    std::error_code             Failure;
    if (!Engine.empty() && std::filesystem::is_regular_file(Engine / Relative, Failure)) return Engine / Relative;
    return ResolveContentDirectory("EngineContent") / Relative;
}

std::unique_ptr<ParticleEditorWorkspace>& ResolveWorkspace()
{
    static std::unique_ptr<ParticleEditorWorkspace> Workspace;
    if (!Workspace) Workspace = std::make_unique<ParticleEditorWorkspace>(ResolveLibraryFile());
    return Workspace;
}

} // namespace

ParticleEditorWorkspace* ParticleEditor::AccessWorkspace()
{
    return ResolveWorkspace().get();
}

bool ParticleEditor::SaveSpace()
{
    return ResolveWorkspace()->SaveSpaceFile();
}

std::string ParticleEditor::NameSpaceFile(const std::filesystem::path& LevelFile, const std::string& RecordName)
{
    std::string Name = LevelFile.stem().string() + "." + RecordName;
    for (char& Letter : Name)
    {
        const bool Kept = (Letter >= 'a' && Letter <= 'z') || (Letter >= 'A' && Letter <= 'Z') || (Letter >= '0' && Letter <= '9') || Letter == '-' ||
                          Letter == '_' || Letter == '.';
        if (!Kept) Letter = '_';
    }
    return Name + ".particlespace";
}

std::filesystem::path ParticleEditor::ResolveSpaceFile(const ParticleConfiguration& Parameters)
{
    const std::filesystem::path& Level = InspectorRuntime::Instance().QueryLevelFile();
    if (Level.empty() || Parameters.SpaceFile.empty()) return {};
    return Level.parent_path() / DecodeUtf8Path(Parameters.SpaceFile);
}

void ParticleEditor::ConstructInterface(RecordToken Token, ParticleConfiguration& Parameters)
{
    ParticleEditorWorkspace& Workspace = *ResolveWorkspace();
    InspectorRuntime&        Runtime   = InspectorRuntime::Instance();
    Workspace.Presenter                = Runtime.QueryParticleViewportPresenter();
    Workspace.LevelFlow                = Runtime.QueryLevelFlowSampler();

    // 📝 The record's space: named on first use (the level then saves the name), read or created beside the level.
    if (!Runtime.QueryLevelFile().empty())
    {
        if (Parameters.SpaceFile.empty())
        {
            Parameters.SpaceFile = NameSpaceFile(Runtime.QueryLevelFile(), SceneDirectory::Instance().QueryName(Token));
            Parameters.AdvanceRevision();
        }
        const std::filesystem::path File = ResolveSpaceFile(Parameters);
        if (Workspace.QuerySpaceFile() != File) (void)Workspace.AttachSpaceFile(File);
    }

    const InspectorPalette Palette = InspectorPalette::Resolve();
    const EditorTypefaces  Faces   = EditorTypefaces::Resolve();
    const float            Scale   = Runtime.ResolveInterfaceScale();
    const ImGuiViewport* Main = ImGui::GetMainViewport();
    const ImVec2 Margin(28.0f * Scale, 24.0f * Scale);
    const ImVec2 Position(Main->WorkPos.x + Margin.x, Main->WorkPos.y + Margin.y);
    const ImVec2 Size(std::max(1100.0f, Main->WorkSize.x - Margin.x * 2.0f), std::max(640.0f, Main->WorkSize.y - Margin.y * 2.0f));

    // 📝 A normal in-app docking window rather than a modal popup: it can live beside the scene, inspector or log panels while
    //    the record's inspector remains its entry point. The editor contents and file contract are unchanged.
    constexpr const char* WindowName = "Frontier \xC2\xB7 Particle editor###FrontierParticleEditor";
    ImGui::SetNextWindowPos(Position, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(Size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowViewport(Main->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(Palette.ViewportBg));
    bool Open = true;
    const bool Shown = ImGui::Begin(WindowName, &Open, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    bool Closing = false;
    if (Shown)
    {
        ImDrawList* Ink = ImGui::GetWindowDrawList();
        const ImVec2 HeaderMin = ImGui::GetCursorScreenPos();
        const ImVec2 Available = ImGui::GetContentRegionAvail();
        const float Header = 50.0f * Scale;
        const ImVec2 HeaderMax(HeaderMin.x + Available.x, HeaderMin.y + Header);
        Ink->AddRectFilled(HeaderMin, HeaderMax, Palette.InspectorTopBg);
        Ink->AddLine(ImVec2(HeaderMin.x, HeaderMax.y), ImVec2(HeaderMax.x, HeaderMax.y), Palette.EdgeBorder, 1.0f);
        ConstructEditorGlyph(Ink, ImVec2(HeaderMin.x + 24.0f * Scale, HeaderMin.y + 25.0f * Scale), 8.0f * Scale, Palette.ThemeAccent, "Sparkles", 1.5f);
        Ink->AddText(Faces.Medium, 14.0f * Scale, ImVec2(HeaderMin.x + 42.0f * Scale, HeaderMin.y + 16.5f * Scale), Palette.TextPrimary,
                     "Frontier \xC2\xB7 Particle editor");

        const float CloseW = 108.0f * Scale;
        const ImVec2 CloseMin(HeaderMax.x - CloseW - 14.0f * Scale, HeaderMin.y + 10.0f * Scale);
        ImGui::SetCursorScreenPos(CloseMin);
        Closing = ImGui::InvisibleButton("##CloseParticleEditor", ImVec2(CloseW, 30.0f * Scale));
        const bool Hovered = ImGui::IsItemHovered();
        Ink->AddRectFilled(CloseMin, ImVec2(CloseMin.x + CloseW, CloseMin.y + 30.0f * Scale),
                           Hovered ? Palette.SegmentedActiveBg : Palette.CountCardBg, 9.0f * Scale);
        Ink->AddRect(CloseMin, ImVec2(CloseMin.x + CloseW, CloseMin.y + 30.0f * Scale), Palette.InsetFieldBorder, 9.0f * Scale);
        Ink->AddText(Faces.Medium, 11.0f * Scale, ImVec2(CloseMin.x + 18.0f * Scale, CloseMin.y + 8.5f * Scale), Palette.TextPrimary, "Close Editor");

        char Readout[256];
        const std::string File = Workspace.QuerySpaceFile().empty() ? std::string("no level file") : Workspace.QuerySpaceFile().filename().string();
        std::snprintf(Readout, sizeof(Readout), "LIVE \xC2\xB7 %s \xC2\xB7 %zu EFFECTS \xC2\xB7 %zu FIELDS \xC2\xB7 %s%s",
                      SceneDirectory::Instance().QueryName(Token).c_str(), Workspace.AccessSpace().Effects.size(), Workspace.AccessSpace().Fields.size(),
                      File.c_str(), Workspace.QuerySpaceChanged() ? " \xC2\xB7 UNSAVED" : "");
        const float ReadoutRight = CloseMin.x - 16.0f * Scale;
        if (ReadoutRight > HeaderMin.x + 310.0f * Scale)
            Ink->AddText(Faces.Monospace, 10.5f * Scale, ImVec2(HeaderMin.x + 300.0f * Scale, HeaderMin.y + 19.5f * Scale), Palette.LiveGreen, Readout);

        const ImVec2 BodyMin(HeaderMin.x + 12.0f * Scale, HeaderMin.y + Header + 12.0f * Scale);
        const ImVec2 BodyMax(HeaderMin.x + Available.x - 12.0f * Scale, HeaderMin.y + Available.y - 12.0f * Scale);
        if (BodyMax.x > BodyMin.x && BodyMax.y > BodyMin.y)
            Workspace.Construct(BodyMin, BodyMax, Scale, ImGui::GetIO().DeltaTime);
    }
    ImGui::End();
    if (Closing || !Open)
    {
        (void)Workspace.SaveSpaceFile();
        Runtime.CloseActiveEditor();
    }
}

} // namespace Frontier

#elif FRONTIER_EDITOR

namespace Frontier
{
void ParticleEditor::ConstructInterface(RecordToken, ParticleConfiguration&) {}
ParticleEditorWorkspace* ParticleEditor::AccessWorkspace() { return nullptr; }
bool ParticleEditor::SaveSpace() { return false; }
std::filesystem::path ParticleEditor::ResolveSpaceFile(const ParticleConfiguration&) { return {}; }
std::string ParticleEditor::NameSpaceFile(const std::filesystem::path&, const std::string&) { return {}; }
} // namespace Frontier

#endif
