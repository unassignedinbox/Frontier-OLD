/*==========================================================================================================================================
                                                      IMPACTSPARKGPURECORD.H
==========================================================================================================================================*/
// 🧩 One world-space spark streak: current head/width, prior-frame tail/pixel-width, and cooled linear radiance/shutter.

#pragma once

#include <array>
#include <cstdint>

namespace Frontier::GlobalIllumination
{

inline constexpr uint32_t ImpactSparkCapacity = 1024u;

struct alignas(16) ImpactSparkGpuRecord
{
    std::array<float, 4> HeadWidth{};        // xyz [m] current head, w [m] world-space streak width
    std::array<float, 4> TailPixelWidth{};   // xyz [m] previous head, w [-] bounded pixel-width factor
    std::array<float, 4> RadianceShutter{};  // rgb [linear radiance], w [-] motion-shutter fraction
};

static_assert(sizeof(ImpactSparkGpuRecord) == 48);
static_assert(alignof(ImpactSparkGpuRecord) == 16);

} // namespace Frontier::GlobalIllumination
