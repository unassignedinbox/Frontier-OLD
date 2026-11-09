/*==========================================================================================================================================
                                                          FLUIDPLUMEINSPECTOR.CPP
==========================================================================================================================================*/
// 🧩 FluidPlumeConfiguration: the Eulerian gas & explosion plume page over ControlColumn + PageControls, with its 1:1 FluidPlumeEditor.

#include "Engine/Interface/Inspectors/FluidPlumeInspector.h"

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

#include "Engine/Interface/Controls/PageControls.h"
#include "Engine/Interface/FluidPlumeEditor.h"
#include "Engine/Interface/InspectorControls.h"
#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"
#include "Engine/Scene/SceneDirectory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#endif

namespace Frontier
{

#if FRONTIER_EDITOR

std::string FluidPlumeInspector::QueryDirectoryReadout() const
{
    if (!Parameters.CombustionEnabled) return "Smoke";
    char Buffer[24]{};
    std::snprintf(Buffer, sizeof(Buffer), "%.0f K", static_cast<double>(Parameters.BlackbodyCoreKelvin));
    return Buffer;
}

#endif

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

namespace
{

[[nodiscard]] CardLook CoreCardLook() noexcept
{
    CardLook Look{};
    Look.Fill          = ResolveCssColour(0x211D17FF);
    Look.FillEnd       = ResolveCssColour(0x181715FF);
    Look.Glow          = ResolveCssColour(0xFF9A3C18);
    Look.GlowCentre    = ImVec2(0.50f, 0.44f);
    Look.GlowExtent    = 0.72f;
    Look.Border        = ResolveCssColour(0xFFFFFF0B);
    Look.Radius        = 23.0f;
    Look.PaddingTop    = 18.0f;
    Look.PaddingSide   = 18.0f;
    Look.PaddingBottom = 18.0f;
    Look.MarginBottom  = 14.0f;
    return Look;
}

[[nodiscard]] CardLook SectionCardLook() noexcept
{
    CardLook Look{};
    Look.Fill          = ResolveCssColour(0x1C1B18FF);
    Look.Border        = ResolveCssColour(0xFFFFFF09);
    Look.Radius        = 22.0f;
    Look.PaddingTop    = 18.0f;
    Look.PaddingSide   = 18.0f;
    Look.PaddingBottom = 18.0f;
    Look.MarginBottom  = 12.0f;
    return Look;
}

[[nodiscard]] std::string FormatScalar(const char* Pattern, double Value)
{
    char Buffer[48]{};
    std::snprintf(Buffer, sizeof(Buffer), Pattern, Value);
    return Buffer;
}

[[nodiscard]] ImU32 ResolveKelvinTint(float Kelvin, uint8_t Alpha = 255u) noexcept
{
    const float T = std::clamp((Kelvin - 800.0f) / (3600.0f - 800.0f), 0.0f, 1.0f);
    const float R = std::clamp(0.58f + 0.42f * std::pow(T, 0.35f), 0.0f, 1.0f);
    const float G = std::clamp(0.16f + 0.78f * std::pow(T, 0.85f), 0.0f, 1.0f);
    const float B = std::clamp(0.05f + 0.82f * std::pow(T, 2.10f), 0.0f, 1.0f);
    return IM_COL32(static_cast<int>(R * 255.0f), static_cast<int>(G * 255.0f), static_cast<int>(B * 255.0f), Alpha);
}

} // namespace

void FluidPlumeInspector::ConstructEditor(RecordToken Token)
{
    FluidPlumeEditor::ConstructInterface(Token, *this);
}

void FluidPlumeInspector::EnumerateSwitches(std::vector<InspectorSwitchEntry>& OutSwitches)
{
    OutSwitches.push_back({ "Combustion", "Blackbody core", &Parameters.CombustionEnabled });
    OutSwitches.push_back({ "Self-Shadow", "Beer-Lambert", &Parameters.SelfShadowing });
}

void FluidPlumeInspector::ConstructInspector()
{
    const ControlTheme     Theme    = ControlTheme::ResolveSolar();
    const EditorTypefaces  Faces    = EditorTypefaces::Resolve();
    InspectorRuntime&      Runtime  = InspectorRuntime::Instance();
    ControlColumn          Page(Theme, Faces, Runtime.ResolveInterfaceScale());
    const float            S        = Page.QueryScale();
    const RecordToken      Token    = Runtime.QueryInspectedToken();
    const std::string      Name     = Token.IsValid() ? SceneDirectory::Instance().QueryName(Token) : std::string("Fluid plume");
    bool                   Changed  = false;

    ImGui::PushID("FluidPlumeInspectorPage");

    if (PageHeading::Construct(Page, "Header", "SOLVER / FLUID PLUME", Name.c_str(), "Fluid Editor \xE2\x86\x97",
                               "3D Eulerian gas, combustion & detonation volume.",
                               "Open the 1:1 Gas & Explosion Fluid Editor"))
    {
        if (Token.IsValid())
        {
            (void)Runtime.RequestEditorOpen(Token);
        }
    }

    static constexpr std::array<const char*, 3> Panes = { "Overview", "Placement", "Dynamics" };
    (void)TabStrip::Construct(Page, "FluidPlumeSections", Panes.data(), static_cast<int>(Panes.size()), ActivePane);

    const auto Presets = FluidDynamics::EnumerateFluidPresets();
    std::vector<const char*> PresetTitles;
    PresetTitles.reserve(Presets.size());
    int CurrentPresetIndex = 0;
    for (size_t Index = 0; Index < Presets.size(); ++Index)
    {
        PresetTitles.push_back(Presets[Index].Title);
        if (Parameters.PresetIdentity == Presets[Index].Identity)
        {
            CurrentPresetIndex = static_cast<int>(Index);
        }
    }

    if (ActivePane == 0)
    {
        // ── 01 / BLACKBODY CORE ──────────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(CoreCardLook(), "Thermal plume core", "01 / RADIANCE");
        DiagramDrag      Drag{};
        const DiagramPen Pen = DiagramCanvas::Begin(Page, "##PlumeCoreCurve", 640.0f, 250.0f, 6.0f, 0.0f, &Drag,
                                                    "Planckian core temperature \xC2\xB7 drag horizontally to tune Kelvin");

        // Planckian blackbody emission envelope across normalized reaction progress
        const ImU32 CoreInk = Parameters.CombustionEnabled ? ResolveKelvinTint(Parameters.BlackbodyCoreKelvin, 235u)
                                                           : ResolveCssColour(0x8D9590CC);
        for (const float GridY : { 70.0f, 130.0f, 190.0f })
        {
            Pen.Line(ImVec2(24.0f, GridY), ImVec2(616.0f, GridY), ResolveCssColour(0xE8C78B1A), 1.0f, 2.0f, 6.0f);
        }
        const float NormK   = std::clamp((Parameters.BlackbodyCoreKelvin - 800.0f) / (3600.0f - 800.0f), 0.0f, 1.0f);
        const float MarkerX = 48.0f + NormK * 544.0f;
        ImVec2      PrevPt{};
        for (int Step = 0; Step <= 48; ++Step)
        {
            const float U     = static_cast<float>(Step) / 48.0f;
            const float X     = 24.0f + U * 592.0f;
            const float Shape = std::exp(-std::pow((U - (0.22f + 0.56f * NormK)) / 0.24f, 2.0f));
            const float Y     = 206.0f - Shape * (64.0f + 78.0f * NormK);
            const ImVec2 Pt(X, Y);
            if (Step > 0)
            {
                Pen.Line(PrevPt, Pt, CoreInk, 2.0f);
            }
            PrevPt = Pt;
        }
        const float PeakY = 206.0f - (64.0f + 78.0f * NormK);
        if (Parameters.CombustionEnabled)
        {
            Pen.Glow(ImVec2(MarkerX, PeakY), 32.0f, ResolveKelvinTint(Parameters.BlackbodyCoreKelvin, 255u), 0.24f);
        }
        Pen.Circle(ImVec2(MarkerX, PeakY), 16.0f, ResolveKelvinTint(Parameters.BlackbodyCoreKelvin, 36u),
                   ResolveKelvinTint(Parameters.BlackbodyCoreKelvin, 130u));
        Pen.Circle(ImVec2(MarkerX, PeakY), 5.5f, CoreInk);
        Pen.Text(ImVec2(28.0f, 232.0f), "800 K DRIFT", ResolveCssColour(0xA49886FF), 13.0f, -1);
        Pen.Text(ImVec2(612.0f, 232.0f), "3600 K ORDNANCE", ResolveCssColour(0xA49886FF), 13.0f, 1);

        if (Drag.Engaged)
        {
            const float DragFrac           = std::clamp((Drag.Point.x - 48.0f) / 544.0f, 0.0f, 1.0f);
            Parameters.BlackbodyCoreKelvin = std::round((800.0f + DragFrac * (3600.0f - 800.0f)) / 10.0f) * 10.0f;
            Changed                        = true;
        }

        const ImVec2      CardOrigin   = Page.QueryCardOrigin();
        const float       CardWidth    = Page.QueryCardWidth();
        const ImVec2      HeroMin(CardOrigin.x + 19.0f * S, CardOrigin.y + 48.0f * S);
        const std::string KelvinFigure = FormatScalar("%.0f", static_cast<double>(Parameters.BlackbodyCoreKelvin));
        (void)ReadoutBlock::ConstructAt(Page, HeroMin, "Blackbody core", KelvinFigure.c_str(), "K", 34.0f, 15.0f, -1.5f, 1.25f);

        const char* DetonateLabel = "Detonate \xE2\x86\x97";
        const float PillWidth     = TagPill::Measure(Page, DetonateLabel, 9.0f, 10.0f);
        bool        DetonatePress = false;
        (void)TagPill::ConstructAt(Page, "##DetonateTag", ImVec2(CardOrigin.x + CardWidth - 19.0f * S - PillWidth, HeroMin.y),
                                   DetonateLabel, true, &DetonatePress, 9.0f, 10.0f, 7.0f, 28.0f,
                                   ResolveCssColour(0x33281DFF), ResolveCssColour(0xF0D5ACFF),
                                   "Trigger an immediate burst impulse in the active world plume");
        if (DetonatePress)
        {
            ++Parameters.BurstGeneration;
            Changed = true;
        }

        const std::string SidecarName = Parameters.SpaceFile.empty() ? (Parameters.PresetIdentity + ".fluidspace") : Parameters.SpaceFile;
        FooterRow::ConstructAtlas(Page, "SIDECAR ASSET", SidecarName.c_str(), 9.0f, 10.5f, 0.0f);
        Page.EndCard();

        // ── KPI METRIC TILES & QUICK SWITCH TILES ────────────────────────────────────────────────────────────────────
        const std::array<MetricTile, 3> Metrics = {{
            { "Buoyancy", FormatScalar("%.2f", static_cast<double>(Parameters.BuoyancyLift)), "m/s\xC2\xB2 lift" },
            { "Vorticity", FormatScalar("%.2f", static_cast<double>(Parameters.VorticityConfinement)), "curl confinement" },
            { "Domain scale", FormatScalar("%.2f\xC3\x97", static_cast<double>(Parameters.DomainScale)), "world bounds" },
        }};
        MetricTiles::Construct(Page, Metrics.data(), static_cast<int>(Metrics.size()));

        const std::array<SwitchTile, 3> QuickSwitches = {{
            { "Volume active", &Parameters.Enabled, TileGlyph::Sun, false, false, "Enable world-anchored Eulerian plume solver" },
            { "Combustion", &Parameters.CombustionEnabled, TileGlyph::Sun, false, false, "Enable exothermic fuel burn and Planckian glow" },
            { "Self-shadow", &Parameters.SelfShadowing, TileGlyph::Orbit, false, false, "Enable Beer-Lambert volumetric self-shadowing" },
        }};
        const bool Narrow = Page.QueryContainerWidth() <= 369.0f;
        if (SwitchTiles::Construct(Page, "FluidQuickSwitches", QuickSwitches.data(), static_cast<int>(QuickSwitches.size()), Narrow ? 2 : 3))
        {
            Changed = true;
        }

        // ── 02 / PRESET & DETONATION ─────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(SectionCardLook(), "Preset & burst impulse", "02 / SOURCE");
        if (!PresetTitles.empty() &&
            SelectRow::Construct(Page, "PresetSelect", "Effect preset", PresetTitles.data(), static_cast<int>(PresetTitles.size()),
                                 CurrentPresetIndex, true, "Load one of the 18 calibrated gas & pyrotechnic presets"))
        {
            const auto& Picked              = Presets[static_cast<size_t>(CurrentPresetIndex)];
            Parameters.PresetIdentity       = Picked.Identity;
            const auto PresetParams         = FluidDynamics::ConstructPresetParameters(Picked.Identity);
            Parameters.BuoyancyLift         = PresetParams.Buoyancy;
            Parameters.VorticityConfinement = PresetParams.VorticityConfinement;
            Parameters.DissipationRate      = PresetParams.SmokeDissipation;
            Parameters.FuelInjectionRate    = PresetParams.SourceFuel;
            Parameters.BlackbodyCoreKelvin  = std::clamp(1000.0f + PresetParams.TemperatureScale * 1200.0f, 800.0f, 3600.0f);
            Parameters.CombustionEnabled    = (PresetParams.SourceFuel > 0.05f || PresetParams.BlastFuel > 0.05f);
            if (Picked.TriggerExplosionOnLoad) ++Parameters.BurstGeneration;
            Changed = true;
        }

        static constexpr std::array<const char*, 5> BurstLabels = {
            "Single", "Shrapnel", "Salvo", "Collide", "Mushroom"
        };
        int PatternIdx = static_cast<int>(std::clamp<uint32_t>(Parameters.BurstPattern, 0u, 4u));
        if (ChoiceRow::Construct(Page, "BurstPattern", "Burst pattern", BurstLabels.data(), static_cast<int>(BurstLabels.size()), PatternIdx,
                                 true, nullptr, "Multi-lobe impulse geometry injected on detonation"))
        {
            Parameters.BurstPattern = static_cast<uint32_t>(PatternIdx);
            Changed                 = true;
        }

        const std::array<ActionEntry, 2> Actions = {{
            { "DetonateScene", "Detonate in scene", true, "Inject an immediate explosive impulse into the level plume" },
            { "OpenFluidStudio", "Open Fluid Editor \xE2\x86\x97", Token.IsValid(), "Launch the full 1:1 Gas & Explosion authoring workspace" },
        }};
        ActionRowLook RowLook{};
        RowLook.MarginTop    = 12.0f;
        RowLook.MarginBottom = 4.0f;
        const int ClickedAction = ActionRow::Construct(Page, Actions, RowLook);
        if (ClickedAction == 0)
        {
            ++Parameters.BurstGeneration;
            Changed = true;
        }
        else if (ClickedAction == 1 && Token.IsValid())
        {
            (void)Runtime.RequestEditorOpen(Token);
        }
        Page.EndCard();
    }
    else if (ActivePane == 1)
    {
        // ── 01 / WORLD PLACEMENT ─────────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(SectionCardLook(), "World coordinates", "01 / PLACEMENT");
        Changed |= SliderRow::Construct(Page, "PosX", "Position X", Parameters.PositionMetres[0], -120.0f, 120.0f, 0.1f, "m");
        Changed |= SliderRow::Construct(Page, "PosY", "Position Y", Parameters.PositionMetres[1], -120.0f, 120.0f, 0.1f, "m");
        Changed |= SliderRow::Construct(Page, "PosZ", "Position Z (elevation)", Parameters.PositionMetres[2], -5.0f, 40.0f, 0.05f, "m");
        Changed |= SliderRow::Construct(Page, "DomainScale", "Domain scale", Parameters.DomainScale, 0.25f, 8.0f, 0.05f, "\xC3\x97");
        Page.EndCard();
    }
    else
    {
        // ── 01 / PLUME DYNAMICS ──────────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(SectionCardLook(), "Advection & thermodynamics", "01 / DYNAMICS");
        Changed |= SliderRow::Construct(Page, "Buoyancy", "Buoyancy lift", Parameters.BuoyancyLift, -4.0f, 10.0f, 0.05f, "m/s\xC2\xB2");
        Changed |= SliderRow::Construct(Page, "Vorticity", "Vorticity confinement", Parameters.VorticityConfinement, 0.0f, 16.0f, 0.05f, "");
        Changed |= SliderRow::Construct(Page, "Dissipation", "Smoke dissipation", Parameters.DissipationRate, 0.0f, 0.60f, 0.005f, "1/s");
        Changed |= SliderRow::Construct(Page, "FuelRate", "Fuel injection", Parameters.FuelInjectionRate, 0.0f, 6.0f, 0.05f, "");
        Changed |= SliderRow::Construct(Page, "CoreKelvin", "Blackbody core", Parameters.BlackbodyCoreKelvin, 800.0f, 3600.0f, 10.0f, "K");
        Changed |= SliderRow::Construct(Page, "SootExt", "Soot extinction", Parameters.SootExtinction, 0.0f, 4.0f, 0.05f, "1/m");
        Page.EndCard();
    }

    FooterRow::Construct(Page, "EULERIAN VOLUME / Z UP", "Settings apply live \xE2\x86\x97");
    Page.Finalize();
    ImGui::PopID();

    if (Changed)
    {
        Parameters.AdvanceRevision();
    }
}

#elif FRONTIER_EDITOR

void FluidPlumeInspector::ConstructEditor(RecordToken) {}
void FluidPlumeInspector::EnumerateSwitches(std::vector<InspectorSwitchEntry>&) {}
void FluidPlumeInspector::ConstructInspector() {}

#endif

} // namespace Frontier
