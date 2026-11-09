/*==========================================================================================================================================
                                                        FRACTUREEDITOR.CPP
==========================================================================================================================================*/
// 🧩 Dockable Voronoi convex-decomposition authoring window bound to a FractureConfiguration.

#include "Engine/Interface/FractureEditor.h"

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

#include "Engine/Interface/DiagramControls.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/Interface/Inspectors/FractureInspector.h"
#include "Engine/PhysicalDynamics/FractureDynamics/VoronoiFracture.h"
#include "Engine/Scene/EngineConfigurations.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace Frontier
{

namespace
{

using FractureDynamics::Vector3;
using FractureDynamics::VoronoiFractureRequest;
using FractureDynamics::VoronoiFractureResult;

constexpr double IsoX = 0.7071067811865475244;
constexpr double IsoY = 0.4082482904638630164;
constexpr double IsoZ = -0.8164965809277260327;

struct Projection
{
    double PixelsPerMetre = 1.0;
    double CentreU = 0.0;
    double CentreV = 0.0;
    ImVec2 ScreenCentre{};

    [[nodiscard]] ImVec2 Project(const Vector3& Point) const noexcept
    {
        const double U = IsoX * Point[0] - IsoX * Point[1];
        const double V = IsoY * Point[0] + IsoY * Point[1] + IsoZ * Point[2];
        return { ScreenCentre.x + static_cast<float>((U - CentreU) * PixelsPerMetre),
                 ScreenCentre.y + static_cast<float>((V - CentreV) * PixelsPerMetre) };
    }
};

struct FracturePreviewState
{
    uint64_t                 Token = 0;
    Vector3                  ImpactPoint{};
    bool                     HasRequest = false;
    bool                     Pending = false;
    bool                     Valid = false;
    double                   PendingSince = 0.0;
    std::string              Diagnostic;
    VoronoiFractureRequest   LastRequest{};
    VoronoiFractureResult    Result{};
};

FracturePreviewState& AccessPreviewState()
{
    static FracturePreviewState State;
    return State;
}

bool SameVector(const Vector3& A, const Vector3& B) noexcept
{
    return A[0] == B[0] && A[1] == B[1] && A[2] == B[2];
}

bool SameRequest(const VoronoiFractureRequest& A, const VoronoiFractureRequest& B) noexcept
{
    return A.CellCount == B.CellCount && SameVector(A.HalfExtentsMetres, B.HalfExtentsMetres) &&
           SameVector(A.ImpactPointMetres, B.ImpactPointMetres) && A.ImpactRadialBias == B.ImpactRadialBias && A.Seed == B.Seed;
}

Projection ResolveProjection(ImVec2 Minimum, ImVec2 Maximum, const Vector3& HalfExtents, float Scale)
{
    double MinU = std::numeric_limits<double>::infinity();
    double MaxU = -std::numeric_limits<double>::infinity();
    double MinV = std::numeric_limits<double>::infinity();
    double MaxV = -std::numeric_limits<double>::infinity();
    for (uint32_t Corner = 0; Corner < 8; ++Corner)
    {
        const Vector3 Point = { (Corner & 1u) ? HalfExtents[0] : -HalfExtents[0],
                                (Corner & 2u) ? HalfExtents[1] : -HalfExtents[1],
                                (Corner & 4u) ? HalfExtents[2] : -HalfExtents[2] };
        const double U = IsoX * Point[0] - IsoX * Point[1];
        const double V = IsoY * Point[0] + IsoY * Point[1] + IsoZ * Point[2];
        MinU = std::min(MinU, U);
        MaxU = std::max(MaxU, U);
        MinV = std::min(MinV, V);
        MaxV = std::max(MaxV, V);
    }

    const float AvailableWidth  = std::max(1.0f, Maximum.x - Minimum.x - 44.0f * Scale);
    const float AvailableHeight = std::max(1.0f, Maximum.y - Minimum.y - 92.0f * Scale);
    const double WidthScale = AvailableWidth / std::max(1.0e-9, MaxU - MinU);
    const double HeightScale = AvailableHeight / std::max(1.0e-9, MaxV - MinV);
    return { std::min(WidthScale, HeightScale),
             0.5 * (MinU + MaxU),
             0.5 * (MinV + MaxV),
             ImVec2(0.5f * (Minimum.x + Maximum.x), 0.5f * (Minimum.y + Maximum.y) + 8.0f * Scale) };
}

ImU32 ResolveCutColour(uint32_t Token, float Alpha)
{
    const float Hue = static_cast<float>(std::fmod(static_cast<double>(Token) * 0.6180339887498948482, 1.0));
    constexpr float Saturation = 0.62f;
    constexpr float Value = 0.96f;
    const float Chromatic = Value * Saturation;
    const float X = Chromatic * (1.0f - std::abs(std::fmod(Hue * 6.0f, 2.0f) - 1.0f));
    const float M = Value - Chromatic;
    float R = 0.0f, G = 0.0f, B = 0.0f;
    switch (static_cast<int>(Hue * 6.0f) % 6)
    {
    case 0: R = Chromatic; G = X; B = 0.0f; break;
    case 1: R = X; G = Chromatic; B = 0.0f; break;
    case 2: R = 0.0f; G = Chromatic; B = X; break;
    case 3: R = 0.0f; G = X; B = Chromatic; break;
    case 4: R = X; G = 0.0f; B = Chromatic; break;
    default: R = Chromatic; G = 0.0f; B = X; break;
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(R + M, G + M, B + M, std::clamp(Alpha, 0.0f, 1.0f)));
}

void DrawSourceBounds(ImDrawList* DrawList, const Projection& Project, const Vector3& HalfExtents, ImU32 Colour, float Thickness)
{
    for (uint32_t Corner = 0; Corner < 8; ++Corner)
    {
        const Vector3 A = { (Corner & 1u) ? HalfExtents[0] : -HalfExtents[0],
                            (Corner & 2u) ? HalfExtents[1] : -HalfExtents[1],
                            (Corner & 4u) ? HalfExtents[2] : -HalfExtents[2] };
        for (uint32_t Axis = 0; Axis < 3; ++Axis)
        {
            const uint32_t Other = Corner ^ (1u << Axis);
            if (Other <= Corner) continue;
            const Vector3 B = { (Other & 1u) ? HalfExtents[0] : -HalfExtents[0],
                                (Other & 2u) ? HalfExtents[1] : -HalfExtents[1],
                                (Other & 4u) ? HalfExtents[2] : -HalfExtents[2] };
            DrawList->AddLine(Project.Project(A), Project.Project(B), Colour, Thickness);
        }
    }
}

Vector3 ResolveImpactFromPointer(ImVec2 Pointer, const Projection& Project, const Vector3& HalfExtents)
{
    const double U = (Pointer.x - Project.ScreenCentre.x) / Project.PixelsPerMetre + Project.CentreU;
    const double V = (Pointer.y - Project.ScreenCentre.y) / Project.PixelsPerMetre + Project.CentreV;
    const double X = 0.5 * (U / IsoX + V / IsoY);
    const double Y = 0.5 * (V / IsoY - U / IsoX);
    return { std::clamp(X, -HalfExtents[0], HalfExtents[0]), std::clamp(Y, -HalfExtents[1], HalfExtents[1]), 0.0 };
}

void ConstructFracturePreview(ImDrawList* DrawList, ImVec2 Minimum, ImVec2 Maximum,
                              const InspectorPalette& Palette, const EditorTypefaces& Faces,
                              FracturePreviewState& State, float Scale)
{
    const Vector3& HalfExtents = State.HasRequest ? State.LastRequest.HalfExtentsMetres : State.Result.HalfExtentsMetres;
    const ImVec2 Size(std::max(1.0f, Maximum.x - Minimum.x), std::max(1.0f, Maximum.y - Minimum.y));
    const Projection ProjectionMap = ResolveProjection(Minimum, Maximum, HalfExtents, Scale);
    char ButtonId[80];
    std::snprintf(ButtonId, sizeof(ButtonId), "##FractureImpact-%llu", static_cast<unsigned long long>(State.Token));
    ImGui::SetCursorScreenPos(Minimum);
    const bool Pressed = ImGui::InvisibleButton(ButtonId, Size);
    if (Pressed && ProjectionMap.PixelsPerMetre > 0.0)
    {
        State.ImpactPoint = ResolveImpactFromPointer(ImGui::GetIO().MousePos, ProjectionMap, HalfExtents);
        State.Pending = true;
        State.Valid = false;
        State.PendingSince = ImGui::GetTime();
        State.Diagnostic.clear();
    }

    DrawSourceBounds(DrawList, ProjectionMap, HalfExtents, Palette.InsetFieldBorder, 1.15f * Scale);
    if (State.Valid)
    {
        const float CellCount = static_cast<float>(std::max<size_t>(State.Result.Cells.size(), 1u));
        const float LineAlpha = std::clamp(0.78f * std::sqrt(32.0f / CellCount), 0.16f, 0.78f);
        for (const FractureDynamics::ConvexCell& Cell : State.Result.Cells)
        {
            const ImVec2 Site = ProjectionMap.Project(Cell.Site);
            DrawList->AddCircleFilled(Site, 2.1f * Scale, ResolveCutColour(Cell.SiteIndex + 1u, 0.88f), 8);
            for (const FractureDynamics::ConvexFace& Face : Cell.Faces)
            {
                if (Face.CutToken == 0) continue;
                const uint32_t LowerSite = (Face.CutToken - 1u) / State.LastRequest.CellCount;
                if (Cell.SiteIndex != LowerSite) continue;   // each shared interface is drawn once
                const ImU32 Ink = ResolveCutColour(Face.CutToken, LineAlpha);
                for (size_t Edge = 0; Edge < Face.Vertices.size(); ++Edge)
                {
                    const Vector3& A = Cell.Vertices[Face.Vertices[Edge]];
                    const Vector3& B = Cell.Vertices[Face.Vertices[(Edge + 1u) % Face.Vertices.size()]];
                    DrawList->AddLine(ProjectionMap.Project(A), ProjectionMap.Project(B), Ink, 1.05f * Scale);
                }
            }
        }
    }

    const ImVec2 Impact = ProjectionMap.Project(State.ImpactPoint);
    DrawList->AddCircle(Impact, 7.0f * Scale, Palette.WarmAmber, 16, 1.5f * Scale);
    DrawList->AddLine(ImVec2(Impact.x - 10.0f * Scale, Impact.y), ImVec2(Impact.x + 10.0f * Scale, Impact.y), Palette.WarmAmber, 1.0f * Scale);
    DrawList->AddLine(ImVec2(Impact.x, Impact.y - 10.0f * Scale), ImVec2(Impact.x, Impact.y + 10.0f * Scale), Palette.WarmAmber, 1.0f * Scale);

    char Status[192] = {};
    if (State.Pending)
    {
        std::snprintf(Status, sizeof(Status), "UPDATING \xC2\xB7 %u CONVEX CELLS", State.LastRequest.CellCount);
    }
    else if (State.Valid)
    {
        const double RelativeResidual = State.Result.SourceVolumeCubicMetres > 0.0
                                            ? State.Result.VolumeResidualCubicMetres / State.Result.SourceVolumeCubicMetres
                                            : 0.0;
        std::snprintf(Status, sizeof(Status), "%zu CELLS \xC2\xB7 \xCE\xA3V %.5f / %.5f m^3 \xC2\xB7 CLOSURE %+.2g",
                      State.Result.Cells.size(), State.Result.CellVolumeCubicMetres, State.Result.SourceVolumeCubicMetres, RelativeResidual);
    }
    else
    {
        std::snprintf(Status, sizeof(Status), "PREVIEW UNAVAILABLE \xC2\xB7 %.120s", State.Diagnostic.c_str());
    }
    DrawList->AddText(Faces.Monospace, 9.5f * Scale, ImVec2(Minimum.x + 14.0f * Scale, Maximum.y - 19.0f * Scale),
                      State.Valid ? Palette.LiveGreen : State.Pending ? Palette.WarmAmber : Palette.TextMuted, Status);
    DrawList->AddText(Faces.Regular, 9.0f * Scale, ImVec2(Minimum.x + 14.0f * Scale, Minimum.y + 31.0f * Scale),
                      Palette.TextMuted, "CLICK XY TO MOVE THE PREVIEW IMPACT \xC2\xB7 Z-UP ISOMETRIC");
}

void RefreshPreview(RecordToken Token, FractureConfiguration& Configuration,
                    const std::array<double, 3>& HalfExtents, FracturePreviewState& State)
{
    const double Now = ImGui::GetTime();
    if (State.Token != Token.Value)
    {
        State = FracturePreviewState{};
        State.Token = Token.Value;
    }

    VoronoiFractureRequest Request;
    Request.CellCount = std::clamp(Configuration.VoronoiCellCount, 1u, 256u);
    Request.HalfExtentsMetres = { std::max(1.0e-6, HalfExtents[0]), std::max(1.0e-6, HalfExtents[1]), std::max(1.0e-6, HalfExtents[2]) };
    Request.ImpactPointMetres = State.ImpactPoint;
    Request.ImpactRadialBias = std::clamp(static_cast<double>(Configuration.ImpactRadialBias), 0.0, 1.0);
    Request.Seed = Token.Value ^ (static_cast<uint64_t>(Configuration.PrefractureTriggerCount) * 0x9e3779b97f4a7c15ull);

    const bool RequestChanged = !State.HasRequest || !SameRequest(Request, State.LastRequest);
    if (RequestChanged)
    {
        const bool TargetChanged = !State.HasRequest || Request.Seed != State.LastRequest.Seed ||
                                   !SameVector(Request.HalfExtentsMetres, State.LastRequest.HalfExtentsMetres);
        State.LastRequest = Request;
        State.HasRequest = true;
        State.Pending = true;
        State.Valid = false;
        State.PendingSince = Now;
        State.Diagnostic.clear();
        if (TargetChanged) State.PendingSince = Now - 1.0;   // a new target or an explicit recompute builds on this frame
    }

    if (State.Pending && Now - State.PendingSince >= 0.14)
    {
        VoronoiFractureResult Candidate;
        State.Valid = FractureDynamics::BuildVoronoiFracture(State.LastRequest, Candidate, &State.Diagnostic);
        if (State.Valid) State.Result = std::move(Candidate);
        State.Pending = false;
    }
}

} // namespace

void FractureEditor::ConstructInterface(RecordToken Token, FractureConfiguration& Configuration,
                                        const std::array<double, 3>& HalfExtentsMetres)
{
    FracturePreviewState& State = AccessPreviewState();
    RefreshPreview(Token, Configuration, HalfExtentsMetres, State);
    FractureInspector FractureParameters(Configuration);
    const InspectorPalette Palette = InspectorPalette::Resolve();
    const EditorTypefaces Faces = EditorTypefaces::Resolve();
    const float Scale = InspectorRuntime::Instance().ResolveInterfaceScale();

    DiagramControls::ConstructEditorWindowFrame(
        "Fracture editor \xC2\xB7 Voronoi convex cells", "Hammer", Token, FractureParameters,
        [&](ImDrawList* DrawList, ImVec2 Minimum, ImVec2 Maximum) {
            ConstructFracturePreview(DrawList, Minimum, Maximum, Palette, Faces, State, Scale);
        });
}

} // namespace Frontier

#elif FRONTIER_EDITOR

namespace Frontier
{
void FractureEditor::ConstructInterface(RecordToken, FractureConfiguration&, const std::array<double, 3>&) {}
} // namespace Frontier

#endif
