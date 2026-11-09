/*==========================================================================================================================================
                                                             PLUMESPACE.H
==========================================================================================================================================*/
// 🧩 World-anchored toroidal Eulerian smoke & combustion lattice on the global illumination device (PlumeSpace.js 1:1).

#pragma once

#include "Engine/PhysicalDynamics/FluidDynamics/PlumeSourceProjection.h"
#include "ShaderInterchange.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Frontier::GlobalIllumination
{

struct PlumeQualityProfile
{
    std::array<uint32_t, 3> Extent           = { 128u, 128u, 32u };
    float                   VoxelSize        = 0.25f;   // [m]
    uint32_t                JacobiIterations = 12u;
    uint32_t                MarchSteps       = 48u;
    uint32_t                ShadowSteps      = 10u;
    float                   ShadowStepScale  = 1.15f;
};

class PlumeSpace
{
  public:
    PlumeSpace(DeviceExchange& Exchange, ShaderInterchange& Shaders);

    void ConstructPrograms(const std::shared_ptr<DeviceRegion>& RefreshRegion,
                           const std::shared_ptr<DeviceRegion>& OccupantRegion,
                           const std::shared_ptr<DescriptorLayout>& DistanceMarchLayout);

    void EnsureLattice(int QualityGrade);

    void ResizeTargets(uint32_t TraceWidth, uint32_t TraceHeight,
                       const std::shared_ptr<DeviceTextureView>& SurfaceDepthView,
                       const std::shared_ptr<DeviceTextureView>& ShadowLayeredView,
                       const std::shared_ptr<DeviceSampler>& ShadowSampler,
                       const std::shared_ptr<DeviceRegion>& ShadowReceiverRegion,
                       const std::shared_ptr<DeviceTextureView>& SkyView,
                       const std::shared_ptr<DeviceSampler>& SkySampler);

    void Reset();

    // Prepares uniform/source writes and returns true when transport + march should execute this refresh.
    bool PrepareRefresh(const FluidDynamics::PlumeSettings& Settings,
                        std::span<const FluidDynamics::PlumeSourceEntry> Sources,
                        const FluidDynamics::PlumeFireLightEntry& FireLight,
                        const std::array<float, 3>& FocusPosition,
                        float DeltaSeconds,
                        double ElapsedSeconds);

    void RecordTransport(CommandSequence& Sequence, const std::shared_ptr<DescriptorGroup>& DistanceMarchGroup);
    void RecordMarch(CommandSequence& Sequence);

    [[nodiscard]] bool Active() const noexcept { return ActiveThisRefresh_; }

    std::shared_ptr<DeviceTexture>     MarchTexture{};
    std::shared_ptr<DeviceTextureView> MarchView{};

  private:
    void ConstructDetailTexture();
    void RebuildDescriptorGroups();
    void RecentreWindow(const std::array<float, 3>& FocusPosition, float FloorHeight) noexcept;

    DeviceExchange&                       Exchange_;
    ShaderInterchange&                    Shaders_;
    std::shared_ptr<DeviceRegion>         RefreshRegion_{};
    std::shared_ptr<DeviceRegion>         OccupantRegion_{};
    std::shared_ptr<DescriptorLayout>     DistanceMarchLayout_{};

    std::shared_ptr<DescriptorLayout>     AdvectLayout_{};
    std::shared_ptr<DescriptorLayout>     VorticityLayout_{};
    std::shared_ptr<DescriptorLayout>     PressureLayout_{};
    std::shared_ptr<DescriptorLayout>     MarchLayout_{};

    std::shared_ptr<DeviceProgram>        AdvectVelocityProgram_{};
    std::shared_ptr<DeviceProgram>        AdvectThermoProgram_{};
    std::shared_ptr<DeviceProgram>        SunTransmittanceProgram_{};
    std::shared_ptr<DeviceProgram>        ResolveCurlProgram_{};
    std::shared_ptr<DeviceProgram>        ConfineVorticityProgram_{};
    std::shared_ptr<DeviceProgram>        ResolveDivergenceProgram_{};
    std::shared_ptr<DeviceProgram>        RelaxPressureProgram_{};
    std::shared_ptr<DeviceProgram>        ProjectVelocityProgram_{};
    std::shared_ptr<DeviceProgram>        MarchProgram_{};

    std::shared_ptr<DeviceSampler>        LinearRepeat_{};
    std::shared_ptr<DeviceTexture>        DetailTexture_{};
    std::shared_ptr<DeviceTextureView>    DetailView_{};
    std::shared_ptr<DeviceRegion>         UniformRegion_{};
    std::shared_ptr<DeviceRegion>         FireLightRegion_{};
    std::shared_ptr<DeviceRegion>         SourceRegion_{};
    std::shared_ptr<DeviceRegion>         CoverageRegion_{};

    std::array<std::shared_ptr<DeviceTexture>, 2>     VelocityTextures_{};
    std::array<std::shared_ptr<DeviceTextureView>, 2> VelocityViews_{};
    std::array<std::shared_ptr<DeviceTexture>, 2>     ThermoTextures_{};
    std::array<std::shared_ptr<DeviceTextureView>, 2> ThermoViews_{};
    std::array<std::shared_ptr<DeviceTexture>, 2>     PressureTextures_{};
    std::array<std::shared_ptr<DeviceTextureView>, 2> PressureViews_{};
    std::shared_ptr<DeviceTexture>                    CurlTexture_{};
    std::shared_ptr<DeviceTextureView>                CurlView_{};
    std::shared_ptr<DeviceTexture>                    DivergenceTexture_{};
    std::shared_ptr<DeviceTextureView>                DivergenceView_{};
    std::shared_ptr<DeviceTexture>                    TransmittanceTexture_{};
    std::shared_ptr<DeviceTextureView>                TransmittanceView_{};

    std::shared_ptr<DeviceTextureView>    SurfaceDepthView_{};
    std::shared_ptr<DeviceTextureView>    ShadowLayeredView_{};
    std::shared_ptr<DeviceSampler>        ShadowSampler_{};
    std::shared_ptr<DeviceRegion>         ShadowReceiverRegion_{};
    std::shared_ptr<DeviceTextureView>    SkyView_{};
    std::shared_ptr<DeviceSampler>        SkySampler_{};

    std::array<std::shared_ptr<DescriptorGroup>, 2> AdvectGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 2> AdvectThermoGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 2> TransmittanceGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 2> CurlGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 2> ConfineGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 2> DivergenceGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 4> RelaxGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 4> ProjectGroups_{};
    std::array<std::shared_ptr<DescriptorGroup>, 2> MarchGroups_{};

    PlumeQualityProfile                   Profile_{};
    int                                   CurrentGrade_       = -1;
    uint32_t                              TraceWidth_         = 0u;
    uint32_t                              TraceHeight_        = 0u;
    uint32_t                              VelocityIndex_      = 0u;
    uint32_t                              ThermoIndex_        = 0u;
    uint32_t                              PressureIndex_      = 0u;
    uint32_t                              StepIndex_          = 0u;
    std::array<int32_t, 3>                OriginCell_         = { 0, 0, 0 };
    std::array<int32_t, 3>                CoarseOrigin_       = { 0, 0, 0 };
    std::array<int32_t, 3>                PreviousOrigin_     = { 0, 0, 0 };
    std::array<int32_t, 3>                PreviousCoarse_     = { 0, 0, 0 };
    bool                                  WindowInitialized_  = false;
    bool                                  ResetPending_       = true;
    bool                                  ActiveThisRefresh_  = false;
    float                                 LingeringSeconds_   = 0.0f;
};

} // namespace Frontier::GlobalIllumination
