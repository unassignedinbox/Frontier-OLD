/*==========================================================================================================================================
                                                            FLUIDEXTENSION.CPP
==========================================================================================================================================*/
// 🧩 Host bridge between the Fluid editor and FluidSimulationSequence on the editor's Vulkan device (counterpart to ParticleExtension).

#include "Frontier/Extensions/Fluid/FluidExtension.h"

namespace Frontier
{

FluidExtension::FluidExtension(std::filesystem::path ShaderRoot, const VulkanDeviceAccess* HostDevice)
    : ShaderRoot_(std::move(ShaderRoot))
{
    if (HostDevice && HostDevice->Device != VK_NULL_HANDLE)
    {
        HostDeviceAttached_ = true;
        Device_             = *HostDevice;
    }
}

FluidViewportOutcome FluidExtension::Render(const FluidViewportRequest& Request)
{
    FluidViewportOutcome Outcome{};
    if (!Failure_.empty())
    {
        Outcome.Image.Failure = Failure_;
        return Outcome;
    }

    if (!Sequence_)
    {
        auto Candidate = std::make_unique<FluidSimulation::FluidSimulationSequence>();
        std::string Diagnostic;
        if (!Candidate->Construct(HostDeviceAttached_ ? &Device_ : nullptr, ShaderRoot_ / "FluidSimulation", &Diagnostic))
        {
            Failure_              = Diagnostic.empty() ? "FluidSimulationSequence bring-up failed." : Diagnostic;
            Outcome.Image.Failure = Failure_;
            return Outcome;
        }
        Sequence_ = std::move(Candidate);
    }

    if (HostDeviceAttached_)
    {
        Sequence_->AwaitIdle();
    }

    std::string Diagnostic;
    if (!Sequence_->Refresh(Request, Outcome.Image, &Diagnostic))
    {
        Outcome.Image.Failure = Diagnostic.empty() ? "FluidSimulationSequence refresh failed." : Diagnostic;
        return Outcome;
    }

    Outcome.View = Sequence_->QueryColorView();
    return Outcome;
}

} // namespace Frontier
