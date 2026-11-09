/*==========================================================================================================================================
                                                            PLUMESPACE.CPP
==========================================================================================================================================*/
// 🧩 World-anchored toroidal Eulerian smoke & combustion lattice on the global illumination device (PlumeSpace.js 1:1).

#include "PlumeSpace.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Frontier::GlobalIllumination
{

namespace
{

constexpr std::array<PlumeQualityProfile, 4> QualityProfiles = { {
    { { 64u, 64u, 20u },  0.40f,  8u, 32u,  6u, 1.35f }, // Low
    { { 96u, 96u, 24u },  0.30f, 10u, 40u,  8u, 1.25f }, // Medium
    { { 128u, 128u, 32u }, 0.25f, 12u, 48u, 10u, 1.15f }, // High (default)
    { { 160u, 160u, 48u }, 0.20f, 14u, 60u, 12u, 1.05f }, // Ultra
} };

struct GpuPlumeConstants
{
    int32_t  OriginCell[4];
    uint32_t Extent[4];
    int32_t  CoarseOrigin[4];
    uint32_t CoarseExtent[4];
    int32_t  PreviousOrigin[4];
    int32_t  PreviousCoarse[4];
    float    CellMetrics[4];
    float    WindowMinimum[4];
    float    WindowMaximum[4];
    float    WindVector[4];
    float    BuoyancyTerms[4];
    float    Dissipation[4];
    float    OpticalTerms[4];
    float    ScatteringTerms[4];
    float    FlameTerms[4];
    uint32_t TraceDimensions[4];
};
static_assert(sizeof(GpuPlumeConstants) == 256, "GpuPlumeConstants must match PlumeConstants in PlumeLayouts.slang");

struct GpuPlumeFireLight
{
    float PositionIntensity[4];
    float ColourReach[4];
};
static_assert(sizeof(GpuPlumeFireLight) == 32, "GpuPlumeFireLight must match PlumeFireLight in PlumeLayouts.slang");

float HashPeriodic3(int X, int Y, int Z, int Period, uint32_t Seed) noexcept
{
    const int Wx   = ((X % Period) + Period) % Period;
    const int Wy   = ((Y % Period) + Period) % Period;
    const int Wz   = ((Z % Period) + Period) % Period;
    uint32_t  Bits = static_cast<uint32_t>(Wx) * 374761393u + static_cast<uint32_t>(Wy) * 668265263u +
                     static_cast<uint32_t>(Wz) * 2147483647u + Seed * 1597334677u;
    Bits = (Bits ^ (Bits >> 13u)) * 1274126177u;
    return static_cast<float>(Bits ^ (Bits >> 16u)) * (1.0f / 4294967295.0f);
}

float SamplePeriodicValue(float X, float Y, float Z, int Period, uint32_t Seed) noexcept
{
    const float Sx  = X * static_cast<float>(Period);
    const float Sy  = Y * static_cast<float>(Period);
    const float Sz  = Z * static_cast<float>(Period);
    const int   Ix  = static_cast<int>(std::floor(Sx));
    const int   Iy  = static_cast<int>(std::floor(Sy));
    const int   Iz  = static_cast<int>(std::floor(Sz));
    const float Fx  = Sx - std::floor(Sx);
    const float Fy  = Sy - std::floor(Sy);
    const float Fz  = Sz - std::floor(Sz);
    const float Ux  = Fx * Fx * (3.0f - 2.0f * Fx);
    const float Uy  = Fy * Fy * (3.0f - 2.0f * Fy);
    const float Uz  = Fz * Fz * (3.0f - 2.0f * Fz);
    const float C00 = HashPeriodic3(Ix, Iy, Iz, Period, Seed) * (1.0f - Ux) + HashPeriodic3(Ix + 1, Iy, Iz, Period, Seed) * Ux;
    const float C10 = HashPeriodic3(Ix, Iy + 1, Iz, Period, Seed) * (1.0f - Ux) + HashPeriodic3(Ix + 1, Iy + 1, Iz, Period, Seed) * Ux;
    const float C01 = HashPeriodic3(Ix, Iy, Iz + 1, Period, Seed) * (1.0f - Ux) + HashPeriodic3(Ix + 1, Iy, Iz + 1, Period, Seed) * Ux;
    const float C11 = HashPeriodic3(Ix, Iy + 1, Iz + 1, Period, Seed) * (1.0f - Ux) + HashPeriodic3(Ix + 1, Iy + 1, Iz + 1, Period, Seed) * Ux;
    const float Cy0 = C00 * (1.0f - Uy) + C10 * Uy;
    const float Cy1 = C01 * (1.0f - Uy) + C11 * Uy;
    return Cy0 * (1.0f - Uz) + Cy1 * Uz;
}

float SamplePeriodicWorley(float X, float Y, float Z, int Cells, uint32_t Seed) noexcept
{
    const float Cx = X * static_cast<float>(Cells);
    const float Cy = Y * static_cast<float>(Cells);
    const float Cz = Z * static_cast<float>(Cells);
    const int   Bx = static_cast<int>(std::floor(Cx));
    const int   By = static_cast<int>(std::floor(Cy));
    const int   Bz = static_cast<int>(std::floor(Cz));
    float BestSq   = 10.0f;
    for (int Dz = -1; Dz <= 1; ++Dz)
    {
        for (int Dy = -1; Dy <= 1; ++Dy)
        {
            for (int Dx = -1; Dx <= 1; ++Dx)
            {
                const int   Nx = Bx + Dx;
                const int   Ny = By + Dy;
                const int   Nz = Bz + Dz;
                const float Px = static_cast<float>(Nx) + HashPeriodic3(Nx, Ny, Nz, Cells, Seed + 11u);
                const float Py = static_cast<float>(Ny) + HashPeriodic3(Nx, Ny, Nz, Cells, Seed + 29u);
                const float Pz = static_cast<float>(Nz) + HashPeriodic3(Nx, Ny, Nz, Cells, Seed + 47u);
                const float Ox = Px - Cx;
                const float Oy = Py - Cy;
                const float Oz = Pz - Cz;
                BestSq         = std::min(BestSq, Ox * Ox + Oy * Oy + Oz * Oz);
            }
        }
    }
    return std::clamp(1.0f - std::sqrt(BestSq), 0.0f, 1.0f);
}

} // namespace

PlumeSpace::PlumeSpace(DeviceExchange& Exchange, ShaderInterchange& Shaders) : Exchange_(Exchange), Shaders_(Shaders) {}

void PlumeSpace::ConstructPrograms(const std::shared_ptr<DeviceRegion>& RefreshRegion,
                                   const std::shared_ptr<DeviceRegion>& OccupantRegion,
                                   const std::shared_ptr<DescriptorLayout>& DistanceMarchLayout)
{
    RefreshRegion_       = RefreshRegion;
    OccupantRegion_      = OccupantRegion;
    DistanceMarchLayout_ = DistanceMarchLayout;

    using D = DescriptorCode;
    AdvectLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Uniform },
            { D::StorageRead },
            { D::Texture3D },
            { D::Texture3D },
            { D::Sampler },
            { D::StorageTexture3D, TextureFormat::Rgba16Float },
            { D::StorageTexture3D, TextureFormat::Rgba16Float },
            { D::StorageTexture3D, TextureFormat::R16Float },
            { D::Storage },
            { D::StorageRead } },
        "Plume advection");

    VorticityLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Texture3D },
            { D::Texture3D },
            { D::StorageTexture3D, TextureFormat::Rgba16Float } },
        "Plume vorticity");

    PressureLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::StorageTexture3D, TextureFormat::R32Float },
            { D::StorageTexture3D, TextureFormat::Rgba16Float } },
        "Plume pressure");

    MarchLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Uniform },
            { D::Uniform },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::Sampler },
            { D::TextureDepth2D },
            { D::StorageRead },
            { D::StorageTexture2D, TextureFormat::Rgba16Float },
            { D::TextureDepth2DLayered },
            { D::SamplerComparison },
            { D::Uniform },
            { D::Texture2D },
            { D::Sampler } },
        "Plume march");

    AdvectVelocityProgram_    = Shaders_.ConstructComputeProgram("PlumeAdvection", "AdvectVelocity", { AdvectLayout_, DistanceMarchLayout_ });
    AdvectThermoProgram_      = Shaders_.ConstructComputeProgram("PlumeAdvection", "AdvectThermo", { AdvectLayout_, DistanceMarchLayout_ });
    SunTransmittanceProgram_  = Shaders_.ConstructComputeProgram("PlumeAdvection", "ResolveSunTransmittance", { AdvectLayout_, DistanceMarchLayout_ });
    ResolveCurlProgram_       = Shaders_.ConstructComputeProgram("PlumeVorticity", "ResolveCurl", { VorticityLayout_ });
    ConfineVorticityProgram_  = Shaders_.ConstructComputeProgram("PlumeVorticity", "ConfineVorticity", { VorticityLayout_ });
    ResolveDivergenceProgram_ = Shaders_.ConstructComputeProgram("PlumePressure", "ResolveDivergence", { PressureLayout_ });
    RelaxPressureProgram_     = Shaders_.ConstructComputeProgram("PlumePressure", "RelaxPressure", { PressureLayout_ });
    ProjectVelocityProgram_   = Shaders_.ConstructComputeProgram("PlumePressure", "ProjectVelocity", { PressureLayout_ });
    MarchProgram_             = Shaders_.ConstructComputeProgram("PlumeMarch", "MarchPlume", { MarchLayout_ });

    SamplerSpecification RepeatLinear{};
    RepeatLinear.MagFilter    = FilterMode::Linear;
    RepeatLinear.MinFilter    = FilterMode::Linear;
    RepeatLinear.MipmapFilter = FilterMode::Nearest;
    RepeatLinear.AddressU     = AddressMode::Repeat;
    RepeatLinear.AddressV     = AddressMode::Repeat;
    RepeatLinear.AddressW     = AddressMode::Repeat;
    LinearRepeat_             = Exchange_.ConstructSampler(RepeatLinear);

    UniformRegion_   = Exchange_.ConstructRegion(sizeof(GpuPlumeConstants), "Plume constants");
    FireLightRegion_ = Exchange_.ConstructRegion(sizeof(GpuPlumeFireLight), "Plume fire light");
    SourceRegion_    = Exchange_.ConstructRegion(sizeof(FluidDynamics::PlumeSourceEntry) * FluidDynamics::PlumeSourceProjector::MaximumSources, "Plume sources");
    CoverageRegion_  = Exchange_.ConstructRegion(32u, "Plume coverage bounds");

    ConstructDetailTexture();
    EnsureLattice(2);
}

