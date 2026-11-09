/*==========================================================================================================================================
                                                    FLUIDSIMULATIONSEQUENCE.CPP
==========================================================================================================================================*/
// 🧩 Vulkan 3D Eulerian gas & combustion solver, volumetric raymarcher, GPU embers, and 2D tiled atlas minimap (Experimental/Fluid 1:1).

#include "FluidSimulationSequence.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Frontier::FluidSimulation
{

namespace
{

struct GpuFluidSimulationConstants
{
    float GridTiming[4];
    float DomainBounds[4];
    float SourceGeometry[4];
    float SourcePayload[4];
    float SourceMotion[4];
    float BrushPosition[4];
    float BrushVelocity[4];
    float BlastGeometry[4];
    float BlastPayload[4];
    float BlastPosition[4];
    float BlastVelocity[4];
    float ObstacleGeometry[4];
    float ObstaclePosition[4];
    float ObstacleVelocity[4];
    float BuoyancyTurbulence[4];
    float WindCombustion[4];
    float ThermalDissipation[4];
};
static_assert(sizeof(GpuFluidSimulationConstants) == 272, "Must match FluidSimulationConstants in FluidLayouts.slang");

struct GpuFluidRaymarchConstants
{
    float    InverseViewProjection[16];
    float    CameraPosition[4];
    float    DomainBounds[4];
    float    SunLight[4];
    float    VolumeShading[4];
    float    ScatteringOptics[4];
    float    GlowShockwave[4];
    float    ShockwaveOrigin[4];
    float    ObstacleGeometry[4];
    float    ObstaclePosition[4];
    uint32_t RenderControls[4];
    uint32_t OverlayControls[4];
    uint32_t ExportControls[4];
};
static_assert(sizeof(GpuFluidRaymarchConstants) == 256, "Must match FluidRaymarchConstants in FluidLayouts.slang");

struct GpuFluidEmberConstants
{
    float    ViewProjection[16];
    float    CameraRightSize[4];
    float    CameraUpIntensity[4];
    float    DomainTime[4];
    float    MotionControls[4];
    uint32_t StyleControls[4];
};
static_assert(sizeof(GpuFluidEmberConstants) == 144, "Must match FluidEmberConstants in FluidLayouts.slang");

struct GpuFluidAtlasConstants
{
    float    PanelRectangle[4];
    uint32_t AtlasControls[4];
};
static_assert(sizeof(GpuFluidAtlasConstants) == 32, "Must match FluidAtlasConstants in FluidLayouts.slang");

} // namespace

FluidSimulationSequence::FluidSimulationSequence()  = default;
FluidSimulationSequence::~FluidSimulationSequence() = default;

bool FluidSimulationSequence::Construct(const VulkanDeviceAccess* HostDevice, const std::filesystem::path& ShaderFolder,
                                        std::string* OutDiagnostic)
{
    const bool Ready = (HostDevice && HostDevice->Device != VK_NULL_HANDLE)
                           ? Exchange_.AttachDevice(*HostDevice, OutDiagnostic)
                           : Exchange_.Initialize("", OutDiagnostic);
    if (!Ready) return false;

    Adapter_ = Exchange_.AdapterDescription();
    Shaders_ = std::make_unique<GlobalIllumination::ShaderInterchange>(Exchange_, ShaderFolder);

    using D = DescriptorCode;
    ComputeLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::Sampler },
            { D::StorageTexture3D, TextureFormat::Rgba16Float },
            { D::StorageTexture3D, TextureFormat::Rgba16Float } },
        "Fluid compute");

    RaymarchLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::Sampler },
            { D::Sampler } },
        "Fluid raymarch");

    OverlayLayout_ = Exchange_.ConstructDescriptorLayout(
        std::vector<DescriptorSlot>{
            { D::Uniform },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::Texture3D },
            { D::Sampler },
            { D::Sampler },
            { D::Uniform } },
        "Fluid overlay");

    InjectProgram_     = Shaders_->ConstructComputeProgram("FluidSimulation", "InjectSources", { ComputeLayout_ });
    AdvectProgram_     = Shaders_->ConstructComputeProgram("FluidSimulation", "AdvectFields", { ComputeLayout_ });
    CurlProgram_       = Shaders_->ConstructComputeProgram("FluidSimulation", "EvaluateCurl", { ComputeLayout_ });
    ForcesProgram_     = Shaders_->ConstructComputeProgram("FluidSimulation", "IntegrateForces", { ComputeLayout_ });
    DivergenceProgram_ = Shaders_->ConstructComputeProgram("FluidSimulation", "EvaluateDivergence", { ComputeLayout_ });
    RelaxProgram_      = Shaders_->ConstructComputeProgram("FluidSimulation", "RelaxPressure", { ComputeLayout_ });
    ProjectProgram_    = Shaders_->ConstructComputeProgram("FluidSimulation", "ProjectGradient", { ComputeLayout_ });

    RasterProgramSpecification RaySpec{};
    RaySpec.Label        = "Fluid volumetric raymarch";
    RaySpec.VertexEntry  = "RaymarchVertex";
    RaySpec.FragmentEntry = "RaymarchFragment";
    RaySpec.Targets      = { ColorTarget{ TextureFormat::Rgba8Unorm, false } };
    RaySpec.Layouts      = { RaymarchLayout_ };
    RaymarchProgram_     = Shaders_->ConstructRasterProgram("FluidRaymarch", RaySpec);

    ColorTarget AlphaBlendTarget{};
    AlphaBlendTarget.Format             = TextureFormat::Rgba8Unorm;
    AlphaBlendTarget.BlendEnabled       = true;
    AlphaBlendTarget.Color.Operation    = BlendOperation::Add;
    AlphaBlendTarget.Color.Source       = BlendFactor::SourceAlpha;
    AlphaBlendTarget.Color.Destination  = BlendFactor::OneMinusSourceAlpha;
    AlphaBlendTarget.Alpha.Operation    = BlendOperation::Add;
    AlphaBlendTarget.Alpha.Source       = BlendFactor::One;
    AlphaBlendTarget.Alpha.Destination  = BlendFactor::OneMinusSourceAlpha;

    RasterProgramSpecification EmberSpec{};
    EmberSpec.Label         = "Fluid ember particles";
    EmberSpec.VertexEntry   = "EmberVertex";
    EmberSpec.FragmentEntry = "EmberFragment";
    EmberSpec.Targets       = { AlphaBlendTarget };
    EmberSpec.Layouts       = { OverlayLayout_ };
    EmberProgram_           = Shaders_->ConstructRasterProgram("FluidOverlayRasterization", EmberSpec);

    RasterProgramSpecification AtlasSpec{};
    AtlasSpec.Label         = "Fluid voxel atlas minimap";
    AtlasSpec.VertexEntry   = "AtlasVertex";
    AtlasSpec.FragmentEntry = "AtlasFragment";
    AtlasSpec.Targets       = { AlphaBlendTarget };
    AtlasSpec.Layouts       = { OverlayLayout_ };
    AtlasProgram_           = Shaders_->ConstructRasterProgram("FluidOverlayRasterization", AtlasSpec);

    if (!InjectProgram_ || !AdvectProgram_ || !CurlProgram_ || !ForcesProgram_ || !DivergenceProgram_ ||
        !RelaxProgram_ || !ProjectProgram_ || !RaymarchProgram_ || !EmberProgram_ || !AtlasProgram_)
    {
        if (OutDiagnostic) *OutDiagnostic = "Failed to construct FluidSimulation programs: " + Shaders_->LastDiagnostic();
        return false;
    }

    SamplerSpecification LinearSpec{};
    LinearSpec.MagFilter    = FilterMode::Linear;
    LinearSpec.MinFilter    = FilterMode::Linear;
    LinearSpec.MipmapFilter = FilterMode::Nearest;
    LinearSpec.AddressU     = AddressMode::ClampToEdge;
    LinearSpec.AddressV     = AddressMode::ClampToEdge;
    LinearSpec.AddressW     = AddressMode::ClampToEdge;
    LinearClamp_            = Exchange_.ConstructSampler(LinearSpec);

    SamplerSpecification NearestSpec = LinearSpec;
    NearestSpec.MagFilter            = FilterMode::Nearest;
    NearestSpec.MinFilter            = FilterMode::Nearest;
    NearestClamp_                    = Exchange_.ConstructSampler(NearestSpec);

    SimUniformRegion_      = Exchange_.ConstructRegion(sizeof(GpuFluidSimulationConstants), "Fluid simulation constants");
    RaymarchUniformRegion_ = Exchange_.ConstructRegion(sizeof(GpuFluidRaymarchConstants), "Fluid raymarch constants");
    EmberUniformRegion_    = Exchange_.ConstructRegion(sizeof(GpuFluidEmberConstants), "Fluid ember constants");
    AtlasUniformRegion_    = Exchange_.ConstructRegion(sizeof(GpuFluidAtlasConstants), "Fluid atlas constants");

    TextureSpecification ScratchSpec{};
    ScratchSpec.Label         = "Fluid scratch storage";
    ScratchSpec.Width         = 4u;
    ScratchSpec.Height        = 4u;
    ScratchSpec.DepthOrLayers = 4u;
    ScratchSpec.Dimension     = TextureDimension::Volumetric;
    ScratchSpec.Format        = TextureFormat::Rgba16Float;
    ScratchSpec.Usage         = TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::CopyTarget;
    ScratchVolumeTexture_     = Exchange_.ConstructTexture(ScratchSpec);
    TextureViewSpecification VolViewSpec{};
    VolViewSpec.Dimension    = ViewDimension::Volumetric;
    VolViewSpec.DimensionSet = true;
    ScratchVolumeView_       = Exchange_.ConstructView(ScratchVolumeTexture_, VolViewSpec);

    return true;
}

