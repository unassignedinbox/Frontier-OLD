/*==========================================================================================================================================
                                                         DISPLAYPROJECTION.CPP
==========================================================================================================================================*/
// 🧩 Recombination, temporal antialiasing, eye adaptation and the present raster (DisplayProjection.js).

#include "DisplayProjection.h"
#include "ImpactSparkGpuRecord.h"

#include "ResamplingSequence.h"
#include "SurfaceCardDepot.h"
#include "SurfaceProjection.h"

#include <algorithm>

namespace Frontier::GlobalIllumination
{

namespace
{
using D = DescriptorCode;
} // namespace

DisplayProjection::DisplayProjection(DeviceExchange& Exchange, ShaderInterchange& Shaders) : Exchange_(Exchange), Shaders_(Shaders)
{
}

void DisplayProjection::ConstructPrograms(const std::shared_ptr<DeviceRegion>& RefreshRegion, const std::shared_ptr<DeviceRegion>& MaterialRegion,
                                          const std::shared_ptr<DeviceRegion>& OccupantRegion, TextureFormat PresentFormat,
                                          const std::shared_ptr<DeviceRegion>& ImpactSparkRegion)
{
    RefreshRegion_    = RefreshRegion;
    MaterialRegion_   = MaterialRegion;
    OccupantRegion_   = OccupantRegion;
    ImpactSparkRegion_ = ImpactSparkRegion;
    const std::vector<DescriptorSlot> RecombineSlots = {{D::Uniform}, {D::StorageRead}, {D::Texture2D}, {D::TextureDepth2D}, {D::Texture2D}, {D::Texture2D},
                                                        {D::Texture2D}, {D::Texture2D}, {D::Texture2D}, {D::Texture2D},
                                                        {D::StorageTexture2D, TextureFormat::Rgba16Float}, {D::StorageRead}, {D::Texture2D}, {D::Sampler},
                                                        {D::Texture2D}};
    const std::vector<DescriptorSlot> AntialiasSlots = {{D::Uniform}, {D::Texture2D}, {D::Texture2D}, {D::Texture2D}, {D::Sampler},
                                                        {D::StorageTexture2D, TextureFormat::Rgba16Float}};
    const std::vector<DescriptorSlot> ExposureSlots = {{D::Uniform}, {D::Texture2D}, {D::Storage}};
    const std::vector<DescriptorSlot> PresentSlots = {{D::Uniform}, {D::Texture2D}, {D::StorageRead}, {D::Texture2D}, {D::Texture2D}, {D::Sampler},
                                                      {D::Texture2D}, {D::Texture2D}, {D::TextureDepth2D}, {D::Texture2D}, {D::Uniform}, {D::Texture2D}};
    RecombineLayout_ = Exchange_.ConstructDescriptorLayout(RecombineSlots, "Display recombination");
    AntialiasLayout_ = Exchange_.ConstructDescriptorLayout(AntialiasSlots, "Display antialias");
    ExposureLayout_  = Exchange_.ConstructDescriptorLayout(ExposureSlots, "Display exposure");
    PresentLayout_   = Exchange_.ConstructDescriptorLayout(PresentSlots, "Display present");
    const std::vector<DescriptorSlot> ImpactSparkSlots = {{D::Uniform}, {D::Uniform}, {D::StorageRead}};
    ImpactSparkLayout_ = Exchange_.ConstructDescriptorLayout(ImpactSparkSlots, "Impact spark effect");
    RecombineProgram_ = Shaders_.ConstructComputeProgram("DisplayRecombination", "ResolveRecombination", {RecombineLayout_});
    AntialiasProgram_ = Shaders_.ConstructComputeProgram("DisplayAntialias", "IntegrateAntialias", {AntialiasLayout_});
    ExposureProgram_  = Shaders_.ConstructComputeProgram("DisplayExposure", "IntegrateExposure", {ExposureLayout_});
    RasterProgramSpecification Present;
    Present.VertexEntry   = "PresentVertex";
    Present.FragmentEntry = "PresentFragment";
    Present.Targets       = {{PresentFormat}};
    Present.Layouts       = {PresentLayout_};
    PresentProgram_ = Shaders_.ConstructRasterProgram("DisplayPresent", Present);
    RasterProgramSpecification SparkRaster;
    SparkRaster.Label         = "Jolt impact sparks";
    SparkRaster.VertexEntry   = "ImpactSparkVertex";
    SparkRaster.FragmentEntry = "ImpactSparkFragment";
    SparkRaster.Layouts       = {ImpactSparkLayout_};
    SparkRaster.DepthEnabled  = true;
    SparkRaster.DepthFormat   = TextureFormat::Depth32Float;
    SparkRaster.DepthWrite    = false;
    SparkRaster.DepthCompare  = CompareFunction::Greater;
    ColorTarget SparkTarget;
    SparkTarget.Format       = TextureFormat::Rgba16Float;
    SparkTarget.BlendEnabled = true;
    SparkTarget.Color        = {BlendOperation::Add, BlendFactor::One, BlendFactor::One};
    SparkTarget.Alpha        = {BlendOperation::Add, BlendFactor::One, BlendFactor::One};
    SparkTarget.WriteMask    = 0x7u;
    SparkRaster.Targets      = {SparkTarget};
    ImpactSparkProgram_      = Shaders_.ConstructRasterProgram("ImpactSpark", SparkRaster);
    SamplerSpecification Linear;
    Linear.MagFilter = Linear.MinFilter = FilterMode::Linear;
    LinearClamp      = Exchange_.ConstructSampler(Linear);
    AdaptationRegion = Exchange_.ConstructRegion(16, "Eye adaptation");
}

void DisplayProjection::ConstructTargets(std::array<uint32_t, 2> Extent, const SurfaceProjection& Surface, const ResamplingSequence& Resampling,
                                         const VarianceIntegrator& Diffuse, const VarianceIntegrator& Glossy, const SurfaceCardDepot& Cards,
                                         const DisplayNeutralResources& Neutral)
{
    ScreenExtent = Extent;
    DepthView_ = Surface.DepthView;
    auto Construct = [&](const std::string& Label, std::array<uint32_t, 2> Size, TextureFormat Format, uint32_t Usage) {
        TextureSpecification Specification;
        Specification.Label  = Label;
        Specification.Width  = Size[0];
        Specification.Height = Size[1];
        Specification.Format = Format;
        Specification.Usage  = Usage;
        return Exchange_.ConstructTexture(Specification);
    };
    const uint32_t Usage = TextureUsage::Storage | TextureUsage::Sampled;
    RecombinedTexture   = Construct("Recombined radiance", Extent, TextureFormat::Rgba16Float, Usage | TextureUsage::RenderAttachment);
    InspectionTexture   = Construct("Inspection radiance", Extent, TextureFormat::Rgba16Float, Usage);
    ResolvedTextures[0] = Construct("Resolved radiance A", Extent, TextureFormat::Rgba16Float, Usage | TextureUsage::CopySource | TextureUsage::RenderAttachment);
    ResolvedTextures[1] = Construct("Resolved radiance B", Extent, TextureFormat::Rgba16Float, Usage | TextureUsage::CopySource | TextureUsage::RenderAttachment);
    PostTexture         = Construct("Post radiance", Extent, TextureFormat::Rgba16Float, Usage | TextureUsage::RenderAttachment);
    FlareTransmissionTexture_ = Construct("Cabin optical transmission", Extent, TextureFormat::Rgba8Unorm, TextureUsage::Sampled | TextureUsage::RenderAttachment);
    FlareVisibilityTexture_   = Construct("Solar source visibility", {1, 1}, TextureFormat::Rgba16Float, Usage);
    FlareImageTexture_        = Construct("Optical flare image", {std::min(960u, (Extent[0] + 1) / 2), std::min(960u, (Extent[1] + 1) / 2)},
                                          TextureFormat::Rgba16Float, Usage);
    IdleSmokeTexture_         = Construct("Idle plume", {1, 1}, TextureFormat::Rgba16Float, TextureUsage::Sampled);
    RecombinedView    = Exchange_.ConstructView(RecombinedTexture);
    InspectionView    = Exchange_.ConstructView(InspectionTexture);
    ResolvedViews     = {Exchange_.ConstructView(ResolvedTextures[0]), Exchange_.ConstructView(ResolvedTextures[1])};
    PostView          = Exchange_.ConstructView(PostTexture);
    auto FlareVisibility   = Exchange_.ConstructView(FlareVisibilityTexture_);
    auto FlareImage        = Exchange_.ConstructView(FlareImageTexture_);
    auto FlareTransmission = Exchange_.ConstructView(FlareTransmissionTexture_);
    auto SmokeView         = Neutral.Smoke ? Neutral.Smoke : Exchange_.ConstructView(IdleSmokeTexture_);
    if (InspectionLayout_) InspectionGroup_ = Exchange_.ConstructDescriptorGroup(InspectionLayout_, {RefreshRegion_, InspectionView}, "Inspection march");
    RecombineGroup_ = Exchange_.ConstructDescriptorGroup(RecombineLayout_, {RefreshRegion_, MaterialRegion_, Surface.AttributeView, Surface.DepthView,
                                                                            Surface.DirectView, Diffuse.ScreenView, Glossy.ScreenView, Resampling.IrradianceView,
                                                                            InspectionView, Cards.FinalView, RecombinedView, OccupantRegion_, SmokeView, LinearClamp,
                                                                            Resampling.MirrorView},
                                                         "Display recombination");
    for (uint32_t Parity = 0; Parity < 2; ++Parity)
    {
        AntialiasGroups_[Parity] = Exchange_.ConstructDescriptorGroup(AntialiasLayout_, {RefreshRegion_, RecombinedView, ResolvedViews[1 - Parity], Surface.MotionView,
                                                                                         LinearClamp, ResolvedViews[Parity]});
        ExposureGroups_[Parity] = Exchange_.ConstructDescriptorGroup(ExposureLayout_, {RefreshRegion_, ResolvedViews[Parity], AdaptationRegion});
        PresentGroups_[Parity]  = Exchange_.ConstructDescriptorGroup(PresentLayout_, {RefreshRegion_, ResolvedViews[Parity], AdaptationRegion, FlareVisibility,
                                                                                      FlareImage, LinearClamp, Neutral.CloudColour, Neutral.CloudGuide, Surface.DepthView,
                                                                                      FlareTransmission, Neutral.CloudParams, Neutral.Lightning});
        ImpactSparkGroups_[Parity] = Exchange_.ConstructDescriptorGroup(
            ImpactSparkLayout_, {RefreshRegion_, DescriptorResource(ImpactSparkRegion_, 0, 16),
                                 DescriptorResource(ImpactSparkRegion_, 16, static_cast<VkDeviceSize>(ImpactSparkCapacity) * sizeof(ImpactSparkGpuRecord))},
            "Jolt impact particles");
    }
    RecombinedExposureGroup_ = Exchange_.ConstructDescriptorGroup(ExposureLayout_, {RefreshRegion_, RecombinedView, AdaptationRegion});
    SourceAntialiasGroups_   = {};
    AntialiasSource_.reset();
}

// 📝 Recombination, temporal antialiasing and eye adaptation; additive, depth-tested Jolt particle streaks are a separate post-AA layer.
void DisplayProjection::RecordResolve(CommandSequence& Sequence, uint32_t Parity)
{
    const uint32_t GroupsX = (ScreenExtent[0] + 7) / 8, GroupsY = (ScreenExtent[1] + 7) / 8;
    Sequence.BindProgram(RecombineProgram_);
    Sequence.BindGroup(0, RecombineGroup_);
    Sequence.Dispatch(GroupsX, GroupsY);
    Sequence.BindProgram(AntialiasProgram_);
    Sequence.BindGroup(0, AntialiasGroups_[Parity]);
    Sequence.Dispatch(GroupsX, GroupsY);
    Sequence.BindProgram(ExposureProgram_);
    Sequence.BindGroup(0, ExposureGroups_[Parity]);
    Sequence.Dispatch(1);
}

void DisplayProjection::ConstructInspection(const std::shared_ptr<DescriptorLayout>& DistanceLayout, const std::shared_ptr<DescriptorLayout>& SamplingLayout,
                                            const std::shared_ptr<DescriptorLayout>& RayTracingLayout)
{
    const std::vector<DescriptorSlot> InspectionSlots = {{D::Uniform}, {D::StorageTexture2D, TextureFormat::Rgba16Float}};
    InspectionLayout_  = Exchange_.ConstructDescriptorLayout(InspectionSlots, "Inspection march");
    InspectionProgram_ = Shaders_.ConstructComputeProgram("InspectionMarch", "MarchInspection",
                                                          {InspectionLayout_, DistanceLayout, SamplingLayout, RayTracingLayout});
}

void DisplayProjection::RecordInspection(CommandSequence& Sequence, const std::shared_ptr<DescriptorGroup>& DistanceGroup,
                                         const std::shared_ptr<DescriptorGroup>& SamplingGroup, const std::shared_ptr<DescriptorGroup>& RayTracingGroup)
{
    if (!InspectionProgram_ || !InspectionGroup_) return;
    Sequence.BindProgram(InspectionProgram_);
    Sequence.BindGroup(0, InspectionGroup_);
    Sequence.BindGroup(1, DistanceGroup);
    Sequence.BindGroup(2, SamplingGroup);
    Sequence.BindGroup(3, RayTracingGroup);
    Sequence.Dispatch((ScreenExtent[0] + 7) / 8, (ScreenExtent[1] + 7) / 8);
}

void DisplayProjection::RecordRecombination(CommandSequence& Sequence)
{
    Sequence.BindProgram(RecombineProgram_);
    Sequence.BindGroup(0, RecombineGroup_);
    Sequence.Dispatch((ScreenExtent[0] + 7) / 8, (ScreenExtent[1] + 7) / 8);
}

void DisplayProjection::RecordAntialias(CommandSequence& Sequence, uint32_t Parity, const std::shared_ptr<DeviceTextureView>& Source)
{
    if (Source && Source != AntialiasSource_)
    {
        // 📝 Same bindings as the antialias groups, slot 1 (the radiance integrated with the history) swapped for the supplied one.
        for (uint32_t Slot = 0; Slot < 2; ++Slot)
        {
            std::vector<DescriptorResource> Resources = AntialiasGroups_[Slot]->Resources;
            Resources[1] = DescriptorResource(Source);
            SourceAntialiasGroups_[Slot] =
                Exchange_.ConstructDescriptorGroup(AntialiasLayout_, Resources, "Display antialias (supplied radiance)");
        }
        AntialiasSource_ = Source;
    }
    Sequence.BindProgram(AntialiasProgram_);
    Sequence.BindGroup(0, Source ? SourceAntialiasGroups_[Parity] : AntialiasGroups_[Parity]);
    Sequence.Dispatch((ScreenExtent[0] + 7) / 8, (ScreenExtent[1] + 7) / 8);
    Sequence.BindProgram(ExposureProgram_);
    Sequence.BindGroup(0, RecombinedExposureGroup_);
    Sequence.Dispatch(1);
}

void DisplayProjection::RecordImpactSparks(CommandSequence& Sequence, uint32_t Parity, uint32_t InstanceCount)
{
    const uint32_t Count = std::min(InstanceCount, ImpactSparkCapacity);
    if (Count == 0 || !ImpactSparkProgram_ || !DepthView_ || Parity >= ImpactSparkGroups_.size() || !ResolvedViews[Parity]) return;
    const std::array<ColorAttachment, 1> Colours = {{{ResolvedViews[Parity], LoadOperation::Load, {0.0f, 0.0f, 0.0f, 0.0f}}}};
    const DepthAttachment Depth{DepthView_, LoadOperation::Load, 0.0f};
    Sequence.BeginRaster(Colours, &Depth, "Jolt impact particles");
    Sequence.BindProgram(ImpactSparkProgram_);
    Sequence.BindGroup(0, ImpactSparkGroups_[Parity]);
    Sequence.Draw(6, Count);
    Sequence.EndRaster();
}

void DisplayProjection::RecordPresent(CommandSequence& Sequence, uint32_t Parity, const std::shared_ptr<DeviceTextureView>& Target)
{
    const std::array<ColorAttachment, 1> Colors = {{{Target, LoadOperation::Clear, {0.0f, 0.0f, 0.0f, 1.0f}}}};
    Sequence.BeginRaster(Colors, nullptr, "Display present");
    Sequence.BindProgram(PresentProgram_);
    Sequence.BindGroup(0, PresentGroups_[Parity]);
    Sequence.Draw(3);
    Sequence.EndRaster();
}

} // namespace Frontier::GlobalIllumination