void PlumeSpace::ConstructDetailTexture()
{
    constexpr uint32_t Edge = 32u;
    TextureSpecification Spec{};
    Spec.Label         = "Plume detail noise";
    Spec.Width         = Edge;
    Spec.Height        = Edge;
    Spec.DepthOrLayers = Edge;
    Spec.Dimension     = TextureDimension::Volumetric;
    Spec.Format        = TextureFormat::Rgba8Unorm;
    Spec.Usage         = TextureUsage::Sampled | TextureUsage::CopyTarget;
    DetailTexture_     = Exchange_.ConstructTexture(Spec);

    TextureViewSpecification ViewSpec{};
    ViewSpec.Dimension    = ViewDimension::Volumetric;
    ViewSpec.DimensionSet = true;
    DetailView_           = Exchange_.ConstructView(DetailTexture_, ViewSpec);

    std::vector<uint8_t> Bytes(Edge * Edge * Edge * 4u);
    for (uint32_t Z = 0; Z < Edge; ++Z)
    {
        for (uint32_t Y = 0; Y < Edge; ++Y)
        {
            for (uint32_t X = 0; X < Edge; ++X)
            {
                const float U   = (static_cast<float>(X) + 0.5f) / static_cast<float>(Edge);
                const float V   = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Edge);
                const float W   = (static_cast<float>(Z) + 0.5f) / static_cast<float>(Edge);
                const float Fbm = 0.58f * SamplePeriodicValue(U, V, W, 4, 1u) +
                                  0.28f * SamplePeriodicValue(U, V, W, 8, 2u) +
                                  0.14f * SamplePeriodicValue(U, V, W, 16, 3u);
                const float WorleyA = SamplePeriodicWorley(U, V, W, 4, 5u);
                const float WorleyB = SamplePeriodicWorley(U, V, W, 8, 9u);
                const float Fine    = SamplePeriodicValue(U, V, W, 16, 13u);
                const size_t Offset = ((static_cast<size_t>(Z) * Edge + Y) * Edge + X) * 4u;
                Bytes[Offset + 0u]  = static_cast<uint8_t>(std::clamp(Fbm * 255.0f, 0.0f, 255.0f));
                Bytes[Offset + 1u]  = static_cast<uint8_t>(std::clamp((WorleyA * 0.65f + WorleyB * 0.35f) * 255.0f, 0.0f, 255.0f));
                Bytes[Offset + 2u]  = static_cast<uint8_t>(std::clamp(Fine * 255.0f, 0.0f, 255.0f));
                Bytes[Offset + 3u]  = 255u;
            }
        }
    }
    Exchange_.WriteTexture(DetailTexture_, 0u, Bytes.data(), Bytes.size(), Edge, Edge, Edge);
}

