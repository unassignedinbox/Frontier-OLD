/*==========================================================================================================================================
                                                         FRACTUREEDITOR.H
==========================================================================================================================================*/
// 🧩 Standalone Voronoi convex decomposition authoring editor bound to FractureInspector.

#pragma once

#include "Engine/Diagnostics/BuildConfiguration.h"
#include "Engine/Scene/RecordToken.h"

#include <array>

namespace Frontier
{

#if FRONTIER_EDITOR

struct FractureConfiguration;

class FractureEditor
{
  public:
    static void ConstructInterface(RecordToken Token,
                                   FractureConfiguration& Configuration,
                                   const std::array<double, 3>& HalfExtentsMetres = { 0.5, 0.5, 0.5 });
};

#endif // FRONTIER_EDITOR

} // namespace Frontier
