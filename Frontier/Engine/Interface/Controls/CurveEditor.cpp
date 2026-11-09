/*==========================================================================================================================================
                                                              CURVEEDITOR.CPP
==========================================================================================================================================*/
// 🧩 A native inspector control for authored lookup curves.

#include "Engine/Interface/Controls/CurveEditor.h"

#include "Engine/Interface/Controls/ActionRow.h"
#include "Engine/Interface/Controls/ControlColumn.h"
#include "Engine/Interface/Controls/SectionRows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace Frontier
{

bool CurveEditor::Construct(ControlColumn& Page, const char* Id, Vehicle::Curve& Curve, const CurveEditorOptions& Options, CurveEditorState& State)
{
    using namespace SectionRows;
    bool Changed = false;
    const float DomainMinX = std::isfinite(Options.MinimumX) ? Options.MinimumX : 0.0f;
    const float DomainMaxX = std::isfinite(Options.MaximumX) && Options.MaximumX > DomainMinX ? Options.MaximumX : DomainMinX + 1.0f;
    const float DomainMinY = std::isfinite(Options.MinimumY) ? Options.MinimumY : 0.0f;
    const float DomainMaxY = std::isfinite(Options.MaximumY) && Options.MaximumY > DomainMinY ? Options.MaximumY : DomainMinY + 1.0f;
    const size_t MaximumKnots = std::max<size_t>(2u, Options.MaximumKnots);

    // 📝 Keep malformed/empty in-memory curves editable; a pack importer rejects these instead of repairing them.
    if (Curve.X.size() < 2u || Curve.X.size() != Curve.Y.size())
    {
        Curve = Vehicle::Curve{};
        Curve.Add(DomainMinX, std::clamp(DomainMinY, DomainMinY, DomainMaxY));
        Curve.Add(DomainMaxX, std::clamp(DomainMinY, DomainMinY, DomainMaxY));
        State.SelectedKnot = 0;
        State.DraggedKnot = -1;
        Changed = true;
    }
    State.SelectedKnot = std::clamp(State.SelectedKnot, 0, static_cast<int>(Curve.X.size()) - 1);

    const float MinimumX = std::max(DomainMinX, Curve.X.front());
    const float MaximumX = std::min(DomainMaxX, Curve.X.back());
    const float SpanX = std::max(MaximumX - MinimumX, std::max(1.0e-3f, (DomainMaxX - DomainMinX) * 1.0e-4f));
    const float PlotMaximumX = MinimumX + SpanX;

    float DataMinimumY = Curve.Y.front();
    float DataMaximumY = Curve.Y.front();
    for (const float Value : Curve.Y)
    {
        DataMinimumY = std::min(DataMinimumY, Value);
        DataMaximumY = std::max(DataMaximumY, Value);
    }
    const float DataSpanY = std::max(DataMaximumY - DataMinimumY, (DomainMaxY - DomainMinY) * 0.08f);
    const float PaddingY = std::max(DataSpanY * 0.10f, (DomainMaxY - DomainMinY) * 0.01f);
    float PlotMinimumY = std::max(DomainMinY, DataMinimumY - PaddingY);
    float PlotMaximumY = std::min(DomainMaxY, DataMaximumY + PaddingY);
    if (!(PlotMaximumY > PlotMinimumY))
    {
        PlotMinimumY = DomainMinY;
        PlotMaximumY = DomainMaxY;
    }

    const float Scale = Page.QueryScale();
    const float CanvasHeight = std::max(112.0f, Options.Height) * Scale;
    const ImVec2 Origin = Page.Space(std::max(112.0f, Options.Height), 4.0f, 8.0f);
    const float Width = Page.QueryWidth();
    ImDrawList* Draw = ImGui::GetWindowDrawList();
    const ImU32 Panel = IM_COL32(24, 26, 31, 255);
    const ImU32 Rule = IM_COL32(255, 255, 255, 20);
    const ImU32 CurveInk = IM_COL32(236, 174, 78, 255);
    const ImU32 ChosenInk = IM_COL32(92, 220, 160, 255);
    const ImU32 MutedInk = IM_COL32(170, 175, 184, 255);

    Draw->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + CanvasHeight), Panel, 10.0f * Scale);
    Draw->AddRect(Origin, ImVec2(Origin.x + Width, Origin.y + CanvasHeight), Rule, 10.0f * Scale);
    const float Left = Origin.x + 38.0f * Scale;
    const float Right = Origin.x + Width - 18.0f * Scale;
    const float Top = Origin.y + 25.0f * Scale;
    const float Bottom = Origin.y + CanvasHeight - 24.0f * Scale;

    for (int Grid = 1; Grid <= 3; ++Grid)
    {
        const float Y = Bottom - (Bottom - Top) * static_cast<float>(Grid) / 4.0f;
        Draw->AddLine(ImVec2(Left, Y), ImVec2(Right, Y), Rule, 1.0f);
    }
    const auto ToScreen = [&](float X, float Y)
    {
        const float NormX = std::clamp((X - MinimumX) / SpanX, 0.0f, 1.0f);
        const float NormY = std::clamp((Y - PlotMinimumY) / (PlotMaximumY - PlotMinimumY), 0.0f, 1.0f);
        return ImVec2(Left + (Right - Left) * NormX, Bottom - (Bottom - Top) * NormY);
    };

    ImGui::SetCursorScreenPos(Origin);
    const std::string CanvasId = std::string("##CurveCanvas-") + (Id != nullptr ? Id : "Curve");
    ImGui::InvisibleButton(CanvasId.c_str(), ImVec2(Width, CanvasHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool Hovered = ImGui::IsItemHovered();
    const ImVec2 Mouse = ImGui::GetIO().MousePos;

    if (Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        int BestKnot = -1;
        float BestDistanceSquared = (12.0f * Scale) * (12.0f * Scale);
        for (size_t Index = 0; Index < Curve.X.size(); ++Index)
        {
            const ImVec2 Handle = ToScreen(Curve.X[Index], Curve.Y[Index]);
            const float Dx = Mouse.x - Handle.x;
            const float Dy = Mouse.y - Handle.y;
            const float DistanceSquared = Dx * Dx + Dy * Dy;
            if (DistanceSquared <= BestDistanceSquared)
            {
                BestDistanceSquared = DistanceSquared;
                BestKnot = static_cast<int>(Index);
            }
        }
        if (BestKnot >= 0)
        {
            State.SelectedKnot = BestKnot;
            State.DraggedKnot = BestKnot;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) State.DraggedKnot = -1;
    if (Hovered && State.DraggedKnot >= 0 && static_cast<size_t>(State.DraggedKnot) < Curve.X.size() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
    {
        const size_t Index = static_cast<size_t>(State.DraggedKnot);
        const float NormY = std::clamp((Bottom - Mouse.y) / std::max(1.0f, Bottom - Top), 0.0f, 1.0f);
        const float NewY = std::clamp(PlotMinimumY + NormY * (PlotMaximumY - PlotMinimumY), DomainMinY, DomainMaxY);
        bool KnotChanged = std::fabs(NewY - Curve.Y[Index]) > 1.0e-6f;
        Curve.Y[Index] = NewY;

        if (Index > 0u && Index + 1u < Curve.X.size())
        {
            const float NormX = std::clamp((Mouse.x - Left) / std::max(1.0f, Right - Left), 0.0f, 1.0f);
            const float MarginX = std::max((DomainMaxX - DomainMinX) * 1.0e-5f, SpanX * 1.0e-4f);
            const float LowerX = std::max(DomainMinX, Curve.X[Index - 1u] + MarginX);
            const float UpperX = std::min(DomainMaxX, Curve.X[Index + 1u] - MarginX);
            if (UpperX > LowerX)
            {
                const float NewX = std::clamp(MinimumX + NormX * SpanX, LowerX, UpperX);
                KnotChanged = KnotChanged || std::fabs(NewX - Curve.X[Index]) > 1.0e-6f;
                Curve.X[Index] = NewX;
            }
        }
        Changed = Changed || KnotChanged;
    }

    // 📝 Double-click inserts on the existing sampled curve; a new knot never changes the response until it is deliberately dragged.
    if (Hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && Curve.X.size() < MaximumKnots)
    {
        const float NormX = std::clamp((Mouse.x - Left) / std::max(1.0f, Right - Left), 0.01f, 0.99f);
        const float NewX = std::clamp(MinimumX + NormX * SpanX, DomainMinX, DomainMaxX);
        const auto Insert = std::lower_bound(Curve.X.begin(), Curve.X.end(), NewX);
        const size_t Offset = static_cast<size_t>(std::distance(Curve.X.begin(), Insert));
        const float MinimumGap = std::max((DomainMaxX - DomainMinX) * 1.0e-5f, SpanX * 1.0e-4f);
        if (Offset > 0u && Offset < Curve.X.size() && NewX - Curve.X[Offset - 1u] > MinimumGap && Curve.X[Offset] - NewX > MinimumGap)
        {
            const float NewY = std::clamp(Curve.Sample(NewX), DomainMinY, DomainMaxY);
            Curve.X.insert(Curve.X.begin() + static_cast<std::ptrdiff_t>(Offset), NewX);
            Curve.Y.insert(Curve.Y.begin() + static_cast<std::ptrdiff_t>(Offset), NewY);
            State.SelectedKnot = static_cast<int>(Offset);
            Changed = true;
        }
    }
    if (Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && Curve.X.size() > 2u)
    {
        for (size_t Index = 0; Index < Curve.X.size(); ++Index)
        {
            const ImVec2 Handle = ToScreen(Curve.X[Index], Curve.Y[Index]);
            const float Dx = Mouse.x - Handle.x;
            const float Dy = Mouse.y - Handle.y;
            if (Dx * Dx + Dy * Dy <= (12.0f * Scale) * (12.0f * Scale))
            {
                Curve.X.erase(Curve.X.begin() + static_cast<std::ptrdiff_t>(Index));
                Curve.Y.erase(Curve.Y.begin() + static_cast<std::ptrdiff_t>(Index));
                State.SelectedKnot = std::clamp(State.SelectedKnot, 0, static_cast<int>(Curve.X.size()) - 1);
                State.DraggedKnot = -1;
                Changed = true;
                break;
            }
        }
    }

    ImVec2 Previous{};
    for (int Sample = 0; Sample <= 100; ++Sample)
    {
        const float X = MinimumX + SpanX * static_cast<float>(Sample) / 100.0f;
        const ImVec2 Current = ToScreen(X, Curve.Sample(X));
        if (Sample > 0) Draw->AddLine(Previous, Current, CurveInk, 2.1f * Scale);
        Previous = Current;
    }
    for (size_t Index = 0; Index < Curve.X.size(); ++Index)
    {
        const ImVec2 Handle = ToScreen(Curve.X[Index], Curve.Y[Index]);
        const bool Selected = static_cast<int>(Index) == State.SelectedKnot;
        Draw->AddCircleFilled(Handle, (Selected ? 5.5f : 4.0f) * Scale, Selected ? ChosenInk : CurveInk);
        Draw->AddCircle(Handle, (Selected ? 7.0f : 5.0f) * Scale, IM_COL32(16, 18, 22, 240), 0, 1.5f * Scale);
    }

    char Caption[160];
    std::snprintf(Caption, sizeof(Caption), "%s · %s → %s · %zu knots", Options.Caption, Options.AxisX, Options.AxisY, Curve.X.size());
    Draw->AddText(ImVec2(Origin.x + 12.0f * Scale, Origin.y + 5.0f * Scale), MutedInk, Caption);
    std::snprintf(Caption, sizeof(Caption), "%.3g – %.3g %s", MinimumX, PlotMaximumX, Options.AxisX);
    Draw->AddText(ImVec2(Left, Bottom + 4.0f * Scale), MutedInk, Caption);
    std::snprintf(Caption, sizeof(Caption), "%.3g – %.3g %s", PlotMinimumY, PlotMaximumY, Options.AxisY);
    Draw->AddText(ImVec2(Right - ImGui::CalcTextSize(Caption).x, Bottom + 4.0f * Scale), MutedInk, Caption);

    int KnotNumber = State.SelectedKnot + 1;
    if (Count(Page, (std::string(Id) + "Knot").c_str(), "Selected knot", KnotNumber, 1, static_cast<int>(Curve.X.size()), ""))
        State.SelectedKnot = std::clamp(KnotNumber - 1, 0, static_cast<int>(Curve.X.size()) - 1);
    const size_t Knot = static_cast<size_t>(State.SelectedKnot);
    double KnotX = Curve.X[Knot];
    if (Measure(Page, (std::string(Id) + "KnotX").c_str(), "Knot X", KnotX, DomainMinX, DomainMaxX,
                std::max(1.0e-4, static_cast<double>(DomainMaxX - DomainMinX) / 250.0), Options.AxisX))
    {
        if (Knot > 0u && Knot + 1u < Curve.X.size())
        {
            const double MarginX = std::max((DomainMaxX - DomainMinX) * 1.0e-5, 1.0e-5);
            const double LowerX = std::max<double>(DomainMinX, Curve.X[Knot - 1u] + MarginX);
            const double UpperX = std::min<double>(DomainMaxX, Curve.X[Knot + 1u] - MarginX);
            if (UpperX > LowerX)
            {
                const float NewX = static_cast<float>(std::clamp(KnotX, LowerX, UpperX));
                if (std::fabs(NewX - Curve.X[Knot]) > 1.0e-6f)
                {
                    Curve.X[Knot] = NewX;
                    Changed = true;
                }
            }
        }
    }
    double KnotY = Curve.Y[Knot];
    if (Measure(Page, (std::string(Id) + "KnotY").c_str(), "Knot Y", KnotY, DomainMinY, DomainMaxY,
                std::max(1.0e-3, static_cast<double>(DomainMaxY - DomainMinY) / 300.0), Options.AxisY))
    {
        const float NewY = static_cast<float>(std::clamp(KnotY, static_cast<double>(DomainMinY), static_cast<double>(DomainMaxY)));
        if (std::fabs(NewY - Curve.Y[Knot]) > 1.0e-6f)
        {
            Curve.Y[Knot] = NewY;
            Changed = true;
        }
    }

    const std::string InsertActionId = std::string("##") + Id + "Insert";
    const std::string DeleteActionId = std::string("##") + Id + "Delete";
    const std::array<ActionEntry, 2> KnotActions = {{
        { InsertActionId.c_str(), "+ Insert knot after selected", Curve.X.size() < MaximumKnots, nullptr },
        { DeleteActionId.c_str(), "− Remove selected knot", Curve.X.size() > 2u, nullptr },
    }};
    const int KnotAction = ActionRow::Construct(Page, KnotActions, ActionRowLook{ .MarginTop = 8.0f, .MarginBottom = 10.0f });
    if (KnotAction == 0 && Curve.X.size() < MaximumKnots)
    {
        const size_t LeftIndex = Knot + 1u < Curve.X.size() ? Knot : Curve.X.size() - 2u;
        const float MidX = 0.5f * (Curve.X[LeftIndex] + Curve.X[LeftIndex + 1u]);
        const float MidY = Curve.Sample(MidX);
        Curve.X.insert(Curve.X.begin() + static_cast<std::ptrdiff_t>(LeftIndex + 1u), MidX);
        Curve.Y.insert(Curve.Y.begin() + static_cast<std::ptrdiff_t>(LeftIndex + 1u), MidY);
        State.SelectedKnot = static_cast<int>(LeftIndex + 1u);
        Changed = true;
    }
    else if (KnotAction == 1 && Curve.X.size() > 2u)
    {
        Curve.X.erase(Curve.X.begin() + static_cast<std::ptrdiff_t>(Knot));
        Curve.Y.erase(Curve.Y.begin() + static_cast<std::ptrdiff_t>(Knot));
        State.SelectedKnot = std::clamp(State.SelectedKnot, 0, static_cast<int>(Curve.X.size()) - 1);
        State.DraggedKnot = -1;
        Changed = true;
    }
    return Changed;
}

} // namespace Frontier