void PlumeSpace::EnsureLattice(int QualityGrade)
{
    const int ClampedGrade = std::clamp(QualityGrade, 0, 3);
    if (ClampedGrade == CurrentGrade_ && VelocityTextures_[0] != nullptr)
    {
        return;
    }
    CurrentGrade_ = ClampedGrade;
    Profile_      = QualityProfiles[static_cast<size_t>(ClampedGrade)];

    const uint32_t FineX   = Profile_.Extent[0];
    const uint32_t FineY   = Profile_.Extent[1];
    const uint32_t FineZ   = Profile_.Extent[2];
    const uint32_t CoarseX = FineX / 2u;
    const uint32_t CoarseY = FineY / 2u;
    const uint32_t CoarseZ = FineZ / 2u;

    auto MakeVolume = [&](const std::string& Label, uint32_t W, uint32_t H, uint32_t D, TextureFormat Format) {
        TextureSpecification Spec{};
        Spec.Label         = Label;
        Spec.Width         = W;
        Spec.Height        = H;
        Spec.DepthOrLayers = D;
        Spec.Dimension     = TextureDimension::Volumetric;
        Spec.Format        = Format;
        Spec.Usage         = TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::CopyTarget;
        auto Tex           = Exchange_.ConstructTexture(Spec);
        TextureViewSpecification ViewSpec{};
        ViewSpec.Dimension    = ViewDimension::Volumetric;
        ViewSpec.DimensionSet = true;
        auto View          = Exchange_.ConstructView(Tex, ViewSpec);
        return std::pair{ std::move(Tex), std::move(View) };
    };

    for (int Pair = 0; Pair < 2; ++Pair)
    {
        auto [VelTex, VelView]               = MakeVolume("Plume velocity", CoarseX, CoarseY, CoarseZ, TextureFormat::Rgba16Float);
        auto [ThermoTex, ThermoView]         = MakeVolume("Plume thermo", FineX, FineY, FineZ, TextureFormat::Rgba16Float);
        auto [PressTex, PressView]           = MakeVolume("Plume pressure", CoarseX, CoarseY, CoarseZ, TextureFormat::R32Float);
        VelocityTextures_[Pair]              = std::move(VelTex);
        VelocityViews_[Pair]                 = std::move(VelView);
        ThermoTextures_[Pair]                = std::move(ThermoTex);
        ThermoViews_[Pair]                   = std::move(ThermoView);
        PressureTextures_[Pair]              = std::move(PressTex);
        PressureViews_[Pair]                 = std::move(PressView);
    }

    std::tie(CurlTexture_, CurlView_)                   = MakeVolume("Plume curl", CoarseX, CoarseY, CoarseZ, TextureFormat::Rgba16Float);
    std::tie(DivergenceTexture_, DivergenceView_)       = MakeVolume("Plume divergence", CoarseX, CoarseY, CoarseZ, TextureFormat::R32Float);
    std::tie(TransmittanceTexture_, TransmittanceView_) = MakeVolume("Plume sun transmittance", CoarseX, CoarseY, CoarseZ, TextureFormat::R16Float);

    VelocityIndex_     = 0u;
    ThermoIndex_       = 0u;
    PressureIndex_     = 0u;
    WindowInitialized_ = false;
    ResetPending_      = true;
    RebuildDescriptorGroups();
}

