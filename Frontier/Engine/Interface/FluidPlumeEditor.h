/*==========================================================================================================================================
                                                           FLUIDPLUMEEDITOR.H
==========================================================================================================================================*/
// 🧩 Full-window 1:1 Gas & Explosion Fluid Editor (ported from Experimental/Fluid): multi-document tabs, scene outliner, 3D
//    volumetric raymarched viewport with interactive flame/explosion/wind/collider brush, 3-tab inspector, .fluidspace /
//    .gasscene.toml sidecar persistence, and transparent RGBA flipbook sprite-sheet baker.

#pragma once

#include "Engine/Diagnostics/BuildConfiguration.h"
#include "Engine/Scene/RecordToken.h"

namespace Frontier
{

#if FRONTIER_EDITOR

class FluidPlumeInspector;

class FluidPlumeEditor final
{
  public:
    FluidPlumeEditor() = delete;

    static void ConstructInterface(RecordToken Token, FluidPlumeInspector& Inspector);
};

#endif // FRONTIER_EDITOR

} // namespace Frontier
