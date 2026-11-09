/*==========================================================================================================================================
                                                          FLUIDPLUMEEDITOR.CPP
==========================================================================================================================================*/
// 🧩 Full-window 1:1 Gas & Explosion Fluid Editor (ported from Experimental/Fluid): multi-document tabs, scene outliner, 3D
//    volumetric raymarched viewport with interactive orbit/brush/detonate/collider tools, 3-tab inspector, .fluidspace /
//    .gasscene.toml sidecar persistence, and transparent RGBA flipbook sprite-sheet baker.

#include "Engine/Interface/FluidPlumeEditor.h"

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

#include "Engine/Interface/FluidEditor/FluidViewportPresentation.h"
#include "Engine/Interface/ControlKit.h"
#include "Engine/Interface/DropdownControls.h"
#include "Engine/Interface/InspectorControls.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "UI/TextureStudio/PanelChrome.h"
#include "Engine/Interface/Inspectors/FluidPlumeInspector.h"
#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"
#include "Engine/PhysicalDynamics/FluidDynamics/FluidSpaceCodec.h"
#include "Engine/Scene/SceneDirectory.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Frontier
{

namespace
{

//------------------------------------------------------------------------------------------------------------------------
//                                      SELF-CONTAINED RGBA PNG WRITER (FLIPBOOK EXPORT)
//------------------------------------------------------------------------------------------------------------------------

uint32_t ComputeCrc32(const uint8_t* Data, size_t Length) noexcept
{
    uint32_t Crc = 0xFFFFFFFFu;
    for (size_t Index = 0; Index < Length; ++Index)
    {
        Crc ^= Data[Index];
        for (int Bit = 0; Bit < 8; ++Bit)
        {
            Crc = (Crc >> 1u) ^ (0xEDB88320u & (0u - (Crc & 1u)));
        }
    }
    return ~Crc;
}

void AppendBigEndian32(std::vector<uint8_t>& Stream, uint32_t Value)
{
    Stream.push_back(static_cast<uint8_t>((Value >> 24u) & 0xFFu));
    Stream.push_back(static_cast<uint8_t>((Value >> 16u) & 0xFFu));
    Stream.push_back(static_cast<uint8_t>((Value >> 8u) & 0xFFu));
    Stream.push_back(static_cast<uint8_t>(Value & 0xFFu));
}

void AppendPngChunk(std::vector<uint8_t>& Stream, const char Type[4], const uint8_t* Payload, size_t Length)
{
    AppendBigEndian32(Stream, static_cast<uint32_t>(Length));
    const size_t TypeOffset = Stream.size();
    Stream.insert(Stream.end(), Type, Type + 4);
    if (Length > 0u)
    {
        Stream.insert(Stream.end(), Payload, Payload + Length);
    }
    const uint32_t Crc = ComputeCrc32(Stream.data() + TypeOffset, 4u + Length);
    AppendBigEndian32(Stream, Crc);
}

bool WriteUncompressedRgbaPng(const std::filesystem::path& FilePath, uint32_t Width, uint32_t Height,
                              const std::vector<uint8_t>& Rgba)
{
    if (Width == 0u || Height == 0u || Rgba.size() < static_cast<size_t>(Width) * Height * 4u)
    {
        return false;
    }

    std::vector<uint8_t> RawScanlines;
    RawScanlines.reserve(static_cast<size_t>(Height) * (1u + static_cast<size_t>(Width) * 4u));
    for (uint32_t Row = 0u; Row < Height; ++Row)
    {
        RawScanlines.push_back(0u);
        const uint8_t* Src = Rgba.data() + static_cast<size_t>(Row) * Width * 4u;
        RawScanlines.insert(RawScanlines.end(), Src, Src + static_cast<size_t>(Width) * 4u);
    }

    std::vector<uint8_t> ZlibStream;
    ZlibStream.reserve(RawScanlines.size() + (RawScanlines.size() / 65535u + 2u) * 5u + 6u);
    ZlibStream.push_back(0x78u);
    ZlibStream.push_back(0x01u);

    uint32_t AdlerA = 1u;
    uint32_t AdlerB = 0u;
    for (uint8_t Byte : RawScanlines)
    {
        AdlerA = (AdlerA + Byte) % 65521u;
        AdlerB = (AdlerB + AdlerA) % 65521u;
    }

    size_t Offset = 0u;
    while (Offset < RawScanlines.size())
    {
        const size_t Chunk   = std::min<size_t>(65535u, RawScanlines.size() - Offset);
        const bool   IsFinal = (Offset + Chunk) >= RawScanlines.size();
        ZlibStream.push_back(IsFinal ? 0x01u : 0x00u);
        const uint16_t Len  = static_cast<uint16_t>(Chunk);
        const uint16_t NLen = static_cast<uint16_t>(~Len);
        ZlibStream.push_back(static_cast<uint8_t>(Len & 0xFFu));
        ZlibStream.push_back(static_cast<uint8_t>((Len >> 8u) & 0xFFu));
        ZlibStream.push_back(static_cast<uint8_t>(NLen & 0xFFu));
        ZlibStream.push_back(static_cast<uint8_t>((NLen >> 8u) & 0xFFu));
        ZlibStream.insert(ZlibStream.end(), RawScanlines.data() + Offset, RawScanlines.data() + Offset + Chunk);
        Offset += Chunk;
    }
    AppendBigEndian32(ZlibStream, (AdlerB << 16u) | AdlerA);

    std::vector<uint8_t> PngBytes = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
    uint8_t              Ihdr[13]{};
    Ihdr[0]  = static_cast<uint8_t>((Width >> 24u) & 0xFFu);
    Ihdr[1]  = static_cast<uint8_t>((Width >> 16u) & 0xFFu);
    Ihdr[2]  = static_cast<uint8_t>((Width >> 8u) & 0xFFu);
    Ihdr[3]  = static_cast<uint8_t>(Width & 0xFFu);
    Ihdr[4]  = static_cast<uint8_t>((Height >> 24u) & 0xFFu);
    Ihdr[5]  = static_cast<uint8_t>((Height >> 16u) & 0xFFu);
    Ihdr[6]  = static_cast<uint8_t>((Height >> 8u) & 0xFFu);
    Ihdr[7]  = static_cast<uint8_t>(Height & 0xFFu);
    Ihdr[8]  = 8u;
    Ihdr[9]  = 6u;
    Ihdr[10] = 0u;
    Ihdr[11] = 0u;
    Ihdr[12] = 0u;
    AppendPngChunk(PngBytes, "IHDR", Ihdr, sizeof(Ihdr));
    AppendPngChunk(PngBytes, "IDAT", ZlibStream.data(), ZlibStream.size());
    AppendPngChunk(PngBytes, "IEND", nullptr, 0u);

    std::error_code Error;
    if (FilePath.has_parent_path())
    {
        std::filesystem::create_directories(FilePath.parent_path(), Error);
    }
    std::ofstream Output(FilePath, std::ios::binary | std::ios::trunc);
    if (!Output) return false;
    Output.write(reinterpret_cast<const char*>(PngBytes.data()), static_cast<std::streamsize>(PngBytes.size()));
    return Output.good();
}

bool WriteFlipbookMetadata(const std::filesystem::path& PngPath, const FluidDynamics::FluidSceneDocument& Scene,
                           int Frames, int Columns, int Rows, int TileSize, int FramesPerSecond,
                           float WarmupSeconds, bool Transparent, bool BurstOnStart)
{
    auto JsonEscape = [](std::string_view Value)
    {
        std::string Result;
        Result.reserve(Value.size() + 8u);
        for (char Character : Value)
        {
            if (Character == '\\' || Character == '"') Result.push_back('\\');
            if (Character == '\n') Result += "\\n";
            else if (Character == '\r') Result += "\\r";
            else Result.push_back(Character);
        }
        return Result;
    };
    std::filesystem::path MetadataPath = PngPath;
    MetadataPath.replace_extension(".json");
    std::error_code Error;
    if (MetadataPath.has_parent_path()) std::filesystem::create_directories(MetadataPath.parent_path(), Error);
    std::ofstream Output(MetadataPath, std::ios::trunc);
    if (!Output) return false;
    Output << "{\n"
           << "  \"format\": \"frontier-fluid-flipbook\",\n"
           << "  \"version\": 1,\n"
           << "  \"scene\": \"" << JsonEscape(Scene.Name) << "\",\n"
           << "  \"image\": \"" << JsonEscape(PngPath.filename().string()) << "\",\n"
           << "  \"frames\": " << Frames << ",\n"
           << "  \"columns\": " << Columns << ",\n"
           << "  \"rows\": " << Rows << ",\n"
           << "  \"tileSize\": " << TileSize << ",\n"
           << "  \"framesPerSecond\": " << FramesPerSecond << ",\n"
           << "  \"warmupSeconds\": " << WarmupSeconds << ",\n"
           << "  \"transparent\": " << (Transparent ? "true" : "false") << ",\n"
           << "  \"burstOnStart\": " << (BurstOnStart ? "true" : "false") << "\n"
           << "}\n";
    return Output.good();
}

//------------------------------------------------------------------------------------------------------------------------
//                                             DOCUMENT & EDITOR SESSION STATE
//------------------------------------------------------------------------------------------------------------------------

enum class OutlinerNodeId : int
{
    Domain   = 0,
    Source   = 1,
    Collider = 2,
    Sun      = 3
};

struct FluidDocumentTab
{
    std::string                                        Title           = "Untitled.fluidspace";
    std::string                                        PresetIdentity  = "fuel_air_blast";
    std::filesystem::path                              FilePath{};
    FluidDynamics::FluidSceneDocument                  Scene{};
    std::vector<FluidDynamics::FluidSceneDocument>     UndoStack{};
    std::vector<FluidDynamics::FluidSceneDocument>     RedoStack{};
    bool                                               Dirty           = false;
    bool                                               RestartPending  = true;
    bool                                               StepOncePending = false;
    float                                              BrushRadius     = 0.08f;
    std::vector<FluidBlastImpulse>                     PendingBlasts{};
    FluidDynamics::FluidBurstPattern                   BurstPattern    = FluidDynamics::FluidBurstPattern::SingleBurst;
    uint32_t                                           GpuSlot         = 0u;
};

struct FlipbookExportState
{
    bool                  DialogOpen       = false;
    bool                  Recording        = false;
    int                   FrameCount       = 16;
    int                   Columns          = 4;
    int                   Rows             = 4;
    int                   FrameWidth       = 256;
    int                   FrameHeight      = 256;
    int                   FramesPerSecond  = 24;
    float                 WarmupSeconds    = 0.0f;
    float                 DurationSec      = 2.0f;
    float                 WarmupRemaining  = 0.0f;
    bool                  Transparent      = true;
    bool                  DetonateStart    = true;
    bool                  RestartIssued    = false;
    bool                  BurstIssued      = false;
    int                   Captured         = 0;
    std::vector<uint8_t>  SheetRgba{};
    char                  OutputPath[256]  = "FluidFlipbook.png";
    std::string           LastMessage{};
};

struct FluidEditorSession
{
    RecordToken                     BoundToken{};
    std::vector<FluidDocumentTab>   Tabs{};
    int                             ActiveTab            = 0;
    int                             ActiveInspector      = 0;   // 0 = Source, 1 = Simulation, 2 = Rendering
    int                             ActiveViewportTool   = 0;   // 0 = Orbit, 1 = Flame Brush, 2 = Blast, 3 = Collider
    int                             CameraView           = 0;   // 0 = Perspective, 1 = Front, 2 = Side, 3 = Top
    OutlinerNodeId                  SelectedNode         = OutlinerNodeId::Source;
    bool                            ShowPresetsView      = false;
    bool                            MaximizeViewport     = false;
    bool                            ShowShortcuts        = false;
    bool                            DiagnosticsOpen      = false;
    bool                            CollectionExpanded   = true;
    float                           PlaybackSpeed        = 1.0f;
    char                            DocumentName[96]     = {};
    std::string                     DocumentNameForTab{};
    int                             RenamingNode         = -1;
    char                            RenameField[96]      = {};
    char                            OutlinerSearch[96]   = {};
    int                             OutlinerFilter       = 0;
    char                            PresetSearch[96]     = {};
    int                             PresetFilter         = 0;
    char                            ImportPathField[256] = {};
    bool                            ShowImportModal      = false;
    bool                            ShowOutliner         = true;
    bool                            ShowInspector        = true;
    bool                            ShowStatusBar        = true;
    bool                            ShowDomainWire       = true;
    bool                            ShowHudOverlay       = true;

    std::array<float, 3>            PreviousBrushUvw     = { 0.5f, 0.35f, 0.5f };
    bool                            HasPreviousBrush     = false;

    FluidViewportImage              LastImage{};
    FlipbookExportState             Flipbook{};
    std::string                     StatusNotice         = "Ready \xC2\xB7 3D Eulerian Gas & Combustion Solver";
};

FluidEditorSession& AccessSession() noexcept
{
    static FluidEditorSession Session;
    return Session;
}

const char* FluidSliderUnit(const char* Id) noexcept
{
    const std::string_view Key = Id != nullptr ? Id : "";
    if (Key == "##FgBw" || Key == "##FgBh" || Key == "##FsRad" || Key == "##FsHgt" || Key == "##FoRad") return "m";
    if (Key == "##FlSunEl" || Key == "##FlSunAz") return "°";
    if (Key == "##FgTime" || Key == "##FrScale" || Key == "##FgSurge" || Key == "##FsRate" ||
        Key == "##FlTempK" || Key == "##FeLife") return "×";
    return "—";
}

bool FluidSliderRow(const InspectorPalette& Palette, const EditorTypefaces& Faces, const char* Id, const char* Label,
                    float& Value, float Minimum, float Maximum, const char* DisplayFormat)
{
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const float Width = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    constexpr float LabelHeight = 16.0f;
    constexpr float GroupGap = 9.0f;
    constexpr float ControlHeight = 30.0f;
    constexpr float ValueWidth = 92.0f;
    constexpr float ValueHeight = 28.0f;
    constexpr float UnitWidth = 34.0f;
    constexpr float ControlGap = 10.0f;
    constexpr float BottomMargin = 17.0f;
    constexpr float RowHeight = LabelHeight + GroupGap + ControlHeight + BottomMargin;
    const float ControlY = Origin.y + LabelHeight + GroupGap;
    const float SliderX = Origin.x + ValueWidth + ControlGap;
    const float SliderWidth = std::max(32.0f, Width - ValueWidth - ControlGap);
    const float ThumbSize = 24.0f;
    const float Fraction = Maximum > Minimum ? std::clamp((Value - Minimum) / (Maximum - Minimum), 0.0f, 1.0f) : 0.0f;
    const float ThumbCentreX = SliderX + ThumbSize * 0.5f + Fraction * std::max(0.0f, SliderWidth - ThumbSize);

    ImDrawList* Draw = ImGui::GetWindowDrawList();
    Draw->AddText(Faces.Regular, 12.0f, Origin, IM_COL32(162, 162, 162, 255), Label);

    char Number[32]{};
    std::string_view Format = DisplayFormat != nullptr ? DisplayFormat : "%.2f";
    const size_t Start = Format.find('%');
    size_t Conversion = std::string_view::npos;
    if (Start != std::string_view::npos)
    {
        for (size_t Index = Start + 1; Index < Format.size(); ++Index)
        {
            if (std::string_view("fFeEgG").find(Format[Index]) != std::string_view::npos)
            {
                Conversion = Index;
                break;
            }
        }
    }
    if (Conversion != std::string_view::npos)
    {
        const std::string NumericFormat(Format.substr(Start, Conversion - Start + 1));
        std::snprintf(Number, sizeof(Number), NumericFormat.c_str(), static_cast<double>(Value));
    }
    else
    {
        std::snprintf(Number, sizeof(Number), "%.2f", static_cast<double>(Value));
    }

    PixelSpace Surface;
    const ImGuiIO& Io = ImGui::GetIO();
    (void)Surface.Begin(SurfaceLayer::Window, Io.DisplaySize.x, Io.DisplaySize.y, 1.0f);
    const float PillY = ControlY + (ControlHeight - ValueHeight) * 0.5f;
    const PlaneExtent Pill = Spanning(Origin.x, PillY, ValueWidth, ValueHeight);
    const PlaneExtent NumberCell = Spanning(Origin.x, PillY, ValueWidth - UnitWidth, ValueHeight);
    const PlaneExtent UnitCell = Spanning(Origin.x + ValueWidth - UnitWidth, PillY, UnitWidth, ValueHeight);
    Surface.FillRectangle(Pill, ColorQuad{ 0.0f, 0.0f, 0.0f, 1.0f }, ValueHeight * 0.5f);
    const ColorQuad UnitFill{ 0x1A / 255.0f, 0x1A / 255.0f, 0x1A / 255.0f, 1.0f };
    Surface.FillRectangle(UnitCell, UnitFill, ValueHeight * 0.5f);
    Surface.FillRectangle(Spanning(UnitCell.MinimumX, PillY, UnitWidth * 0.5f, ValueHeight), UnitFill);
    Surface.FillRectangle(Spanning(UnitCell.MinimumX, PillY, 1.0f, ValueHeight), ColorQuad{ 1.0f, 1.0f, 1.0f, 0.05f });
    ControlKit::OutlineRounded(Surface, Pill, ColorQuad{ 1.0f, 1.0f, 1.0f, 0.05f }, ValueHeight * 0.5f);
    const ImVec2 NumberSize = ImGui::CalcTextSize(Number);
    Draw->AddText(Faces.Regular, 12.0f,
                  ImVec2(NumberCell.MinimumX + (NumberCell.Width() - NumberSize.x) * 0.5f,
                         PillY + (ValueHeight - NumberSize.y) * 0.5f),
                  IM_COL32(240, 240, 240, 255), Number);
    const char* Unit = FluidSliderUnit(Id);
    const ImVec2 UnitSize = ImGui::CalcTextSize(Unit);
    Draw->AddText(Faces.Regular, 10.0f,
                  ImVec2(UnitCell.MinimumX + (UnitCell.Width() - UnitSize.x) * 0.5f,
                         PillY + (ValueHeight - UnitSize.y) * 0.5f),
                  IM_COL32(117, 117, 117, 255), Unit);

    ImGui::PushID(Id);
    const ImVec2 HitMin(SliderX, ControlY);
    ImGui::SetCursorScreenPos(HitMin);
    ImGui::InvisibleButton("##FluidSlider", ImVec2(SliderWidth, ControlHeight));
    const bool Active = ImGui::IsItemActive();
    const bool Hovered = ImGui::IsItemHovered();
    const float TrackY = ControlY + (ControlHeight - 26.0f) * 0.5f;
    const PlaneExtent Track = Spanning(SliderX, TrackY, SliderWidth, 26.0f);
    const float FillWidth = std::max(24.0f, ThumbSize * 0.5f + std::max(0.0f, SliderWidth - ThumbSize) * Fraction);
    Surface.FillRectangle(Track, ColorQuad{ 0x22 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, 1.0f }, 13.0f);
    Surface.FillRectangle(Spanning(Track.MinimumX, Track.MinimumY, std::min(FillWidth, SliderWidth), Track.Height()),
                          ColorQuad{ 0x4A / 255.0f, 0x4A / 255.0f, 0x4A / 255.0f, 1.0f }, 13.0f);
    Surface.FillRectangle(Spanning(ThumbCentreX - ThumbSize * 0.5f, TrackY + 2.0f, ThumbSize, ThumbSize),
                          ColorQuad{ 0.0f, 0.0f, 0.0f, 0.35f }, 12.0f);
    Surface.FillRectangle(Spanning(ThumbCentreX - ThumbSize * 0.5f, TrackY + 1.0f, ThumbSize, ThumbSize),
                          ColorQuad{ 0xE0 / 255.0f, 0xE0 / 255.0f, 0xE0 / 255.0f, 1.0f }, 12.0f);
    if (Active || Hovered)
    {
        Draw->AddRect(ImVec2(ThumbCentreX - ThumbSize * 0.5f, TrackY + 1.0f),
                      ImVec2(ThumbCentreX + ThumbSize * 0.5f, TrackY + 1.0f + ThumbSize),
                      IM_COL32(255, 255, 255, 30), 12.0f);
    }
    ControlPointer Pointer{};
    Pointer.X = Io.MousePos.x;
    Pointer.Y = Io.MousePos.y;
    Pointer.Down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    Pointer.Pressed = ImGui::IsItemActivated();
    Pointer.Released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    Pointer.Enabled = Active || Hovered;
    bool Changed = false;
    if (Active && Pointer.Down && SliderWidth > ThumbSize)
    {
        const float Ratio = std::clamp((Pointer.X - SliderX - ThumbSize * 0.5f) / (SliderWidth - ThumbSize), 0.0f, 1.0f);
        const float Candidate = Minimum + Ratio * (Maximum - Minimum);
        if (std::fabs(Candidate - Value) > 1.0e-6f)
        {
            Value = Candidate;
            Changed = true;
        }
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(Origin.x, Origin.y + RowHeight - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(Width, 0.0f));
    return Changed;
}

bool FluidSegmentedPropertyRow(const EditorTypefaces& Faces, const char* Id, const char* Label,
                               int& Selected, const char* const* Labels, int Count)
{
    if (Count <= 0) return false;
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const float Width = std::max(100.0f, ImGui::GetContentRegionAvail().x);
    constexpr float LabelHeight = 16.0f;
    constexpr float GroupGap = 9.0f;
    constexpr float ControlHeight = 34.0f;
    constexpr float BottomMargin = 17.0f;
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    Draw->AddText(Faces.Regular, 12.0f, Origin, IM_COL32(162, 162, 162, 255), Label);
    const float Y = Origin.y + LabelHeight + GroupGap;
    const float CellWidth = Width / static_cast<float>(Count);
    const PlaneExtent Outer = Spanning(Origin.x, Y, Width, ControlHeight);
    Draw->AddRectFilled(ImVec2(Outer.MinimumX, Outer.MinimumY), ImVec2(Outer.MaximumX, Outer.MaximumY), IM_COL32(8, 8, 8, 255), 17.0f);
    Draw->AddRect(ImVec2(Outer.MinimumX, Outer.MinimumY), ImVec2(Outer.MaximumX, Outer.MaximumY), IM_COL32(255, 255, 255, 18), 17.0f);
    bool Changed = false;
    for (int Index = 0; Index < Count; ++Index)
    {
        const ImVec2 Cell(Origin.x + static_cast<float>(Index) * CellWidth + 3.0f, Y + 3.0f);
        const float CellH = ControlHeight - 6.0f;
        ImGui::PushID(Id);
        ImGui::PushID(Index);
        ImGui::SetCursorScreenPos(Cell);
        const bool Clicked = ImGui::InvisibleButton("##FluidSegment", ImVec2(std::max(1.0f, CellWidth - 6.0f), CellH));
        const bool Hovered = ImGui::IsItemHovered();
        if (Clicked && Selected != Index) { Selected = Index; Changed = true; }
        if (Selected == Index || Hovered)
        {
            Draw->AddRectFilled(Cell, ImVec2(Cell.x + std::max(1.0f, CellWidth - 6.0f), Cell.y + CellH),
                                Selected == Index ? IM_COL32(41, 41, 41, 255) : IM_COL32(255, 255, 255, 10), 14.0f);
        }
        const ImVec2 TextSize = ImGui::CalcTextSize(Labels[Index]);
        Draw->AddText(ImVec2(Cell.x + (CellWidth - TextSize.x) * 0.5f,
                             Cell.y + (CellH - TextSize.y) * 0.5f),
                      Selected == Index ? IM_COL32(239, 239, 239, 255) : IM_COL32(142, 142, 142, 255), Labels[Index]);
        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(Origin.x, Y + ControlHeight + BottomMargin - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(Width, 0.0f));
    return Changed;
}

void FluidPropertyHint(const EditorTypefaces& Faces, const char* Text)
{
    if (Text == nullptr || Text[0] == '\0') return;
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const float Width = std::max(100.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    Draw->AddText(Faces.Regular, 10.0f, Origin, IM_COL32(119, 119, 119, 255), Text, nullptr, Width);
    const ImVec2 Size = Faces.Regular != nullptr ? Faces.Regular->CalcTextSizeA(10.0f, Width, 0.0f, Text, nullptr) : ImGui::CalcTextSize(Text, nullptr, false, Width);
    ImGui::SetCursorScreenPos(ImVec2(Origin.x, Origin.y + Size.y + 17.0f - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(Width, 0.0f));
}

bool SliderIntRow(const InspectorPalette& Palette, const EditorTypefaces& Faces, const char* Id, const char* Label,
                  int& Value, int MinValue, int MaxValue, const char* Suffix = "")
{
    float Temp = static_cast<float>(Value);
    char Format[24]{};
    std::snprintf(Format, sizeof(Format), "%%.0f%s", Suffix != nullptr ? Suffix : "");
    if (FluidSliderRow(Palette, Faces, Id, Label, Temp, static_cast<float>(MinValue), static_cast<float>(MaxValue), Format))
    {
        Value = static_cast<int>(std::lround(Temp));
        return true;
    }
    return false;
}

using FluidSectionCardBounds = std::vector<std::pair<float, float>>;

void CloseFluidSection(FluidSectionCardBounds& Cards, float& CurrentTop, bool& CurrentOpen)
{
    if (CurrentTop < 0.0f) return;
    if (CurrentOpen)
    {
        ImGui::Unindent(12.0f);
        ImGui::Dummy(ImVec2(1.0f, std::max(0.0f, 20.0f - ImGui::GetStyle().ItemSpacing.y)));
    }
    ImGui::Unindent(8.0f);
    const float Bottom = ImGui::GetCursorScreenPos().y;
    Cards.emplace_back(CurrentTop, std::max(CurrentTop + 60.0f, Bottom));
    ImGui::Dummy(ImVec2(1.0f, std::max(0.0f, 10.0f - ImGui::GetStyle().ItemSpacing.y)));
    CurrentTop = -1.0f;
    CurrentOpen = false;
}

bool FluidPropertyGroup(const EditorTypefaces& Faces, const char* Id, const char* Title, const char* Badge,
                        bool DefaultOpen, FluidSectionCardBounds& Cards, float& CurrentTop, bool& CurrentOpen)
{
    CloseFluidSection(Cards, CurrentTop, CurrentOpen);
    static std::unordered_map<std::string, bool> GroupOpen;
    auto [State, Inserted] = GroupOpen.try_emplace(Id != nullptr ? Id : Title, DefaultOpen);
    (void)Inserted;
    CurrentOpen = State->second;
    CurrentTop = ImGui::GetCursorScreenPos().y;
    ImGui::Indent(8.0f);

    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const float Width = std::max(100.0f, ImGui::GetContentRegionAvail().x);
    constexpr float HeaderHeight = 60.0f;
    ImGui::PushID(Id != nullptr ? Id : Title);
    ImGui::InvisibleButton("##FluidPropertyGroup", ImVec2(Width, HeaderHeight));
    const bool Hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    {
        State->second = !State->second;
        CurrentOpen = State->second;
    }
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    if (Hovered) Draw->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + HeaderHeight), IM_COL32(255, 255, 255, 8), 18.0f);
    const ImVec2 ChevronCentre(Origin.x + 16.0f, Origin.y + 30.0f);
    if (CurrentOpen)
    {
        Draw->AddLine(ImVec2(ChevronCentre.x - 4.0f, ChevronCentre.y - 2.0f), ChevronCentre, IM_COL32(110, 110, 110, 255), 1.4f);
        Draw->AddLine(ChevronCentre, ImVec2(ChevronCentre.x + 4.0f, ChevronCentre.y - 2.0f), IM_COL32(110, 110, 110, 255), 1.4f);
    }
    else
    {
        Draw->AddLine(ImVec2(ChevronCentre.x - 2.0f, ChevronCentre.y - 4.0f), ImVec2(ChevronCentre.x + 2.0f, ChevronCentre.y), IM_COL32(110, 110, 110, 255), 1.4f);
        Draw->AddLine(ImVec2(ChevronCentre.x + 2.0f, ChevronCentre.y), ImVec2(ChevronCentre.x - 2.0f, ChevronCentre.y + 4.0f), IM_COL32(110, 110, 110, 255), 1.4f);
    }
    Draw->AddText(Faces.Regular, 18.0f, ImVec2(Origin.x + 34.0f, Origin.y + 19.0f), IM_COL32(224, 224, 224, 255), Title);
    if (Badge != nullptr && Badge[0] != '\0')
    {
        const ImVec2 BadgeSize = ImGui::CalcTextSize(Badge);
        Draw->AddText(Faces.Regular, 8.0f,
                      ImVec2(Origin.x + Width - 19.0f - BadgeSize.x, Origin.y + 25.0f),
                      IM_COL32(100, 100, 100, 255), Badge);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(Origin.x, Origin.y + HeaderHeight));
    if (CurrentOpen)
    {
        ImGui::Indent(12.0f);
    }
    return CurrentOpen;
}

// The reference editor uses compact pill selects and switches rather than backend-default ImGui widgets. Keep the
// selection surface on the shared DropdownControls theme, and draw switches with the same neutral track + bright knob.
bool FluidSwitch(const char* Label, bool* Value)
{
    constexpr float RowHeight = 25.0f;
    constexpr float SwitchWidth = 44.0f;
    constexpr float SwitchHeight = 25.0f;
    const ImVec2 RowMin = ImGui::GetCursorScreenPos();
    const float RowWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 SwitchMin(RowMin.x + std::max(0.0f, RowWidth - SwitchWidth), RowMin.y);
    ImGui::PushID(Label);
    ImGui::SetCursorScreenPos(SwitchMin);
    const bool Pressed = ImGui::InvisibleButton("##FluidSwitch", ImVec2(SwitchWidth, SwitchHeight));
    const bool Hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    PixelSpace Surface;
    (void)Surface.Begin(SurfaceLayer::Window, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y, 1.0f);
    const float Alpha = ImGui::GetStyle().Alpha;
    const PlaneExtent SwitchExtent = Spanning(SwitchMin.x, SwitchMin.y, SwitchWidth, SwitchHeight);
    const ColorQuad Track = *Value ? ColorQuad{ 1.0f, 1.0f, 1.0f, Alpha }
                                   : ColorQuad{ 0x22 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, Alpha };
    Surface.FillRectangle(SwitchExtent, Track, 12.5f);
    ControlKit::OutlineRounded(Surface, SwitchExtent,
                               *Value ? ColorQuad{ 1.0f, 1.0f, 1.0f, Alpha }
                                      : ColorQuad{ 1.0f, 1.0f, 1.0f, 0.05f * Alpha },
                               12.5f);
    const float KnobX = SwitchMin.x + 2.0f + (*Value ? 19.0f : 0.0f);
    ControlKit::FillCircle(Surface, KnobX + 9.5f, SwitchMin.y + 2.0f + 9.5f, 9.5f,
                           *Value ? ColorQuad{ 0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f, Alpha }
                                  : ColorQuad{ 0xBD / 255.0f, 0xBD / 255.0f, 0xBD / 255.0f, Alpha });
    if (Hovered)
        ImGui::GetWindowDrawList()->AddRect(ImVec2(SwitchMin.x, SwitchMin.y),
                                             ImVec2(SwitchMin.x + SwitchWidth, SwitchMin.y + SwitchHeight),
                                             IM_COL32(255, 255, 255, 16), 12.5f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 12.0f,
                                       ImVec2(RowMin.x, RowMin.y + (RowHeight - 12.0f) * 0.5f),
                                       ImGui::GetColorU32(ImGuiCol_Text), Label);
    if (Pressed) *Value = !*Value;
    ImGui::SetCursorScreenPos(ImVec2(RowMin.x, RowMin.y + RowHeight + 20.0f));
    return Pressed;
}

bool FluidSelect(const char* Id, const char* Label, int* Value, const char* const* Options, int Count, float Width = 150.0f)
{
    if (Count <= 0) return false;
    *Value = std::clamp(*Value, 0, Count - 1);
    ImGui::PushID(Id);
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const float RowWidth = std::max(100.0f, std::min(Width, ImGui::GetContentRegionAvail().x));
    if (Label && *Label)
    {
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 12.0f, Origin, IM_COL32(162, 162, 162, 255), Label);
    }
    ImGui::SetCursorScreenPos(ImVec2(Origin.x, Origin.y + 25.0f));
    ImGui::SetNextItemWidth(RowWidth);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 17.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(11, 11, 11, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(28, 28, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(34, 34, 34, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 18));
    const bool Open = DropdownControlSurface::BeginSelection("##FluidSelect", Options[*Value]);
    bool Changed = false;
    if (Open)
    {
        for (int Index = 0; Index < Count; ++Index)
        {
            if (DropdownControlSurface::Option(Options[Index], Index == *Value))
            {
                *Value = Index;
                Changed = true;
            }
        }
        DropdownControlSurface::EndSelection();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(ImVec2(Origin.x, Origin.y + 76.0f - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(RowWidth, 0.0f));
    ImGui::PopID();
    return Changed;
}

bool FluidSelectableRow(const char* Label, OutlinerNodeId Id, bool Selected, bool Enabled, bool HasEyeToggle,
                        float Width, float Height, bool* DoubleClicked = nullptr, bool* ToggleClicked = nullptr)
{
    ImGui::PushID(Label);
    const ImVec2 Min = ImGui::GetCursorScreenPos();
    const float MainWidth = std::max(24.0f, Width - (HasEyeToggle ? 30.0f : 0.0f));
    const bool Clicked = ImGui::InvisibleButton("##FluidRow", ImVec2(MainWidth, Height));
    const bool Hovered = ImGui::IsItemHovered();
    if (DoubleClicked) *DoubleClicked = Clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    if (Selected || Hovered)
    {
        Draw->AddRectFilled(Min, ImVec2(Min.x + Width, Min.y + Height),
                            Selected ? IM_COL32(31, 33, 37, 255) : IM_COL32(255, 255, 255, 12), 7.0f);
    }
    if (Selected)
    {
        const ImU32 Accent = Id == OutlinerNodeId::Domain ? IM_COL32(90, 169, 255, 255)
                             : Id == OutlinerNodeId::Source || Id == OutlinerNodeId::Sun ? IM_COL32(255, 180, 84, 255)
                                                                                         : IM_COL32(190, 190, 190, 255);
        Draw->AddRectFilled(ImVec2(Min.x, Min.y + 1.0f), ImVec2(Min.x + 3.0f, Min.y + Height - 1.0f), Accent, 2.0f);
    }

    const ImU32 IconInk = !Enabled ? IM_COL32(100, 100, 100, 150)
                          : Id == OutlinerNodeId::Domain ? IM_COL32(90, 169, 255, 255)
                          : Id == OutlinerNodeId::Source || Id == OutlinerNodeId::Sun ? IM_COL32(255, 180, 84, 255)
                                                                                     : IM_COL32(180, 180, 180, 255);
    const ImVec2 ChevronCentre(Min.x + 15.0f, Min.y + Height * 0.5f);
    Draw->AddLine(ImVec2(ChevronCentre.x - 2.0f, ChevronCentre.y - 4.0f), ImVec2(ChevronCentre.x + 2.0f, ChevronCentre.y), IM_COL32(112, 112, 112, 255), 1.2f);
    Draw->AddLine(ImVec2(ChevronCentre.x + 2.0f, ChevronCentre.y), ImVec2(ChevronCentre.x - 2.0f, ChevronCentre.y + 4.0f), IM_COL32(112, 112, 112, 255), 1.2f);
    const ImVec2 IconOrigin(Min.x + 36.0f, Min.y + (Height - 16.0f) * 0.5f);
    if (Id == OutlinerNodeId::Domain)
        PanelChrome::DrawGlyph("M3 7.5 12 3l9 4.5v9L12 21l-9-4.5v-9ZM12 3v18m9-13.5-9 4.5-9-4.5", IconOrigin, 16.0f, IconInk, 1.5f);
    else if (Id == OutlinerNodeId::Source)
        PanelChrome::DrawGlyph("M12 22a8 8 0 0 0 8-8c0-3-2-5-4-7-1 3-2 4-4 5-1-3-3-5-5-8-1 4-3 6-3 10a8 8 0 0 0 8 8Z", IconOrigin, 16.0f, IconInk, 1.5f);
    else if (Id == OutlinerNodeId::Collider)
        PanelChrome::DrawGlyph("M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18Z", IconOrigin, 16.0f, IconInk, 1.5f);
    else
        PanelChrome::DrawGlyph("M12 2v3m0 14v3M2 12h3m14 0h3M4.9 4.9 7 7m10 10 2.1 2.1m0-14.2L17 7M7 17l-2.1 2.1M12 7a5 5 0 1 0 0 10a5 5 0 1 0 0-10Z", IconOrigin, 16.0f, IconInk, 1.5f);
    Draw->AddText(ImGui::GetFont(), 13.0f, ImVec2(Min.x + 66.0f, Min.y + (Height - 13.0f) * 0.5f),
                  Enabled ? (Selected ? IM_COL32(240, 240, 240, 255) : IM_COL32(196, 196, 196, 255)) : IM_COL32(98, 98, 98, 255), Label);
    if (Id == OutlinerNodeId::Domain)
        Draw->AddText(ImGui::GetFont(), 8.0f, ImVec2(Min.x + Width - 48.0f, Min.y + (Height - 8.0f) * 0.5f), IM_COL32(104, 104, 104, 255), "VOLUME");

    bool Toggled = false;
    if (HasEyeToggle)
    {
        const ImVec2 EyeMin(Min.x + Width - 28.0f, Min.y + (Height - 24.0f) * 0.5f);
        ImGui::SetCursorScreenPos(EyeMin);
        Toggled = ImGui::InvisibleButton("##EyeToggle", ImVec2(24.0f, 24.0f));
        const bool EyeHovered = ImGui::IsItemHovered();
        if (ToggleClicked) *ToggleClicked = Toggled;
        if (Selected || Hovered || EyeHovered || !Enabled)
        {
            const char* EyePath = Enabled ? "M2 12Q12 0 22 12Q12 24 2 12ZM12 9A3 3 0 1 0 12 15A3 3 0 1 0 12 9Z"
                                          : "M2 12Q12 0 22 12Q12 24 2 12ZM12 9A3 3 0 1 0 12 15A3 3 0 1 0 12 9ZM4 4L20 20";
            PanelChrome::DrawGlyph(EyePath, ImVec2(EyeMin.x + 4.0f, EyeMin.y + 4.0f), 16.0f,
                                   Enabled ? IM_COL32(132, 132, 132, 255) : IM_COL32(244, 72, 66, 255), 1.5f);
        }
    }
    if (ToggleClicked && !HasEyeToggle) *ToggleClicked = false;
    ImGui::SetCursorScreenPos(ImVec2(Min.x, Min.y + Height + 2.0f));
    ImGui::PopID();
    return Clicked;
}

bool FluidChip(const char* Label, bool Active, float Width)
{
    ImGui::PushID(Label);
    const ImVec2 Min = ImGui::GetCursorScreenPos();
    constexpr float Height = 25.0f;
    const bool Clicked = ImGui::InvisibleButton("##FluidChip", ImVec2(Width, Height));
    const bool Hovered = ImGui::IsItemHovered();
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    const ImU32 Fill = Active ? IM_COL32(43, 43, 43, 255) : (Hovered ? IM_COL32(36, 36, 36, 255) : IM_COL32(22, 22, 22, 255));
    Draw->AddRectFilled(Min, ImVec2(Min.x + Width, Min.y + Height), Fill, Height * 0.5f);
    Draw->AddRect(Min, ImVec2(Min.x + Width, Min.y + Height), IM_COL32(255, 255, 255, Active ? 28 : 16), Height * 0.5f);
    const ImVec2 TextSize = ImGui::GetFont()->CalcTextSizeA(10.0f, 200.0f, 0.0f, Label, nullptr);
    Draw->AddText(ImGui::GetFont(), 10.0f,
                  ImVec2(Min.x + (Width - TextSize.x) * 0.5f, Min.y + (Height - TextSize.y) * 0.5f),
                  Active ? IM_COL32(235, 235, 235, 255) : IM_COL32(149, 149, 149, 255), Label);
    ImGui::PopID();
    return Clicked;
}

void FluidInspectorTabStrip(const EditorTypefaces& Faces, int& Active)
{
    static constexpr const char* Labels[3] = { "Source", "Simulation", "Rendering" };
    const ImVec2 Parent = ImGui::GetCursorScreenPos();
    const float Available = ImGui::GetContentRegionAvail().x;
    const float X = Parent.x + 3.0f;
    const float Width = std::max(120.0f, Available - 6.0f);
    constexpr float Height = 39.0f;
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    Draw->AddRectFilled(ImVec2(X, Parent.y), ImVec2(X + Width, Parent.y + Height), IM_COL32(8, 8, 8, 255), 20.0f);
    Draw->AddRect(ImVec2(X, Parent.y), ImVec2(X + Width, Parent.y + Height), IM_COL32(255, 255, 255, 18), 20.0f);
    constexpr float InnerPad = 4.0f;
    constexpr float Gap = 3.0f;
    const float CellWidth = (Width - InnerPad * 2.0f - Gap * 2.0f) / 3.0f;
    for (int Index = 0; Index < 3; ++Index)
    {
        const ImVec2 Cell(X + InnerPad + static_cast<float>(Index) * (CellWidth + Gap), Parent.y + InnerPad);
        ImGui::PushID(Index);
        ImGui::SetCursorScreenPos(Cell);
        const bool Clicked = ImGui::InvisibleButton("##InspectorTab", ImVec2(CellWidth, Height - InnerPad * 2.0f));
        const bool Hovered = ImGui::IsItemHovered();
        if (Clicked) Active = Index;
        const bool Selected = Active == Index;
        if (Selected || Hovered)
        {
            Draw->AddRectFilled(Cell, ImVec2(Cell.x + CellWidth, Cell.y + Height - InnerPad * 2.0f),
                                Selected ? IM_COL32(41, 41, 41, 255) : IM_COL32(255, 255, 255, 10), 16.0f);
            if (Selected) Draw->AddRect(Cell, ImVec2(Cell.x + CellWidth, Cell.y + Height - InnerPad * 2.0f), IM_COL32(255, 255, 255, 15), 16.0f);
        }
        const ImVec2 LabelSize = Faces.Regular ? Faces.Regular->CalcTextSizeA(12.0f, 1000.0f, 0.0f, Labels[Index], nullptr) : ImGui::CalcTextSize(Labels[Index]);
        Draw->AddText(Faces.Regular, 12.0f,
                      ImVec2(Cell.x + (CellWidth - LabelSize.x) * 0.5f,
                             Cell.y + ((Height - InnerPad * 2.0f) - LabelSize.y) * 0.5f),
                      Selected ? IM_COL32(239, 239, 239, 255) : IM_COL32(119, 119, 119, 255), Labels[Index]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(Parent.x, Parent.y + Height + 14.0f - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(Available, 0.0f));
}

bool FluidPresetCard(const FluidDynamics::FluidPresetDescriptor& Preset, bool Selected, float Width)
{
    ImGui::PushID(Preset.Identity);
    const ImVec2 Min = ImGui::GetCursorScreenPos();
    constexpr float Height = 62.0f;
    const bool Clicked = ImGui::InvisibleButton("##PresetCard", ImVec2(Width, Height));
    const bool Hovered = ImGui::IsItemHovered();
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    const ImU32 Fill = Selected ? IM_COL32(42, 42, 42, 255) : (Hovered ? IM_COL32(34, 34, 34, 255) : IM_COL32(26, 26, 26, 255));
    Draw->AddRectFilled(Min, ImVec2(Min.x + Width, Min.y + Height), Fill, 16.0f);
    Draw->AddRect(Min, ImVec2(Min.x + Width, Min.y + Height), IM_COL32(255, 255, 255, Selected ? 24 : 12), 16.0f);
    Draw->PushClipRect(Min, ImVec2(Min.x + Width, Min.y + Height), true);
    const ImVec2 IconMin(Min.x + 12.0f, Min.y + 10.0f);
    Draw->AddRectFilled(IconMin, ImVec2(IconMin.x + 38.0f, IconMin.y + 42.0f), IM_COL32(255, 255, 255, 8), 12.0f);
    const uint32_t Tint = Preset.Category == FluidDynamics::FluidPresetCategory::Smoke ? 0xAEBAC6u :
                          Preset.Category == FluidDynamics::FluidPresetCategory::Blast ? 0xE0E0E0u : 0xFF9838u;
    const char* FlamePath = "M12 22a8 8 0 0 0 8-8c0-3-2-5-4-7-1 3-2 4-4 5-1-3-3-5-5-8-1 4-3 6-3 10a8 8 0 0 0 8 8Z";
    PanelChrome::DrawGlyph(FlamePath, ImVec2(IconMin.x + 10.0f, IconMin.y + 10.0f), 18.0f, Tint, 1.6f);
    Draw->AddText(ImGui::GetFont(), 12.0f, ImVec2(Min.x + 62.0f, Min.y + 14.0f), IM_COL32(224, 224, 224, 255), Preset.Title);
    Draw->AddText(ImGui::GetFont(), 10.0f, ImVec2(Min.x + 62.0f, Min.y + 36.0f), IM_COL32(125, 125, 125, 255), Preset.Summary);
    const ImVec2 ArrowTip(Min.x + Width - 14.0f, Min.y + 20.0f);
    Draw->AddLine(ImVec2(ArrowTip.x - 9.0f, ArrowTip.y + 9.0f), ArrowTip, IM_COL32(146, 146, 146, 255), 1.1f);
    Draw->AddLine(ArrowTip, ImVec2(ArrowTip.x - 1.0f, ArrowTip.y + 5.0f), IM_COL32(146, 146, 146, 255), 1.1f);
    Draw->AddLine(ArrowTip, ImVec2(ArrowTip.x - 5.0f, ArrowTip.y + 1.0f), IM_COL32(146, 146, 146, 255), 1.1f);
    Draw->PopClipRect();
    ImGui::PopID();
    return Clicked;
}

ControlKitPalette ResolveFluidControlPalette()
{
    auto C = [](uint32_t Rgb, float Alpha = 1.0f) {
        return ColorQuad{ static_cast<float>((Rgb >> 16u) & 255u) / 255.0f,
                          static_cast<float>((Rgb >> 8u) & 255u) / 255.0f,
                          static_cast<float>(Rgb & 255u) / 255.0f, Alpha };
    };
    ControlKitPalette P{};
    P.TabBackground = C(0x0B0B0B);
    P.Panel = C(0x121212);
    P.Inset = C(0x0B0B0B);
    P.Field = C(0x000000);
    P.Raised = C(0x1A1A1A);
    P.Selected = C(0x2A2A2A);
    P.Stroke = C(0xFFFFFF, 0.07f);
    P.StrokeStrong = C(0xFFFFFF, 0.13f);
    P.Divider = C(0xFFFFFF, 0.07f);
    P.Card = C(0x1A1A1A);
    P.CardSub = C(0x222222);
    P.Text = C(0xF0F0F0);
    P.TextDim = C(0xA0A0A0);
    P.TextFaint = C(0x707070);
    P.Primary = C(0xE0E0E0);
    P.PrimaryInk = C(0x111111);
    P.Accent = C(0xE0E0E0);
    P.AccentInk = C(0x111111);
    P.AccentSoft = C(0xFFFFFF, 0.08f);
    P.SliderTrack = C(0x222222);
    P.SliderFill = C(0x4A4A4A);
    P.SliderThumb = C(0xE0E0E0);
    P.SwitchKnobOff = C(0xBDBDBD);
    return P;
}

std::string SanitizeSlug(std::string_view Name)
{
    std::string Result;
    Result.reserve(Name.size());
    for (char Ch : Name)
    {
        if (std::isalnum(static_cast<unsigned char>(Ch)))
        {
            Result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(Ch))));
        }
        else if (Ch == ' ' || Ch == '-' || Ch == '_')
        {
            if (Result.empty() || Result.back() != '_') Result.push_back('_');
        }
    }
    while (!Result.empty() && Result.back() == '_') Result.pop_back();
    return Result.empty() ? "fluid_plume" : Result;
}

std::filesystem::path ResolveSidecarPath(RecordToken Token, FluidPlumeConfiguration& Config)
{
    const std::filesystem::path& LevelFile = InspectorRuntime::Instance().QueryLevelFile();
    if (LevelFile.empty()) return {};
    if (Config.SpaceFile.empty())
    {
        const std::string RecordName = SceneDirectory::Instance().QueryName(Token);
        Config.SpaceFile             = SanitizeSlug(RecordName) + ".fluidspace";
    }
    return LevelFile.parent_path() / Config.SpaceFile;
}

void PushUndoSnapshot(FluidDocumentTab& Tab)
{
    if (Tab.UndoStack.size() >= 48u)
    {
        Tab.UndoStack.erase(Tab.UndoStack.begin());
    }
    Tab.UndoStack.push_back(Tab.Scene);
    Tab.RedoStack.clear();
    Tab.Dirty = true;
}

void EnqueueBurstPattern(FluidDocumentTab& Tab, FluidDynamics::FluidBurstPattern Pattern,
                         std::array<float, 3> BaseCentre = { 0.5f, 0.24f, 0.5f })
{
    const auto& P = Tab.Scene.Parameters;
    FluidBlastImpulse Base{};
    Base.CentreUvw     = BaseCentre;
    Base.Radius        = P.BlastRadius;
    Base.Strength      = P.BlastStrength;
    Base.Temperature   = P.BlastTemperature;
    Base.Fuel          = P.BlastFuel;
    Base.Smoke         = P.BlastSmoke;
    Base.Lobes         = P.BlastLobes;
    Base.SpawnShrapnel = P.ShrapnelEnabled;

    switch (Pattern)
    {
        case FluidDynamics::FluidBurstPattern::SingleBurst:
            Tab.PendingBlasts.push_back(Base);
            break;

        case FluidDynamics::FluidBurstPattern::Shrapnel:
            Base.SpawnShrapnel = true;
            Base.Strength *= 1.18f;
            Tab.PendingBlasts.push_back(Base);
            break;

        case FluidDynamics::FluidBurstPattern::ClusterSalvo:
            for (int Idx = 0; Idx < 4; ++Idx)
            {
                const float Angle     = static_cast<float>(Idx) * 1.5707963f + 0.35f;
                FluidBlastImpulse Sub = Base;
                Sub.CentreUvw         = {
                    std::clamp(BaseCentre[0] + std::cos(Angle) * 0.14f, 0.14f, 0.86f),
                    std::clamp(BaseCentre[1] + 0.04f * static_cast<float>(Idx % 2), 0.10f, 0.78f),
                    std::clamp(BaseCentre[2] + std::sin(Angle) * 0.14f, 0.14f, 0.86f)
                };
                Sub.Radius   = P.BlastRadius * 0.78f;
                Sub.Strength = P.BlastStrength * 0.88f;
                Tab.PendingBlasts.push_back(Sub);
            }
            break;

        case FluidDynamics::FluidBurstPattern::CollidingWaves:
        {
            FluidBlastImpulse Left    = Base;
            Left.CentreUvw            = { 0.28f, BaseCentre[1], 0.50f };
            Left.HasDirectionalVel    = true;
            Left.DirectionalVelocity  = { P.BlastStrength * 0.85f, P.BlastStrength * 0.18f, 0.0f };
            FluidBlastImpulse Right   = Base;
            Right.CentreUvw           = { 0.72f, BaseCentre[1], 0.50f };
            Right.HasDirectionalVel   = true;
            Right.DirectionalVelocity = { -P.BlastStrength * 0.85f, P.BlastStrength * 0.18f, 0.0f };
            Tab.PendingBlasts.push_back(Left);
            Tab.PendingBlasts.push_back(Right);
            break;
        }

        case FluidDynamics::FluidBurstPattern::MushroomCloud:
        {
            FluidBlastImpulse Stem   = Base;
            Stem.CentreUvw           = { BaseCentre[0], 0.12f, BaseCentre[2] };
            Stem.Radius              = P.BlastRadius * 0.75f;
            Stem.HasDirectionalVel   = true;
            Stem.DirectionalVelocity = { 0.0f, P.BlastStrength * 1.25f, 0.0f };
            FluidBlastImpulse Cap    = Base;
            Cap.CentreUvw            = { BaseCentre[0], 0.32f, BaseCentre[2] };
            Cap.Radius               = P.BlastRadius * 1.28f;
            Cap.Temperature          = P.BlastTemperature * 1.22f;
            Cap.Fuel                 = P.BlastFuel * 1.25f;
            Tab.PendingBlasts.push_back(Stem);
            Tab.PendingBlasts.push_back(Cap);
            break;
        }
    }
}

void SyncDocumentToRecord(const FluidDocumentTab& Tab, RecordToken Token, FluidPlumeConfiguration& Config, bool SaveSidecarFile)
{
    const auto& P               = Tab.Scene.Parameters;
    Config.BuoyancyLift         = P.Buoyancy;
    Config.VorticityConfinement = P.VorticityConfinement;
    Config.DissipationRate      = P.SmokeDissipation;
    Config.FuelInjectionRate    = P.SourceFuel;
    Config.BlackbodyCoreKelvin  = std::clamp(1000.0f + P.TemperatureScale * 1200.0f, 800.0f, 3600.0f);
    Config.CombustionEnabled    = (P.SourceFuel > 0.05f || P.BlastFuel > 0.05f);
    Config.BurstPattern         = static_cast<uint32_t>(Tab.BurstPattern);
    if (!Tab.PresetIdentity.empty())
    {
        Config.PresetIdentity = Tab.PresetIdentity;
    }

    if (SaveSidecarFile)
    {
        const std::filesystem::path Sidecar = ResolveSidecarPath(Token, Config);
        if (!Sidecar.empty())
        {
            (void)FluidDynamics::SaveFluidSpaceFile(Sidecar, Tab.Scene);
        }
    }
    Config.AdvanceRevision();
}

void BindSessionToRecord(FluidEditorSession& Session, RecordToken Token, FluidPlumeConfiguration& Config)
{
    if (Session.BoundToken == Token && !Session.Tabs.empty())
    {
        return;
    }
    Session.BoundToken = Token;
    Session.Tabs.clear();
    Session.ActiveTab = 0;
    Session.ActiveInspector = 0;
    Session.ActiveViewportTool = 0;
    Session.CameraView = 0;
    Session.ShowPresetsView = false;
    Session.MaximizeViewport = false;
    Session.ShowShortcuts = false;
    Session.DiagnosticsOpen = false;
    Session.ShowImportModal = false;
    Session.SelectedNode = OutlinerNodeId::Source;
    Session.RenamingNode = -1;
    Session.DocumentName[0] = '\0';
    Session.DocumentNameForTab.clear();
    Session.HasPreviousBrush = false;
    Session.Flipbook.DialogOpen = false;
    Session.Flipbook.Recording = false;
    Session.Flipbook.RestartIssued = false;
    Session.Flipbook.BurstIssued = false;
    Session.Flipbook.WarmupRemaining = 0.0f;
    Session.Flipbook.Captured = 0;
    Session.Flipbook.SheetRgba.clear();

    FluidDocumentTab MainTab{};
    MainTab.GpuSlot        = 0u;
    MainTab.PresetIdentity = Config.PresetIdentity.empty() ? "fuel_air_blast" : Config.PresetIdentity;
    MainTab.BurstPattern   = static_cast<FluidDynamics::FluidBurstPattern>(std::clamp<uint32_t>(Config.BurstPattern, 0u, 4u));

    const std::filesystem::path Sidecar = ResolveSidecarPath(Token, Config);
    if (!Sidecar.empty() && std::filesystem::exists(Sidecar) && FluidDynamics::LoadFluidSpaceFile(Sidecar, MainTab.Scene))
    {
        MainTab.FilePath     = Sidecar;
        MainTab.Title        = Sidecar.filename().string();
        Session.StatusNotice = "Loaded sidecar " + MainTab.Title;
    }
    else
    {
        MainTab.Scene.Name       = SceneDirectory::Instance().QueryName(Token);
        MainTab.Scene.Parameters = FluidDynamics::ConstructPresetParameters(MainTab.PresetIdentity);
        MainTab.Scene.Camera.SnapToTarget();
        MainTab.FilePath         = Sidecar;
        MainTab.Title            = Sidecar.empty() ? (MainTab.PresetIdentity + ".fluidspace") : Sidecar.filename().string();
        if (const auto* Preset = FluidDynamics::FindFluidPreset(MainTab.PresetIdentity);
            Preset && Preset->TriggerExplosionOnLoad)
        {
            EnqueueBurstPattern(MainTab, MainTab.BurstPattern);
        }
        Session.StatusNotice = "Initialized from preset '" + MainTab.PresetIdentity + "'";
    }
    MainTab.RestartPending = true;
    Session.Tabs.push_back(std::move(MainTab));
}

} // namespace

//========================================================================================================================
//                                             MAIN FLUID EDITOR CONSTRUCTION
//========================================================================================================================

void FluidPlumeEditor::ConstructInterface(RecordToken Token, FluidPlumeInspector& Inspector)
{
    FluidEditorSession&      Session = AccessSession();
    FluidPlumeConfiguration& Config  = Inspector.AccessParameters();
    BindSessionToRecord(Session, Token, Config);

    const ControlKitPalette PreviousControlPalette = ControlKit::Palette();
    ControlKit::ConfigurePalette(ResolveFluidControlPalette());
    InspectorPalette Palette = InspectorPalette::Resolve();
    Palette.ViewportBg = IM_COL32(11, 11, 11, 255);
    Palette.ColumnBg = IM_COL32(18, 18, 18, 255);
    Palette.StatusBarBg = IM_COL32(11, 11, 11, 255);
    Palette.ViewportBarBg = IM_COL32(23, 23, 23, 255);
    Palette.InspectorTopBg = IM_COL32(18, 18, 18, 255);
    Palette.InspectorBottomBg = IM_COL32(18, 18, 18, 255);
    Palette.EdgeBorder = IM_COL32(255, 255, 255, 18);
    Palette.CountCardBg = IM_COL32(255, 255, 255, 9);
    Palette.CountCardBorder = IM_COL32(255, 255, 255, 18);
    Palette.SearchBg = IM_COL32(255, 255, 255, 11);
    Palette.SearchBorder = IM_COL32(255, 255, 255, 18);
    Palette.RowHoverBg = IM_COL32(255, 255, 255, 11);
    Palette.RowSelectedTop = IM_COL32(255, 255, 255, 18);
    Palette.RowSelectedBottom = IM_COL32(255, 255, 255, 13);
    Palette.RowSelectedBorder = IM_COL32(255, 255, 255, 28);
    Palette.InsetFieldBg = IM_COL32(0, 0, 0, 255);
    Palette.InsetFieldBorder = IM_COL32(255, 255, 255, 18);
    Palette.SliderTrackBg = IM_COL32(34, 34, 34, 255);
    Palette.SliderFillBg = IM_COL32(74, 74, 74, 255);
    Palette.SliderThumbBg = IM_COL32(224, 224, 224, 255);
    Palette.ToggleOnBg = IM_COL32(224, 224, 224, 255);
    Palette.ToggleOffBg = IM_COL32(34, 34, 34, 255);
    Palette.SegmentedBg = IM_COL32(8, 8, 8, 255);
    Palette.SegmentedActiveBg = IM_COL32(41, 41, 41, 255);
    Palette.TextPrimary = IM_COL32(240, 240, 240, 255);
    Palette.TextSecondary = IM_COL32(190, 190, 190, 255);
    Palette.TextMuted = IM_COL32(142, 142, 142, 255);
    Palette.TextFaint = IM_COL32(102, 102, 102, 255);
    Palette.LiveGreen = IM_COL32(52, 199, 89, 255);
    Palette.ThemeAccent = IM_COL32(224, 224, 224, 255);
    Palette.WarmAmber = IM_COL32(255, 152, 56, 255);
    const EditorTypefaces  Faces   = EditorTypefaces::Resolve();
    ImGuiIO&               Io      = ImGui::GetIO();

    FluidDocumentTab& Tab    = Session.Tabs[std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1)];
    auto&             Params = Tab.Scene.Parameters;

    InspectorRuntime&    Runtime      = InspectorRuntime::Instance();
    const float          Scale        = Runtime.ResolveInterfaceScale();
    const ImGuiViewport* Main         = ImGui::GetMainViewport();
    const ImVec2         Position    = Main->WorkPos;
    const ImVec2         Size         = Main->WorkSize;

    constexpr const char* WindowName = "##FluidPlumeEditorWindow";
    if (!ImGui::IsPopupOpen(WindowName))
    {
        ImGui::OpenPopup(WindowName);
    }
    ImGui::SetNextWindowPos(Position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Palette.ViewportBg));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.02f, 0.02f, 0.03f, 0.58f));
    const bool Shown = ImGui::BeginPopupModal(WindowName, nullptr,
                                              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!Shown)
    {
        ControlKit::ConfigurePalette(PreviousControlPalette);
        return;
    }

    bool ClosingModal = false;

    if (!Io.WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (Session.ShowShortcuts) Session.ShowShortcuts = false;
            else if (Session.Flipbook.DialogOpen) Session.Flipbook.DialogOpen = false;
            else if (Session.ShowImportModal) Session.ShowImportModal = false;
            else if (Session.MaximizeViewport || Session.ShowPresetsView)
            {
                Session.MaximizeViewport = false;
                Session.ShowPresetsView = false;
            }
            else ClosingModal = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) Params.Paused = !Params.Paused;
        if (ImGui::IsKeyPressed(ImGuiKey_Period) || ImGui::IsKeyPressed(ImGuiKey_S)) Tab.StepOncePending = true;
        if (ImGui::IsKeyPressed(ImGuiKey_R) || ImGui::IsKeyPressed(ImGuiKey_C))
        {
            Tab.RestartPending   = true;
            Session.StatusNotice = "Restarted the live simulation";
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_D))
        {
            EnqueueBurstPattern(Tab, Tab.BurstPattern);
            ++Config.BurstGeneration;
            Config.AdvanceRevision();
            Session.StatusNotice = "Triggered the selected burst";
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F))
        {
            Tab.Scene.Camera.Reset();
            Tab.Scene.Camera.SnapToTarget();
            Session.StatusNotice = "Focused the simulation volume";
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B)) Session.ShowDomainWire = !Session.ShowDomainWire;
        if (ImGui::IsKeyPressed(ImGuiKey_T))
        {
            Params.AutoTurntable = !Params.AutoTurntable;
            Tab.Scene.Camera.AutoTurntable = Params.AutoTurntable;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_1)) Session.ActiveViewportTool = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) Session.ActiveViewportTool = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) Session.ActiveViewportTool = 2;
        if (Io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        {
            SyncDocumentToRecord(Tab, Token, Config, true);
            Tab.Dirty = false;
            Session.StatusNotice = "Saved scene settings and camera";
        }
        if (Io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !Tab.UndoStack.empty())
        {
            Tab.RedoStack.push_back(Tab.Scene);
            Tab.Scene = Tab.UndoStack.back();
            Tab.UndoStack.pop_back();
            Session.StatusNotice = "Undo parameter change";
        }
        if (Io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y) && !Tab.RedoStack.empty())
        {
            Tab.UndoStack.push_back(Tab.Scene);
            Tab.Scene = Tab.RedoStack.back();
            Tab.RedoStack.pop_back();
            Session.StatusNotice = "Redo parameter change";
        }
    }

    ImDrawList* ModalInk = ImGui::GetWindowDrawList();
    const float HeaderH = 68.0f * Scale;
    const float DocumentBarH = 35.0f * Scale;
    const float HeaderPad = 24.0f * Scale;
    ModalInk->AddRectFilled(Position, ImVec2(Position.x + Size.x, Position.y + Size.y), Palette.ViewportBg);
    ModalInk->AddRectFilled(Position, ImVec2(Position.x + Size.x, Position.y + HeaderH), Palette.ViewportBg);
    ModalInk->AddLine(ImVec2(Position.x, Position.y + HeaderH), ImVec2(Position.x + Size.x, Position.y + HeaderH), IM_COL32(255, 255, 255, 10));

    auto DrawPillButton = [&](const char* Id, const char* Label, const ImVec2& Min, const ImVec2& ButtonSize,
                              bool Selected = false, bool Primary = false) -> bool
    {
        ImGui::SetCursorScreenPos(Min);
        const bool Pressed = ImGui::InvisibleButton(Id, ButtonSize);
        const bool Hovered = ImGui::IsItemHovered();
        const ImU32 Fill = Primary ? IM_COL32(224, 224, 224, 255) :
                           Selected ? IM_COL32(42, 42, 42, 255) :
                           Hovered ? IM_COL32(36, 36, 36, 255) : IM_COL32(255, 255, 255, 8);
        const ImU32 Ink = Primary ? IM_COL32(17, 17, 17, 255) :
                          Selected || Hovered ? IM_COL32(240, 240, 240, 255) : IM_COL32(156, 156, 156, 255);
        ModalInk->AddRectFilled(Min, ImVec2(Min.x + ButtonSize.x, Min.y + ButtonSize.y), Fill, ButtonSize.y * 0.5f);
        if (!Primary) ModalInk->AddRect(Min, ImVec2(Min.x + ButtonSize.x, Min.y + ButtonSize.y), IM_COL32(255, 255, 255, Selected ? 18 : 10), ButtonSize.y * 0.5f);
        const ImVec2 TextSize = ImGui::CalcTextSize(Label);
        ModalInk->AddText(Faces.Regular, 12.0f * Scale,
                          ImVec2(Min.x + (ButtonSize.x - TextSize.x * Scale) * 0.5f,
                                 Min.y + (ButtonSize.y - TextSize.y * Scale) * 0.5f), Ink, Label);
        return Pressed;
    };

    auto CreateNewDocument = [&]()
    {
        if (Session.Tabs.size() >= 4u) return;
        FluidDocumentTab Fresh{};
        Fresh.GpuSlot          = static_cast<uint32_t>(Session.Tabs.size());
        Fresh.Title            = "Document_" + std::to_string(Session.Tabs.size() + 1u) + ".fluidspace";
        Fresh.PresetIdentity   = "ue5_pyro_default";
        Fresh.Scene.Name       = "New fluid scene";
        Fresh.Scene.Parameters = FluidDynamics::ConstructPresetParameters(Fresh.PresetIdentity);
        Fresh.Scene.Camera.SnapToTarget();
        Fresh.RestartPending   = true;
        Session.Tabs.push_back(std::move(Fresh));
        Session.ActiveTab = static_cast<int>(Session.Tabs.size()) - 1;
        Session.DocumentNameForTab.clear();
        Session.StatusNotice = "Created a new fluid document";
    };
    auto LoadSampleScene = [&](FluidDocumentTab& Target, const FluidDynamics::FluidSampleDescriptor& Sample)
    {
        PushUndoSnapshot(Target);
        Target.Scene          = FluidDynamics::ConstructSampleFluidScene(Sample.Identity);
        Target.PresetIdentity = Sample.PresetKey;
        Target.RestartPending = true;
        if (const auto* Preset = FluidDynamics::FindFluidPreset(Sample.PresetKey); Preset && Preset->TriggerExplosionOnLoad)
            EnqueueBurstPattern(Target, Target.BurstPattern);
        SyncDocumentToRecord(Target, Token, Config, false);
        Session.DocumentNameForTab.clear();
        Session.StatusNotice = std::string("Loaded sample: ") + Sample.Name;
    };
    auto SaveActiveScene = [&](FluidDocumentTab& Target)
    {
        SyncDocumentToRecord(Target, Token, Config, true);
        Target.Dirty = false;
        Session.StatusNotice = Config.SpaceFile.empty() ? "Saved scene settings" : "Saved scene to " + Config.SpaceFile;
    };

    // App header: overlapping slanted document tabs, workspace navigation, live compute badge and scene actions.
    const float TabHeight = 24.0f * Scale;
    const float TabWidth = 190.0f * Scale;
    const float DockX = Position.x + HeaderPad;
    const float DockY = Position.y + (HeaderH - TabHeight) * 0.5f;
    for (int TabIndex = 0; TabIndex < static_cast<int>(Session.Tabs.size()); ++TabIndex)
    {
        const bool Selected = TabIndex == Session.ActiveTab;
        const float TabX = DockX + static_cast<float>(TabIndex) * (TabWidth - 24.0f * Scale);
        const ImVec2 TabMin(TabX, DockY);
        ImGui::SetCursorScreenPos(TabMin);
        ImGui::PushID(TabIndex);
        const bool Clicked = ImGui::InvisibleButton("##FluidDocumentTab", ImVec2(TabWidth, TabHeight));
        const bool Hovered = ImGui::IsItemHovered();
        const ImVec2 Polygon[4] = {
            { TabX + 14.0f * Scale, DockY }, { TabX + TabWidth - 14.0f * Scale, DockY },
            { TabX + TabWidth, DockY + TabHeight }, { TabX, DockY + TabHeight }
        };
        const ImU32 TabFill = Selected ? IM_COL32(26, 26, 26, 255) :
                              Hovered ? IM_COL32(35, 35, 35, 255) : IM_COL32(255, 255, 255, 7);
        ModalInk->AddConvexPolyFilled(Polygon, 4, TabFill);
        std::string TabLabel = Session.Tabs[TabIndex].Title;
        const size_t Extension = TabLabel.rfind(".fluidspace");
        if (Extension != std::string::npos) TabLabel.resize(Extension);
        else if (const size_t Dot = TabLabel.rfind('.'); Dot != std::string::npos) TabLabel.resize(Dot);
        const float TextX = TabX + 24.0f * Scale;
        ModalInk->PushClipRect(TabMin, ImVec2(TabX + TabWidth, DockY + TabHeight), true);
        ModalInk->AddText(Faces.Regular, 11.0f * Scale, ImVec2(TextX, DockY + 6.0f * Scale),
                          Selected ? IM_COL32(238, 238, 238, 255) : IM_COL32(142, 142, 142, 255), TabLabel.c_str());
        ModalInk->PopClipRect();
        if (Clicked) Session.ActiveTab = TabIndex;
        bool CloseTab = false;
        if (Session.Tabs.size() > 1u && (Selected || Hovered))
        {
            const ImVec2 TabCloseMin(TabX + TabWidth - 21.0f * Scale, DockY + 4.0f * Scale);
            ImGui::SetCursorScreenPos(TabCloseMin);
            CloseTab = ImGui::InvisibleButton("##CloseFluidDocumentTab", ImVec2(16.0f * Scale, 16.0f * Scale));
            const bool CloseHovered = ImGui::IsItemHovered();
            if (Session.Tabs[TabIndex].Dirty)
                ModalInk->AddCircleFilled(ImVec2(TabX + TabWidth - 30.0f * Scale, DockY + 12.0f * Scale), 2.0f * Scale, IM_COL32(224, 224, 224, 255));
            ModalInk->AddText(Faces.Regular, 12.0f * Scale, ImVec2(TabCloseMin.x + 4.0f * Scale, TabCloseMin.y + 1.0f * Scale),
                              CloseHovered ? IM_COL32(235, 235, 235, 255) : IM_COL32(121, 121, 121, 255), "x");
        }
        ImGui::PopID();
        if (CloseTab)
        {
            Session.Tabs.erase(Session.Tabs.begin() + TabIndex);
            Session.ActiveTab = std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1);
            Session.DocumentNameForTab.clear();
            break;
        }
    }
    const float AddTabX = DockX + static_cast<float>(Session.Tabs.size()) * (TabWidth - 24.0f * Scale) + 5.0f * Scale;
    if (DrawPillButton("##FluidAddDocument", "+", ImVec2(AddTabX, DockY), ImVec2(28.0f * Scale, TabHeight), false, false)) CreateNewDocument();

    const float NavX = Position.x + std::min(820.0f * Scale, Size.x * 0.44f);
    const float NavY = Position.y + (HeaderH - 38.0f * Scale) * 0.5f;
    if (DrawPillButton("##FluidWorkspaceNav", "Workspace", ImVec2(NavX, NavY), ImVec2(112.0f * Scale, 38.0f * Scale), !Session.ShowPresetsView))
        Session.ShowPresetsView = false;
    if (DrawPillButton("##FluidPresetsNav", "Presets", ImVec2(NavX + 118.0f * Scale, NavY), ImVec2(94.0f * Scale, 38.0f * Scale), Session.ShowPresetsView))
        Session.ShowPresetsView = true;

    const float CloseX = Position.x + Size.x - HeaderPad - 30.0f * Scale;
    const ImVec2 CloseMin(CloseX, Position.y + (HeaderH - 28.0f * Scale) * 0.5f);
    ImGui::SetCursorScreenPos(CloseMin);
    const bool ClosePressed = ImGui::InvisibleButton("##CloseFluidPlumeEditorHeader", ImVec2(28.0f * Scale, 28.0f * Scale));
    const bool CloseHovered = ImGui::IsItemHovered();
    if (CloseHovered) ModalInk->AddCircleFilled(ImVec2(CloseMin.x + 14.0f * Scale, CloseMin.y + 14.0f * Scale), 14.0f * Scale, IM_COL32(255, 255, 255, 15));
    ModalInk->AddLine(ImVec2(CloseMin.x + 9.0f * Scale, CloseMin.y + 9.0f * Scale), ImVec2(CloseMin.x + 19.0f * Scale, CloseMin.y + 19.0f * Scale), IM_COL32(190, 190, 190, 255), 1.4f * Scale);
    ModalInk->AddLine(ImVec2(CloseMin.x + 19.0f * Scale, CloseMin.y + 9.0f * Scale), ImVec2(CloseMin.x + 9.0f * Scale, CloseMin.y + 19.0f * Scale), IM_COL32(190, 190, 190, 255), 1.4f * Scale);
    if (ClosePressed) ClosingModal = true;

    const float SaveX = CloseX - 118.0f * Scale;
    if (DrawPillButton("##FluidSaveScene", "Save scene", ImVec2(SaveX, NavY + 2.0f * Scale), ImVec2(108.0f * Scale, 34.0f * Scale), false, true))
    {
        FluidDocumentTab& Current = Session.Tabs[std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1)];
        SaveActiveScene(Current);
    }
    const float HelpX = SaveX - 42.0f * Scale;
    if (DrawPillButton("##FluidShortcutsButton", "?", ImVec2(HelpX, NavY + 5.0f * Scale), ImVec2(32.0f * Scale, 28.0f * Scale)))
        Session.ShowShortcuts = true;
    const float BadgeX = HelpX - 174.0f * Scale;
    ModalInk->AddCircleFilled(ImVec2(BadgeX + 5.0f * Scale, Position.y + HeaderH * 0.5f), 3.0f * Scale, IM_COL32(52, 199, 89, 255));
    ModalInk->AddText(Faces.Regular, 11.0f * Scale, ImVec2(BadgeX + 15.0f * Scale, Position.y + (HeaderH - 13.0f * Scale) * 0.5f),
                      IM_COL32(144, 144, 144, 255), "Vulkan Compute \xC2\xB7 Live");

    // 35 px document bar: scene name, format, dirty state, import, sample and TOML export.
    const ImVec2 DocumentBarMin(Position.x, Position.y + HeaderH);
    const ImVec2 DocumentBarMax(Position.x + Size.x, DocumentBarMin.y + DocumentBarH);
    ModalInk->AddRectFilled(DocumentBarMin, DocumentBarMax, IM_COL32(11, 11, 11, 255));
    ModalInk->AddLine(ImVec2(DocumentBarMin.x, DocumentBarMax.y), DocumentBarMax, IM_COL32(255, 255, 255, 11));
    ModalInk->AddRect(ImVec2(Position.x + 25.0f * Scale, DocumentBarMin.y + 11.0f * Scale),
                      ImVec2(Position.x + 37.0f * Scale, DocumentBarMin.y + 24.0f * Scale), IM_COL32(138, 138, 138, 255), 2.0f * Scale, 0, 1.0f * Scale);
    ModalInk->AddLine(ImVec2(Position.x + 28.0f * Scale, DocumentBarMin.y + 15.0f * Scale), ImVec2(Position.x + 34.0f * Scale, DocumentBarMin.y + 15.0f * Scale), IM_COL32(138, 138, 138, 255));
    ModalInk->AddLine(ImVec2(Position.x + 28.0f * Scale, DocumentBarMin.y + 19.0f * Scale), ImVec2(Position.x + 34.0f * Scale, DocumentBarMin.y + 19.0f * Scale), IM_COL32(138, 138, 138, 255));

    FluidDocumentTab& Document = Session.Tabs[std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1)];
    const std::string NameKey = Document.Title + "\n" + Document.Scene.Name;
    if (Session.DocumentNameForTab != NameKey)
    {
        std::snprintf(Session.DocumentName, sizeof(Session.DocumentName), "%s", Document.Scene.Name.c_str());
        Session.DocumentNameForTab = NameKey;
    }
    ImGui::SetCursorScreenPos(ImVec2(Position.x + 45.0f * Scale, DocumentBarMin.y + 4.0f * Scale));
    ImGui::SetNextItemWidth(132.0f * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 3.0f * Scale));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 8));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 12));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    if (ImGui::InputText("##FluidDocumentName", Session.DocumentName, sizeof(Session.DocumentName)))
    {
        Document.Scene.Name = Session.DocumentName;
        Document.Dirty = true;
        Session.DocumentNameForTab = Document.Title + "\n" + Document.Scene.Name;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ModalInk->AddText(Faces.Regular, 11.0f * Scale, ImVec2(Position.x + 184.0f * Scale, DocumentBarMin.y + 11.0f * Scale), IM_COL32(111, 111, 111, 255), ".fluid");
    if (Document.Dirty) ModalInk->AddCircleFilled(ImVec2(Position.x + 229.0f * Scale, DocumentBarMin.y + 17.0f * Scale), 2.5f * Scale, IM_COL32(224, 224, 224, 255));
    ModalInk->AddLine(ImVec2(Position.x + 246.0f * Scale, DocumentBarMin.y + 8.0f * Scale), ImVec2(Position.x + 246.0f * Scale, DocumentBarMin.y + 27.0f * Scale), IM_COL32(255, 255, 255, 18));
    ModalInk->AddText(Faces.Regular, 10.0f * Scale, ImVec2(Position.x + 259.0f * Scale, DocumentBarMin.y + 12.0f * Scale), IM_COL32(102, 102, 102, 255), "Smoke / fire authoring");

    const float DocActionY = DocumentBarMin.y + 4.0f * Scale;
    const float TomlX = Position.x + Size.x - HeaderPad - 118.0f * Scale;
    const float SampleX = TomlX - 175.0f * Scale;
    const float OpenX = SampleX - 113.0f * Scale;
    if (DrawPillButton("##FluidOpenScene", "Open scene", ImVec2(OpenX, DocActionY), ImVec2(105.0f * Scale, 27.0f * Scale)))
        Session.ShowImportModal = true;
    if (DrawPillButton("##FluidLoadSample", "Load a sample...", ImVec2(SampleX, DocActionY), ImVec2(164.0f * Scale, 27.0f * Scale)))
        ImGui::OpenPopup("##FluidDocumentSamples");
    if (DropdownControlSurface::BeginMenu("##FluidDocumentSamples", Scale))
    {
        for (const auto& Sample : FluidDynamics::EnumerateSampleFluidScenes())
        {
            if (ImGui::MenuItem(Sample.Name))
                LoadSampleScene(Session.Tabs[std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1)], Sample);
        }
        DropdownControlSurface::EndMenu();
    }
    if (DrawPillButton("##FluidSaveToml", "Save as TOML", ImVec2(TomlX, DocActionY), ImVec2(118.0f * Scale, 27.0f * Scale)))
    {
        const auto Sidecar = ResolveSidecarPath(Token, Config);
        if (!Sidecar.empty())
        {
            auto ExportPath = Sidecar;
            ExportPath.replace_extension(".gasscene.toml");
            if (FluidDynamics::SaveFluidSpaceFile(ExportPath, Document.Scene))
                Session.StatusNotice = "Exported " + ExportPath.filename().string();
        }
    }

    const ImVec2 BodyMin(Position.x, Position.y + HeaderH + DocumentBarH);
    const ImVec2 BodySize(Size.x, std::max(120.0f, Size.y - HeaderH - DocumentBarH));
    ImGui::SetCursorScreenPos(BodyMin);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Palette.ColumnBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(Palette.InsetFieldBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(Palette.SegmentedActiveBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::ColorConvertU32ToFloat4(Palette.SegmentedActiveBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Palette.InsetFieldBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    if (ImGui::BeginChild("##FluidPlumeEditorRoot", BodySize, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar))
    {
        FluidDocumentTab& ActiveDoc = Session.Tabs[std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1)];
        auto& P = ActiveDoc.Scene.Parameters;
        const float TotalWidth = ImGui::GetContentRegionAvail().x;

        //----------------------------------------------------------------------------------------------------------------
        // 3. THREE-COLUMN WORKSPACE: OUTLINER | 3D VOLUMETRIC VIEWPORT | 3-TAB INSPECTOR
        //----------------------------------------------------------------------------------------------------------------
        const float StatusHeight = Session.ShowStatusBar ? 26.0f * Scale : 0.0f;
        const float WorkspacePadding = 12.0f * Scale;
        const float ColumnGap = 10.0f * Scale;
        const float BodyHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - StatusHeight - 9.0f * Scale);
        const bool ShowOutliner = Session.ShowOutliner && !Session.MaximizeViewport && !Session.ShowPresetsView;
        const bool ShowInspector = Session.ShowInspector && !Session.MaximizeViewport && !Session.ShowPresetsView;
        const int VisibleGaps = (ShowOutliner ? 1 : 0) + (ShowInspector ? 1 : 0);
        const float MinimumViewportWidth = 320.0f * Scale;
        const float ColumnAreaWidth = std::max(MinimumViewportWidth,
            TotalWidth - WorkspacePadding * 2.0f - ColumnGap * static_cast<float>(VisibleGaps));
        const float SideScale = (ShowOutliner && ShowInspector)
            ? std::min(1.0f, std::max(0.0f, ColumnAreaWidth - MinimumViewportWidth) / (316.0f + 368.0f) / Scale)
            : 1.0f;
        const float OutlinerWidth = ShowOutliner ? 316.0f * Scale * SideScale : 0.0f;
        const float InspectorWidth = ShowInspector ? 368.0f * Scale * SideScale : 0.0f;
        const float ViewportWidth = std::max(MinimumViewportWidth, ColumnAreaWidth - OutlinerWidth - InspectorWidth);
        const ImVec2 WorkspaceMin(Position.x + WorkspacePadding, BodyMin.y);
        ImGui::SetCursorScreenPos(WorkspaceMin);

        if (Session.ShowPresetsView)
        {
            const ImVec2 PresetPanelMax(WorkspaceMin.x + TotalWidth - WorkspacePadding * 2.0f, WorkspaceMin.y + BodyHeight);
            ImDrawList* PresetDraw = ImGui::GetWindowDrawList();
            PresetDraw->AddRectFilled(WorkspaceMin, PresetPanelMax, IM_COL32(18, 18, 18, 255), 22.0f * Scale);
            PresetDraw->AddRect(WorkspaceMin, PresetPanelMax, IM_COL32(255, 255, 255, 14), 22.0f * Scale);
            PresetDraw->AddText(Faces.Light ? Faces.Light : Faces.Regular, 26.0f * Scale,
                                ImVec2(WorkspaceMin.x + 28.0f * Scale, WorkspaceMin.y + 25.0f * Scale), IM_COL32(238, 238, 238, 255), "Preset library");
            PresetDraw->AddText(Faces.Regular, 12.0f * Scale,
                                ImVec2(WorkspaceMin.x + 29.0f * Scale, WorkspaceMin.y + 61.0f * Scale), IM_COL32(125, 125, 125, 255),
                                "Start from a tuned scene, then continue authoring in the live workspace.");
            ImGui::SetCursorScreenPos(ImVec2(WorkspaceMin.x + 28.0f * Scale, WorkspaceMin.y + 91.0f * Scale));
            ImGui::SetNextItemWidth(320.0f * Scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * Scale, 9.0f * Scale));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f * Scale);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 10));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 14));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 17));
            ImGui::InputTextWithHint("##FluidFullPresetSearch", "Find an effect...", Session.PresetSearch, sizeof(Session.PresetSearch));
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            const char* PresetFilterLabels[] = { "All", "Fire", "Smoke", "Blast" };
            const float PresetFilterWidth[] = { 42.0f, 52.0f, 60.0f, 52.0f };
            float FilterX = WorkspaceMin.x + 374.0f * Scale;
            for (int FilterIndex = 0; FilterIndex < 4; ++FilterIndex)
            {
                const float Width = PresetFilterWidth[FilterIndex] * Scale;
                ImGui::SetCursorScreenPos(ImVec2(FilterX, WorkspaceMin.y + 93.0f * Scale));
                if (FluidChip(PresetFilterLabels[FilterIndex], Session.PresetFilter == FilterIndex, Width))
                    Session.PresetFilter = FilterIndex;
                FilterX += Width + 8.0f * Scale;
            }

            std::string Needle = Session.PresetSearch;
            std::transform(Needle.begin(), Needle.end(), Needle.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
            const auto Presets = FluidDynamics::EnumerateFluidPresets();
            const float GridLeft = WorkspaceMin.x + 28.0f * Scale;
            const float GridTop = WorkspaceMin.y + 145.0f * Scale;
            const float GridWidth = PresetPanelMax.x - GridLeft - 28.0f * Scale;
            const float CardGap = 12.0f * Scale;
            const int GridColumns = 3;
            const float CardWidth = (GridWidth - CardGap * (GridColumns - 1)) / GridColumns;
            int VisiblePresetIndex = 0;
            for (const auto& Preset : Presets)
            {
                if (Session.PresetFilter != 0 && static_cast<int>(Preset.Category) != Session.PresetFilter) continue;
                std::string Haystack = std::string(Preset.Title) + " " + Preset.Summary + " " + Preset.Description;
                std::transform(Haystack.begin(), Haystack.end(), Haystack.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
                if (!Needle.empty() && Haystack.find(Needle) == std::string::npos) continue;
                const int Column = VisiblePresetIndex % GridColumns;
                const int Row = VisiblePresetIndex / GridColumns;
                ImGui::SetCursorScreenPos(ImVec2(GridLeft + Column * (CardWidth + CardGap), GridTop + Row * (76.0f * Scale)));
                if (FluidPresetCard(Preset, ActiveDoc.PresetIdentity == Preset.Identity, CardWidth))
                {
                    PushUndoSnapshot(ActiveDoc);
                    ActiveDoc.PresetIdentity = Preset.Identity;
                    ActiveDoc.Scene.Name = Preset.Title;
                    ActiveDoc.Scene.Parameters = FluidDynamics::ConstructPresetParameters(Preset.Identity);
                    ActiveDoc.RestartPending = true;
                    Session.DocumentNameForTab.clear();
                    if (Preset.TriggerExplosionOnLoad) EnqueueBurstPattern(ActiveDoc, ActiveDoc.BurstPattern);
                    SyncDocumentToRecord(ActiveDoc, Token, Config, false);
                    Session.ShowPresetsView = false;
                    Session.StatusNotice = std::string("Loaded preset: ") + Preset.Title;
                }
                ++VisiblePresetIndex;
            }
            if (VisiblePresetIndex == 0)
                PresetDraw->AddText(Faces.Regular, 13.0f * Scale, ImVec2(GridLeft, GridTop), IM_COL32(135, 135, 135, 255), "No matching effects. Clear the search or choose another category.");
        }
        else
        {
        // --- LEFT COLUMN: SCENE OUTLINER ---
        if (ShowOutliner)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            if (ImGui::BeginChild("##FluidOutlinerColumn", ImVec2(OutlinerWidth, BodyHeight), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoBackground))
            {
                ImDrawList* ColDraw = ImGui::GetWindowDrawList();
                const ImVec2 ColMin = ImGui::GetWindowPos();
                const bool ColliderVisible = P.ObstacleCategory != 0 || Session.SelectedNode == OutlinerNodeId::Collider;
                const size_t ObjectCount = 3u + (ColliderVisible ? 1u : 0u);
                const int EnabledCount = 1 + (P.SourceEnabled ? 1 : 0) + (P.ObstacleCategory != 0 ? 1 : 0) + (P.SunIntensity > 0.0f ? 1 : 0);
                const int DisabledCount = std::max(0, static_cast<int>(ObjectCount) - EnabledCount);
                ColDraw->AddRectFilled(ColMin, ImVec2(ColMin.x + OutlinerWidth, ColMin.y + BodyHeight), IM_COL32(18, 18, 18, 255), 22.0f);
                ColDraw->AddRect(ColMin, ImVec2(ColMin.x + OutlinerWidth, ColMin.y + BodyHeight), IM_COL32(255, 255, 255, 13), 22.0f);
                const ImVec2 OutlinerHeading(ColMin.x + 22.0f * Scale, ColMin.y + 20.0f * Scale);
                ColDraw->AddText(Faces.Light ? Faces.Light : Faces.Regular, 20.0f * Scale, OutlinerHeading, IM_COL32(232, 232, 232, 255), "Outliner");
                char CountText[64]{};
                std::snprintf(CountText, sizeof(CountText), "Scene \xC2\xB7 %zu nodes", ObjectCount);
                ColDraw->AddText(Faces.Regular, 11.0f * Scale, ImVec2(OutlinerHeading.x, OutlinerHeading.y + 27.0f * Scale), IM_COL32(115, 115, 115, 255), CountText);

                const float StatY = ColMin.y + 70.0f * Scale;
                const float StatGap = 8.0f * Scale;
                const float StatW = (OutlinerWidth - 28.0f * Scale - StatGap) * 0.5f;
                for (int StatIndex = 0; StatIndex < 2; ++StatIndex)
                {
                    const float X = ColMin.x + 14.0f * Scale + static_cast<float>(StatIndex) * (StatW + StatGap);
                    const ImVec2 StatMin(X, StatY);
                    const ImVec2 StatMax(X + StatW, StatY + 64.0f * Scale);
                    ColDraw->AddRectFilled(StatMin, StatMax, IM_COL32(255, 255, 255, 9), 18.0f * Scale);
                    ColDraw->AddRect(StatMin, StatMax, IM_COL32(255, 255, 255, 16), 18.0f * Scale);
                    const bool Enabled = StatIndex == 0;
                    ColDraw->AddCircleFilled(ImVec2(X + 25.0f * Scale, StatY + 24.0f * Scale), 11.0f * Scale,
                                             Enabled ? IM_COL32(52, 199, 89, 255) : IM_COL32(255, 255, 255, 15));
                    if (Enabled)
                    {
                        ColDraw->AddLine(ImVec2(X + 20.0f * Scale, StatY + 24.0f * Scale), ImVec2(X + 24.0f * Scale, StatY + 28.0f * Scale), IM_COL32(10, 26, 16, 255), 1.6f * Scale);
                        ColDraw->AddLine(ImVec2(X + 24.0f * Scale, StatY + 28.0f * Scale), ImVec2(X + 31.0f * Scale, StatY + 20.0f * Scale), IM_COL32(10, 26, 16, 255), 1.6f * Scale);
                    }
                    else
                    {
                        ColDraw->AddLine(ImVec2(X + 21.0f * Scale, StatY + 24.0f * Scale), ImVec2(X + 29.0f * Scale, StatY + 24.0f * Scale), IM_COL32(135, 135, 135, 255), 1.2f * Scale);
                    }
                    ColDraw->AddText(Faces.Regular, 11.0f * Scale, ImVec2(X + 14.0f * Scale, StatY + 39.0f * Scale), IM_COL32(150, 150, 150, 255), Enabled ? "Enabled" : "Disabled");
                    char StatCount[8]{};
                    std::snprintf(StatCount, sizeof(StatCount), "%d", Enabled ? EnabledCount : DisabledCount);
                    ColDraw->AddText(Faces.Light ? Faces.Light : Faces.Regular, 27.0f * Scale,
                                     ImVec2(X + StatW - 37.0f * Scale, StatY + 20.0f * Scale), IM_COL32(232, 232, 232, 255), StatCount);
                }

                const ImVec2 SearchMin(ColMin.x + 14.0f * Scale, ColMin.y + 144.0f * Scale);
                ImGui::SetCursorScreenPos(SearchMin);
                ImGui::SetNextItemWidth(OutlinerWidth - 28.0f * Scale);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f * Scale, 10.0f * Scale));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.0f * Scale);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 11));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 15));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 18));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::InputTextWithHint("##FluidOutlinerFilter", "Search scene...", Session.OutlinerSearch, sizeof(Session.OutlinerSearch));
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(3);
                ImGui::SetCursorScreenPos(ImVec2(ColMin.x + 14.0f * Scale, ColMin.y + 193.0f * Scale));
                static constexpr std::array<const char*, 4> OutlinerFilters = { "All", "Gas", "Geometry", "Lights" };
                static constexpr std::array<float, 4> OutlinerFilterWidths = { 36.0f, 38.0f, 66.0f, 48.0f };
                for (int FilterIndex = 0; FilterIndex < static_cast<int>(OutlinerFilters.size()); ++FilterIndex)
                {
                    if (FilterIndex > 0) ImGui::SameLine(0.0f, 4.0f);
                    if (FluidChip(OutlinerFilters[FilterIndex], Session.OutlinerFilter == FilterIndex, OutlinerFilterWidths[FilterIndex]))
                        Session.OutlinerFilter = FilterIndex;
                }

                const ImVec2 CollectionMin(ColMin.x + 14.0f * Scale, ColMin.y + 225.0f * Scale);
                ImGui::SetCursorScreenPos(CollectionMin);
                const bool CollectionPressed = ImGui::InvisibleButton("##FluidSceneCollection", ImVec2(OutlinerWidth - 60.0f * Scale, 28.0f * Scale));
                if (CollectionPressed) Session.CollectionExpanded = !Session.CollectionExpanded;
                const ImVec2 CollectionChevron(CollectionMin.x + 8.0f * Scale, CollectionMin.y + 14.0f * Scale);
                if (Session.CollectionExpanded)
                {
                    ColDraw->AddLine(ImVec2(CollectionChevron.x - 4.0f * Scale, CollectionChevron.y - 2.0f * Scale), CollectionChevron, IM_COL32(121, 121, 121, 255), 1.3f * Scale);
                    ColDraw->AddLine(CollectionChevron, ImVec2(CollectionChevron.x + 4.0f * Scale, CollectionChevron.y - 2.0f * Scale), IM_COL32(121, 121, 121, 255), 1.3f * Scale);
                }
                else
                {
                    ColDraw->AddLine(ImVec2(CollectionChevron.x - 2.0f * Scale, CollectionChevron.y - 4.0f * Scale), ImVec2(CollectionChevron.x + 2.0f * Scale, CollectionChevron.y), IM_COL32(121, 121, 121, 255), 1.3f * Scale);
                    ColDraw->AddLine(ImVec2(CollectionChevron.x + 2.0f * Scale, CollectionChevron.y), ImVec2(CollectionChevron.x - 2.0f * Scale, CollectionChevron.y + 4.0f * Scale), IM_COL32(121, 121, 121, 255), 1.3f * Scale);
                }
                PanelChrome::DrawGlyph("M3 7.5 12 3l9 4.5v9L12 21l-9-4.5v-9Z", ImVec2(CollectionMin.x + 22.0f * Scale, CollectionMin.y + 5.0f * Scale),
                                       15.0f * Scale, IM_COL32(150, 150, 150, 255), 1.4f);
                ColDraw->AddText(Faces.Regular, 11.0f * Scale, ImVec2(CollectionMin.x + 45.0f * Scale, CollectionMin.y + 7.0f * Scale), IM_COL32(177, 177, 177, 255), "Scene collection");
                const ImVec2 AddMin(ColMin.x + OutlinerWidth - 42.0f * Scale, CollectionMin.y + 1.0f * Scale);
                ImGui::SetCursorScreenPos(AddMin);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * Scale);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 7));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 15));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 18));
                const bool AddPressed = ImGui::Button("+##FluidOutlinerAdd", ImVec2(26.0f * Scale, 26.0f * Scale));
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(3);
                if (AddPressed) ImGui::OpenPopup("##FluidOutlinerAddMenu");
                auto AddObject = [&](int Kind)
                {
                    PushUndoSnapshot(ActiveDoc);
                    if (Kind == 0)
                    {
                        P.SourceEnabled = true;
                        ActiveDoc.Scene.Names.Source = "Fire source";
                        Session.SelectedNode = OutlinerNodeId::Source;
                    }
                    else if (Kind == 1)
                    {
                        P.SourceEnabled = true;
                        P.SourceTemperature = 0.0f;
                        P.SourceFuel = 0.0f;
                        P.SourceSmoke = std::max(P.SourceSmoke, 2.0f);
                        ActiveDoc.Scene.Names.Source = "Smoke plume";
                        Session.SelectedNode = OutlinerNodeId::Source;
                    }
                    else
                    {
                        P.ObstacleCategory = Kind == 2 ? 1 : Kind == 3 ? 2 : 4;
                        ActiveDoc.Scene.Names.Collider = Kind == 2 ? "Sphere collider" : Kind == 3 ? "Cylinder collider" : "Slab collider";
                        Session.SelectedNode = OutlinerNodeId::Collider;
                        Session.ActiveInspector = 1;
                    }
                    ActiveDoc.Dirty = true;
                    SyncDocumentToRecord(ActiveDoc, Token, Config, false);
                };
                if (ImGui::BeginPopup("##FluidOutlinerAddMenu"))
                {
                    if (ImGui::MenuItem("Fire source")) AddObject(0);
                    if (ImGui::MenuItem("Smoke plume")) AddObject(1);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Sphere collider")) AddObject(2);
                    if (ImGui::MenuItem("Cylinder collider")) AddObject(3);
                    if (ImGui::MenuItem("Slab collider")) AddObject(4);
                    ImGui::EndPopup();
                }
                ImGui::SetCursorScreenPos(ImVec2(ColMin.x + 21.0f * Scale, ColMin.y + 260.0f * Scale));

                auto MatchesFilter = [&](const std::string& Name) -> bool {
                    if (Session.OutlinerSearch[0] == '\0') return true;
                    std::string Haystack = Name;
                    std::string Needle   = Session.OutlinerSearch;
                    std::transform(Haystack.begin(), Haystack.end(), Haystack.begin(),
                                   [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
                    std::transform(Needle.begin(), Needle.end(), Needle.begin(),
                                   [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
                    return Haystack.find(Needle) != std::string::npos;
                };

                struct OutlinerEntrySpec
                {
                    OutlinerNodeId Id;
                    std::string*   MutableName;
                    int            TargetInspectorTab;
                    int            Category;
                    bool           HasEyeToggle;
                    bool           IsEnabled;
                };

                auto& Names = ActiveDoc.Scene.Names;
                std::array<OutlinerEntrySpec, 4> Nodes = { {
                    { OutlinerNodeId::Domain,   &Names.Domain,   1, 2, false, true },
                    { OutlinerNodeId::Source,   &Names.Source,   0, 1, true,  P.SourceEnabled },
                    { OutlinerNodeId::Collider, &Names.Collider, 1, 2, true,  P.ObstacleCategory != 0 },
                    { OutlinerNodeId::Sun,      &Names.Sun,      2, 3, true,  P.SunIntensity > 0.0f },
                } };

                ImGui::Dummy(ImVec2(1.0f, 4.0f));
                for (size_t Idx = 0; Idx < Nodes.size(); ++Idx)
                {
                    if (!Session.CollectionExpanded) break;
                    auto& Node = Nodes[Idx];
                    if (Node.Id == OutlinerNodeId::Collider && !ColliderVisible) continue;
                    if (!MatchesFilter(*Node.MutableName)) continue;
                    if (Session.OutlinerFilter != 0 && Node.Category != Session.OutlinerFilter) continue;

                    ImGui::PushID(static_cast<int>(Idx));
                    const float Indent = (Node.Id == OutlinerNodeId::Domain) ? 0.0f : 12.0f;
                    if (Indent > 0.0f) ImGui::Indent(Indent);

                    if (Session.RenamingNode == static_cast<int>(Idx))
                    {
                        ImGui::SetNextItemWidth(OutlinerWidth - 36.0f - Indent);
                        if (ImGui::InputText("##RenameFluidNode", Session.RenameField, sizeof(Session.RenameField),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                        {
                            if (Session.RenameField[0] != '\0') *Node.MutableName = Session.RenameField;
                            Session.RenamingNode = -1;
                            ActiveDoc.Dirty      = true;
                        }
                        if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        {
                            Session.RenamingNode = -1;
                        }
                    }
                    else
                    {
                        const bool Selected = (Session.SelectedNode == Node.Id);
                        const float RowW = OutlinerWidth - 22.0f - Indent;
                        bool DoubleClicked = false;
                        bool ToggleClicked = false;
                        if (FluidSelectableRow(Node.MutableName->c_str(), Node.Id, Selected, Node.IsEnabled, Node.HasEyeToggle,
                                               std::max(60.0f, RowW), 39.0f, &DoubleClicked, &ToggleClicked))
                        {
                            Session.SelectedNode = Node.Id;
                            Session.ActiveInspector = Node.TargetInspectorTab;
                            if (DoubleClicked)
                            {
                                Session.RenamingNode = static_cast<int>(Idx);
                                std::snprintf(Session.RenameField, sizeof(Session.RenameField), "%s", Node.MutableName->c_str());
                            }
                        }
                        if (ToggleClicked)
                        {
                            PushUndoSnapshot(ActiveDoc);
                            if (Node.Id == OutlinerNodeId::Source) P.SourceEnabled = !P.SourceEnabled;
                            else if (Node.Id == OutlinerNodeId::Collider) P.ObstacleCategory = (P.ObstacleCategory == 0) ? 1 : 0;
                            else if (Node.Id == OutlinerNodeId::Sun) P.SunIntensity = (P.SunIntensity <= 0.0f) ? 2.2f : 0.0f;
                            ActiveDoc.Dirty = true;
                            SyncDocumentToRecord(ActiveDoc, Token, Config, false);
                        }
                    }
                    if (Indent > 0.0f) ImGui::Unindent(Indent);
                    ImGui::PopID();
                }

                ImGui::SetCursorScreenPos(ImVec2(ColMin.x + 18.0f * Scale, ImGui::GetCursorScreenPos().y + 10.0f * Scale));
                const ImVec2 PresetHeading = ImGui::GetCursorScreenPos();
                ColDraw->AddText(Faces.Regular, 15.0f * Scale, PresetHeading, IM_COL32(224, 224, 224, 255), "Preset library");
                char PresetCount[16]{};
                std::snprintf(PresetCount, sizeof(PresetCount), "%zu", FluidDynamics::EnumerateFluidPresets().size());
                ColDraw->AddText(Faces.Regular, 10.0f * Scale, ImVec2(PresetHeading.x + 111.0f * Scale, PresetHeading.y + 4.0f * Scale), IM_COL32(115, 115, 115, 255), PresetCount);
                ImGui::SetCursorScreenPos(ImVec2(ColMin.x + 14.0f * Scale, PresetHeading.y + 29.0f * Scale));
                ImGui::SetNextItemWidth(OutlinerWidth - 28.0f * Scale);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11.0f * Scale, 8.0f * Scale));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f * Scale);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 10));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 14));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 17));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::InputTextWithHint("##FluidPresetFilter", "Find an effect...", Session.PresetSearch, sizeof(Session.PresetSearch));
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(3);
                ImGui::SetCursorScreenPos(ImVec2(ColMin.x + 14.0f * Scale, PresetHeading.y + 75.0f * Scale));
                static constexpr std::array<const char*, 4> PresetFilters = { "All", "Fire", "Smoke", "Blast" };
                static constexpr std::array<float, 4> PresetFilterWidths = { 38.0f, 46.0f, 54.0f, 48.0f };
                for (int FilterIndex = 0; FilterIndex < static_cast<int>(PresetFilters.size()); ++FilterIndex)
                {
                    if (FilterIndex > 0) ImGui::SameLine(0.0f, 4.0f * Scale);
                    if (FluidChip(PresetFilters[FilterIndex], Session.PresetFilter == FilterIndex, PresetFilterWidths[FilterIndex] * Scale))
                        Session.PresetFilter = FilterIndex;
                }
                const ImVec2 PresetListMin(ColMin.x + 12.0f * Scale, PresetHeading.y + 107.0f * Scale);
                const float FooterHeight = 32.0f * Scale;
                const float PresetListHeight = std::max(80.0f * Scale, ColMin.y + BodyHeight - FooterHeight - PresetListMin.y - 1.0f * Scale);
                ImGui::SetCursorScreenPos(PresetListMin);
                if (ImGui::BeginChild("##FluidPresetCards", ImVec2(OutlinerWidth - 24.0f * Scale, PresetListHeight),
                                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar))
                {
                    const auto Presets = FluidDynamics::EnumerateFluidPresets();
                    std::string Needle = Session.PresetSearch;
                    std::transform(Needle.begin(), Needle.end(), Needle.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
                    for (const auto& Preset : Presets)
                    {
                        if (Session.PresetFilter != 0 && static_cast<int>(Preset.Category) != Session.PresetFilter) continue;
                        std::string Haystack = std::string(Preset.Title) + " " + Preset.Summary + " " + Preset.Description;
                        std::transform(Haystack.begin(), Haystack.end(), Haystack.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
                        if (!Needle.empty() && Haystack.find(Needle) == std::string::npos) continue;
                        if (!FluidPresetCard(Preset, ActiveDoc.PresetIdentity == Preset.Identity, ImGui::GetContentRegionAvail().x)) continue;
                        PushUndoSnapshot(ActiveDoc);
                        ActiveDoc.PresetIdentity   = Preset.Identity;
                        ActiveDoc.Scene.Name       = Preset.Title;
                        ActiveDoc.Scene.Parameters = FluidDynamics::ConstructPresetParameters(Preset.Identity);
                        ActiveDoc.RestartPending   = true;
                        Session.DocumentNameForTab.clear();
                        if (Preset.TriggerExplosionOnLoad) EnqueueBurstPattern(ActiveDoc, ActiveDoc.BurstPattern);
                        SyncDocumentToRecord(ActiveDoc, Token, Config, false);
                        Session.StatusNotice = std::string("Loaded preset: ") + Preset.Title;
                    }
                }
                ImGui::EndChild();
                const ImVec2 FooterMin(ColMin.x, ColMin.y + BodyHeight - FooterHeight);
                ColDraw->AddLine(FooterMin, ImVec2(ColMin.x + OutlinerWidth, FooterMin.y), IM_COL32(255, 255, 255, 12));
                PanelChrome::DrawGlyph("M4 7h16M4 12h16M4 17h16", ImVec2(ColMin.x + 15.0f * Scale, FooterMin.y + 9.0f * Scale),
                                       12.0f * Scale, IM_COL32(107, 107, 107, 255), 1.4f);
                ColDraw->AddText(Faces.Regular, 9.0f * Scale, ImVec2(ColMin.x + 34.0f * Scale, FooterMin.y + 10.0f * Scale),
                                 IM_COL32(111, 111, 111, 255), "One domain \xC2\xB7 one source \xC2\xB7 one collider");
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::SameLine(0.0f, 10.0f);
        }

        // --- CENTER COLUMN: 3D VOLUMETRIC RAYMARCH VIEWPORT ---
        if (ImGui::BeginChild("##FluidViewportColumn", ImVec2(ViewportWidth, BodyHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImDrawList* VpDraw = ImGui::GetWindowDrawList();
            const ImVec2 ColumnMin = ImGui::GetCursorScreenPos();
            const ImVec2 ViewportPanelMin = ImGui::GetWindowPos();
            VpDraw->AddRectFilled(ViewportPanelMin, ImVec2(ViewportPanelMin.x + ViewportWidth, ViewportPanelMin.y + BodyHeight), IM_COL32(18, 18, 18, 255), 22.0f);
            VpDraw->AddRect(ViewportPanelMin, ImVec2(ViewportPanelMin.x + ViewportWidth, ViewportPanelMin.y + BodyHeight), IM_COL32(255, 255, 255, 13), 22.0f);
            constexpr float ViewBarHeight = 44.0f;
            constexpr float TransportHeight = 52.0f;
            constexpr float TimelineHeight = 88.0f;
            const ImVec2 ViewBarMax(ColumnMin.x + ViewportWidth, ColumnMin.y + ViewBarHeight);
            VpDraw->AddRectFilled(ColumnMin, ViewBarMax, IM_COL32(23, 23, 23, 255), 22.0f, ImDrawFlags_RoundCornersTop);
            VpDraw->AddRectFilled(ImVec2(ColumnMin.x, ColumnMin.y + 20.0f), ViewBarMax, IM_COL32(23, 23, 23, 255));
            PanelChrome::DrawGlyph("M3 4h18v16H3zM3 9h18M8 4v5", ImVec2(ColumnMin.x + 15.0f * Scale, ColumnMin.y + 14.0f * Scale),
                                   15.0f * Scale, IM_COL32(145, 145, 145, 255), 1.35f);
            VpDraw->AddText(Faces.Medium, 12.0f * Scale, ImVec2(ColumnMin.x + 38.0f * Scale, ColumnMin.y + 15.0f * Scale), IM_COL32(224, 224, 224, 255), "Scene view");
            VpDraw->AddLine(ImVec2(ColumnMin.x + 112.0f * Scale, ColumnMin.y + 11.0f * Scale),
                            ImVec2(ColumnMin.x + 112.0f * Scale, ColumnMin.y + 33.0f * Scale), IM_COL32(255, 255, 255, 15));

            const char* CameraViews[] = { "Perspective", "Front", "Side", "Top", "Reset view" };
            int CameraIndex = std::clamp(Session.CameraView, 0, 3);
            ImGui::SetCursorScreenPos(ImVec2(ColumnMin.x + 125.0f * Scale, ColumnMin.y + 8.0f * Scale));
            ImGui::SetNextItemWidth(112.0f * Scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f * Scale, 5.0f * Scale));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * Scale);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 6));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 12));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 12));
            if (DropdownControlSurface::BeginSelection("##FluidCameraView", CameraViews[CameraIndex], Scale))
            {
                for (int Index = 0; Index < 5; ++Index)
                {
                    if (!DropdownControlSurface::Option(CameraViews[Index], Index == CameraIndex, Scale)) continue;
                    if (Index == 4)
                    {
                        ActiveDoc.Scene.Camera.Reset();
                        Session.CameraView = 0;
                    }
                    else
                    {
                        Session.CameraView = Index;
                        if (Index > 0) ActiveDoc.Scene.Camera.ConfigureViewPreset(Index == 1 ? "front" : Index == 2 ? "side" : "top");
                        else { ActiveDoc.Scene.Camera.Reset(); ActiveDoc.Scene.Camera.SnapToTarget(); }
                    }
                }
                DropdownControlSurface::EndSelection();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);

            auto ViewAction = [&](const char* Id, const char* Label, float X, float Width, bool Active = false) -> bool
            {
                const ImVec2 Min(X, ColumnMin.y + 8.0f * Scale);
                ImGui::SetCursorScreenPos(Min);
                const bool Pressed = ImGui::InvisibleButton(Id, ImVec2(Width, 28.0f * Scale));
                const bool HoveredButton = ImGui::IsItemHovered();
                if (Active || HoveredButton)
                    VpDraw->AddRectFilled(Min, ImVec2(Min.x + Width, Min.y + 28.0f * Scale),
                                          Active ? IM_COL32(255, 255, 255, 13) : IM_COL32(255, 255, 255, 8), 14.0f * Scale);
                const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
                VpDraw->AddText(Faces.Regular, 10.5f * Scale,
                                ImVec2(Min.x + (Width - LabelSize.x * Scale) * 0.5f, Min.y + 8.0f * Scale),
                                Active || HoveredButton ? IM_COL32(225, 225, 225, 255) : IM_COL32(145, 145, 145, 255), Label);
                return Pressed;
            };
            const float ActionRight = ColumnMin.x + ViewportWidth - 12.0f * Scale;
            const float MaxX = ActionRight - 28.0f * Scale;
            const float FocusX = MaxX - 32.0f * Scale;
            const float BoundsX = FocusX - 32.0f * Scale;
            const float ChannelW = 105.0f * Scale;
            const float ChannelX = BoundsX - ChannelW - 6.0f * Scale;
            const float DiagnosticX = ChannelX - 31.0f * Scale;
            const float BakeX = DiagnosticX - 54.0f * Scale;
            if (ViewAction("##FluidBake", "Bake", BakeX, 49.0f * Scale)) Session.Flipbook.DialogOpen = true;
            if (ViewAction("##FluidDiagnostics", "...", DiagnosticX, 27.0f * Scale, Session.DiagnosticsOpen)) Session.DiagnosticsOpen = !Session.DiagnosticsOpen;
            const auto Channels = FluidDynamics::EnumerateFluidDebugChannels();
            const int RenderIndex = std::clamp(P.RenderChannel, 0, static_cast<int>(Channels.size()) - 1);
            ImGui::SetCursorScreenPos(ImVec2(ChannelX, ColumnMin.y + 8.0f * Scale));
            ImGui::SetNextItemWidth(ChannelW);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * Scale, 5.0f * Scale));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f * Scale);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 6));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 12));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 12));
            if (DropdownControlSurface::BeginSelection("##FluidRenderChannel", Channels[RenderIndex], Scale))
            {
                for (int Index = 0; Index < static_cast<int>(Channels.size()); ++Index)
                    if (DropdownControlSurface::Option(Channels[Index], Index == P.RenderChannel, Scale)) P.RenderChannel = Index;
                DropdownControlSurface::EndSelection();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            if (ViewAction("##FluidBoundsToggle", "B", BoundsX, 26.0f * Scale, Session.ShowDomainWire))
                Session.ShowDomainWire = !Session.ShowDomainWire;
            if (ViewAction("##FluidFocus", "F", FocusX, 26.0f * Scale))
            {
                ActiveDoc.Scene.Camera.Reset();
                ActiveDoc.Scene.Camera.SnapToTarget();
                Session.CameraView = 0;
            }
            if (ViewAction("##FluidMaximize", "[]", MaxX, 28.0f * Scale, Session.MaximizeViewport))
                Session.MaximizeViewport = !Session.MaximizeViewport;
            VpDraw->AddLine(ImVec2(ColumnMin.x, ViewBarMax.y), ViewBarMax, IM_COL32(255, 255, 255, 14), 1.0f);

            const ImVec2 CanvasMin(ColumnMin.x, ColumnMin.y + ViewBarHeight);
            const float CanvasHeight = std::max(64.0f * Scale, BodyHeight - ViewBarHeight - TransportHeight - TimelineHeight);
            const ImVec2 CanvasSize(std::max(64.0f * Scale, ViewportWidth), CanvasHeight);
            ImGui::SetCursorScreenPos(CanvasMin);
            ImGui::InvisibleButton("##Fluid3DCanvasHitbox", CanvasSize,
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            const bool Hovered = ImGui::IsItemHovered();

            ActiveDoc.Scene.Camera.AutoTurntable = P.AutoTurntable;
            ActiveDoc.Scene.Camera.Advance(std::clamp(Io.DeltaTime, 1.0f / 240.0f, 0.05f));

            bool                 BrushActiveNow = false;
            std::array<float, 3> BrushUvw       = { 0.5f, 0.35f, 0.5f };
            std::array<float, 3> BrushVelocity  = { 0.0f, 0.0f, 0.0f };

            const std::array<float, 3> DomainScale = (Session.LastImage.Extent[0] > 0u)
                                                         ? Session.LastImage.DynamicScale
                                                         : std::array<float, 3>{ P.BoundsWidth, P.BoundsHeight, P.BoundsWidth };
            const std::array<float, 3> HalfBox     = { 0.5f * DomainScale[0], 0.5f * DomainScale[1], 0.5f * DomainScale[2] };

            if (Hovered)
            {
                if (std::fabs(Io.MouseWheel) > 1.0e-3f)
                {
                    ActiveDoc.Scene.Camera.Dolly(Io.MouseWheel);
                }

                const bool OrbitGesture = ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                                          (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (Io.KeyAlt || Session.ActiveViewportTool == 0));
                const bool PanGesture   = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                          (ImGui::IsMouseDown(ImGuiMouseButton_Right) && Io.KeyShift);

                if (PanGesture)
                {
                    ActiveDoc.Scene.Camera.Pan(Io.MouseDelta.x, Io.MouseDelta.y, CanvasSize.y);
                }
                else if (OrbitGesture)
                {
                    ActiveDoc.Scene.Camera.Orbit(-Io.MouseDelta.x * 0.0075f, -Io.MouseDelta.y * 0.0065f);
                }
                else if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    const float NormX  = (Io.MousePos.x - CanvasMin.x) / CanvasSize.x;
                    const float NormY  = (Io.MousePos.y - CanvasMin.y) / CanvasSize.y;
                    const float Aspect = CanvasSize.x / CanvasSize.y;
                    if (const auto Hit = ActiveDoc.Scene.Camera.RaycastVolumeUvw(NormX, NormY, Aspect, HalfBox); Hit.has_value())
                    {
                        BrushUvw          = *Hit;
                        const float InvDt = 1.0f / std::max(Io.DeltaTime, 1.0f / 120.0f);
                        if (Session.HasPreviousBrush)
                        {
                            for (int Axis = 0; Axis < 3; ++Axis)
                            {
                                BrushVelocity[Axis] = std::clamp((BrushUvw[Axis] - Session.PreviousBrushUvw[Axis]) * InvDt * 1.4f,
                                                                 -12.0f, 12.0f);
                            }
                        }
                        Session.PreviousBrushUvw = BrushUvw;
                        Session.HasPreviousBrush = true;

                        if (Session.ActiveViewportTool == 1)
                        {
                            BrushActiveNow = true;
                            BrushVelocity[1] += 2.2f;
                        }
                        else if (Session.ActiveViewportTool == 2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        {
                            EnqueueBurstPattern(ActiveDoc, ActiveDoc.BurstPattern, BrushUvw);
                        }
                        else if (Session.ActiveViewportTool == 3)
                        {
                            if (P.ObstacleCategory == 0) P.ObstacleCategory = 1;
                            P.ObstacleX = BrushUvw[0];
                            P.ObstacleY = BrushUvw[1];
                            P.ObstacleZ = BrushUvw[2];
                        }
                    }
                }
                else
                {
                    Session.HasPreviousBrush = false;
                }
            }
            else
            {
                Session.HasPreviousBrush = false;
            }

            // Issue GPU simulation + volumetric raymarch request
            const auto& Presenter = InspectorRuntime::Instance().QueryFluidViewportPresenter();
            if (Presenter)
            {
                if (Session.Flipbook.Recording)
                {
                    const int TotalFrames = Session.Flipbook.Columns * Session.Flipbook.Rows;
                    FluidViewportRequest BakeReq{};
                    BakeReq.DocumentSlot      = ActiveDoc.GpuSlot;
                    BakeReq.ViewportWidth     = static_cast<uint32_t>(Session.Flipbook.FrameWidth);
                    BakeReq.ViewportHeight    = static_cast<uint32_t>(Session.Flipbook.FrameHeight);
                    const bool Warming = Session.Flipbook.WarmupRemaining > 1.0e-4f;
                    BakeReq.DeltaSeconds      = 1.0f / static_cast<float>(std::max(1, Session.Flipbook.FramesPerSecond));
                    BakeReq.Parameters        = P;
                    BakeReq.Parameters.Paused = false;
                    BakeReq.Camera            = ActiveDoc.Scene.Camera;
                    BakeReq.ExportFlipbook    = !Warming;
                    if (!Session.Flipbook.RestartIssued)
                    {
                        BakeReq.Restart = true;
                        Session.Flipbook.RestartIssued = true;
                    }
                    if (!Warming && !Session.Flipbook.BurstIssued)
                    {
                        if (Session.Flipbook.DetonateStart)
                        {
                            FluidBlastImpulse InitBlast{};
                            InitBlast.Radius      = P.BlastRadius;
                            InitBlast.Strength    = P.BlastStrength;
                            InitBlast.Temperature = P.BlastTemperature;
                            InitBlast.Fuel        = P.BlastFuel;
                            InitBlast.Smoke       = P.BlastSmoke;
                            InitBlast.Lobes       = P.BlastLobes;
                            BakeReq.Blasts.push_back(InitBlast);
                        }
                        Session.Flipbook.BurstIssued = true;
                    }
                    FluidViewportImage Baked = Presenter(BakeReq);
                    if (Warming)
                    {
                        Session.Flipbook.WarmupRemaining = std::max(0.0f, Session.Flipbook.WarmupRemaining - BakeReq.DeltaSeconds);
                    }
                    else if (Baked.ReadbackRgba.size() == static_cast<size_t>(Session.Flipbook.FrameWidth) * Session.Flipbook.FrameHeight * 4u)
                    {
                        const int Col    = Session.Flipbook.Captured % Session.Flipbook.Columns;
                        const int Row    = Session.Flipbook.Captured / Session.Flipbook.Columns;
                        const int SheetW = Session.Flipbook.Columns * Session.Flipbook.FrameWidth;
                        for (int Y = 0; Y < Session.Flipbook.FrameHeight; ++Y)
                        {
                            const size_t DstOff = (static_cast<size_t>(Row * Session.Flipbook.FrameHeight + Y) * SheetW +
                                                   static_cast<size_t>(Col * Session.Flipbook.FrameWidth)) * 4u;
                            const size_t SrcOff = static_cast<size_t>(Y) * Session.Flipbook.FrameWidth * 4u;
                            std::memcpy(Session.Flipbook.SheetRgba.data() + DstOff,
                                        Baked.ReadbackRgba.data() + SrcOff,
                                        static_cast<size_t>(Session.Flipbook.FrameWidth) * 4u);
                            if (!Session.Flipbook.Transparent)
                            {
                                for (int X = 0; X < Session.Flipbook.FrameWidth; ++X)
                                    Session.Flipbook.SheetRgba[DstOff + static_cast<size_t>(X) * 4u + 3u] = 255u;
                            }
                        }
                    }
                    if (!Warming) ++Session.Flipbook.Captured;
                    if (Session.Flipbook.Captured >= TotalFrames)
                    {
                        Session.Flipbook.Recording = false;
                        const uint32_t SheetW      = static_cast<uint32_t>(Session.Flipbook.Columns * Session.Flipbook.FrameWidth);
                        const uint32_t SheetH      = static_cast<uint32_t>(Session.Flipbook.Rows * Session.Flipbook.FrameHeight);
                        const std::filesystem::path PngPath(Session.Flipbook.OutputPath);
                        if (WriteUncompressedRgbaPng(PngPath, SheetW, SheetH, Session.Flipbook.SheetRgba))
                        {
                            const bool MetadataSaved = WriteFlipbookMetadata(PngPath, ActiveDoc.Scene, TotalFrames,
                                Session.Flipbook.Columns, Session.Flipbook.Rows, Session.Flipbook.FrameWidth,
                                Session.Flipbook.FramesPerSecond, Session.Flipbook.WarmupSeconds,
                                Session.Flipbook.Transparent, Session.Flipbook.DetonateStart);
                            Session.Flipbook.LastMessage = MetadataSaved
                                ? std::string("Saved PNG and JSON metadata: ") + PngPath.string()
                                : std::string("Saved PNG, but metadata export failed: ") + PngPath.string();
                            Session.StatusNotice = Session.Flipbook.LastMessage;
                        }
                        else
                        {
                            Session.Flipbook.LastMessage = "Failed writing flipbook PNG.";
                        }
                    }
                    Session.LastImage = std::move(Baked);
                }
                else
                {
                    FluidViewportRequest Req{};
                    Req.DocumentSlot   = ActiveDoc.GpuSlot;
                    Req.ViewportWidth  = static_cast<uint32_t>(CanvasSize.x);
                    Req.ViewportHeight = static_cast<uint32_t>(CanvasSize.y);
                    Req.DeltaSeconds   = std::clamp(Io.DeltaTime, 1.0f / 240.0f, 1.0f / 20.0f);
                    Req.StepOnce       = ActiveDoc.StepOncePending;
                    Req.Restart        = ActiveDoc.RestartPending;
                    Req.Parameters     = P;
                    Req.Camera         = ActiveDoc.Scene.Camera;
                    Req.BrushActive    = BrushActiveNow;
                    Req.BrushUvw       = BrushUvw;
                    Req.BrushVelocity  = BrushVelocity;
                    Req.BrushRadius    = ActiveDoc.BrushRadius;
                    if (!ActiveDoc.PendingBlasts.empty())
                    {
                        Req.Blasts.push_back(ActiveDoc.PendingBlasts.front());
                        ActiveDoc.PendingBlasts.erase(ActiveDoc.PendingBlasts.begin());
                    }
                    ActiveDoc.StepOncePending = false;
                    ActiveDoc.RestartPending  = false;
                    Session.LastImage         = Presenter(Req);
                }
            }

            const ImVec2 CanvasMax = ImVec2(CanvasMin.x + CanvasSize.x, CanvasMin.y + CanvasSize.y);
            VpDraw->AddRectFilled(CanvasMin, CanvasMax, IM_COL32(11, 11, 11, 255));

            if (Session.LastImage.Texture != 0u)
            {
                VpDraw->AddImage(static_cast<ImTextureID>(Session.LastImage.Texture), CanvasMin, CanvasMax);
            }
            else
            {
                const char* Msg = Session.LastImage.Failure.empty()
                                      ? "FluidViewportPresenter not registered on this host."
                                      : Session.LastImage.Failure.c_str();
                VpDraw->AddText(Faces.Medium, 13.0f, ImVec2(CanvasMin.x + 24.0f, CanvasMin.y + 24.0f),
                                Palette.WarmAmber, Msg);
            }

            if (Session.ShowDomainWire || P.ShowBoundingBox)
            {
                VpDraw->PushClipRect(CanvasMin, CanvasMax, true);
                for (const auto& Seg : ActiveDoc.Scene.Camera.ProjectBoundsSegments(HalfBox, CanvasSize.x, CanvasSize.y))
                {
                    VpDraw->AddLine(ImVec2(CanvasMin.x + Seg[0], CanvasMin.y + Seg[1]),
                                    ImVec2(CanvasMin.x + Seg[2], CanvasMin.y + Seg[3]),
                                    IM_COL32(90, 169, 255, 90), 1.15f);
                }
                VpDraw->PopClipRect();
            }

            // Viewport status, floating tools, telemetry and world-axis gizmo.
            const ImVec2 CaptionMin(CanvasMin.x + 18.0f * Scale, CanvasMin.y + 16.0f * Scale);
            const char* SelectedObject = Session.SelectedNode == OutlinerNodeId::Domain ? ActiveDoc.Scene.Names.Domain.c_str() :
                                         Session.SelectedNode == OutlinerNodeId::Collider ? ActiveDoc.Scene.Names.Collider.c_str() :
                                         Session.SelectedNode == OutlinerNodeId::Sun ? ActiveDoc.Scene.Names.Sun.c_str() : ActiveDoc.Scene.Names.Source.c_str();
            const char* LiveLabel = P.Paused ? "PAUSED" : "LIVE";
            const ImVec2 LiveSize = ImGui::CalcTextSize(LiveLabel);
            const float LiveWidth = LiveSize.x * Scale + 35.0f * Scale;
            VpDraw->AddRectFilled(CaptionMin, ImVec2(CaptionMin.x + LiveWidth, CaptionMin.y + 25.0f * Scale), IM_COL32(18, 18, 18, 225), 14.0f * Scale);
            VpDraw->AddRect(CaptionMin, ImVec2(CaptionMin.x + LiveWidth, CaptionMin.y + 25.0f * Scale), IM_COL32(255, 255, 255, 18), 14.0f * Scale);
            VpDraw->AddCircleFilled(ImVec2(CaptionMin.x + 12.0f * Scale, CaptionMin.y + 12.5f * Scale), 3.0f * Scale,
                                    P.Paused ? IM_COL32(183, 183, 183, 255) : IM_COL32(52, 199, 89, 255));
            VpDraw->AddText(Faces.Medium, 9.0f * Scale, ImVec2(CaptionMin.x + 21.0f * Scale, CaptionMin.y + 7.0f * Scale), IM_COL32(218, 218, 218, 255), LiveLabel);
            const ImVec2 ObjectLabelMin(CaptionMin.x + LiveWidth + 8.0f * Scale, CaptionMin.y);
            const ImVec2 ObjectLabelSize = ImGui::CalcTextSize(SelectedObject);
            const float ObjectPillW = std::min(CanvasSize.x * 0.34f, ObjectLabelSize.x * Scale + 22.0f * Scale);
            VpDraw->AddRectFilled(ObjectLabelMin, ImVec2(ObjectLabelMin.x + ObjectPillW, ObjectLabelMin.y + 25.0f * Scale), IM_COL32(18, 18, 18, 225), 14.0f * Scale);
            VpDraw->AddText(Faces.Regular, 10.0f * Scale, ImVec2(ObjectLabelMin.x + 11.0f * Scale, ObjectLabelMin.y + 7.0f * Scale), IM_COL32(190, 190, 190, 255), SelectedObject);
            VpDraw->AddText(Faces.Regular, 9.0f * Scale, ImVec2(ObjectLabelMin.x, ObjectLabelMin.y + 33.0f * Scale), IM_COL32(119, 119, 119, 255), "Eulerian volume \xC2\xB7 physically shaded");

            const ImVec2 ToolsMin(CanvasMin.x + 16.0f * Scale, CanvasMin.y + 83.0f * Scale);
            constexpr float ToolBoxW = 42.0f;
            constexpr float ToolButtonH = 38.0f;
            VpDraw->AddRectFilled(ToolsMin, ImVec2(ToolsMin.x + ToolBoxW * Scale, ToolsMin.y + 4.0f * ToolButtonH * Scale + 12.0f * Scale),
                                  IM_COL32(18, 18, 18, 218), 21.0f * Scale);
            VpDraw->AddRect(ImVec2(ToolsMin.x, ToolsMin.y), ImVec2(ToolsMin.x + ToolBoxW * Scale, ToolsMin.y + 4.0f * ToolButtonH * Scale + 12.0f * Scale),
                            IM_COL32(255, 255, 255, 18), 21.0f * Scale);
            const char* ToolGlyphs[] = {
                "M12 3a9 9 0 1 0 9 9M12 7v5l3 2",
                "M14 4 20 10M5 19l2.5-.5L19 7a2.1 2.1 0 0 0-3-3L4.5 15.5 4 18Z",
                "M12 2v7m0 6v7M2 12h7m6 0h7M5 5l5 5m4 4 5 5m0-14-5 5m-4 4-5 5"
            };
            const char* ToolTips[] = { "Orbit camera 1", "Source brush 2", "Place burst 3" };
            for (int ToolIndex = 0; ToolIndex < 3; ++ToolIndex)
            {
                const ImVec2 ButtonMin(ToolsMin.x + 4.0f * Scale, ToolsMin.y + (4.0f + static_cast<float>(ToolIndex) * ToolButtonH) * Scale);
                ImGui::SetCursorScreenPos(ButtonMin);
                const bool Pressed = ImGui::InvisibleButton(ToolTips[ToolIndex], ImVec2(34.0f * Scale, 34.0f * Scale));
                const bool ToolHovered = ImGui::IsItemHovered();
                const bool Selected = Session.ActiveViewportTool == ToolIndex;
                if (Pressed)
                {
                    Session.ActiveViewportTool = ToolIndex;
                    P.InteractionTool = static_cast<FluidDynamics::FluidInteractionTool>(ToolIndex);
                }
                if (Selected || ToolHovered)
                    VpDraw->AddRectFilled(ButtonMin, ImVec2(ButtonMin.x + 34.0f * Scale, ButtonMin.y + 34.0f * Scale),
                                          Selected ? IM_COL32(255, 255, 255, 15) : IM_COL32(255, 255, 255, 8), 17.0f * Scale);
                PanelChrome::DrawGlyph(ToolGlyphs[ToolIndex], ImVec2(ButtonMin.x + 9.0f * Scale, ButtonMin.y + 9.0f * Scale),
                                       16.0f * Scale, Selected ? IM_COL32(235, 235, 235, 255) : IM_COL32(139, 139, 139, 255), 1.5f);
                if (ToolHovered) ImGui::SetTooltip("%s", ToolTips[ToolIndex]);
            }
            const float TurntableY = ToolsMin.y + 4.0f * Scale + 3.0f * ToolButtonH * Scale;
            VpDraw->AddLine(ImVec2(ToolsMin.x + 10.0f * Scale, TurntableY), ImVec2(ToolsMin.x + 32.0f * Scale, TurntableY), IM_COL32(255, 255, 255, 18));
            const ImVec2 TurnMin(ToolsMin.x + 4.0f * Scale, TurntableY + 4.0f * Scale);
            ImGui::SetCursorScreenPos(TurnMin);
            const bool TurnPressed = ImGui::InvisibleButton("##FluidTurntable", ImVec2(34.0f * Scale, 34.0f * Scale));
            if (TurnPressed)
            {
                P.AutoTurntable = !P.AutoTurntable;
                ActiveDoc.Scene.Camera.AutoTurntable = P.AutoTurntable;
            }
            const bool TurnHovered = ImGui::IsItemHovered();
            if (P.AutoTurntable || TurnHovered)
                VpDraw->AddRectFilled(TurnMin, ImVec2(TurnMin.x + 34.0f * Scale, TurnMin.y + 34.0f * Scale),
                                      P.AutoTurntable ? IM_COL32(255, 255, 255, 15) : IM_COL32(255, 255, 255, 8), 17.0f * Scale);
            PanelChrome::DrawGlyph("M20 7v5h-5M4 17v-5h5m10-2a7 7 0 0 0-12-3L4 9m16 6-3 2a7 7 0 0 1-12-3", ImVec2(TurnMin.x + 9.0f * Scale, TurnMin.y + 9.0f * Scale),
                                   16.0f * Scale, P.AutoTurntable ? IM_COL32(232, 232, 232, 255) : IM_COL32(139, 139, 139, 255), 1.5f);
            if (TurnHovered) ImGui::SetTooltip("Turntable T");

            const float Fps = Io.DeltaTime > 1.0e-4f ? std::min(999.0f, 1.0f / Io.DeltaTime) : 0.0f;
            char FpsText[24]{};
            std::snprintf(FpsText, sizeof(FpsText), "%.0f FPS", Fps);
            char GridBuffer[32]{};
            std::snprintf(GridBuffer, sizeof(GridBuffer), "%d\xC2\xB3 GRID", P.GridResolution);
            const float HudY = CanvasMin.y + 16.0f * Scale;
            const float GridW = ImGui::CalcTextSize(GridBuffer).x * Scale + 18.0f * Scale;
            const float FpsW = ImGui::CalcTextSize(FpsText).x * Scale + 18.0f * Scale;
            const float HudRight = CanvasMax.x - 16.0f * Scale;
            VpDraw->AddRectFilled(ImVec2(HudRight - GridW, HudY), ImVec2(HudRight, HudY + 25.0f * Scale), IM_COL32(18, 18, 18, 205), 14.0f * Scale);
            VpDraw->AddText(Faces.Regular, 9.0f * Scale, ImVec2(HudRight - GridW + 9.0f * Scale, HudY + 7.0f * Scale), IM_COL32(163, 163, 163, 255), GridBuffer);
            VpDraw->AddRectFilled(ImVec2(HudRight - GridW - FpsW - 6.0f * Scale, HudY), ImVec2(HudRight - GridW - 6.0f * Scale, HudY + 25.0f * Scale), IM_COL32(18, 18, 18, 205), 14.0f * Scale);
            VpDraw->AddText(Faces.Regular, 9.0f * Scale, ImVec2(HudRight - GridW - FpsW + 3.0f * Scale, HudY + 7.0f * Scale), IM_COL32(163, 163, 163, 255), FpsText);

            const char* MouseHint = "Drag to orbit   \xC2\xB7   Right-drag to pan   \xC2\xB7   Scroll to zoom";
            const ImVec2 HintSize = ImGui::CalcTextSize(MouseHint);
            const ImVec2 HintMin(CanvasMin.x + 18.0f * Scale, CanvasMax.y - 32.0f * Scale);
            VpDraw->AddRectFilled(ImVec2(HintMin.x - 8.0f * Scale, HintMin.y - 4.0f * Scale),
                                  ImVec2(HintMin.x + HintSize.x * Scale + 8.0f * Scale, HintMin.y + 22.0f * Scale), IM_COL32(18, 18, 18, 176), 12.0f * Scale);
            VpDraw->AddText(Faces.Regular, 9.0f * Scale, HintMin, IM_COL32(137, 137, 137, 255), MouseHint);

            const ImVec2 GizmoCentre(CanvasMax.x - 52.0f * Scale, CanvasMax.y - 54.0f * Scale);
            VpDraw->AddCircleFilled(GizmoCentre, 3.0f * Scale, IM_COL32(215, 215, 215, 255));
            VpDraw->AddLine(GizmoCentre, ImVec2(GizmoCentre.x, GizmoCentre.y - 28.0f * Scale), IM_COL32(105, 208, 109, 230), 1.6f * Scale);
            VpDraw->AddLine(GizmoCentre, ImVec2(GizmoCentre.x - 26.0f * Scale, GizmoCentre.y + 15.0f * Scale), IM_COL32(239, 83, 80, 230), 1.6f * Scale);
            VpDraw->AddLine(GizmoCentre, ImVec2(GizmoCentre.x + 26.0f * Scale, GizmoCentre.y + 15.0f * Scale), IM_COL32(91, 140, 255, 230), 1.6f * Scale);
            VpDraw->AddText(Faces.Medium, 9.0f * Scale, ImVec2(GizmoCentre.x - 3.0f * Scale, GizmoCentre.y - 42.0f * Scale), IM_COL32(105, 208, 109, 255), "Y");
            VpDraw->AddText(Faces.Medium, 9.0f * Scale, ImVec2(GizmoCentre.x - 37.0f * Scale, GizmoCentre.y + 14.0f * Scale), IM_COL32(239, 83, 80, 255), "X");
            VpDraw->AddText(Faces.Medium, 9.0f * Scale, ImVec2(GizmoCentre.x + 30.0f * Scale, GizmoCentre.y + 14.0f * Scale), IM_COL32(91, 140, 255, 255), "Z");

            if (Session.DiagnosticsOpen)
            {
                const ImVec2 DiagMin(CanvasMax.x - 292.0f * Scale, CanvasMin.y + 54.0f * Scale);
                const ImVec2 DiagMax(DiagMin.x + 276.0f * Scale, DiagMin.y + 146.0f * Scale);
                VpDraw->AddRectFilled(DiagMin, DiagMax, IM_COL32(18, 18, 18, 238), 18.0f * Scale);
                VpDraw->AddRect(DiagMin, DiagMax, IM_COL32(255, 255, 255, 22), 18.0f * Scale);
                VpDraw->AddText(Faces.Medium, 10.0f * Scale, ImVec2(DiagMin.x + 15.0f * Scale, DiagMin.y + 13.0f * Scale), IM_COL32(196, 196, 196, 255), "LIVE DIAGNOSTICS");
                char Diagnostics[160]{};
                std::snprintf(Diagnostics, sizeof(Diagnostics), "Grid       %d x %d x %d\nFrame time %.2f ms\nRay steps   %u\nAdapter     %s",
                              P.GridResolution, P.GridResolution, P.GridResolution, Session.LastImage.RefreshMilliseconds,
                              Session.LastImage.EffectiveRaySteps, Session.LastImage.Adapter.empty() ? "Vulkan Compute" : Session.LastImage.Adapter.c_str());
                VpDraw->AddText(Faces.Monospace, 9.0f * Scale, ImVec2(DiagMin.x + 15.0f * Scale, DiagMin.y + 35.0f * Scale), IM_COL32(142, 142, 142, 255), Diagnostics);
            }
            const float TransportY = CanvasMin.y + CanvasSize.y;
            const ImVec2 TransportMin(ColumnMin.x, TransportY);
            const ImVec2 TransportMax(ColumnMin.x + ViewportWidth, TransportY + TransportHeight);
            VpDraw->AddRectFilled(TransportMin, TransportMax, IM_COL32(22, 22, 22, 255));
            VpDraw->AddLine(TransportMin, ImVec2(TransportMax.x, TransportMin.y), IM_COL32(255, 255, 255, 15), 1.0f);

            auto TransportButton = [&](const char* Id, const char* Label, float X, float Width, bool Primary = false) -> bool
            {
                const ImVec2 Min(X, TransportY + 10.0f * Scale);
                ImGui::SetCursorScreenPos(Min);
                const bool Pressed = ImGui::InvisibleButton(Id, ImVec2(Width, 32.0f * Scale));
                const bool ButtonHovered = ImGui::IsItemHovered();
                const float Rounding = Primary ? 16.0f * Scale : 16.0f * Scale;
                const ImU32 Fill = Primary ? IM_COL32(224, 224, 224, 255) : ButtonHovered ? IM_COL32(42, 42, 42, 255) : IM_COL32(31, 31, 31, 255);
                VpDraw->AddRectFilled(Min, ImVec2(Min.x + Width, Min.y + 32.0f * Scale), Fill, Rounding);
                if (Primary && (std::string_view(Label) == "play" || std::string_view(Label) == "pause"))
                {
                    const ImVec2 C(Min.x + Width * 0.5f, Min.y + 16.0f * Scale);
                    if (P.Paused)
                    {
                        const ImVec2 Triangle[3] = { { C.x - 4.0f * Scale, C.y - 6.0f * Scale }, { C.x - 4.0f * Scale, C.y + 6.0f * Scale }, { C.x + 6.0f * Scale, C.y } };
                        VpDraw->AddConvexPolyFilled(Triangle, 3, IM_COL32(18, 18, 18, 255));
                    }
                    else
                    {
                        VpDraw->AddRectFilled(ImVec2(C.x - 5.0f * Scale, C.y - 6.0f * Scale), ImVec2(C.x - 2.0f * Scale, C.y + 6.0f * Scale), IM_COL32(18, 18, 18, 255), 1.0f * Scale);
                        VpDraw->AddRectFilled(ImVec2(C.x + 2.0f * Scale, C.y - 6.0f * Scale), ImVec2(C.x + 5.0f * Scale, C.y + 6.0f * Scale), IM_COL32(18, 18, 18, 255), 1.0f * Scale);
                    }
                }
                else
                {
                    const ImVec2 TextSize = ImGui::CalcTextSize(Label);
                    VpDraw->AddText(Faces.Regular, 10.0f * Scale,
                                    ImVec2(Min.x + (Width - TextSize.x * Scale) * 0.5f, Min.y + 10.0f * Scale),
                                    IM_COL32(204, 204, 204, 255), Label);
                }
                return Pressed;
            };

            float TransportX = ColumnMin.x + 14.0f * Scale;
            if (TransportButton("##FluidRestart", "R", TransportX, 30.0f * Scale))
            {
                ActiveDoc.RestartPending = true;
                Session.StatusNotice = "Restarted live simulation";
            }
            TransportX += 36.0f * Scale;
            if (TransportButton("##FluidPlayback", P.Paused ? "play" : "pause", TransportX, 34.0f * Scale, true))
                P.Paused = !P.Paused;
            TransportX += 40.0f * Scale;
            if (TransportButton("##FluidStep", "S", TransportX, 30.0f * Scale))
            {
                P.Paused = true;
                ActiveDoc.StepOncePending = true;
            }
            TransportX += 44.0f * Scale;
            char ClockText[32]{};
            std::snprintf(ClockText, sizeof(ClockText), "%02d:%02d.%03d", static_cast<int>(Session.LastImage.ElapsedSeconds) / 60,
                          static_cast<int>(Session.LastImage.ElapsedSeconds) % 60,
                          static_cast<int>((Session.LastImage.ElapsedSeconds - std::floor(Session.LastImage.ElapsedSeconds)) * 1000.0f));
            VpDraw->AddText(Faces.Monospace, 10.0f * Scale, ImVec2(TransportX, TransportY + 20.0f * Scale), IM_COL32(191, 191, 191, 255), ClockText);

            const char* SpeedLabels[] = { "0.25x", "0.5x", "1.0x", "2.0x" };
            const float SpeedValues[] = { 0.25f, 0.5f, 1.0f, 2.0f };
            int SpeedIndex = 2;
            for (int Index = 0; Index < 4; ++Index) if (std::fabs(Session.PlaybackSpeed - SpeedValues[Index]) < 1.0e-4f) SpeedIndex = Index;
            const float SpeedX = ColumnMin.x + 264.0f * Scale;
            ImGui::SetCursorScreenPos(ImVec2(SpeedX, TransportY + 10.0f * Scale));
            ImGui::SetNextItemWidth(75.0f * Scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * Scale, 6.0f * Scale));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f * Scale);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(31, 31, 31, 255));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(42, 42, 42, 255));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 14));
            if (DropdownControlSurface::BeginSelection("##FluidSimulationSpeed", SpeedLabels[SpeedIndex], Scale))
            {
                for (int Index = 0; Index < 4; ++Index)
                {
                    if (!DropdownControlSurface::Option(SpeedLabels[Index], Index == SpeedIndex, Scale)) continue;
                    SpeedIndex = Index;
                    Session.PlaybackSpeed = SpeedValues[Index];
                    P.TimeScale = SpeedValues[Index];
                }
                DropdownControlSurface::EndSelection();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            const float LiveX = SpeedX + 90.0f * Scale;
            VpDraw->AddCircleFilled(ImVec2(LiveX + 3.0f * Scale, TransportY + 26.0f * Scale), 3.0f * Scale, IM_COL32(52, 199, 89, 255));
            VpDraw->AddText(Faces.Regular, 9.0f * Scale, ImVec2(LiveX + 12.0f * Scale, TransportY + 21.0f * Scale), IM_COL32(135, 135, 135, 255), "Live / uncached");

            const char* BurstNames[] = { "Single burst", "Shrapnel", "Cluster salvo", "Colliding waves", "Mushroom cloud" };
            int BurstIndex = std::clamp(static_cast<int>(ActiveDoc.BurstPattern), 0, 4);
            const float TriggerWidth = 92.0f * Scale;
            const float TriggerX = ColumnMin.x + ViewportWidth - 14.0f * Scale - TriggerWidth;
            const float BurstSelectWidth = std::min(150.0f * Scale, std::max(92.0f * Scale, TriggerX - (LiveX + 132.0f * Scale)));
            const float BurstSelectX = TriggerX - 8.0f * Scale - BurstSelectWidth;
            ImGui::SetCursorScreenPos(ImVec2(BurstSelectX, TransportY + 10.0f * Scale));
            ImGui::SetNextItemWidth(BurstSelectWidth);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * Scale, 6.0f * Scale));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f * Scale);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(31, 31, 31, 255));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(42, 42, 42, 255));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 14));
            if (DropdownControlSurface::BeginSelection("##FluidBurstKind", BurstNames[BurstIndex], Scale))
            {
                for (int Index = 0; Index < 5; ++Index)
                {
                    if (!DropdownControlSurface::Option(BurstNames[Index], Index == BurstIndex, Scale)) continue;
                    BurstIndex = Index;
                    ActiveDoc.BurstPattern = static_cast<FluidDynamics::FluidBurstPattern>(Index);
                    Config.BurstPattern = static_cast<uint32_t>(Index);
                    Config.AdvanceRevision();
                }
                DropdownControlSurface::EndSelection();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            if (TransportButton("##FluidTriggerBurst", "Trigger  D", TriggerX, TriggerWidth))
            {
                EnqueueBurstPattern(ActiveDoc, ActiveDoc.BurstPattern);
                ++Config.BurstGeneration;
                Config.AdvanceRevision();
                Session.StatusNotice = "Triggered " + std::string(BurstNames[BurstIndex]);
            }

            const float TimelineY = TransportY + TransportHeight;
            const ImVec2 TimelineMin(ColumnMin.x, TimelineY);
            const ImVec2 TimelineMax(ColumnMin.x + ViewportWidth, TimelineY + TimelineHeight);
            VpDraw->AddRectFilled(TimelineMin, TimelineMax, IM_COL32(17, 17, 17, 255));
            VpDraw->AddLine(TimelineMin, ImVec2(TimelineMax.x, TimelineMin.y), IM_COL32(255, 255, 255, 13));
            VpDraw->AddText(Faces.Medium, 9.0f * Scale, ImVec2(TimelineMin.x + 16.0f * Scale, TimelineMin.y + 10.0f * Scale), IM_COL32(135, 135, 135, 255), "SIMULATION");
            char FrameText[32]{};
            std::snprintf(FrameText, sizeof(FrameText), "FRAME %04llu", static_cast<unsigned long long>(Session.LastImage.StepCount));
            const ImVec2 FrameSize = ImGui::CalcTextSize(FrameText);
            VpDraw->AddText(Faces.Monospace, 9.0f * Scale, ImVec2(TimelineMax.x - FrameSize.x * Scale - 16.0f * Scale, TimelineMin.y + 10.0f * Scale), IM_COL32(121, 121, 121, 255), FrameText);
            const float RulerX = TimelineMin.x + 18.0f * Scale;
            const float RulerW = std::max(50.0f * Scale, ViewportWidth - 36.0f * Scale);
            const float RulerY = TimelineMin.y + 39.0f * Scale;
            VpDraw->AddLine(ImVec2(RulerX, RulerY), ImVec2(RulerX + RulerW, RulerY), IM_COL32(75, 75, 75, 255), 1.0f * Scale);
            for (int Tick = 0; Tick <= 6; ++Tick)
            {
                const float TickX = RulerX + RulerW * (static_cast<float>(Tick) / 6.0f);
                VpDraw->AddLine(ImVec2(TickX, RulerY - 4.0f * Scale), ImVec2(TickX, RulerY + 4.0f * Scale), IM_COL32(104, 104, 104, 255), 1.0f * Scale);
                char Mark[12]{};
                std::snprintf(Mark, sizeof(Mark), "%ds", Tick * 5);
                const ImVec2 MarkSize = ImGui::CalcTextSize(Mark);
                VpDraw->AddText(Faces.Regular, 8.5f * Scale, ImVec2(TickX - MarkSize.x * 0.5f * Scale, TimelineMin.y + 47.0f * Scale), IM_COL32(114, 114, 114, 255), Mark);
            }
            const float TrackY = TimelineMin.y + 67.0f * Scale;
            VpDraw->AddRectFilled(ImVec2(RulerX, TrackY), ImVec2(RulerX + RulerW, TrackY + 13.0f * Scale), IM_COL32(36, 36, 36, 255), 6.0f * Scale);
            const float TrackFill = Session.LastImage.ElapsedSeconds > 0.0f
                                        ? std::fmod(Session.LastImage.ElapsedSeconds, 30.0f) / 30.0f * RulerW : 0.0f;
            VpDraw->AddRectFilled(ImVec2(RulerX, TrackY), ImVec2(RulerX + std::max(2.0f * Scale, TrackFill), TrackY + 13.0f * Scale), IM_COL32(61, 61, 61, 255), 6.0f * Scale);
            VpDraw->AddText(Faces.Regular, 8.0f * Scale, ImVec2(RulerX + 8.0f * Scale, TrackY + 2.0f * Scale), IM_COL32(159, 159, 159, 255), P.SourceEnabled ? "Continuous emission" : "Source disabled");
            const float PlayheadX = std::clamp(RulerX + TrackFill, RulerX, RulerX + RulerW);
            VpDraw->AddLine(ImVec2(PlayheadX, RulerY - 5.0f * Scale), ImVec2(PlayheadX, TrackY + 16.0f * Scale), IM_COL32(218, 218, 218, 230), 1.0f * Scale);
            VpDraw->AddCircleFilled(ImVec2(PlayheadX, RulerY - 5.0f * Scale), 3.0f * Scale, IM_COL32(218, 218, 218, 255));
        }
        ImGui::EndChild();

        // --- RIGHT COLUMN: 3-TAB INSPECTOR (Source | Simulation | Rendering) ---
        if (ShowInspector)
        {
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * Scale, 15.0f * Scale));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * Scale, 5.0f * Scale));
            if (ImGui::BeginChild("##FluidInspectorColumn", ImVec2(InspectorWidth, BodyHeight), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoBackground))
            {
                ImDrawList* ColDraw = ImGui::GetWindowDrawList();
                const ImVec2 ColMin = ImGui::GetWindowPos();
                ColDraw->AddRectFilled(ColMin, ImVec2(ColMin.x + InspectorWidth, ColMin.y + BodyHeight), IM_COL32(18, 18, 18, 255), 22.0f);
                ColDraw->AddRect(ColMin, ImVec2(ColMin.x + InspectorWidth, ColMin.y + BodyHeight), IM_COL32(255, 255, 255, 13), 22.0f);
                const ImVec2 InspectorHeading = ImGui::GetCursorScreenPos();
                ColDraw->AddText(Faces.Light ? Faces.Light : Faces.Regular, 16.0f * Scale, InspectorHeading, IM_COL32(224, 224, 224, 255), "Inspector");
                ImGui::SetCursorScreenPos(ImVec2(InspectorHeading.x + InspectorWidth - 102.0f, InspectorHeading.y - 2.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(25, 25, 25, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(38, 38, 38, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(45, 45, 45, 255));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 14));
                if (ImGui::Button("Reset", ImVec2(58.0f, 25.0f)))
                {
                    PushUndoSnapshot(ActiveDoc);
                    ActiveDoc.Scene.Parameters = FluidDynamics::ConstructPresetParameters(ActiveDoc.PresetIdentity);
                    ActiveDoc.RestartPending = true;
                }
                ImGui::PopStyleColor(5);
                ImGui::PopStyleVar(2);
                ImGui::SetCursorScreenPos(ImVec2(InspectorHeading.x, InspectorHeading.y + 30.0f));
                const ImVec2 ObjectMin = ImGui::GetCursorScreenPos();
                const float ObjectWidth = ImGui::GetContentRegionAvail().x;
                const ImVec2 ObjectMax(ObjectMin.x + ObjectWidth, ObjectMin.y + 78.0f);
                ColDraw->AddRectFilled(ObjectMin, ObjectMax, IM_COL32(26, 26, 26, 255), 16.0f);
                ColDraw->AddRect(ObjectMin, ObjectMax, IM_COL32(255, 255, 255, 14), 16.0f);
                const char* ObjectName = Session.SelectedNode == OutlinerNodeId::Domain ? ActiveDoc.Scene.Names.Domain.c_str() :
                                         Session.SelectedNode == OutlinerNodeId::Collider ? ActiveDoc.Scene.Names.Collider.c_str() :
                                         Session.SelectedNode == OutlinerNodeId::Sun ? ActiveDoc.Scene.Names.Sun.c_str() : ActiveDoc.Scene.Names.Source.c_str();
                const char* ObjectType = Session.SelectedNode == OutlinerNodeId::Domain ? "EULERIAN VOLUME" :
                                         Session.SelectedNode == OutlinerNodeId::Collider ? "SIGNED DISTANCE COLLIDER" :
                                         Session.SelectedNode == OutlinerNodeId::Sun ? "DIRECTIONAL LIGHT" : "CONTINUOUS SOURCE";
                ColDraw->AddRectFilled(ImVec2(ObjectMin.x + 14.0f, ObjectMin.y + 17.0f), ImVec2(ObjectMin.x + 52.0f, ObjectMin.y + 61.0f), IM_COL32(43, 43, 43, 255), 14.0f);
                PanelChrome::DrawGlyph("M12 22a8 8 0 0 0 8-8c0-3-2-5-4-7-1 3-2 4-4 5-1-3-3-5-5-8-1 4-3 6-3 10a8 8 0 0 0 8 8Z",
                                       ImVec2(ObjectMin.x + 23.0f, ObjectMin.y + 30.0f), 18.0f, 0xFFB454u, 1.6f);
                ColDraw->AddText(Faces.Medium, 23.0f, ImVec2(ObjectMin.x + 64.0f, ObjectMin.y + 15.0f), IM_COL32(235, 235, 235, 255), ObjectName);
                ColDraw->AddText(Faces.Regular, 8.0f, ImVec2(ObjectMin.x + 65.0f, ObjectMin.y + 49.0f), IM_COL32(115, 115, 115, 255), ObjectType);
                ImGui::SetCursorScreenPos(ImVec2(ObjectMin.x, ObjectMin.y + 45.0f));
                if (FluidSwitch("", &Config.Enabled)) { Config.AdvanceRevision(); ActiveDoc.Dirty = true; }
                ImGui::SetCursorScreenPos(ImVec2(ObjectMin.x, ObjectMax.y + 12.0f));

                FluidInspectorTabStrip(Faces, Session.ActiveInspector);
                FluidSectionCardBounds SectionCards{};
                float SectionCardTop = -1.0f;
                bool CurrentGroupOpen = false;
                bool Edited = false;
                ColDraw->ChannelsSplit(2);
                ColDraw->ChannelsSetCurrent(1);
                const float PropertyStartY = ImGui::GetCursorScreenPos().y;
                const float RemoveReserve = Session.SelectedNode == OutlinerNodeId::Domain ? 0.0f : 48.0f * Scale;
                const float PropertiesHeight = std::max(100.0f * Scale,
                    ColMin.y + BodyHeight - PropertyStartY - RemoveReserve - 34.0f * Scale - 5.0f * Scale);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::SetCursorScreenPos(ImVec2(ColMin.x + 22.0f * Scale, PropertyStartY));
                const bool PropertyBodyOpen = ImGui::BeginChild("##FluidInspectorProperties",
                    ImVec2(std::max(100.0f * Scale, InspectorWidth - 44.0f * Scale), PropertiesHeight),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
                if (PropertyBodyOpen)
                {

                auto DrawColliderGroup = [&](const char* GroupId, bool DefaultOpen)
                {
                    if (!FluidPropertyGroup(Faces, GroupId, "Collider", "SDF", DefaultOpen, SectionCards, SectionCardTop, CurrentGroupOpen)) return;
                    FluidPropertyHint(Faces, "Position is normalized within the domain. The collider changes simulated flow without restarting it.");
                    const auto Labels = FluidDynamics::EnumerateFluidColliderLabels();
                    if (FluidSelect("ColliderShape", "Collider type", &P.ObstacleCategory, Labels.data(), static_cast<int>(Labels.size()), 10000.0f)) Edited = true;
                    Edited |= FluidSliderRow(Palette, Faces, "##FoPx", "Center X", P.ObstacleX, 0.05f, 0.95f, "%.2f");
                    Edited |= FluidSliderRow(Palette, Faces, "##FoPy", "Center Y", P.ObstacleY, 0.03f, 0.95f, "%.2f");
                    Edited |= FluidSliderRow(Palette, Faces, "##FoPz", "Center Z", P.ObstacleZ, 0.05f, 0.95f, "%.2f");
                    Edited |= FluidSliderRow(Palette, Faces, "##FoRad", "Collider radius", P.ObstacleRadius, 0.04f, 0.40f, "%.3f");
                    if (FluidSwitch("Animate orbital motion", &P.ColliderAutoMove)) Edited = true;
                    if (P.ColliderAutoMove) Edited |= FluidSliderRow(Palette, Faces, "##FoSpd", "Orbit speed", P.ColliderSpeed, 0.1f, 5.0f, "%.2f rad/s");
                };

                if (Session.ActiveInspector == 0)
                {
                    const bool ColliderSelected = Session.SelectedNode == OutlinerNodeId::Collider;
                    if (ColliderSelected) DrawColliderGroup("Source.Collider.Selected", true);
                    else
                    {
                        if (FluidPropertyGroup(Faces, "Source.Emission", "Emission", "SOURCE", true, SectionCards, SectionCardTop, CurrentGroupOpen))
                        {
                            FluidPropertyHint(Faces, "A continuous source at the domain’s X/Z centre. Height and radius are live simulation parameters.");
                            if (FluidSwitch("Enable continuous source", &P.SourceEnabled)) Edited = true;
                            Edited |= FluidSliderRow(Palette, Faces, "##FsRate", "Injection rate", P.SourceRate, 0.0f, 6.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FsRad", "Source radius", P.SourceRadius, 0.02f, 0.35f, "%.3f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FsHgt", "Base height", P.SourceHeight, 0.02f, 0.85f, "%.2f");
                        }
                        if (FluidPropertyGroup(Faces, "Source.FuelTemperature", "Fuel & temperature", "INJECTION", true, SectionCards, SectionCardTop, CurrentGroupOpen))
                        {
                            Edited |= FluidSliderRow(Palette, Faces, "##FsTemp", "Core temperature", P.SourceTemperature, 0.0f, 8.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FsFuel", "Fuel feed", P.SourceFuel, 0.0f, 6.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FsSmoke", "Soot / smoke", P.SourceSmoke, 0.0f, 6.0f, "%.2f");
                        }
                        if (FluidPropertyGroup(Faces, "Source.Velocity", "Velocity", "FLOW", true, SectionCards, SectionCardTop, CurrentGroupOpen))
                        {
                            Edited |= FluidSliderRow(Palette, Faces, "##FsUp", "Upward jet", P.SourceUpwardVelocity, -3.0f, 8.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FsSwirl", "Helical swirl", P.SourceSwirl, -6.0f, 6.0f, "%.2f");
                        }
                    }
                    if (FluidPropertyGroup(Faces, "Source.BurstGenerator", "Burst generator", "IMPULSE", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        if (FluidSwitch("Ballistic shrapnel streamers", &P.ShrapnelEnabled)) Edited = true;
                        Edited |= FluidSliderRow(Palette, Faces, "##FbStr", "Shock strength", P.BlastStrength, 0.5f, 22.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FbRad", "Blast radius", P.BlastRadius, 0.05f, 0.42f, "%.3f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FbTmp", "Fireball temp", P.BlastTemperature, 0.0f, 12.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FbFuel", "Blast fuel", P.BlastFuel, 0.0f, 10.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FbSmk", "Blast smoke", P.BlastSmoke, 0.0f, 8.0f, "%.2f");
                        Edited |= SliderIntRow(Palette, Faces, "##FbLobes", "Rayleigh lobes", P.BlastLobes, 0, 14);
                    }
                    if (!ColliderSelected) DrawColliderGroup("Source.Collider", false);
                }
                else if (Session.ActiveInspector == 1)
                {
                    if (FluidPropertyGroup(Faces, "Simulation.Domain", "Domain", "3D", true, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        Edited |= FluidSliderRow(Palette, Faces, "##FgBw", "Domain width", P.BoundsWidth, 0.8f, 4.5f, "%.2f m");
                        Edited |= FluidSliderRow(Palette, Faces, "##FgBh", "Domain height", P.BoundsHeight, 0.8f, 5.5f, "%.2f m");
                        if (FluidSwitch("Dynamic surge bounds", &P.DynamicBounds)) Edited = true;
                        if (P.DynamicBounds) Edited |= FluidSliderRow(Palette, Faces, "##FgSurge", "Maximum surge scale", P.DynamicBoundsMax, 1.0f, 3.0f, "%.2fx");
                    }
                    if (FluidPropertyGroup(Faces, "Simulation.VoxelGrid", "Voxel grid", "SOLVER", true, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        FluidPropertyHint(Faces, "Changing voxel resolution rebuilds the grid and restarts the simulation. Other properties update live.");
                        static const std::array<const char*, 6> ResLabels = { "32³", "48³", "64³", "80³", "96³", "128³" };
                        static constexpr int ResValues[6] = { 32, 48, 64, 80, 96, 128 };
                        int ResIdx = 1;
                        for (int Idx = 0; Idx < 6; ++Idx) if (P.GridResolution == ResValues[Idx]) ResIdx = Idx;
                        if (FluidSegmentedPropertyRow(Faces, "##FgRes", "Grid resolution", ResIdx, ResLabels.data(), static_cast<int>(ResLabels.size())))
                        {
                            P.GridResolution = ResValues[ResIdx]; Edited = true;
                        }
                        Edited |= SliderIntRow(Palette, Faces, "##FgJacobi", "Pressure iterations", P.PressureIterations, 4, 56);
                        if (FluidSwitch("MacCormack BFECC advection", &P.MacCormackAdvection)) Edited = true;
                        if (FluidSwitch("Enclosed ceiling and walls", &P.EnclosedBox)) Edited = true;
                        Edited |= FluidSliderRow(Palette, Faces, "##FgTime", "Time scale", P.TimeScale, 0.05f, 2.5f, "%.2fx");
                    }
                    if (FluidPropertyGroup(Faces, "Simulation.Forces", "Forces & turbulence", "FLOW", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        Edited |= FluidSliderRow(Palette, Faces, "##FsVort", "Vorticity confinement", P.VorticityConfinement, 0.0f, 16.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsBuoy", "Thermal buoyancy", P.Buoyancy, -4.0f, 10.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsWgt", "Heavy soot weight", P.SmokeWeight, -2.0f, 4.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsTurb", "Curl turbulence", P.TurbulenceStrength, 0.0f, 8.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsTSc", "Turbulence scale", P.TurbulenceScale, 0.5f, 10.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsWx", "Crosswind X", P.WindX, -4.0f, 4.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsWz", "Crosswind Z", P.WindZ, -4.0f, 4.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FsDmp", "Velocity drag", P.VelocityDamping, 0.0f, 2.0f, "%.2f");
                    }
                    if (FluidPropertyGroup(Faces, "Simulation.Combustion", "Combustion", "REACTION", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        Edited |= FluidSliderRow(Palette, Faces, "##FcBurn", "Burn rate", P.BurnRate, 0.0f, 8.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FcHeat", "Reaction enthalpy", P.BurnHeat, 0.0f, 8.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FcSoot", "Soot yield", P.SootGeneration, 0.0f, 4.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FcExp", "Gas expansion", P.CombustionExpansion, 0.0f, 6.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FcCool", "Radiative cooling", P.CoolingRate, 0.0f, 4.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FcDiss", "Smoke dissipation", P.SmokeDissipation, 0.0f, 2.0f, "%.3f");
                    }
                }
                else
                {
                    const bool SunSelected = Session.SelectedNode == OutlinerNodeId::Sun;
                    auto DrawDirectionalLight = [&]()
                    {
                        if (FluidPropertyGroup(Faces, "Rendering.DirectionalLight", "Directional light", "LIGHT", SunSelected, SectionCards, SectionCardTop, CurrentGroupOpen))
                        {
                            Edited |= FluidSliderRow(Palette, Faces, "##FlSunIn", "Sun intensity", P.SunIntensity, 0.0f, 8.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FlSunAz", "Sun azimuth", P.SunAzimuth, -180.0f, 180.0f, "%.1f°");
                            Edited |= FluidSliderRow(Palette, Faces, "##FlSunEl", "Sun elevation", P.SunElevation, 2.0f, 88.0f, "%.1f°");
                            Edited |= FluidSliderRow(Palette, Faces, "##FlAmb", "Sky ambient", P.AmbientIntensity, 0.0f, 3.0f, "%.2f");
                        }
                    };
                    if (SunSelected) DrawDirectionalLight();
                    if (FluidPropertyGroup(Faces, "Rendering.VolumeShading", "Volume shading", "VOLUME", true, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        const auto Labels = FluidDynamics::EnumerateFluidColorPalettes();
                        if (FluidSelect("ColorPalette", "Color palette", &P.ColorPalette, Labels.data(), static_cast<int>(Labels.size()), 10000.0f)) Edited = true;
                        Edited |= FluidSliderRow(Palette, Faces, "##FlFire", "Blackbody radiance", P.FireIntensity, 0.0f, 12.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlExt", "Density extinction", P.DensityExtinction, 1.0f, 36.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlAlb", "Smoke albedo", P.SmokeAlbedo, 0.02f, 0.98f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlExp", "Exposure", P.Exposure, 0.15f, 4.0f, "%.2f");
                    }
                    if (!SunSelected) DrawDirectionalLight();
                    if (FluidPropertyGroup(Faces, "Rendering.ScatteringGlow", "Scattering & glow", "LIGHT", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        Edited |= FluidSliderRow(Palette, Faces, "##FlBloom", "Fireball bloom", P.BloomIntensity, 0.0f, 2.5f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlRays", "Crepuscular rays", P.GodRaysIntensity, 0.0f, 2.5f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlShock", "Shockwave refraction", P.ShockwaveStrength, 0.0f, 2.5f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlTempK", "Temperature scale", P.TemperatureScale, 0.25f, 3.0f, "%.2fx");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlIntS", "Core glow scatter", P.InternalScattering, 0.0f, 6.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlShDn", "Self-shadowing", P.ShadowDensity, 0.0f, 24.0f, "%.2f");
                        Edited |= FluidSliderRow(Palette, Faces, "##FlAnis", "Phase anisotropy", P.PhaseAnisotropy, -0.5f, 0.85f, "%.2f");
                    }
                    if (FluidPropertyGroup(Faces, "Rendering.Quality", "Render quality", "GPU", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        Edited |= SliderIntRow(Palette, Faces, "##FrSteps", "Raymarch steps", P.RaymarchSteps, 24, 192);
                        Edited |= SliderIntRow(Palette, Faces, "##FrShad", "Shadow steps", P.ShadowSteps, 2, 24);
                        Edited |= FluidSliderRow(Palette, Faces, "##FrScale", "Render scale", P.RenderScale, 0.35f, 1.25f, "%.2fx");
                        if (FluidSwitch("Adaptive GPU governor", &P.AutoGpuGovernor)) Edited = true;
                        if (FluidSwitch("Auto turntable camera", &P.AutoTurntable)) Edited = true;
                        Edited |= FluidSliderRow(Palette, Faces, "##FrVoxQ", "Voxel quantization", P.VoxelQuantization, 0.0f, 1.0f, "%.2f");
                    }
                    if (FluidPropertyGroup(Faces, "Rendering.EmbersAsh", "Embers & ash", "PARTICLES", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        if (FluidSwitch("GPU ballistic embers and ash", &P.ShowEmbers)) Edited = true;
                        if (P.ShowEmbers)
                        {
                            Edited |= SliderIntRow(Palette, Faces, "##FeCnt", "Ember count", P.EmberCount, 16, 1200);
                            Edited |= FluidSliderRow(Palette, Faces, "##FeSz", "Spark size", P.EmberSize, 0.2f, 3.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FeInt", "Spark radiance", P.EmberIntensity, 0.1f, 6.0f, "%.2f");
                            Edited |= FluidSliderRow(Palette, Faces, "##FeLife", "Lifetime", P.EmberLifetime, 0.25f, 4.0f, "%.2f s");
                            Edited |= FluidSliderRow(Palette, Faces, "##FeAsh", "Cooling ashiness", P.EmberAshiness, 0.0f, 1.0f, "%.2f");
                        }
                    }
                    if (FluidPropertyGroup(Faces, "Rendering.Overlays", "Overlays & diagnostics", "DEBUG", false, SectionCards, SectionCardTop, CurrentGroupOpen))
                    {
                        if (FluidSwitch("Bounding box", &P.ShowBoundingBox)) Edited = true;
                        if (FluidSwitch("Voxel grid lines", &P.ShowVoxelGridLines)) Edited = true;
                        if (FluidSwitch("Floor reference grid", &P.ShowFloorGrid)) Edited = true;
                        if (FluidSwitch("Active voxel cell wireframe", &P.ShowActiveVoxelCells)) Edited = true;
                        if (FluidSwitch("2D tiled voxel atlas minimap", &P.ShowAtlasMinimap)) Edited = true;
                        if (P.ShowAtlasMinimap)
                        {
                            const auto Fields = FluidDynamics::EnumerateFluidAtlasFields();
                            if (FluidSelect("AtlasField", "Atlas field", &P.AtlasMinimapField, Fields.data(), static_cast<int>(Fields.size()), 10000.0f)) Edited = true;
                        }
                        const auto Channels = FluidDynamics::EnumerateFluidDebugChannels();
                        if (FluidSelect("RenderChannel", "Render channel", &P.RenderChannel, Channels.data(), static_cast<int>(Channels.size()), 10000.0f)) Edited = true;
                        if (P.RenderChannel == 7)
                        {
                            static const std::array<const char*, 3> Axes = { "X", "Y", "Z" };
                            if (FluidSegmentedPropertyRow(Faces, "##FrSlAx", "Slice axis", P.SliceAxis, Axes.data(), static_cast<int>(Axes.size()))) Edited = true;
                            Edited |= FluidSliderRow(Palette, Faces, "##FrSlPos", "Slice plane", P.SlicePosition, 0.02f, 0.98f, "%.2f");
                        }
                    }
                }

                CloseFluidSection(SectionCards, SectionCardTop, CurrentGroupOpen);
                if (Edited)
                {
                    ActiveDoc.Dirty = true;
                    SyncDocumentToRecord(ActiveDoc, Token, Config, false);
                }
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
                if (Session.SelectedNode != OutlinerNodeId::Domain)
                {
                    const ImVec2 RemoveMin(ColMin.x + 18.0f * Scale, ColMin.y + BodyHeight - 76.0f * Scale);
                    ImGui::SetCursorScreenPos(RemoveMin);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f * Scale);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * Scale, 8.0f * Scale));
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 7));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 14));
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));
                    const bool RemoveObject = ImGui::Button("Remove object##remove-object", ImVec2(InspectorWidth - 36.0f * Scale, 34.0f * Scale));
                    ImGui::PopStyleColor(3);
                    ImGui::PopStyleVar(2);
                    if (RemoveObject)
                    {
                        PushUndoSnapshot(ActiveDoc);
                        if (Session.SelectedNode == OutlinerNodeId::Source) P.SourceEnabled = false;
                        else if (Session.SelectedNode == OutlinerNodeId::Collider) P.ObstacleCategory = 0;
                        else if (Session.SelectedNode == OutlinerNodeId::Sun) P.SunIntensity = 0.0f;
                        Session.SelectedNode = OutlinerNodeId::Domain;
                        ActiveDoc.Dirty = true;
                        SyncDocumentToRecord(ActiveDoc, Token, Config, false);
                    }
                }
            ColDraw->ChannelsSetCurrent(0);
            for (const auto& [Top, Bottom] : SectionCards)
            {
                if (Bottom <= Top) continue;
                const ImVec2 CardMin(ColMin.x + 18.0f, Top);
                const ImVec2 CardMax(ColMin.x + InspectorWidth - 18.0f, Bottom);
                ColDraw->AddRectFilled(CardMin, CardMax, IM_COL32(26, 26, 26, 255), 20.0f);
                ColDraw->AddRect(CardMin, CardMax, IM_COL32(255, 255, 255, 13), 20.0f);
            }
            const ImVec2 InspectorFooterMin(ColMin.x, ColMin.y + BodyHeight - 31.0f * Scale);
            ColDraw->AddRectFilled(InspectorFooterMin, ImVec2(ColMin.x + InspectorWidth, ColMin.y + BodyHeight), IM_COL32(18, 18, 18, 255));
            ColDraw->AddLine(InspectorFooterMin, ImVec2(ColMin.x + InspectorWidth, InspectorFooterMin.y), IM_COL32(255, 255, 255, 12));
            PanelChrome::DrawGlyph("M10 13a5 5 0 0 0 7.1 0l2-2a5 5 0 0 0-7.1-7.1L11 5m3 6a5 5 0 0 0-7.1 0l-2 2A5 5 0 0 0 9 20.1L13 16", ImVec2(ColMin.x + 19.0f * Scale, InspectorFooterMin.y + 10.0f * Scale),
                                   12.0f * Scale, IM_COL32(112, 112, 112, 255), 1.4f);
            ColDraw->AddText(Faces.Regular, 9.0f * Scale, ImVec2(ColMin.x + 38.0f * Scale, InspectorFooterMin.y + 11.0f * Scale),
                             IM_COL32(115, 115, 115, 255), "Changes apply while rendering");
            ColDraw->ChannelsSetCurrent(1);
            ColDraw->ChannelsMerge();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
        }

        }

        //----------------------------------------------------------------------------------------------------------------
        // 4. STATUS BAR
        //----------------------------------------------------------------------------------------------------------------
        if (Session.ShowStatusBar)
            ImGui::SetCursorScreenPos(ImVec2(Position.x, BodyMin.y + BodySize.y - StatusHeight));
        if (Session.ShowStatusBar)
        {
            if (ImGui::BeginChild("##FluidStatusBar", ImVec2(TotalWidth, StatusHeight), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar))
            {
                ImDrawList*  StDraw = ImGui::GetWindowDrawList();
                const ImVec2 StMin  = ImGui::GetWindowPos();
                const ImVec2 StMax  = ImVec2(StMin.x + TotalWidth, StMin.y + StatusHeight);
                StDraw->AddRectFilled(StMin, StMax, Palette.StatusBarBg);
                StDraw->AddLine(StMin, ImVec2(StMax.x, StMin.y), Palette.EdgeBorder);

                StDraw->AddText(Faces.Regular, 11.0f, ImVec2(StMin.x + 12.0f, StMin.y + 6.0f),
                                Palette.TextPrimary, Session.StatusNotice.c_str());

                char      RightTelemetry[196]{};
                const int VoxelsK = (P.GridResolution * P.GridResolution * P.GridResolution) / 1000;
                std::snprintf(RightTelemetry, sizeof(RightTelemetry),
                              "Voxels: %dK  |  Step #%llu (%.1fs)  |  Undo: %zu  |  %s",
                              VoxelsK,
                              static_cast<unsigned long long>(Session.LastImage.StepCount),
                              Session.LastImage.ElapsedSeconds,
                              ActiveDoc.UndoStack.size(),
                              Session.LastImage.Adapter.empty() ? "Vulkan Compute" : Session.LastImage.Adapter.c_str());
                const ImVec2 TextSize = ImGui::CalcTextSize(RightTelemetry);
                StDraw->AddText(Faces.Regular, 11.0f,
                                ImVec2(std::max(StMin.x + 320.0f, StMax.x - TextSize.x - 14.0f), StMin.y + 6.0f),
                                Palette.TextMuted, RightTelemetry);
            }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    //--------------------------------------------------------------------------------------------------------------------
    // 5. MODAL DIALOGS: FLIPBOOK SPRITE SHEET BAKER & .FLUIDSPACE / .GASSCENE.TOML FILE IMPORT
    //--------------------------------------------------------------------------------------------------------------------
    if (Session.ShowShortcuts)
        ImGui::OpenPopup("Fluid editor shortcuts");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * Scale, 22.0f * Scale));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(26, 26, 26, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 20));
    if (ImGui::BeginPopupModal("Fluid editor shortcuts", &Session.ShowShortcuts,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        if (!Session.ShowShortcuts) ImGui::CloseCurrentPopup();
        ImGui::TextColored(ImVec4(0.88f, 0.88f, 0.88f, 1.0f), "Keyboard shortcuts");
        ImGui::SameLine();
        if (ImGui::SmallButton("x##CloseShortcuts"))
        {
            Session.ShowShortcuts = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
        const char* ShortcutNames[] = { "Play / pause", "Single step", "Trigger selected burst", "Restart simulation",
                                        "Focus volume", "Orbit / brush / place burst", "Domain bounds", "Turntable", "Save scene" };
        const char* ShortcutKeys[] = { "Space", "S", "D", "R", "F", "1 / 2 / 3", "B", "T", "Ctrl S" };
        for (int Index = 0; Index < 9; ++Index)
        {
            ImGui::TextDisabled("%s", ShortcutNames[Index]);
            ImGui::SameLine(265.0f * Scale);
            ImGui::Text("%s", ShortcutKeys[Index]);
        }
        ImGui::Separator();
        ImGui::TextWrapped("Scene files save authoring settings and camera position, not simulation fields. The timeline reports live simulation time; it is not a seekable cache.");
        ImGui::Dummy(ImVec2(1.0f, 4.0f * Scale));
        if (ImGui::Button("Done", ImVec2(110.0f * Scale, 32.0f * Scale)))
        {
            Session.ShowShortcuts = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (Session.Flipbook.DialogOpen)
        ImGui::OpenPopup("Bake flipbook");
    ImGui::SetNextWindowSize(ImVec2(620.0f * Scale, 0.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * Scale, 22.0f * Scale));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(26, 26, 26, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 20));
    if (ImGui::BeginPopupModal("Bake flipbook", &Session.Flipbook.DialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        if (!Session.Flipbook.DialogOpen) ImGui::CloseCurrentPopup();
        const ImVec2 DialogOrigin = ImGui::GetCursorScreenPos();
        ImDrawList* DialogDraw = ImGui::GetWindowDrawList();
        DialogDraw->AddText(Faces.Light ? Faces.Light : Faces.Regular, 24.0f * Scale, DialogOrigin,
                            IM_COL32(238, 238, 238, 255), "Bake flipbook");
        ImGui::SameLine(0.0f, 16.0f * Scale);
        if (ImGui::SmallButton("x##CloseFlipbook"))
        {
            Session.Flipbook.DialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(DialogOrigin.x, DialogOrigin.y + 36.0f * Scale));
        ImGui::TextColored(ImVec4(0.72f, 0.72f, 0.72f, 1.0f), "%s", Document.Scene.Name.c_str());
        ImGui::TextWrapped("Bake a rendered PNG sequence from these scene settings. The live scene remains available while frames are exported.");
        ImGui::Dummy(ImVec2(1.0f, 8.0f * Scale));

        static const char* const FrameOptions[] = { "4", "16", "32", "64" };
        static const int FrameCounts[] = { 4, 16, 32, 64 };
        int FrameIndex = 1;
        for (int Index = 0; Index < 4; ++Index) if (Session.Flipbook.FrameCount == FrameCounts[Index]) FrameIndex = Index;
        static const char* const SizeOptions[] = { "64 x 64", "128 x 128", "256 x 256", "512 x 512" };
        static const int TileSizes[] = { 64, 128, 256, 512 };
        int TileIndex = 2;
        for (int Index = 0; Index < 4; ++Index) if (Session.Flipbook.FrameWidth == TileSizes[Index]) TileIndex = Index;
        static const char* const RateOptions[] = { "12 fps", "24 fps", "30 fps", "60 fps" };
        static const int FrameRates[] = { 12, 24, 30, 60 };
        int RateIndex = 1;
        for (int Index = 0; Index < 4; ++Index) if (Session.Flipbook.FramesPerSecond == FrameRates[Index]) RateIndex = Index;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0f * Scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * Scale, 8.0f * Scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(9, 9, 9, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(25, 25, 25, 255));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 17));
        const float HalfWidth = 265.0f * Scale;
        ImGui::SetNextItemWidth(HalfWidth);
        if (ImGui::Combo("Frames", &FrameIndex, FrameOptions, 4)) Session.Flipbook.FrameCount = FrameCounts[FrameIndex];
        ImGui::SameLine(0.0f, 14.0f * Scale);
        ImGui::SetNextItemWidth(HalfWidth);
        if (ImGui::Combo("Tile resolution", &TileIndex, SizeOptions, 4))
        {
            Session.Flipbook.FrameWidth = TileSizes[TileIndex];
            Session.Flipbook.FrameHeight = TileSizes[TileIndex];
        }
        ImGui::SetNextItemWidth(HalfWidth);
        if (ImGui::Combo("Frame rate", &RateIndex, RateOptions, 4)) Session.Flipbook.FramesPerSecond = FrameRates[RateIndex];
        ImGui::SameLine(0.0f, 14.0f * Scale);
        ImGui::SetNextItemWidth(HalfWidth);
        ImGui::InputText("Output PNG", Session.Flipbook.OutputPath, sizeof(Session.Flipbook.OutputPath));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        (void)FluidSliderRow(Palette, Faces, "##FlipWarmup", "Warm-up before capture", Session.Flipbook.WarmupSeconds, 0.0f, 3.0f, "%.1f s");
        (void)FluidSwitch("Transparent background", &Session.Flipbook.Transparent);
        (void)FluidSwitch("Trigger selected burst on first frame", &Session.Flipbook.DetonateStart);
        ImGui::TextDisabled("Exports a sprite-sheet PNG and matching JSON metadata.");

        if (Session.Flipbook.Recording)
        {
            const int Total = Session.Flipbook.Columns * Session.Flipbook.Rows;
            ImGui::ProgressBar(static_cast<float>(Session.Flipbook.Captured) / static_cast<float>(std::max(1, Total)),
                               ImVec2(540.0f * Scale, 6.0f * Scale), "");
            ImGui::TextDisabled("Rendering frame %d of %d...", Session.Flipbook.Captured, Total);
        }
        else
        {
            if (ImGui::Button("Bake PNG + JSON", ImVec2(170.0f * Scale, 36.0f * Scale)))
            {
                Session.Flipbook.Columns = Session.Flipbook.FrameCount == 4 ? 2 : Session.Flipbook.FrameCount == 32 ? 8 :
                                           Session.Flipbook.FrameCount == 64 ? 8 : 4;
                Session.Flipbook.Rows = Session.Flipbook.FrameCount / Session.Flipbook.Columns;
                Session.Flipbook.FrameHeight = Session.Flipbook.FrameWidth;
                Session.Flipbook.DurationSec = static_cast<float>(Session.Flipbook.FrameCount) /
                                               static_cast<float>(std::max(1, Session.Flipbook.FramesPerSecond));
                const size_t SheetPixels = static_cast<size_t>(Session.Flipbook.Columns * Session.Flipbook.FrameWidth) *
                                           static_cast<size_t>(Session.Flipbook.Rows * Session.Flipbook.FrameHeight) * 4u;
                Session.Flipbook.SheetRgba.assign(SheetPixels, 0u);
                Session.Flipbook.Captured = 0;
                Session.Flipbook.RestartIssued = false;
                Session.Flipbook.BurstIssued = false;
                Session.Flipbook.WarmupRemaining = Session.Flipbook.WarmupSeconds;
                Session.Flipbook.Recording = true;
                Session.Flipbook.LastMessage.clear();
            }
            ImGui::SameLine(0.0f, 10.0f * Scale);
            if (ImGui::Button("Cancel##CloseFlipbook", ImVec2(100.0f * Scale, 36.0f * Scale)))
            {
                Session.Flipbook.DialogOpen = false;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!Session.Flipbook.LastMessage.empty())
        {
            ImGui::Separator();
            ImGui::TextWrapped("%s", Session.Flipbook.LastMessage.c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (Session.ShowImportModal)
    {
        ImGui::OpenPopup("Import Fluid Scene");
    }
    if (ImGui::BeginPopupModal("Import Fluid Scene", &Session.ShowImportModal, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
        ImGui::TextUnformatted("Path to .fluidspace or .gasscene.toml:");
        ImGui::SetNextItemWidth(380.0f);
        ImGui::InputText("##ImportFluidPath", Session.ImportPathField, sizeof(Session.ImportPathField));
        if (ImGui::Button("Load File", ImVec2(120.0f, 26.0f)))
        {
            FluidDynamics::FluidSceneDocument Loaded{};
            std::string                       ErrorMsg;
            if (FluidDynamics::LoadFluidSpaceFile(Session.ImportPathField, Loaded, &ErrorMsg))
            {
                FluidDocumentTab& ActiveDoc = Session.Tabs[Session.ActiveTab];
                PushUndoSnapshot(ActiveDoc);
                ActiveDoc.Scene          = std::move(Loaded);
                ActiveDoc.RestartPending = true;
                Session.StatusNotice     = std::string("Imported ") + Session.ImportPathField;
                Session.ShowImportModal  = false;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                Session.StatusNotice = "Import failed: " + ErrorMsg;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 26.0f)))
        {
            Session.ShowImportModal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (ClosingModal)
    {
        FluidDocumentTab& FinalDoc = Session.Tabs[std::clamp(Session.ActiveTab, 0, static_cast<int>(Session.Tabs.size()) - 1)];
        SyncDocumentToRecord(FinalDoc, Token, Config, true);
        ImGui::CloseCurrentPopup();
        Runtime.CloseActiveEditor();
    }
    ImGui::EndPopup();
    ControlKit::ConfigurePalette(PreviousControlPalette);
}

#elif FRONTIER_EDITOR

void FluidPlumeEditor::ConstructInterface(RecordToken, FluidPlumeInspector&) {}

#endif

} // namespace Frontier