void PlumeSpace::ResizeTargets(uint32_t TraceWidth, uint32_t TraceHeight,
                               const std::shared_ptr<DeviceTextureView>& SurfaceDepthView,
                               const std::shared_ptr<DeviceTextureView>& ShadowLayeredView,
                               const std::shared_ptr<DeviceSampler>& ShadowSampler,
                               const std::shared_ptr<DeviceRegion>& ShadowReceiverRegion,
                               const std::shared_ptr<DeviceTextureView>& SkyView,
                               const std::shared_ptr<DeviceSampler>& SkySampler)
{
    SurfaceDepthView_     = SurfaceDepthView;
    ShadowLayeredView_    = ShadowLayeredView;
    ShadowSampler_        = ShadowSampler;
    ShadowReceiverRegion_ = ShadowReceiverRegion;
    SkyView_              = SkyView;
    SkySampler_           = SkySampler;

    if (TraceWidth != TraceWidth_ || TraceHeight != TraceHeight_ || !MarchTexture)
    {
        TraceWidth_  = std::max(1u, TraceWidth);
        TraceHeight_ = std::max(1u, TraceHeight);
        TextureSpecification Spec{};
        Spec.Label     = "Plume march radiance";
        Spec.Width     = TraceWidth_;
        Spec.Height    = TraceHeight_;
        Spec.Dimension = TextureDimension::Planar;
        Spec.Format    = TextureFormat::Rgba16Float;
        Spec.Usage     = TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::CopyTarget;
        MarchTexture   = Exchange_.ConstructTexture(Spec);
        MarchView      = Exchange_.ConstructView(MarchTexture);
    }
    RebuildDescriptorGroups();
}