void FluidSimulationSequence::EnsureSlotGrid(DocumentSlotState& Slot, int Resolution)
{
    const int Clamped = FluidDynamics::ResolveFluidResolutionOption(Resolution).Resolution;
    if (Slot.GridResolution == Clamped && Slot.VelocityTextures[0] != nullptr)
    {
        return;
    }
    Slot.GridResolution = Clamped;

    auto MakeVolume = [&](const std::string& Label) {
        TextureSpecification Spec{};
        Spec.Label         = Label;
        Spec.Width         = static_cast<uint32_t>(Clamped);
        Spec.Height        = static_cast<uint32_t>(Clamped);
        Spec.DepthOrLayers = static_cast<uint32_t>(Clamped);
        Spec.Dimension     = TextureDimension::Volumetric;
        Spec.Format        = TextureFormat::Rgba16Float;
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
        std::tie(Slot.VelocityTextures[Pair], Slot.VelocityViews[Pair]) = MakeVolume("Fluid velocity");
        std::tie(Slot.DensityTextures[Pair], Slot.DensityViews[Pair])   = MakeVolume("Fluid density");
        std::tie(Slot.PressureTextures[Pair], Slot.PressureViews[Pair]) = MakeVolume("Fluid pressure");
    }
    std::tie(Slot.CurlTexture, Slot.CurlView)             = MakeVolume("Fluid curl");
    std::tie(Slot.DivergenceTexture, Slot.DivergenceView) = MakeVolume("Fluid divergence");

    Slot.InjectGroup = Exchange_.ConstructDescriptorGroup(
        ComputeLayout_,
        { SimUniformRegion_, Slot.VelocityViews[0], Slot.DensityViews[0], ScratchVolumeView_, LinearClamp_,
          Slot.VelocityViews[1], Slot.DensityViews[1] },
        "Fluid inject");

    Slot.AdvectGroup = Exchange_.ConstructDescriptorGroup(
        ComputeLayout_,
        { SimUniformRegion_, Slot.VelocityViews[1], Slot.DensityViews[1], ScratchVolumeView_, LinearClamp_,
          Slot.VelocityViews[0], Slot.DensityViews[0] },
        "Fluid advect");

    Slot.CurlGroup = Exchange_.ConstructDescriptorGroup(
        ComputeLayout_,
        { SimUniformRegion_, Slot.VelocityViews[0], ScratchVolumeView_, ScratchVolumeView_, LinearClamp_,
          Slot.CurlView, ScratchVolumeView_ },
        "Fluid curl");

    Slot.ForcesGroup = Exchange_.ConstructDescriptorGroup(
        ComputeLayout_,
        { SimUniformRegion_, Slot.VelocityViews[0], Slot.DensityViews[0], Slot.CurlView, LinearClamp_,
          Slot.VelocityViews[1], ScratchVolumeView_ },
        "Fluid forces");

    Slot.DivergenceGroup = Exchange_.ConstructDescriptorGroup(
        ComputeLayout_,
        { SimUniformRegion_, Slot.VelocityViews[1], Slot.DensityViews[0], ScratchVolumeView_, LinearClamp_,
          Slot.DivergenceView, ScratchVolumeView_ },
        "Fluid divergence");

    for (uint32_t Parity = 0u; Parity < 2u; ++Parity)
    {
        const uint32_t Next = 1u - Parity;
        Slot.RelaxGroups[Parity] = Exchange_.ConstructDescriptorGroup(
            ComputeLayout_,
            { SimUniformRegion_, Slot.PressureViews[Parity], Slot.DivergenceView, ScratchVolumeView_, LinearClamp_,
              Slot.PressureViews[Next], ScratchVolumeView_ },
            "Fluid relax");

        Slot.ProjectGroups[Parity] = Exchange_.ConstructDescriptorGroup(
            ComputeLayout_,
            { SimUniformRegion_, Slot.VelocityViews[1], Slot.PressureViews[Parity], ScratchVolumeView_, LinearClamp_,
              Slot.VelocityViews[0], ScratchVolumeView_ },
            "Fluid project");

        Slot.RaymarchGroups[Parity] = Exchange_.ConstructDescriptorGroup(
            RaymarchLayout_,
            { RaymarchUniformRegion_, Slot.DensityViews[0], Slot.VelocityViews[0], Slot.PressureViews[Parity], Slot.CurlView,
              LinearClamp_, NearestClamp_ },
            "Fluid raymarch");

        Slot.OverlayGroups[Parity] = Exchange_.ConstructDescriptorGroup(
            OverlayLayout_,
            { EmberUniformRegion_, Slot.DensityViews[0], Slot.VelocityViews[0], Slot.PressureViews[Parity], Slot.CurlView,
              LinearClamp_, NearestClamp_, AtlasUniformRegion_ },
            "Fluid overlay");
    }

    ClearSlotGrid(Slot);
}

