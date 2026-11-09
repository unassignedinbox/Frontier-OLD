/*==========================================================================================================================================
                                                          CAMERAINSPECTOR.CPP
==========================================================================================================================================*/
// 🧩 Native Camera inspector combining Project Zero's thin-lens optics, 8-blade iris schematic, and logarithmic focus study
//    with camera role switching, subtle-by-default camera shake, 3-mode motion blur, and camera-owned Lens Flare subcomponent access.

#include "Engine/Interface/Inspectors/CameraInspector.h"

#if FRONTIER_EDITOR && defined(FRONTIER_WITH_INTERFACE_SDK)

#include "Engine/Interface/Controls/PageControls.h"
#include "Engine/Interface/InspectorControls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace Frontier
{

namespace
{

constexpr float CameraPi = 3.14159265358979323846f;

[[nodiscard]] CardLook OpticsPrimaryLook() noexcept
{
    CardLook Look;
    Look.Fill       = ResolveCssColour(0x1D2029FF);
    Look.FillEnd    = ResolveCssColour(0x171921FF);
    Look.Glow       = ResolveCssColour(0x6D82B824);
    Look.GlowCentre = ImVec2(0.28f, 0.18f);
    Look.Border     = ResolveCssColour(0x8CA6DB2E);
    return Look;
}

[[nodiscard]] CardLook OpticsStudyLook() noexcept
{
    CardLook Look;
    Look.Fill       = ResolveCssColour(0x1B1D24FF);
    Look.FillEnd    = ResolveCssColour(0x16181EFF);
    Look.Glow       = ResolveCssColour(0x8E79B820);
    Look.GlowCentre = ImVec2(0.72f, 0.22f);
    Look.Border     = ResolveCssColour(0x9E8CC829);
    return Look;
}

[[nodiscard]] CardLook MotionCardLook() noexcept
{
    CardLook Look;
    Look.Fill       = ResolveCssColour(0x1A2126FF);
    Look.FillEnd    = ResolveCssColour(0x151B20FF);
    Look.Glow       = ResolveCssColour(0x5B9AB520);
    Look.GlowCentre = ImVec2(0.50f, 0.16f);
    Look.Border     = ResolveCssColour(0x74B2CC2B);
    return Look;
}

[[nodiscard]] CardLook SubcomponentCardLook() noexcept
{
    CardLook Look;
    Look.Fill   = ResolveCssColour(0x191C23FF);
    Look.Border = ResolveCssColour(0x8CA6DB26);
    return Look;
}

void FormatSharpnessDistance(char* Text, size_t Capacity, float Metres) noexcept
{
    if (std::isfinite(Metres))
    {
        std::snprintf(Text, Capacity, "%.1f m", static_cast<double>(Metres));
    }
    else
    {
        std::snprintf(Text, Capacity, "\xE2\x88\x9E");
    }
}

void RasterizeIrisDiagram(const DiagramPen& Pen, float ApertureFNumber)
{
    ImDrawList* DrawList = Pen.QueryDrawList();
    if (DrawList == nullptr)
    {
        return;
    }

    const float  Scale  = Pen.QueryScale();
    const ImVec2 Centre = Pen.QueryScreen(ImVec2(110.0f, 110.0f));
    Pen.Circle(ImVec2(110.0f, 110.0f), 97.0f, ResolveCssColour(0x17171BFF), ResolveCssColour(0x525058FF));
    for (int Ring = 0; Ring <= 32; ++Ring)
    {
        const float Fraction = static_cast<float>(Ring) / 32.0f;
        const int   Red      = static_cast<int>(35.0f + 30.0f * Fraction);
        const int   Green    = static_cast<int>(34.0f + 28.0f * Fraction);
        const int   Blue     = static_cast<int>(39.0f + 34.0f * Fraction);
        Pen.Circle(ImVec2(110.0f, 110.0f), 88.0f * (1.0f - Fraction), IM_COL32(Red, Green, Blue, 255));
    }
    DrawList->AddCircle(Centre, 88.0f * Scale, ResolveCssColour(0x454249FF), 72, Scale);
    for (int Tick = 0; Tick < 48; ++Tick)
    {
        const float Angle = static_cast<float>(Tick) * CameraPi / 24.0f - 0.5f * CameraPi;
        Pen.Line(ImVec2(110.0f + 94.0f * std::cos(Angle), 110.0f + 94.0f * std::sin(Angle)),
                 ImVec2(110.0f + 90.0f * std::cos(Angle), 110.0f + 90.0f * std::sin(Angle)), ResolveCssColour(0x7A7482FF), 0.7f);
    }

    const float ApertureRadius = ResolveIrisApertureRadius(ApertureFNumber);
    auto BladePoint = [&](float Angle, float Radius) -> ImVec2 {
        return Pen.QueryScreen(ImVec2(110.0f + std::cos(Angle) * Radius, 110.0f + std::sin(Angle) * Radius));
    };

    for (int Blade = 0; Blade < 8; ++Blade)
    {
        const float            BaseAngle = static_cast<float>(Blade) * CameraPi * 0.25f;
        std::array<ImVec2, 19> Polygon{};
        Polygon[0] = BladePoint(BaseAngle, ApertureRadius);
        Polygon[1] = BladePoint(BaseAngle + CameraPi * 0.25f, ApertureRadius);
        for (int Step = 0; Step <= 16; ++Step)
        {
            const float ArcAngle = BaseAngle + 1.07f - 0.78f * static_cast<float>(Step) / 16.0f;
            Polygon[static_cast<size_t>(Step + 2)] = BladePoint(ArcAngle, 86.0f);
        }
        std::reverse(Polygon.begin(), Polygon.end());
        DrawList->AddConcavePolyFilled(Polygon.data(), static_cast<int>(Polygon.size()),
                                       (Blade & 1) ? ResolveCssColour(0x393640FF) : ResolveCssColour(0x33313AFF));
        DrawList->AddPolyline(Polygon.data(), static_cast<int>(Polygon.size()), ResolveCssColour(0x797182FF), ImDrawFlags_Closed,
                              0.75f * Scale);
    }

    std::array<ImVec2, 8> PupilOctagon{};
    for (int Vertex = 0; Vertex < 8; ++Vertex)
    {
        PupilOctagon[static_cast<size_t>(Vertex)] = BladePoint(static_cast<float>(Vertex) * CameraPi * 0.25f, ApertureRadius);
    }
    DrawList->AddConvexPolyFilled(PupilOctagon.data(), 8, ResolveCssColour(0x101016FF));
    DrawList->AddPolyline(PupilOctagon.data(), 8, ResolveCssColour(0xB7A7C8FF), ImDrawFlags_Closed, Scale);
    Pen.Circle(ImVec2(110.0f, 110.0f), ApertureRadius * 0.55f, ResolveCssColour(0x9690B612));
    Pen.Circle(ImVec2(110.0f, 110.0f), 2.0f, ResolveCssColour(0xC1B6CD99));
}

void RasterizeFocusReticle(const DiagramPen& Pen, float LogicalX, float OffsetX, float OffsetY, float Opacity)
{
    const ImU32 Tone = ResolveAlpha(ResolveCssColour(0xAABBD1FF), Opacity);
    Pen.Line(ImVec2(LogicalX - 3.0f + OffsetX, 62.0f + OffsetY), ImVec2(LogicalX + 3.0f + OffsetX, 62.0f + OffsetY), Tone, 0.8f);
    Pen.Line(ImVec2(LogicalX + OffsetX, 55.0f + OffsetY), ImVec2(LogicalX + OffsetX, 93.0f + OffsetY), Tone, 0.8f);
    Pen.Line(ImVec2(LogicalX - 3.0f + OffsetX, 86.0f + OffsetY), ImVec2(LogicalX + 3.0f + OffsetX, 86.0f + OffsetY), Tone, 0.8f);
}

void RasterizeFocusDiagram(const DiagramPen& Pen, float SubjectMetres, float NearMetres, float FarMetres)
{
    const float NearX    = ResolveFocusLogCoordinate(NearMetres);
    const float FarX     = ResolveFocusLogCoordinate(FarMetres);
    const float SubjectX = ResolveFocusLogCoordinate(SubjectMetres);

    Pen.Rect(ImVec2(22.0f, 31.0f), ImVec2(316.0f, 90.0f), ResolveCssColour(0x18191CFF), 9.0f);
    Pen.Rect(ImVec2(NearX, 32.0f), ImVec2(std::max(1.5f, FarX - NearX), 88.0f), ResolveCssColour(0x8EACD324), 2.0f);
    Pen.Line(ImVec2(NearX, 32.0f), ImVec2(NearX, 120.0f), ResolveCssColour(0xA3BADB8C), 1.0f, 3.0f, 5.0f);
    Pen.Line(ImVec2(FarX, 32.0f), ImVec2(FarX, 120.0f), ResolveCssColour(0xA3BADB8C), 1.0f, 3.0f, 5.0f);

    for (const float DistanceTick : {1.0f, 3.0f, 10.0f, 30.0f, 100.0f})
    {
        const float TickX = ResolveFocusLogCoordinate(DistanceTick);
        Pen.Line(ImVec2(TickX, 120.0f), ImVec2(TickX, 125.0f), ResolveCssColour(0x6E788AFF), 1.0f);
        char Label[16]{};
        std::snprintf(Label, sizeof(Label), "%.0f m", static_cast<double>(DistanceTick));
        Pen.Text(ImVec2(TickX, 142.0f), Label, ResolveCssColour(0x7E899BFF), 9.0f, 0);
    }

    for (int Sample = 0; Sample < 17; ++Sample)
    {
        const float ReticleX = 30.0f + static_cast<float>(Sample) * 18.5f;
        if (ReticleX >= NearX && ReticleX <= FarX)
        {
            RasterizeFocusReticle(Pen, ReticleX, 0.0f, 0.0f, 0.78f);
        }
        else
        {
            for (int KernelY = -2; KernelY <= 2; ++KernelY)
            {
                for (int KernelX = -2; KernelX <= 2; ++KernelX)
                {
                    const float Weight = std::exp(-0.28f * static_cast<float>(KernelX * KernelX + KernelY * KernelY));
                    RasterizeFocusReticle(Pen, ReticleX, static_cast<float>(KernelX) * 1.15f, static_cast<float>(KernelY) * 1.15f,
                                          0.045f * Weight);
                }
            }
        }
    }

    Pen.Line(ImVec2(SubjectX, 21.0f), ImVec2(SubjectX, 121.0f), ResolveCssColour(0xD2DFF1FF), 1.3f);
    Pen.Circle(ImVec2(SubjectX, 76.0f), 9.0f, ResolveCssColour(0x1F2937FF), ResolveCssColour(0xC4D7EEFF));
    Pen.Line(ImVec2(SubjectX - 3.0f, 76.0f), ImVec2(SubjectX + 3.0f, 76.0f), ResolveCssColour(0xE3EDF7FF), 1.1f);
    Pen.Line(ImVec2(SubjectX, 73.0f), ImVec2(SubjectX, 79.0f), ResolveCssColour(0xE3EDF7FF), 1.1f);
    Pen.Text(ImVec2(22.0f, 166.0f), "DISTANCE \xC2\xB7 LOG SCALE", ResolveCssColour(0x7E899BFF), 8.5f, -1);
}

} // namespace

//------------------------------------------------------------------------------------------------------------------------
//                                                    CAMERA INSPECTOR
//------------------------------------------------------------------------------------------------------------------------

std::string CameraInspector::QueryDirectoryReadout() const
{
    const CameraOpticsEvaluation Optics = EvaluateCameraOptics(Parameters.Optics, Parameters.ViewportAspectRatio);
    char                         Readout[32]{};
    if (Parameters.Active)
    {
        std::snprintf(Readout, sizeof(Readout), "LIVE \xC2\xB7 %.0f\xC2\xB0", static_cast<double>(Optics.VerticalFieldOfViewDegrees));
    }
    else
    {
        std::snprintf(Readout, sizeof(Readout), "%.0f mm", static_cast<double>(Parameters.Optics.FocalLengthMillimetres));
    }
    return Readout;
}

void CameraInspector::EnumerateSwitches(std::vector<InspectorSwitchEntry>& OutSwitches)
{
    OutSwitches.push_back({"Camera Shake", "Subtle impact response", &Parameters.Shake.Enabled});
}

void CameraInspector::ConstructInspector()
{
    const ControlTheme    Theme = ControlTheme::ResolveOptics();
    const EditorTypefaces Faces = EditorTypefaces::Resolve();
    ControlColumn         Page(Theme, Faces, InspectorRuntime::Instance().ResolveInterfaceScale());
    bool                  Changed = false;
    ImGui::PushID("CameraInspectorPage");

    const char* Title    = Parameters.Role == CameraRole::Vehicle ? "Vehicle Camera" : "Editor Camera";
    const char* Subtitle = Parameters.Active ? "Active viewport projection \xC2\xB7 thin-lens optics, shake & motion blur."
                                             : "Standby camera profile \xC2\xB7 activate to drive viewport projection.";
    if (PageHeading::Construct(Page, "Header", "CAMERA / 01", Title, "Reset \xE2\x86\x97", Subtitle, "Reset camera parameters"))
    {
        Parameters.Optics.FocalLengthMillimetres = Parameters.Role == CameraRole::Vehicle ? 24.0f : 28.0f;
        Parameters.Optics.SensorWidthMillimetres = 36.0f;
        Parameters.Optics.ApertureFNumber        = 2.8f;
        Parameters.Optics.SubjectDistanceMetres  = 8.0f;
        Parameters.NearClipMetres                = 0.05f;
        Parameters.FarClipMetres                 = 25000.0f;
        Parameters.Shake                         = CameraShakeSettings{};
        Parameters.Shake.Enabled                 = Parameters.Role == CameraRole::Vehicle;
        Parameters.MotionBlur                    = CameraMotionBlurComponent{};
        Changed                                  = true;
    }

    static constexpr std::array<const char*, 4> Panes = {"Overview", "Aperture & focus", "Shake & motion", "Connected optics"};
    (void)TabStrip::Construct(Page, "Camera inspector sections", Panes.data(), static_cast<int>(Panes.size()), ActivePane);

    const float                  Aspect = std::max(0.25f, Parameters.ViewportAspectRatio);
    const CameraOpticsEvaluation Optics = EvaluateCameraOptics(Parameters.Optics, Aspect);

    if (ActivePane == 0)
    {
        // ── 01 / PROJECTION & LENS ─────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(OpticsPrimaryLook(), "Lens & field of view", "01 / PROJECTION");

        const DiagramPen Pen    = DiagramCanvas::Begin(Page, "##LensSchematic", 640.0f, 250.0f, 4.0f, 8.0f, nullptr,
                                                       "Thin-lens optical path: sensor gate, entrance pupil, and subject cone");
        const float      Extent = std::min(96.0f, 28.0f + Optics.HorizontalFieldOfViewDegrees * 0.74f);
        const float      Gap    = 8.0f + std::min(34.0f, Optics.EntrancePupilMillimetres * 0.68f);
        const ImU32      Accent = ResolveCssColour(0x9BB8F2FF);
        const ImU32      Dim    = ResolveCssColour(0x8CA6DB55);

        Pen.Line(ImVec2(30.0f, 122.0f), ImVec2(610.0f, 122.0f), ResolveCssColour(0x8CA6DB26), 1.0f);
        Pen.Line(ImVec2(235.0f, 122.0f), ImVec2(552.0f, 122.0f - Extent), Accent, 1.4f);
        Pen.Line(ImVec2(235.0f, 122.0f), ImVec2(552.0f, 122.0f + Extent), Accent, 1.4f);
        Pen.Line(ImVec2(552.0f, 122.0f - Extent), ImVec2(552.0f, 122.0f + Extent), Accent, 2.0f);
        for (const float SensorRayY : {92.0f, 152.0f})
        {
            Pen.Line(ImVec2(78.0f, SensorRayY), ImVec2(235.0f, 122.0f), Dim, 1.0f);
            Pen.Line(ImVec2(235.0f, 122.0f), ImVec2(552.0f, SensorRayY < 122.0f ? 122.0f + Extent : 122.0f - Extent), Dim, 1.0f);
        }
        Pen.Rect(ImVec2(66.0f, 84.0f), ImVec2(14.0f, 76.0f), ResolveCssColour(0xA4B6C530), 3.0f);
        Pen.Line(ImVec2(235.0f, 58.0f), ImVec2(235.0f, 122.0f - Gap), Accent, 5.5f);
        Pen.Line(ImVec2(235.0f, 122.0f + Gap), ImVec2(235.0f, 186.0f), Accent, 5.5f);

        char Figure[96]{};
        std::snprintf(Figure, sizeof(Figure), "%.1f\xC2\xB0 H  \xC2\xB7  %.1f\xC2\xB0 V",
                      static_cast<double>(Optics.HorizontalFieldOfViewDegrees), static_cast<double>(Optics.VerticalFieldOfViewDegrees));
        Pen.Text(ImVec2(392.0f, 114.0f), Figure, Accent, 17.0f, 0);
        Pen.Text(ImVec2(392.0f, 134.0f), "PERSPECTIVE FIELD OF VIEW", ResolveCssColour(0x8E9AB0FF), 9.0f, 0);

        std::snprintf(Figure, sizeof(Figure), "%.1f \xC3\x97 %.1f mm", static_cast<double>(Parameters.Optics.SensorWidthMillimetres),
                      static_cast<double>(Optics.SensorHeightMillimetres));
        Pen.Text(ImVec2(74.0f, 212.0f), Figure, ResolveCssColour(0xB8C4D8FF), 9.5f, 0);
        std::snprintf(Figure, sizeof(Figure), "f/%.1f \xC2\xB7 %.1f mm pupil", static_cast<double>(Parameters.Optics.ApertureFNumber),
                      static_cast<double>(Optics.EntrancePupilMillimetres));
        Pen.Text(ImVec2(235.0f, 212.0f), Figure, Accent, 9.5f, 0);
        std::snprintf(Figure, sizeof(Figure), "Frame %.2f \xC3\x97 %.2f m", static_cast<double>(Optics.SubjectFrameWidthMetres),
                      static_cast<double>(Optics.SubjectFrameHeightMetres));
        Pen.Text(ImVec2(520.0f, 212.0f), Figure, ResolveCssColour(0x8E9AB0FF), 9.5f, 0);

        static constexpr const char* RoleChoices[] = {"Editor camera", "Vehicle camera"};
        int                          RoleIndex     = Parameters.Role == CameraRole::Vehicle ? 1 : 0;
        if (ChoiceRow::Construct(Page, "CameraRole", "Camera role", RoleChoices, 2, RoleIndex, true, nullptr,
                                 "Editor camera controls free/orbit authoring; Vehicle camera drives HMI & chase rigs."))
        {
            Parameters.Role = RoleIndex == 1 ? CameraRole::Vehicle : CameraRole::Editor;
            Changed         = true;
        }

        if (!Parameters.Active)
        {
            if (NextButton::Construct(Page, "##ActivateCamera", "Activate Camera \xE2\x86\x97"))
            {
                Parameters.ActivationRequested = true;
                Changed                        = true;
            }
        }
        else
        {
            CaptionText::Construct(Page, "LIVE PROJECTION \xC2\xB7 Lens edits update the viewport frustum and reset GI accumulation.");
        }

        Changed |= SliderRow::Construct(Page, "FocalLength", "Focal length", Parameters.Optics.FocalLengthMillimetres, 10.0f, 200.0f, 0.5f,
                                        "mm");
        float VerticalDegrees = EvaluateCameraOptics(Parameters.Optics, Aspect).VerticalFieldOfViewDegrees;
        if (SliderRow::Construct(Page, "VerticalFov", "Vertical FOV", VerticalDegrees, 10.0f, 120.0f, 0.5f, "\xC2\xB0"))
        {
            Parameters.Optics.FocalLengthMillimetres = ResolveFocalLengthFromVerticalFieldOfView(
                static_cast<double>(VerticalDegrees) * (CameraPi / 180.0), Parameters.Optics.SensorWidthMillimetres, Aspect);
            Changed = true;
        }
        Changed |= SliderRow::Construct(Page, "SensorWidth", "Sensor width", Parameters.Optics.SensorWidthMillimetres, 16.0f, 70.0f, 0.5f,
                                        "mm");
        Changed |= SliderRow::Construct(Page, "NearClip", "Near clip", Parameters.NearClipMetres, 0.01f, 1.0f, 0.01f, "m");
        Page.EndCard();

        // ── 02 / TELEMETRY & QUICK SWITCHES ────────────────────────────────────────────────────────────────────────
        Page.BeginCard(MotionCardLook(), "Optical summary & quick switches", "02 / SUMMARY");
        char HorizontalText[24]{}, VerticalText[24]{}, PupilText[24]{}, HyperfocalText[24]{};
        std::snprintf(HorizontalText, sizeof(HorizontalText), "%.1f", static_cast<double>(Optics.HorizontalFieldOfViewDegrees));
        std::snprintf(VerticalText, sizeof(VerticalText), "%.1f", static_cast<double>(Optics.VerticalFieldOfViewDegrees));
        std::snprintf(PupilText, sizeof(PupilText), "%.1f", static_cast<double>(Optics.EntrancePupilMillimetres));
        std::snprintf(HyperfocalText, sizeof(HyperfocalText), "%.1f", static_cast<double>(Optics.HyperfocalMetres));
        const std::array<MetricTile, 4> Metrics = {{
            {"Horizontal FOV", HorizontalText, "\xC2\xB0"},
            {"Vertical FOV", VerticalText, "\xC2\xB0"},
            {"Entrance pupil", PupilText, "mm"},
            {"Hyperfocal", HyperfocalText, "m"},
        }};
        MetricTiles::Construct(Page, Metrics.data(), static_cast<int>(Metrics.size()));

        Changed |= ToggleRow::Construct(Page, "QuickShake", "Camera shake", Parameters.Shake.Enabled, true,
                                        "Subtle seeded millimetre-scale camera shake.");
        bool MotionBlurActive = Parameters.MotionBlur.Mode != CameraMotionBlurMode::Off;
        if (ToggleRow::Construct(Page, "QuickBlur", "Motion blur", MotionBlurActive, true,
                                 "Quick toggle between Off and analytical Camera motion blur."))
        {
            Parameters.MotionBlur.Mode = MotionBlurActive ? CameraMotionBlurMode::Camera : CameraMotionBlurMode::Off;
            Changed                    = true;
        }
        Page.EndCard();
    }
    else if (ActivePane == 1)
    {
        // ── 03 / APERTURE STUDY ────────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(OpticsStudyLook(), "Aperture study", "03 / APERTURE");
        const DiagramPen IrisPen = DiagramCanvas::Begin(Page, "##ApertureIris", 220.0f, 220.0f, 4.0f, 8.0f, nullptr,
                                                        "8-blade entrance pupil schematic");
        RasterizeIrisDiagram(IrisPen, Parameters.Optics.ApertureFNumber);
        Changed |= SliderRow::Construct(Page, "Aperture", "Aperture", Parameters.Optics.ApertureFNumber, 1.4f, 22.0f, 0.1f, "f/");
        char DepthSummary[128]{};
        char SpanText[32]{};
        FormatSharpnessDistance(SpanText, sizeof(SpanText), Optics.FarSharpnessMetres - Optics.NearSharpnessMetres);
        std::snprintf(DepthSummary, sizeof(DepthSummary), "Entrance pupil %.1f mm \xC2\xB7 acceptable sharpness span %s.",
                      static_cast<double>(Optics.EntrancePupilMillimetres), SpanText);
        CaptionText::Construct(Page, DepthSummary);
        Page.EndCard();

        // ── 04 / SUBJECT PLANE & SHARPNESS ─────────────────────────────────────────────────────────────────────────
        Page.BeginCard(OpticsStudyLook(), "Subject plane & sharpness", "04 / FOCUS");
        DiagramDrag      FocusDrag{};
        const DiagramPen FocusPen = DiagramCanvas::Begin(Page, "##SubjectSharpness", 360.0f, 174.0f, 4.0f, 8.0f, &FocusDrag,
                                                         "Drag horizontally to place the subject plane along the logarithmic distance scale");
        RasterizeFocusDiagram(FocusPen, Parameters.Optics.SubjectDistanceMetres, Optics.NearSharpnessMetres, Optics.FarSharpnessMetres);
        if (FocusDrag.Engaged)
        {
            Parameters.Optics.SubjectDistanceMetres = ResolveFocusDistanceFromCoordinate(FocusDrag.Point.x);
            Changed                                 = true;
        }
        Changed |= SliderRow::Construct(Page, "SubjectDistance", "Subject distance", Parameters.Optics.SubjectDistanceMetres, 1.0f, 100.0f,
                                        0.5f, "m");
        char NearText[32]{}, FarText[32]{}, SharpnessSummary[128]{};
        FormatSharpnessDistance(NearText, sizeof(NearText), Optics.NearSharpnessMetres);
        FormatSharpnessDistance(FarText, sizeof(FarText), Optics.FarSharpnessMetres);
        std::snprintf(SharpnessSummary, sizeof(SharpnessSummary),
                      "Near sharpness %s \xC2\xB7 far sharpness %s \xC2\xB7 hyperfocal %.1f m (0.03 mm CoC).", NearText, FarText,
                      static_cast<double>(Optics.HyperfocalMetres));
        CaptionText::Construct(Page, SharpnessSummary);
        Page.EndCard();
    }
    else if (ActivePane == 2)
    {
        // ── 05 / CAMERA SHAKE ──────────────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(MotionCardLook(), "Camera shake", "05 / SHAKE");
        Changed |= ToggleRow::Construct(Page, "ShakeEnabled", "Enable camera shake", Parameters.Shake.Enabled, true,
                                        "Deterministic seeded harmonic response driven by acceleration impulses.");
        float AmplitudeMillimetres = Parameters.Shake.AmplitudeMetres * 1000.0f;
        if (SliderRow::Construct(Page, "ShakeAmplitude", "Baseline amplitude", AmplitudeMillimetres, 0.0f, 6.0f, 0.1f, "mm", 1.0f,
                                 Parameters.Shake.Enabled))
        {
            Parameters.Shake.AmplitudeMetres = AmplitudeMillimetres * 0.001f;
            Changed                          = true;
        }
        float MaxOffsetMillimetres = Parameters.Shake.MaximumOffsetMetres * 1000.0f;
        if (SliderRow::Construct(Page, "ShakeMaxOffset", "Maximum offset cap", MaxOffsetMillimetres, 0.5f, 20.0f, 0.5f, "mm", 1.0f,
                                 Parameters.Shake.Enabled))
        {
            Parameters.Shake.MaximumOffsetMetres = MaxOffsetMillimetres * 0.001f;
            Changed                              = true;
        }
        Changed |= SliderRow::Construct(Page, "ShakeFrequency", "Harmonic frequency", Parameters.Shake.FrequencyHz, 2.0f, 35.0f, 0.5f, "Hz",
                                        1.0f, Parameters.Shake.Enabled);
        Changed |= SliderRow::Construct(Page, "ShakeThreshold", "Impact threshold", Parameters.Shake.AccelerationThresholdMps2, 1.0f, 30.0f,
                                        0.5f, "m/s\xC2\xB2", 1.0f, Parameters.Shake.Enabled);
        Changed |= SliderRow::Construct(Page, "ShakeAttack", "Attack time", Parameters.Shake.AttackSeconds, 0.01f, 0.25f, 0.01f, "s", 1.0f,
                                        Parameters.Shake.Enabled);
        Changed |= SliderRow::Construct(Page, "ShakeRelease", "Release time", Parameters.Shake.ReleaseSeconds, 0.05f, 1.0f, 0.01f, "s", 1.0f,
                                        Parameters.Shake.Enabled);
        Changed |= SliderRow::Construct(Page, "ShakeImpactGain", "Impact multiplier", Parameters.Shake.ImpactMultiplier, 0.5f, 6.0f, 0.1f,
                                        "\xC3\x97", 1.0f, Parameters.Shake.Enabled);
        CaptionText::Construct(Page, "Default motion is deliberately subtle (1.5 mm baseline, 6.0 mm hard cap) with deterministic harmonics.");
        Page.EndCard();

        // ── 06 / MOTION BLUR (CAPPED TO 3 ALGORITHMS) ──────────────────────────────────────────────────────────────
        Page.BeginCard(MotionCardLook(), "Motion blur", "06 / MOTION BLUR");
        static constexpr const char* BlurAlgorithms[3] = {
            "Off",
            "Camera (analytical reprojection)",
            "Scene (camera + object velocity)",
        };
        int BlurMode = std::clamp(static_cast<int>(Parameters.MotionBlur.Mode), 0, 2);
        if (SelectRow::Construct(Page, "MotionBlurAlgorithm", "Algorithm", BlurAlgorithms, 3, BlurMode, true,
                                 "Capped to three presentation modes: Off, Camera-only reprojection, or Scene velocity blur."))
        {
            Parameters.MotionBlur.Mode = static_cast<CameraMotionBlurMode>(BlurMode);
            Changed                    = true;
        }
        Changed |= SliderRow::Construct(Page, "MotionBlurStrength", "Shutter strength", Parameters.MotionBlur.Strength, 0.0f, 1.0f, 0.01f,
                                        "\xC3\x97", 1.0f, Parameters.MotionBlur.Mode != CameraMotionBlurMode::Off);
        CaptionText::Construct(Page, "Applies during Global Illumination presentation; diagnostic and analytic views remain unblurred.");
        Page.EndCard();
    }
    else
    {
        // ── 07 / PLACEMENT & RIG POSE ──────────────────────────────────────────────────────────────────────────────
        Page.BeginCard(SubcomponentCardLook(), "Authored camera placement", "07 / PLACEMENT");
        const std::array<TransformTableRow, 2> Rows = {{
            {"Position", "m", Parameters.Spatial.PositionMetres, -25000.0f, 25000.0f, 0.05f, "%.2f", {0.0f, -18.0f, 4.0f}},
            {"Rotation", "\xC2\xB0", Parameters.Spatial.OrientationDegrees, -360.0f, 360.0f, 0.25f, "%.1f", {0.0f, 0.0f, 0.0f}},
        }};
        if (TransformTable::Construct(Page, "CameraPlacement", Rows.data(), static_cast<int>(Rows.size()), true))
        {
            Parameters.Spatial.AdvanceRevision();
            Changed = true;
        }
        Page.EndCard();

        // ── 08 / CONNECTED CAMERA OPTICS (LENS FLARE UNDER CAMERA) ─────────────────────────────────────────────────
        Page.BeginCard(SubcomponentCardLook(), "Connected camera optics", "08 / SUBCOMPONENT");
        CaptionText::Construct(
            Page, "Lens flare is an aperture optical subcomponent enclosed beneath this Camera record (not under Sun).");
        if (NextButton::Construct(Page, "##OpenCameraLensFlare", "Open Lens Flare \xE2\x86\x97"))
        {
            InspectorRuntime::Instance().RequestSelection("OpticalFlare");
        }
        Page.EndCard();
    }

    FooterRow::Construct(Page, "PERSPECTIVE CAMERA / Z UP", "Settings apply live \xE2\x86\x97");
    Page.Finalize();
    ImGui::PopID();
    if (Changed)
    {
        Parameters.AdvanceRevision();
    }
}

} // namespace Frontier

#elif FRONTIER_EDITOR

namespace Frontier
{
std::string CameraInspector::QueryDirectoryReadout() const { return {}; }
void        CameraInspector::EnumerateSwitches(std::vector<InspectorSwitchEntry>&) {}
void        CameraInspector::ConstructInspector() {}
} // namespace Frontier

#endif // FRONTIER_EDITOR && FRONTIER_WITH_INTERFACE_SDK