void PlumeSpace::RebuildDescriptorGroups()
{
    if (!VelocityViews_[0] || !ThermoViews_[0]) return;

    for (uint32_t Index = 0u; Index < 2u; ++Index)
    {
        const uint32_t Next = 1u - Index;
        AdvectGroups_[Index] = Exchange_.ConstructDescriptorGroup(
            AdvectLayout_,
            { RefreshRegion_, UniformRegion_, SourceRegion_, VelocityViews_[Index], ThermoViews_[Index], LinearRepeat_,
              VelocityViews_[Next], ThermoViews_[Next], TransmittanceView_, CoverageRegion_, OccupantRegion_ },
            "Plume advect velocity");

        AdvectThermoGroups_[Index] = Exchange_.ConstructDescriptorGroup(
            AdvectLayout_,
            { RefreshRegion_, UniformRegion_, SourceRegion_, VelocityViews_[Next], ThermoViews_[Index], LinearRepeat_,
              VelocityViews_[Index], ThermoViews_[Next], TransmittanceView_, CoverageRegion_, OccupantRegion_ },
            "Plume advect thermo");

        TransmittanceGroups_[Index] = Exchange_.ConstructDescriptorGroup(
            AdvectLayout_,
            { RefreshRegion_, UniformRegion_, SourceRegion_, VelocityViews_[Index], ThermoViews_[Index], LinearRepeat_,
              VelocityViews_[Next], ThermoViews_[Next], TransmittanceView_, CoverageRegion_, OccupantRegion_ },
            "Plume transmittance");

        CurlGroups_[Index] = Exchange_.ConstructDescriptorGroup(
            VorticityLayout_,
            { UniformRegion_, VelocityViews_[Index], CurlView_, CurlView_ },
            "Plume curl");

        ConfineGroups_[Index] = Exchange_.ConstructDescriptorGroup(
            VorticityLayout_,
            { UniformRegion_, VelocityViews_[Index], CurlView_, VelocityViews_[Next] },
            "Plume confine");

        DivergenceGroups_[Index] = Exchange_.ConstructDescriptorGroup(
            PressureLayout_,
            { UniformRegion_, VelocityViews_[Index], DivergenceView_, PressureViews_[0], DivergenceView_, VelocityViews_[Next] },
            "Plume divergence");

        for (uint32_t PressIdx = 0u; PressIdx < 2u; ++PressIdx)
        {
            const uint32_t PressNext = 1u - PressIdx;
            const uint32_t Slot      = Index * 2u + PressIdx;
            RelaxGroups_[Slot] = Exchange_.ConstructDescriptorGroup(
                PressureLayout_,
                { UniformRegion_, VelocityViews_[Index], DivergenceView_, PressureViews_[PressIdx], PressureViews_[PressNext], VelocityViews_[Next] },
                "Plume relax");
            ProjectGroups_[Slot] = Exchange_.ConstructDescriptorGroup(
                PressureLayout_,
                { UniformRegion_, VelocityViews_[Index], DivergenceView_, PressureViews_[PressIdx], DivergenceView_, VelocityViews_[Next] },
                "Plume project");
        }

        if (MarchView && SurfaceDepthView_ && ShadowLayeredView_ && ShadowSampler_ && ShadowReceiverRegion_ && SkyView_ && SkySampler_)
        {
            MarchGroups_[Index] = Exchange_.ConstructDescriptorGroup(
                MarchLayout_,
                { RefreshRegion_, UniformRegion_, FireLightRegion_, ThermoViews_[Index], TransmittanceView_, DetailView_, LinearRepeat_,
                  SurfaceDepthView_, CoverageRegion_, MarchView, ShadowLayeredView_, ShadowSampler_, ShadowReceiverRegion_, SkyView_, SkySampler_ },
                "Plume march");
        }
    }
}

void PlumeSpace::Reset()
{
    WindowInitialized_ = false;
    ResetPending_      = true;
    LingeringSeconds_  = 0.0f;
    ActiveThisRefresh_ = false;
}