void FluidSimulationSequence::ClearSlotGrid(DocumentSlotState& Slot)
{
    CommandSequence ClearSeq = Exchange_.BeginSequence();
    for (int Pair = 0; Pair < 2; ++Pair)
    {
        if (Slot.VelocityTextures[Pair]) ClearSeq.ClearTexture(Slot.VelocityTextures[Pair], { 0, 0, 0, 0 });
        if (Slot.DensityTextures[Pair])  ClearSeq.ClearTexture(Slot.DensityTextures[Pair], { 0, 0, 0, 0 });
        if (Slot.PressureTextures[Pair]) ClearSeq.ClearTexture(Slot.PressureTextures[Pair], { 0, 0, 0, 0 });
    }
    if (Slot.CurlTexture)       ClearSeq.ClearTexture(Slot.CurlTexture, { 0, 0, 0, 0 });
    if (Slot.DivergenceTexture) ClearSeq.ClearTexture(Slot.DivergenceTexture, { 0, 0, 0, 0 });
    (void)Exchange_.Submit(ClearSeq);

    Slot.ElapsedSeconds     = 0.0f;
    Slot.StepCount          = 0u;
    Slot.DynamicSurge       = 0.0f;
    Slot.DynamicSurgeTarget = 0.0f;
    Slot.ShockwaveAge       = 99.0f;
    Slot.ObstacleValid      = false;
    Slot.PressureParity     = 0u;
    Slot.Shrapnel.clear();
}

