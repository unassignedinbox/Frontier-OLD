/*==========================================================================================================================================
                                                     FLUIDSIMULATIONSEQUENCE.H
==========================================================================================================================================*/
// 🧩 Vulkan 3D Eulerian gas & combustion solver, volumetric raymarcher, GPU embers, and 2D tiled atlas minimap (Experimental/Fluid 1:1).

#pragma once

#include "DeviceExchange/DeviceExchange.h"
#include "Engine/Interface/FluidEditor/FluidViewportPresentation.h"
#include "Rendering/GlobalIllumination/ShaderInterchange.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Frontier::FluidSimulation
{

class FluidSimulationSequence
{
  public:
    FluidSimulationSequence();
    ~FluidSimulationSequence();

    FluidSimulationSequence(const FluidSimulationSequence&)            = delete;
    FluidSimulationSequence& operator=(const FluidSimulationSequence&) = delete;

    bool Construct(const VulkanDeviceAccess* HostDevice, const std::filesystem::path& ShaderFolder, std::string* OutDiagnostic = nullptr);

    bool Refresh(const FluidViewportRequest& Request, FluidViewportImage& OutImage, std::string* OutDiagnostic = nullptr);

    [[nodiscard]] VkImageView                QueryColorView() const noexcept { return ColorView_ ? ColorView_->Handle : VK_NULL_HANDLE; }
    [[nodiscard]] std::array<uint32_t, 2>    QueryExtent() const noexcept { return Extent_; }
    [[nodiscard]] const std::string&         QueryAdapter() const noexcept { return Adapter_; }
    bool                                     ReadColor(std::vector<uint8_t>& OutRgba);
    void                                     AwaitIdle() noexcept { Exchange_.AwaitIdle(); }

  private:
    struct ShrapnelStreamer
    {
        std::array<float, 3> Position = { 0.5f, 0.25f, 0.5f };
        std::array<float, 3> Velocity = { 0.0f, 0.0f, 0.0f };
        float                Age      = 0.0f;
        float                Lifetime = 1.4f;
    };

    struct DocumentSlotState
    {
        int                                               GridResolution      = 0;
        float                                             ElapsedSeconds      = 0.0f;
        uint64_t                                          StepCount           = 0u;
        float                                             DynamicSurge        = 0.0f;
        float                                             DynamicSurgeTarget  = 0.0f;
        float                                             ShockwaveAge        = 99.0f;
        std::array<float, 3>                              ShockwaveOrigin     = { 0.5f, 0.24f, 0.5f };
        std::array<float, 3>                              PreviousObstaclePos = { 0.5f, 0.26f, 0.5f };
        bool                                              ObstacleValid       = false;
        uint32_t                                          PressureParity      = 0u;
        std::vector<ShrapnelStreamer>                     Shrapnel{};

        std::array<std::shared_ptr<DeviceTexture>, 2>     VelocityTextures{};
        std::array<std::shared_ptr<DeviceTextureView>, 2> VelocityViews{};
        std::array<std::shared_ptr<DeviceTexture>, 2>     DensityTextures{};
        std::array<std::shared_ptr<DeviceTextureView>, 2> DensityViews{};
        std::array<std::shared_ptr<DeviceTexture>, 2>     PressureTextures{};
        std::array<std::shared_ptr<DeviceTextureView>, 2> PressureViews{};
        std::shared_ptr<DeviceTexture>                    CurlTexture{};
        std::shared_ptr<DeviceTextureView>                CurlView{};
        std::shared_ptr<DeviceTexture>                    DivergenceTexture{};
        std::shared_ptr<DeviceTextureView>                DivergenceView{};

        std::shared_ptr<DescriptorGroup>                  InjectGroup{};
        std::shared_ptr<DescriptorGroup>                  AdvectGroup{};
        std::shared_ptr<DescriptorGroup>                  CurlGroup{};
        std::shared_ptr<DescriptorGroup>                  ForcesGroup{};
        std::shared_ptr<DescriptorGroup>                  DivergenceGroup{};
        std::array<std::shared_ptr<DescriptorGroup>, 2>   RelaxGroups{};
        std::array<std::shared_ptr<DescriptorGroup>, 2>   ProjectGroups{};
        std::array<std::shared_ptr<DescriptorGroup>, 2>   RaymarchGroups{};
        std::array<std::shared_ptr<DescriptorGroup>, 2>   OverlayGroups{};
    };

    void EnsureSlotGrid(DocumentSlotState& Slot, int Resolution);
    void ClearSlotGrid(DocumentSlotState& Slot);
    void EnsureColorTarget(uint32_t Width, uint32_t Height);

    DeviceExchange                                         Exchange_{};
    std::unique_ptr<GlobalIllumination::ShaderInterchange> Shaders_{};
    std::string                                            Adapter_{};

    std::shared_ptr<DescriptorLayout>                      ComputeLayout_{};
    std::shared_ptr<DescriptorLayout>                      RaymarchLayout_{};
    std::shared_ptr<DescriptorLayout>                      OverlayLayout_{};

    std::shared_ptr<DeviceProgram>                         InjectProgram_{};
    std::shared_ptr<DeviceProgram>                         AdvectProgram_{};
    std::shared_ptr<DeviceProgram>                         CurlProgram_{};
    std::shared_ptr<DeviceProgram>                         ForcesProgram_{};
    std::shared_ptr<DeviceProgram>                         DivergenceProgram_{};
    std::shared_ptr<DeviceProgram>                         RelaxProgram_{};
    std::shared_ptr<DeviceProgram>                         ProjectProgram_{};
    std::shared_ptr<DeviceProgram>                         RaymarchProgram_{};
    std::shared_ptr<DeviceProgram>                         EmberProgram_{};
    std::shared_ptr<DeviceProgram>                         AtlasProgram_{};

    std::shared_ptr<DeviceSampler>                         LinearClamp_{};
    std::shared_ptr<DeviceSampler>                         NearestClamp_{};
    std::shared_ptr<DeviceRegion>                          SimUniformRegion_{};
    std::shared_ptr<DeviceRegion>                          RaymarchUniformRegion_{};
    std::shared_ptr<DeviceRegion>                          EmberUniformRegion_{};
    std::shared_ptr<DeviceRegion>                          AtlasUniformRegion_{};
    std::shared_ptr<DeviceTexture>                         ScratchVolumeTexture_{};
    std::shared_ptr<DeviceTextureView>                     ScratchVolumeView_{};

    std::shared_ptr<DeviceTexture>                         ColorTexture_{};
    std::shared_ptr<DeviceTextureView>                     ColorView_{};
    std::array<uint32_t, 2>                                Extent_             = { 0u, 0u };
    double                                                 SmoothedRefreshMs_  = 8.0;

    std::array<DocumentSlotState, 4>                       Slots_{};
};

} // namespace Frontier::FluidSimulation