void PlumeSpace::RecentreWindow(const std::array<float, 3>& FocusPosition, float FloorHeight) noexcept
{
    const float   CoarseSize = Profile_.VoxelSize * 2.0f;
    const int32_t CoarseX    = static_cast<int32_t>(Profile_.Extent[0] / 2u);
    const int32_t CoarseY    = static_cast<int32_t>(Profile_.Extent[1] / 2u);

    const int32_t TargetX = static_cast<int32_t>(std::floor(FocusPosition[0] / CoarseSize)) - CoarseX / 2;
    const int32_t TargetY = static_cast<int32_t>(std::floor(FocusPosition[1] / CoarseSize)) - CoarseY / 2;
    const int32_t TargetZ = static_cast<int32_t>(std::floor(FloorHeight / CoarseSize));

    if (!WindowInitialized_)
    {
        CoarseOrigin_      = { TargetX, TargetY, TargetZ };
        OriginCell_        = { TargetX * 2, TargetY * 2, TargetZ * 2 };
        PreviousCoarse_    = CoarseOrigin_;
        PreviousOrigin_    = OriginCell_;
        WindowInitialized_ = true;
        return;
    }

    PreviousCoarse_ = CoarseOrigin_;
    PreviousOrigin_ = OriginCell_;

    if (std::abs(TargetX - CoarseOrigin_[0]) >= 2 || std::abs(TargetY - CoarseOrigin_[1]) >= 2 || TargetZ != CoarseOrigin_[2])
    {
        CoarseOrigin_ = { TargetX, TargetY, TargetZ };
        OriginCell_   = { TargetX * 2, TargetY * 2, TargetZ * 2 };
    }
}

