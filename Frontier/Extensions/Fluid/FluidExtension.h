/*==========================================================================================================================================
                                                             FLUIDEXTENSION.H
==========================================================================================================================================*/
// 🧩 Host bridge between the Fluid editor and FluidSimulationSequence on the editor's Vulkan device (counterpart to ParticleExtension).

#pragma once

#include "Engine/DeviceExchange/VulkanDeviceAccess.h"
#include "Engine/Interface/FluidEditor/FluidViewportPresentation.h"
#include "Engine/Rendering/FluidSimulation/FluidSimulationSequence.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Frontier
{

struct FluidViewportOutcome
{
    VkImageView        View = VK_NULL_HANDLE;
    FluidViewportImage Image{};
};

class FluidExtension
{
  public:
    FluidExtension(std::filesystem::path ShaderRoot, const VulkanDeviceAccess* HostDevice);

    FluidViewportOutcome Render(const FluidViewportRequest& Request);

    [[nodiscard]] FluidSimulation::FluidSimulationSequence* AccessSequence() noexcept { return Sequence_.get(); }

  private:
    std::filesystem::path                                     ShaderRoot_{};
    bool                                                      HostDeviceAttached_ = false;
    VulkanDeviceAccess                                        Device_{};
    std::unique_ptr<FluidSimulation::FluidSimulationSequence> Sequence_{};
    std::string                                               Failure_{};
};

} // namespace Frontier
