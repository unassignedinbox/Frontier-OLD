/*==========================================================================================================================================
                                                             CAMERAOPTICS.H
==========================================================================================================================================*/
// 🧩 Thin-lens optical evaluation, perspective field-of-view conversion, and schematic coordinates for Camera records.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace Frontier
{

//------------------------------------------------------------------------------------------------------------------------
//                                                    CAMERA OPTICS
//------------------------------------------------------------------------------------------------------------------------

struct CameraOpticsSettings
{
    float FocalLengthMillimetres = 28.0f;   // [mm] lens focal length
    float SensorWidthMillimetres = 36.0f;   // [mm] active sensor horizontal gate
    float ApertureFNumber        = 2.8f;    // [-]  f-number (focal length / entrance pupil)
    float SubjectDistanceMetres  = 8.0f;    // [m]  focus plane distance
};

struct CameraOpticsEvaluation
{
    float HorizontalFieldOfViewDegrees = 65.47f;   // [°]  horizontal perspective angle
    float VerticalFieldOfViewDegrees   = 40.30f;   // [°]  vertical perspective angle
    float EntrancePupilMillimetres     = 10.00f;   // [mm] entrance pupil diameter
    float NearSharpnessMetres          = 4.0f;     // [m]  acceptable near sharpness plane (0.03 mm CoC)
    float FarSharpnessMetres           = 20.0f;    // [m]  acceptable far sharpness plane (∞ when hyperfocal)
    float HyperfocalMetres             = 9.36f;    // [m]  hyperfocal distance (0.03 mm CoC)
    float SubjectFrameWidthMetres      = 10.29f;   // [m]  horizontal frame span at subject plane
    float SubjectFrameHeightMetres     = 5.79f;    // [m]  vertical frame span at subject plane
    float SensorHeightMillimetres      = 20.25f;   // [mm] active sensor vertical gate for aspect ratio
};

// 📝 Evaluates horizontal/vertical FOV, entrance pupil, hyperfocal distance, and near/far acceptable-sharpness planes
//    using the 0.03 mm 35 mm circle-of-confusion convention from Project Zero's native Camera Optics panel.
[[nodiscard]] inline CameraOpticsEvaluation EvaluateCameraOptics(const CameraOpticsSettings& Settings, float AspectRatio) noexcept
{
    constexpr float RadiansToDegrees        = 57.29577951308232f;
    constexpr float CircleOfConfusionMillim = 0.03f;
    const float     SafeAspect              = std::max(0.01f, std::isfinite(AspectRatio) ? AspectRatio : (16.0f / 9.0f));
    const float     FocalMillimetres        = std::clamp(std::isfinite(Settings.FocalLengthMillimetres) ? Settings.FocalLengthMillimetres : 28.0f, 1.0f, 500.0f);
    const float     SensorWidthMillimetres  = std::clamp(std::isfinite(Settings.SensorWidthMillimetres) ? Settings.SensorWidthMillimetres : 36.0f, 4.0f, 100.0f);
    const float     Aperture                = std::clamp(std::isfinite(Settings.ApertureFNumber) ? Settings.ApertureFNumber : 2.8f, 0.7f, 64.0f);
    const float     SubjectMetres           = std::clamp(std::isfinite(Settings.SubjectDistanceMetres) ? Settings.SubjectDistanceMetres : 8.0f, 0.2f, 1000.0f);
    const float     SubjectMillimetres      = std::max(FocalMillimetres + 1.0f, SubjectMetres * 1000.0f);
    const float     SensorHeightMillimetres = SensorWidthMillimetres / SafeAspect;
    const float     HyperfocalMillimetres   = (FocalMillimetres * FocalMillimetres) / (Aperture * CircleOfConfusionMillim) + FocalMillimetres;

    CameraOpticsEvaluation Result{};
    Result.HorizontalFieldOfViewDegrees = 2.0f * std::atan(SensorWidthMillimetres / (2.0f * FocalMillimetres)) * RadiansToDegrees;
    Result.VerticalFieldOfViewDegrees   = 2.0f * std::atan(SensorHeightMillimetres / (2.0f * FocalMillimetres)) * RadiansToDegrees;
    Result.EntrancePupilMillimetres     = FocalMillimetres / Aperture;
    Result.NearSharpnessMetres          = (HyperfocalMillimetres * SubjectMillimetres) /
                                          (HyperfocalMillimetres + SubjectMillimetres - FocalMillimetres) * 0.001f;
    Result.FarSharpnessMetres           = (HyperfocalMillimetres > SubjectMillimetres - FocalMillimetres)
                                              ? ((HyperfocalMillimetres * SubjectMillimetres) /
                                                 (HyperfocalMillimetres - SubjectMillimetres + FocalMillimetres) * 0.001f)
                                              : std::numeric_limits<float>::infinity();
    Result.HyperfocalMetres             = HyperfocalMillimetres * 0.001f;
    Result.SubjectFrameWidthMetres      = SubjectMetres * SensorWidthMillimetres / FocalMillimetres;
    Result.SubjectFrameHeightMetres     = SubjectMetres * SensorHeightMillimetres / FocalMillimetres;
    Result.SensorHeightMillimetres      = SensorHeightMillimetres;
    return Result;
}

// 📝 Inverse vertical-FOV relation: resolves focal length [mm] from vertical field of view [rad], sensor width [mm], and aspect ratio.
[[nodiscard]] inline float ResolveFocalLengthFromVerticalFieldOfView(double VerticalFieldOfViewRadians, float SensorWidthMillimetres,
                                                                     float AspectRatio) noexcept
{
    const float  SafeAspect             = std::max(0.01f, std::isfinite(AspectRatio) ? AspectRatio : (16.0f / 9.0f));
    const float  SafeSensorWidth        = std::clamp(std::isfinite(SensorWidthMillimetres) ? SensorWidthMillimetres : 36.0f, 4.0f, 100.0f);
    const double ClampedVerticalRadians = std::clamp(std::isfinite(VerticalFieldOfViewRadians) ? VerticalFieldOfViewRadians : 1.0471975512,
                                                     0.0174532925, 3.0543261909);
    const float  SensorHeight           = SafeSensorWidth / SafeAspect;
    const double Tangent                = std::max(1.0e-4, std::tan(0.5 * ClampedVerticalRadians));
    return static_cast<float>(std::clamp(static_cast<double>(SensorHeight) / (2.0 * Tangent), 1.0, 500.0));
}

// 📝 Schematic helpers matching the 220×220 8-blade iris and 360×174 logarithmic focus plane diagrams.
[[nodiscard]] inline float ResolveIrisApertureRadius(float ApertureFNumber) noexcept
{
    const float Clamped = std::clamp(ApertureFNumber, 1.4f, 22.0f);
    return 10.0f + std::clamp((16.0f / Clamped - 1.0f) / (16.0f / 1.4f - 1.0f), 0.0f, 1.0f) * 47.0f;
}

[[nodiscard]] inline float ResolveFocusLogCoordinate(float DistanceMetres) noexcept
{
    if (!std::isfinite(DistanceMetres))
    {
        return 338.0f;
    }
    return 22.0f + std::log1p(std::clamp(DistanceMetres, 0.0f, 100.0f)) / std::log(101.0f) * 316.0f;
}

[[nodiscard]] inline float ResolveFocusDistanceFromCoordinate(float LogicalX) noexcept
{
    return std::round(std::clamp(std::expm1((LogicalX - 22.0f) / 316.0f * std::log(101.0f)), 1.0f, 100.0f));
}

} // namespace Frontier