bool PlumeSpace::PrepareRefresh(const FluidDynamics::PlumeSettings& Settings,
                                std::span<const FluidDynamics::PlumeSourceEntry> Sources,
                                const FluidDynamics::PlumeFireLightEntry& FireLight,
                                const std::array<float, 3>& FocusPosition,
                                float DeltaSeconds,
                                double ElapsedSeconds)
{
    EnsureLattice(Settings.QualityGrade);
    if (!Settings.Enabled || !MarchGroups_[0])
    {
        ActiveThisRefresh_ = false;
        return false;
    }

    const float Dt = std::clamp(DeltaSeconds, 1.0f / 240.0f, 0.05f);
    if (!Sources.empty())
    {
        LingeringSeconds_ = std::max(Settings.FadeTime * 1.85f, 12.0f);
    }
    else
    {
        LingeringSeconds_ = std::max(0.0f, LingeringSeconds_ - Dt);
    }

    if (Sources.empty() && LingeringSeconds_ <= 0.0f && !ResetPending_)
    {
        ActiveThisRefresh_ = false;
        return false;
    }

    ActiveThisRefresh_ = true;
    RecentreWindow(FocusPosition, Settings.FloorHeight);

    const uint32_t SourceCount = static_cast<uint32_t>(std::min<size_t>(Sources.size(), FluidDynamics::PlumeSourceProjector::MaximumSources));
    if (SourceCount > 0u)
    {
        Exchange_.WriteRegion(SourceRegion_, 0u, Sources.data(), SourceCount * sizeof(FluidDynamics::PlumeSourceEntry));
    }

    const std::array<uint32_t, 8> InitialCoverage = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u, 0u, 0u, 0u };
    Exchange_.WriteRegion(CoverageRegion_, 0u, InitialCoverage.data(), sizeof(InitialCoverage));

    const float VoxelSize   = Profile_.VoxelSize;
    const float CoarseSize  = VoxelSize * 2.0f;
    const float MinX        = static_cast<float>(OriginCell_[0]) * VoxelSize;
    const float MinY        = static_cast<float>(OriginCell_[1]) * VoxelSize;
    const float MinZ        = static_cast<float>(OriginCell_[2]) * VoxelSize;
    const float MaxX        = MinX + static_cast<float>(Profile_.Extent[0]) * VoxelSize;
    const float MaxY        = MinY + static_cast<float>(Profile_.Extent[1]) * VoxelSize;
    const float MaxZ        = MinZ + static_cast<float>(Profile_.Extent[2]) * VoxelSize;
    const float SmokeFade   = 1.0f / std::max(Settings.FadeTime, 0.35f);

    GpuPlumeConstants Constants{};
    Constants.OriginCell[0]      = OriginCell_[0];
    Constants.OriginCell[1]      = OriginCell_[1];
    Constants.OriginCell[2]      = OriginCell_[2];
    Constants.OriginCell[3]      = static_cast<int32_t>(SourceCount);
    Constants.Extent[0]          = Profile_.Extent[0];
    Constants.Extent[1]          = Profile_.Extent[1];
    Constants.Extent[2]          = Profile_.Extent[2];
    Constants.Extent[3]          = Profile_.JacobiIterations;
    Constants.CoarseOrigin[0]    = CoarseOrigin_[0];
    Constants.CoarseOrigin[1]    = CoarseOrigin_[1];
    Constants.CoarseOrigin[2]    = CoarseOrigin_[2];
    Constants.CoarseOrigin[3]    = ResetPending_ ? 1 : 0;
    Constants.CoarseExtent[0]    = Profile_.Extent[0] / 2u;
    Constants.CoarseExtent[1]    = Profile_.Extent[1] / 2u;
    Constants.CoarseExtent[2]    = Profile_.Extent[2] / 2u;
    Constants.CoarseExtent[3]    = StepIndex_++;
    Constants.PreviousOrigin[0]  = PreviousOrigin_[0];
    Constants.PreviousOrigin[1]  = PreviousOrigin_[1];
    Constants.PreviousOrigin[2]  = PreviousOrigin_[2];
    Constants.PreviousOrigin[3]  = ResetPending_ ? 0 : 1;
    Constants.PreviousCoarse[0]  = PreviousCoarse_[0];
    Constants.PreviousCoarse[1]  = PreviousCoarse_[1];
    Constants.PreviousCoarse[2]  = PreviousCoarse_[2];
    Constants.PreviousCoarse[3]  = static_cast<int32_t>(Profile_.MarchSteps);
    Constants.CellMetrics[0]     = VoxelSize;
    Constants.CellMetrics[1]     = 1.0f / VoxelSize;
    Constants.CellMetrics[2]     = CoarseSize;
    Constants.CellMetrics[3]     = 1.0f / CoarseSize;
    Constants.WindowMinimum[0]   = MinX;
    Constants.WindowMinimum[1]   = MinY;
    Constants.WindowMinimum[2]   = MinZ;
    Constants.WindowMinimum[3]   = Dt;
    Constants.WindowMaximum[0]   = MaxX;
    Constants.WindowMaximum[1]   = MaxY;
    Constants.WindowMaximum[2]   = MaxZ;
    Constants.WindowMaximum[3]   = static_cast<float>(ElapsedSeconds);
    Constants.WindVector[0]      = Settings.WindVector[0];
    Constants.WindVector[1]      = Settings.WindVector[1];
    Constants.WindVector[2]      = Settings.WindVector[2];
    Constants.WindVector[3]      = Settings.VelocityDrag;
    Constants.BuoyancyTerms[0]   = Settings.Buoyancy;
    Constants.BuoyancyTerms[1]   = Settings.GravityLoad;
    Constants.BuoyancyTerms[2]   = Settings.VorticityConfinement;
    Constants.BuoyancyTerms[3]   = Settings.HeatCooling;
    Constants.Dissipation[0]     = SmokeFade;
    Constants.Dissipation[1]     = Settings.FlameBurnRate;
    Constants.Dissipation[2]     = Settings.SwirlScale;
    Constants.Dissipation[3]     = Settings.Extinction;
    Constants.OpticalTerms[0]    = Settings.AlbedoWhite;
    Constants.OpticalTerms[1]    = Settings.AlbedoSoot;
    Constants.OpticalTerms[2]    = Settings.PhaseForward;
    Constants.OpticalTerms[3]    = Settings.PhaseBackward;
    Constants.ScatteringTerms[0] = Settings.PhaseBlend;
    Constants.ScatteringTerms[1] = Settings.MultipleScattering;
    Constants.ScatteringTerms[2] = Profile_.ShadowStepScale;
    Constants.ScatteringTerms[3] = Settings.DetailStrength;
    Constants.FlameTerms[0]      = Settings.FlameRadiance;
    Constants.FlameTerms[1]      = Settings.FlameTemperature;
    Constants.FlameTerms[2]      = Settings.DetailScale;
    Constants.FlameTerms[3]      = Settings.FloorHeight;
    Constants.TraceDimensions[0] = TraceWidth_;
    Constants.TraceDimensions[1] = TraceHeight_;
    Constants.TraceDimensions[2] = Profile_.ShadowSteps;
    Constants.TraceDimensions[3] = static_cast<uint32_t>(CurrentGrade_);
    Exchange_.WriteRegion(UniformRegion_, 0u, &Constants, sizeof(Constants));

    GpuPlumeFireLight GpuFire{};
    GpuFire.PositionIntensity[0] = FireLight.Position[0];
    GpuFire.PositionIntensity[1] = FireLight.Position[1];
    GpuFire.PositionIntensity[2] = FireLight.Position[2];
    GpuFire.PositionIntensity[3] = FireLight.Intensity;
    GpuFire.ColourReach[0]       = FireLight.Colour[0];
    GpuFire.ColourReach[1]       = FireLight.Colour[1];
    GpuFire.ColourReach[2]       = FireLight.Colour[2];
    GpuFire.ColourReach[3]       = FireLight.Reach;
    Exchange_.WriteRegion(FireLightRegion_, 0u, &GpuFire, sizeof(GpuFire));

    ResetPending_ = false;
    return true;
}