void FluidSimulationSequence::EnsureColorTarget(uint32_t Width, uint32_t Height)
{
    const uint32_t ClampedW = std::clamp(Width, 64u, 2560u);
    const uint32_t ClampedH = std::clamp(Height, 64u, 1600u);
    if (Extent_[0] == ClampedW && Extent_[1] == ClampedH && ColorTexture_ != nullptr)
    {
        return;
    }
    Exchange_.AwaitIdle();
    Extent_ = { ClampedW, ClampedH };
    TextureSpecification Spec{};
    Spec.Label     = "Fluid viewport color";
    Spec.Width     = ClampedW;
    Spec.Height    = ClampedH;
    Spec.Dimension = TextureDimension::Planar;
    Spec.Format    = TextureFormat::Rgba8Unorm;
    Spec.Usage     = TextureUsage::RenderAttachment | TextureUsage::Sampled | TextureUsage::CopySource;
    ColorTexture_  = Exchange_.ConstructTexture(Spec);
    ColorView_     = Exchange_.ConstructView(ColorTexture_);
}

bool FluidSimulationSequence::Refresh(const FluidViewportRequest& Request, FluidViewportImage& OutImage, std::string* OutDiagnostic)
{
    const auto StartTime = std::chrono::steady_clock::now();

    FluidDynamics::FluidParameters Params = Request.Parameters;
    Params.ClampToValidRanges();

    float EffectiveScale     = Request.ExportFlipbook ? 1.0f : Params.RenderScale;
    int   EffectiveRaySteps  = Params.RaymarchSteps;
    if (Params.AutoGpuGovernor && !Request.ExportFlipbook)
    {
        if (SmoothedRefreshMs_ > 22.0)
        {
            EffectiveScale    = std::max(0.45f, EffectiveScale * 0.78f);
            EffectiveRaySteps = std::max(40, static_cast<int>(EffectiveRaySteps * 0.72f));
        }
        else if (SmoothedRefreshMs_ > 15.0)
        {
            EffectiveScale    = std::max(0.55f, EffectiveScale * 0.88f);
            EffectiveRaySteps = std::max(52, static_cast<int>(EffectiveRaySteps * 0.85f));
        }
    }

    const uint32_t TargetW = Request.ExportFlipbook
                                 ? std::max(64u, Request.ViewportWidth)
                                 : std::max(64u, static_cast<uint32_t>(std::lround(Request.ViewportWidth * EffectiveScale)));
    const uint32_t TargetH = Request.ExportFlipbook
                                 ? std::max(64u, Request.ViewportHeight)
                                 : std::max(64u, static_cast<uint32_t>(std::lround(Request.ViewportHeight * EffectiveScale)));
    EnsureColorTarget(TargetW, TargetH);

    DocumentSlotState& Slot = Slots_[std::min<uint32_t>(Request.DocumentSlot, 3u)];
    EnsureSlotGrid(Slot, Params.GridResolution);
    if (Request.Restart)
    {
        ClearSlotGrid(Slot);
    }

    const bool  AdvanceSolver = !Params.Paused || Request.StepOnce;
    const float RawDt         = std::clamp(Request.DeltaSeconds, 1.0f / 240.0f, 1.0f / 20.0f);
    const float SimDt         = AdvanceSolver ? (RawDt * Params.TimeScale) : 0.0f;

    if (AdvanceSolver)
    {
        Slot.ElapsedSeconds += SimDt;
        ++Slot.StepCount;
        Slot.ShockwaveAge += SimDt;
    }

    // Consume queued blast impulse (one primary blast per step; spawn ballistic shrapnel streamers when enabled)
    FluidBlastImpulse ActiveBlast{};
    bool              HasBlast = false;
    if (!Request.Blasts.empty() && AdvanceSolver)
    {
        ActiveBlast          = Request.Blasts.front();
        HasBlast             = true;
        Slot.ShockwaveAge    = 0.0f;
        Slot.ShockwaveOrigin = ActiveBlast.CentreUvw;
        if (Params.DynamicBounds)
        {
            Slot.DynamicSurgeTarget = std::max(Slot.DynamicSurgeTarget, std::max(0.0f, Params.DynamicBoundsMax - 1.0f));
        }
        if (ActiveBlast.SpawnShrapnel || Params.ShrapnelEnabled)
        {
            for (int Frag = 0; Frag < 6; ++Frag)
            {
                const float Azimuth   = static_cast<float>(Frag) * 1.04719755f + 0.31f + Slot.ElapsedSeconds * 0.7f;
                const float Elevation = 0.45f + 0.22f * static_cast<float>(Frag % 3);
                const float Speed     = 0.72f + 0.14f * static_cast<float>((Frag * 3) % 4);
                ShrapnelStreamer Stream{};
                Stream.Position = ActiveBlast.CentreUvw;
                Stream.Velocity = { std::cos(Elevation) * std::cos(Azimuth) * Speed,
                                    std::sin(Elevation) * Speed,
                                    std::cos(Elevation) * std::sin(Azimuth) * Speed };
                Stream.Lifetime = 1.15f + 0.25f * static_cast<float>(Frag % 3);
                if (Slot.Shrapnel.size() < 12u) Slot.Shrapnel.push_back(Stream);
            }
        }
    }

    // Advance dynamic bounds surge & shrapnel streamers
    if (AdvanceSolver)
    {
        if (Params.DynamicBounds)
        {
            Slot.DynamicSurgeTarget = std::max(0.0f, Slot.DynamicSurgeTarget - SimDt * 0.14f);
            const float SurgeRate   = (Slot.DynamicSurgeTarget > Slot.DynamicSurge) ? 4.2f : 1.1f;
            Slot.DynamicSurge += (Slot.DynamicSurgeTarget - Slot.DynamicSurge) * (1.0f - std::exp(-SurgeRate * SimDt));
        }
        else
        {
            Slot.DynamicSurge       = 0.0f;
            Slot.DynamicSurgeTarget = 0.0f;
        }

        for (auto Iter = Slot.Shrapnel.begin(); Iter != Slot.Shrapnel.end();)
        {
            Iter->Age += SimDt;
            Iter->Velocity[1] -= 1.35f * SimDt;
            for (int Axis = 0; Axis < 3; ++Axis) Iter->Position[Axis] += Iter->Velocity[Axis] * SimDt;
            if (Iter->Age >= Iter->Lifetime || Iter->Position[0] < 0.04f || Iter->Position[0] > 0.96f ||
                Iter->Position[1] < 0.03f || Iter->Position[1] > 0.96f || Iter->Position[2] < 0.04f || Iter->Position[2] > 0.96f)
            {
                Iter = Slot.Shrapnel.erase(Iter);
            }
            else
            {
                ++Iter;
            }
        }
    }

    const float SurgeFactor                = 1.0f + Slot.DynamicSurge;
    const std::array<float, 3> DomainScale = { Params.BoundsWidth * SurgeFactor, Params.BoundsHeight * SurgeFactor, Params.BoundsWidth * SurgeFactor };
    const std::array<float, 3> HalfBox     = { DomainScale[0] * 0.5f, DomainScale[1] * 0.5f, DomainScale[2] * 0.5f };

    const std::array<float, 3> ObstaclePos = FluidDynamics::ComputeAnimatedColliderPosition(Params, Slot.ElapsedSeconds);
    std::array<float, 3> ObstacleVel       = { 0.0f, 0.0f, 0.0f };
    if (Slot.ObstacleValid && SimDt > 1.0e-4f)
    {
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            ObstacleVel[Axis] = std::clamp((ObstaclePos[Axis] - Slot.PreviousObstaclePos[Axis]) / SimDt, -3.0f, 3.0f);
        }
    }
    Slot.PreviousObstaclePos = ObstaclePos;
    Slot.ObstacleValid       = true;

    // Combine user brush with active shrapnel streamer trail
    bool                 BrushActive   = Request.BrushActive;
    std::array<float, 3> BrushPos      = Request.BrushUvw;
    std::array<float, 3> BrushVelocity = Request.BrushVelocity;
    float                BrushRadius   = Request.BrushRadius;
    if (!BrushActive && !Slot.Shrapnel.empty() && AdvanceSolver)
    {
        const auto& Frag = Slot.Shrapnel[Slot.StepCount % Slot.Shrapnel.size()];
        BrushActive      = true;
        BrushPos         = Frag.Position;
        BrushVelocity    = { Frag.Velocity[0] * 4.5f, Frag.Velocity[1] * 4.5f, Frag.Velocity[2] * 4.5f };
        BrushRadius      = 0.045f;
    }

    if (AdvanceSolver)
    {
        GpuFluidSimulationConstants SimUniforms{};
        SimUniforms.GridTiming[0]         = static_cast<float>(Slot.GridResolution);
        SimUniforms.GridTiming[1]         = 1.0f / static_cast<float>(Slot.GridResolution);
        SimUniforms.GridTiming[2]         = SimDt;
        SimUniforms.GridTiming[3]         = Slot.ElapsedSeconds;
        SimUniforms.DomainBounds[0]       = DomainScale[0];
        SimUniforms.DomainBounds[1]       = DomainScale[1];
        SimUniforms.DomainBounds[2]       = DomainScale[2];
        SimUniforms.DomainBounds[3]       = Params.EnclosedBox ? 1.0f : 0.0f;
        SimUniforms.SourceGeometry[0]     = Params.SourceEnabled ? 1.0f : 0.0f;
        SimUniforms.SourceGeometry[1]     = Params.SourceRadius;
        SimUniforms.SourceGeometry[2]     = Params.SourceHeight;
        SimUniforms.SourceGeometry[3]     = Params.SourceRate;
        SimUniforms.SourcePayload[0]      = Params.SourceFuel;
        SimUniforms.SourcePayload[1]      = Params.SourceSmoke;
        SimUniforms.SourcePayload[2]      = Params.SourceTemperature;
        SimUniforms.SourcePayload[3]      = Params.SourceUpwardVelocity;
        SimUniforms.SourceMotion[0]       = Params.SourceSwirl;
        SimUniforms.SourceMotion[1]       = BrushActive ? 1.0f : 0.0f;
        SimUniforms.SourceMotion[2]       = BrushRadius;
        SimUniforms.SourceMotion[3]       = HasBlast ? 1.0f : 0.0f;
        SimUniforms.BrushPosition[0]      = BrushPos[0];
        SimUniforms.BrushPosition[1]      = BrushPos[1];
        SimUniforms.BrushPosition[2]      = BrushPos[2];
        SimUniforms.BrushVelocity[0]      = BrushVelocity[0];
        SimUniforms.BrushVelocity[1]      = BrushVelocity[1];
        SimUniforms.BrushVelocity[2]      = BrushVelocity[2];
        SimUniforms.BlastGeometry[0]      = HasBlast ? ActiveBlast.Radius : Params.BlastRadius;
        SimUniforms.BlastGeometry[1]      = HasBlast ? ActiveBlast.Strength : Params.BlastStrength;
        SimUniforms.BlastGeometry[2]      = HasBlast ? ActiveBlast.Temperature : Params.BlastTemperature;
        SimUniforms.BlastGeometry[3]      = HasBlast ? ActiveBlast.Fuel : Params.BlastFuel;
        SimUniforms.BlastPayload[0]       = HasBlast ? ActiveBlast.Smoke : Params.BlastSmoke;
        SimUniforms.BlastPayload[1]       = static_cast<float>(HasBlast ? ActiveBlast.Lobes : Params.BlastLobes);
        SimUniforms.BlastPayload[2]       = (HasBlast && ActiveBlast.HasDirectionalVel) ? 1.0f : 0.0f;
        SimUniforms.BlastPayload[3]       = Params.MacCormackAdvection ? 1.0f : 0.0f;
        SimUniforms.BlastPosition[0]      = ActiveBlast.CentreUvw[0];
        SimUniforms.BlastPosition[1]      = ActiveBlast.CentreUvw[1];
        SimUniforms.BlastPosition[2]      = ActiveBlast.CentreUvw[2];
        SimUniforms.BlastVelocity[0]      = ActiveBlast.DirectionalVelocity[0];
        SimUniforms.BlastVelocity[1]      = ActiveBlast.DirectionalVelocity[1];
        SimUniforms.BlastVelocity[2]      = ActiveBlast.DirectionalVelocity[2];
        SimUniforms.ObstacleGeometry[0]   = static_cast<float>(Params.ObstacleCategory);
        SimUniforms.ObstacleGeometry[1]   = Params.ObstacleRadius;
        SimUniforms.ObstacleGeometry[2]   = Params.VelocityDamping;
        SimUniforms.ObstacleGeometry[3]   = Params.VorticityConfinement;
        SimUniforms.ObstaclePosition[0]   = ObstaclePos[0];
        SimUniforms.ObstaclePosition[1]   = ObstaclePos[1];
        SimUniforms.ObstaclePosition[2]   = ObstaclePos[2];
        SimUniforms.ObstacleVelocity[0]   = ObstacleVel[0];
        SimUniforms.ObstacleVelocity[1]   = ObstacleVel[1];
        SimUniforms.ObstacleVelocity[2]   = ObstacleVel[2];
        SimUniforms.BuoyancyTurbulence[0] = Params.Buoyancy;
        SimUniforms.BuoyancyTurbulence[1] = Params.SmokeWeight;
        SimUniforms.BuoyancyTurbulence[2] = Params.TurbulenceStrength;
        SimUniforms.BuoyancyTurbulence[3] = Params.TurbulenceScale;
        SimUniforms.WindCombustion[0]     = Params.WindX;
        SimUniforms.WindCombustion[1]     = Params.WindZ;
        SimUniforms.WindCombustion[2]     = Params.BurnRate;
        SimUniforms.WindCombustion[3]     = Params.BurnHeat;
        SimUniforms.ThermalDissipation[0] = Params.SootGeneration;
        SimUniforms.ThermalDissipation[1] = Params.CombustionExpansion;
        SimUniforms.ThermalDissipation[2] = Params.CoolingRate;
        SimUniforms.ThermalDissipation[3] = Params.SmokeDissipation;
        Exchange_.WriteRegion(SimUniformRegion_, 0u, &SimUniforms, sizeof(SimUniforms));
    }

    const float Aspect = static_cast<float>(Extent_[0]) / static_cast<float>(std::max(1u, Extent_[1]));
    const auto  Vp     = Request.Camera.QueryViewProjectionMatrix(Aspect);
    const auto  InvVp  = FluidDynamics::FluidOrbitCamera::InvertMatrix(Vp);
    const auto  Eye    = Request.Camera.QueryEyePosition();
    const auto  View   = Request.Camera.QueryViewMatrix();

    const float AzRad  = Params.SunAzimuth * (3.14159265358979323846f / 180.0f);
    const float ElRad  = Params.SunElevation * (3.14159265358979323846f / 180.0f);
    const std::array<float, 3> SunDir = {
        std::cos(ElRad) * std::sin(AzRad),
        std::sin(ElRad),
        std::cos(ElRad) * std::cos(AzRad)
    };

    float ShockRadius   = 0.0f;
    float ShockStrength = 0.0f;
    if (Slot.ShockwaveAge < 1.35f && Params.ShockwaveStrength > 0.01f)
    {
        const float Progress = Slot.ShockwaveAge / 1.35f;
        ShockRadius          = 0.08f + 1.15f * std::pow(Progress, 0.68f);
        ShockStrength        = Params.ShockwaveStrength * (1.0f - Progress) * (1.0f - Progress);
    }

    GpuFluidRaymarchConstants RayUniforms{};
    std::memcpy(RayUniforms.InverseViewProjection, InvVp.data(), sizeof(RayUniforms.InverseViewProjection));
    RayUniforms.CameraPosition[0]   = Eye[0];
    RayUniforms.CameraPosition[1]   = Eye[1];
    RayUniforms.CameraPosition[2]   = Eye[2];
    RayUniforms.CameraPosition[3]   = Params.Exposure;
    RayUniforms.DomainBounds[0]     = DomainScale[0];
    RayUniforms.DomainBounds[1]     = DomainScale[1];
    RayUniforms.DomainBounds[2]     = DomainScale[2];
    RayUniforms.DomainBounds[3]     = static_cast<float>(Slot.GridResolution);
    RayUniforms.SunLight[0]         = SunDir[0];
    RayUniforms.SunLight[1]         = SunDir[1];
    RayUniforms.SunLight[2]         = SunDir[2];
    RayUniforms.SunLight[3]         = Params.SunIntensity;
    RayUniforms.VolumeShading[0]    = Params.AmbientIntensity;
    RayUniforms.VolumeShading[1]    = Params.FireIntensity;
    RayUniforms.VolumeShading[2]    = Params.DensityExtinction;
    RayUniforms.VolumeShading[3]    = Params.SmokeAlbedo;
    RayUniforms.ScatteringOptics[0] = Params.PhaseAnisotropy;
    RayUniforms.ScatteringOptics[1] = Params.ShadowDensity;
    RayUniforms.ScatteringOptics[2] = Params.InternalScattering;
    RayUniforms.ScatteringOptics[3] = Params.TemperatureScale;
    RayUniforms.GlowShockwave[0]    = Params.BloomIntensity;
    RayUniforms.GlowShockwave[1]    = Params.GodRaysIntensity;
    RayUniforms.GlowShockwave[2]    = ShockStrength;
    RayUniforms.GlowShockwave[3]    = ShockRadius;
    RayUniforms.ShockwaveOrigin[0]  = Slot.ShockwaveOrigin[0];
    RayUniforms.ShockwaveOrigin[1]  = Slot.ShockwaveOrigin[1];
    RayUniforms.ShockwaveOrigin[2]  = Slot.ShockwaveOrigin[2];
    RayUniforms.ShockwaveOrigin[3]  = 0.085f;
    RayUniforms.ObstacleGeometry[0] = static_cast<float>(Params.ObstacleCategory);
    RayUniforms.ObstacleGeometry[1] = Params.ObstacleRadius;
    RayUniforms.ObstacleGeometry[2] = Params.VoxelQuantization;
    RayUniforms.ObstacleGeometry[3] = Params.SlicePosition;
    RayUniforms.ObstaclePosition[0] = ObstaclePos[0];
    RayUniforms.ObstaclePosition[1] = ObstaclePos[1];
    RayUniforms.ObstaclePosition[2] = ObstaclePos[2];
    RayUniforms.RenderControls[0]   = static_cast<uint32_t>(EffectiveRaySteps);
    RayUniforms.RenderControls[1]   = static_cast<uint32_t>(Params.ShadowSteps);
    RayUniforms.RenderControls[2]   = static_cast<uint32_t>(Params.RenderChannel);
    RayUniforms.RenderControls[3]   = static_cast<uint32_t>(Params.ColorPalette);
    RayUniforms.OverlayControls[0]  = static_cast<uint32_t>(Params.SliceAxis);
    RayUniforms.OverlayControls[1]  = Params.ShowFloorGrid ? 1u : 0u;
    RayUniforms.OverlayControls[2]  = Params.ShowVoxelGridLines ? 1u : 0u;
    RayUniforms.OverlayControls[3]  = Params.ShowActiveVoxelCells ? 1u : 0u;
    RayUniforms.ExportControls[0]   = Request.ExportFlipbook ? 1u : 0u;
    RayUniforms.ExportControls[1]   = Extent_[0];
    RayUniforms.ExportControls[2]   = Extent_[1];
    Exchange_.WriteRegion(RaymarchUniformRegion_, 0u, &RayUniforms, sizeof(RayUniforms));

    GpuFluidEmberConstants EmberUniforms{};
    std::memcpy(EmberUniforms.ViewProjection, Vp.data(), sizeof(EmberUniforms.ViewProjection));
    EmberUniforms.CameraRightSize[0]   = View[0];
    EmberUniforms.CameraRightSize[1]   = View[4];
    EmberUniforms.CameraRightSize[2]   = View[8];
    EmberUniforms.CameraRightSize[3]   = Params.EmberSize;
    EmberUniforms.CameraUpIntensity[0] = View[1];
    EmberUniforms.CameraUpIntensity[1] = View[5];
    EmberUniforms.CameraUpIntensity[2] = View[9];
    EmberUniforms.CameraUpIntensity[3] = Params.EmberIntensity;
    EmberUniforms.DomainTime[0]        = DomainScale[0];
    EmberUniforms.DomainTime[1]        = DomainScale[1];
    EmberUniforms.DomainTime[2]        = DomainScale[2];
    EmberUniforms.DomainTime[3]        = Slot.ElapsedSeconds;
    EmberUniforms.MotionControls[0]    = Params.WindX;
    EmberUniforms.MotionControls[1]    = Params.WindZ;
    EmberUniforms.MotionControls[2]    = Params.EmberLifetime;
    EmberUniforms.MotionControls[3]    = Params.EmberAshiness;
    EmberUniforms.StyleControls[0]     = static_cast<uint32_t>(Params.EmberCount);
    EmberUniforms.StyleControls[1]     = static_cast<uint32_t>(Params.ColorPalette);
    EmberUniforms.StyleControls[2]     = static_cast<uint32_t>(Slot.GridResolution);
    EmberUniforms.StyleControls[3]     = static_cast<uint32_t>(std::lround(Params.Exposure * 1000.0f));
    Exchange_.WriteRegion(EmberUniformRegion_, 0u, &EmberUniforms, sizeof(EmberUniforms));

    const auto ResOption = FluidDynamics::ResolveFluidResolutionOption(Slot.GridResolution);
    GpuFluidAtlasConstants AtlasUniforms{};
    AtlasUniforms.PanelRectangle[0] = -0.95f;
    AtlasUniforms.PanelRectangle[1] = -0.92f;
    AtlasUniforms.PanelRectangle[2] = -0.35f;
    AtlasUniforms.PanelRectangle[3] = -0.32f;
    AtlasUniforms.AtlasControls[0]  = static_cast<uint32_t>(Slot.GridResolution);
    AtlasUniforms.AtlasControls[1]  = static_cast<uint32_t>(Params.AtlasMinimapField);
    AtlasUniforms.AtlasControls[2]  = static_cast<uint32_t>(ResOption.TilesX);
    AtlasUniforms.AtlasControls[3]  = static_cast<uint32_t>(ResOption.TilesY);
    Exchange_.WriteRegion(AtlasUniformRegion_, 0u, &AtlasUniforms, sizeof(AtlasUniforms));

    CommandSequence Sequence = Exchange_.BeginSequence();
    if (AdvanceSolver)
    {
        const uint32_t Groups = static_cast<uint32_t>((Slot.GridResolution + 3) / 4);

        Sequence.BindProgram(InjectProgram_);
        Sequence.BindGroup(0u, Slot.InjectGroup);
        Sequence.Dispatch(Groups, Groups, Groups);

        Sequence.BindProgram(AdvectProgram_);
        Sequence.BindGroup(0u, Slot.AdvectGroup);
        Sequence.Dispatch(Groups, Groups, Groups);

        Sequence.BindProgram(CurlProgram_);
        Sequence.BindGroup(0u, Slot.CurlGroup);
        Sequence.Dispatch(Groups, Groups, Groups);

        Sequence.BindProgram(ForcesProgram_);
        Sequence.BindGroup(0u, Slot.ForcesGroup);
        Sequence.Dispatch(Groups, Groups, Groups);

        Sequence.BindProgram(DivergenceProgram_);
        Sequence.BindGroup(0u, Slot.DivergenceGroup);
        Sequence.Dispatch(Groups, Groups, Groups);

        Sequence.BindProgram(RelaxProgram_);
        for (int Iter = 0; Iter < Params.PressureIterations; ++Iter)
        {
            Sequence.BindGroup(0u, Slot.RelaxGroups[Slot.PressureParity]);
            Sequence.Dispatch(Groups, Groups, Groups);
            Slot.PressureParity = 1u - Slot.PressureParity;
        }

        Sequence.BindProgram(ProjectProgram_);
        Sequence.BindGroup(0u, Slot.ProjectGroups[Slot.PressureParity]);
        Sequence.Dispatch(Groups, Groups, Groups);
    }

    const ColorAttachment Attachment{ ColorView_, LoadOperation::Clear, { 0.0f, 0.0f, 0.0f, 0.0f } };
    Sequence.BeginRaster(std::span<const ColorAttachment>(&Attachment, 1u), nullptr, "Fluid raymarch");
    Sequence.BindProgram(RaymarchProgram_);
    Sequence.BindGroup(0u, Slot.RaymarchGroups[Slot.PressureParity]);
    Sequence.Draw(3u);

    if (Params.ShowEmbers && !Request.ExportFlipbook && Params.RenderChannel <= 1)
    {
        Sequence.BindProgram(EmberProgram_);
        Sequence.BindGroup(0u, Slot.OverlayGroups[Slot.PressureParity]);
        Sequence.Draw(static_cast<uint32_t>(Params.EmberCount) * 6u);
    }

    if (Params.ShowAtlasMinimap && !Request.ExportFlipbook)
    {
        Sequence.BindProgram(AtlasProgram_);
        Sequence.BindGroup(0u, Slot.OverlayGroups[Slot.PressureParity]);
        Sequence.Draw(6u);
    }
    Sequence.EndRaster();

    if (!Exchange_.Submit(Sequence, OutDiagnostic))
    {
        return false;
    }

    const double ElapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count();
    SmoothedRefreshMs_     = SmoothedRefreshMs_ * 0.85 + ElapsedMs * 0.15;

    OutImage.Extent               = Extent_;
    OutImage.Adapter              = Adapter_;
    OutImage.RefreshMilliseconds  = ElapsedMs;
    OutImage.ElapsedSeconds       = Slot.ElapsedSeconds;
    OutImage.StepCount            = Slot.StepCount;
    OutImage.EffectiveRaySteps    = static_cast<uint32_t>(EffectiveRaySteps);
    OutImage.EffectiveRenderScale = EffectiveScale;
    OutImage.DynamicScale         = DomainScale;

    if (Request.ExportFlipbook)
    {
        ReadColor(OutImage.ReadbackRgba);
    }
    return true;
}

bool FluidSimulationSequence::ReadColor(std::vector<uint8_t>& OutRgba)
{
    if (!ColorTexture_) return false;
    return Exchange_.ReadTexture(ColorTexture_, 0u, 0u, OutRgba);
}

} // namespace Frontier::FluidSimulation
