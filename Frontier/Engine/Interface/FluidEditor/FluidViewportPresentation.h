/*==========================================================================================================================================
                                                     FLUIDVIEWPORTPRESENTATION.H
==========================================================================================================================================*/
// 🧩 What the Fluid editor asks of the host that owns the GPU, and what it gets back: one simulation/raymarch request in, the
//    presented image, live solver diagnostics, dynamic bounds, and optional flipbook RGBA frames out.

#pragma once

#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Frontier
{

struct FluidBlastImpulse
{
    std::array<float, 3> CentreUvw           = { 0.5f, 0.24f, 0.5f };
    float                Radius              = 0.18f;
    float                Strength            = 9.8f;
    float                Temperature         = 5.4f;
    float                Fuel                = 4.6f;
    float                Smoke               = 2.6f;
    int                  Lobes               = 6;
    bool                 SpawnShrapnel       = false;
    bool                 HasDirectionalVel   = false;
    std::array<float, 3> DirectionalVelocity = { 0.0f, 0.0f, 0.0f };
};

struct FluidViewportRequest
{
    uint32_t                               DocumentSlot    = 0u;
    uint32_t                               ViewportWidth   = 960u;
    uint32_t                               ViewportHeight  = 540u;
    float                                  DeltaSeconds    = 1.0f / 60.0f;
    bool                                   StepOnce        = false;
    bool                                   Restart         = false;
    bool                                   ExportFlipbook  = false;
    FluidDynamics::FluidParameters         Parameters{};
    FluidDynamics::FluidOrbitCamera        Camera{};
    bool                                   BrushActive     = false;
    std::array<float, 3>                   BrushUvw        = { 0.5f, 0.35f, 0.5f };
    std::array<float, 3>                   BrushVelocity   = { 0.0f, 0.0f, 0.0f };
    float                                  BrushRadius     = 0.08f;
    std::vector<FluidBlastImpulse>         Blasts{};
};

struct FluidViewportImage
{
    uint64_t                Texture             = 0u;      // ImTextureID; 0 = no image
    std::array<uint32_t, 2> Extent              = { 0u, 0u };
    std::string             Adapter{};
    std::string             Failure{};
    double                  RefreshMilliseconds = 0.0;
    float                   ElapsedSeconds      = 0.0f;
    uint64_t                StepCount           = 0u;
    uint32_t                EffectiveRaySteps   = 72u;
    float                   EffectiveRenderScale= 0.8f;
    std::array<float, 3>    DynamicScale        = { 1.85f, 2.10f, 1.85f };
    std::vector<uint8_t>    ReadbackRgba{};                // populated when ExportFlipbook is true
};

using FluidViewportPresenter = std::function<FluidViewportImage(const FluidViewportRequest&)>;

} // namespace Frontier