void PlumeSpace::RecordTransport(CommandSequence& Sequence, const std::shared_ptr<DescriptorGroup>& DistanceMarchGroup)
{
    if (!ActiveThisRefresh_ || !DistanceMarchGroup)
    {
        return;
    }

    const uint32_t FineGroupsX   = (Profile_.Extent[0] + 3u) / 4u;
    const uint32_t FineGroupsY   = (Profile_.Extent[1] + 3u) / 4u;
    const uint32_t FineGroupsZ   = (Profile_.Extent[2] + 3u) / 4u;
    const uint32_t CoarseGroupsX = (Profile_.Extent[0] / 2u + 3u) / 4u;
    const uint32_t CoarseGroupsY = (Profile_.Extent[1] / 2u + 3u) / 4u;
    const uint32_t CoarseGroupsZ = (Profile_.Extent[2] / 2u + 3u) / 4u;

    // 1. Semi-Lagrangian velocity advection + buoyancy + curl swirl + scene SDF obstacle drag
    Sequence.BindProgram(AdvectVelocityProgram_);
    Sequence.BindGroup(0u, AdvectGroups_[VelocityIndex_]);
    Sequence.BindGroup(1u, DistanceMarchGroup);
    Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);
    VelocityIndex_ = 1u - VelocityIndex_;

    // 2. Vorticity confinement (curl -> confine)
    Sequence.BindProgram(ResolveCurlProgram_);
    Sequence.BindGroup(0u, CurlGroups_[VelocityIndex_]);
    Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);

    Sequence.BindProgram(ConfineVorticityProgram_);
    Sequence.BindGroup(0u, ConfineGroups_[VelocityIndex_]);
    Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);
    VelocityIndex_ = 1u - VelocityIndex_;

    // 3. Divergence + warm-started Jacobi pressure relaxation + divergence-free Helmholtz projection
    Sequence.BindProgram(ResolveDivergenceProgram_);
    Sequence.BindGroup(0u, DivergenceGroups_[VelocityIndex_]);
    Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);

    Sequence.BindProgram(RelaxPressureProgram_);
    for (uint32_t Iter = 0u; Iter < Profile_.JacobiIterations; ++Iter)
    {
        Sequence.BindGroup(0u, RelaxGroups_[VelocityIndex_ * 2u + PressureIndex_]);
        Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);
        PressureIndex_ = 1u - PressureIndex_;
    }

    Sequence.BindProgram(ProjectVelocityProgram_);
    Sequence.BindGroup(0u, ProjectGroups_[VelocityIndex_ * 2u + PressureIndex_]);
    Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);
    VelocityIndex_ = 1u - VelocityIndex_;

    // 4. Fine-lattice thermo advection + combustion + source injection + atomic Coverage AABB
    //    VelocityIndex_ is now (1 - ThermoIndex_), holding the divergence-free projected velocity.
    Sequence.BindProgram(AdvectThermoProgram_);
    Sequence.BindGroup(0u, AdvectThermoGroups_[ThermoIndex_]);
    Sequence.BindGroup(1u, DistanceMarchGroup);
    Sequence.Dispatch(FineGroupsX, FineGroupsY, FineGroupsZ);
    ThermoIndex_ = 1u - ThermoIndex_;

    // 5. Coarse-lattice solar self-shadow transmittance
    Sequence.BindProgram(SunTransmittanceProgram_);
    Sequence.BindGroup(0u, TransmittanceGroups_[ThermoIndex_]);
    Sequence.BindGroup(1u, DistanceMarchGroup);
    Sequence.Dispatch(CoarseGroupsX, CoarseGroupsY, CoarseGroupsZ);
}

void PlumeSpace::RecordMarch(CommandSequence& Sequence)
{
    if (!MarchTexture) return;
    if (!ActiveThisRefresh_ || !MarchGroups_[ThermoIndex_])
    {
        Sequence.ClearTexture(MarchTexture, { 0.0f, 0.0f, 0.0f, 1.0f });
        return;
    }

    Sequence.BindProgram(MarchProgram_);
    Sequence.BindGroup(0u, MarchGroups_[ThermoIndex_]);
    Sequence.Dispatch((TraceWidth_ + 7u) / 8u, (TraceHeight_ + 7u) / 8u, 1u);
}

} // namespace Frontier::GlobalIllumination
