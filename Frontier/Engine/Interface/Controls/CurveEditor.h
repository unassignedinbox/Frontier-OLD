/*==========================================================================================================================================
                                                               CURVEEDITOR.H
==========================================================================================================================================*/
// 🧩 A reusable authored-curve canvas: sorted-X knot editing, bounded dragging, sampled insertion and deletion.

#pragma once

#include "Engine/PhysicalDynamics/Vehicle/Drivetrain.h"

#include <cstddef>

namespace Frontier
{

class ControlColumn;

struct CurveEditorState
{
    int SelectedKnot = 0;
    int DraggedKnot = -1;
};

struct CurveEditorOptions
{
    const char* Caption = "Curve";
    const char* AxisX = "X";
    const char* AxisY = "Y";
    float MinimumX = 0.0f;
    float MaximumX = 1.0f;
    float MinimumY = 0.0f;
    float MaximumY = 1.0f;
    float Height = 172.0f;
    size_t MaximumKnots = 20u;
};

class CurveEditor
{
  public:
    // 📝 Draws the canvas and knot controls into an open inspector section. The supplied domain is also enforced while dragging
    //    and importing a knot; X remains strictly increasing and endpoint X values stay fixed.
    [[nodiscard]] static bool Construct(ControlColumn& Page, const char* Id, Vehicle::Curve& Curve,
                                        const CurveEditorOptions& Options, CurveEditorState& State);
};

} // namespace Frontier
