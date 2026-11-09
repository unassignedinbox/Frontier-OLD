/*==========================================================================================================================================
                                                   CAMERAEFFECTSVALIDATION.CPP
==========================================================================================================================================*/
// 🧩 CPU validation for Camera records, thin-lens CameraOptics, subtle CameraShake, 3-mode MotionBlur, and Camera-owned Lens Flare.

#include "Engine/Rendering/SceneViewport/CameraEffects.h"
#include "Engine/Rendering/SceneViewport/CameraOptics.h"
#include "Engine/Scene/WorldSpace.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
using Vector3 = std::array<double, 3>;

[[nodiscard]] double Magnitude(const Vector3& Value)
{
    return std::sqrt(Value[0] * Value[0] + Value[1] * Value[1] + Value[2] * Value[2]);
}

[[nodiscard]] bool Near(double A, double B, double Tolerance = 1.0e-5)
{
    return std::abs(A - B) <= Tolerance;
}
} // namespace

int main(int ArgumentCount, char** Arguments)
{
    uint32_t Checks   = 0;
    uint32_t Failures = 0;
    const auto Check  = [&](bool Passed, std::string_view Name) {
        ++Checks;
        if (!Passed)
        {
            ++Failures;
            std::cerr << "[FAIL] " << Name << '\n';
        }
        else
        {
            std::cout << "[PASS] " << Name << '\n';
        }
    };

    // 1. Subtle-by-default camera shake & bounded 3-mode motion blur
    Frontier::CameraShakeComponent Defaults;
    const Frontier::CameraShakeSettings& DefaultSettings = Defaults.QuerySettings();
    Check(DefaultSettings.Enabled, "camera shake is enabled by default");
    Check(Near(DefaultSettings.AmplitudeMetres, 0.0015), "default shake amplitude is subtle (1.5 mm)");
    Check(Near(DefaultSettings.MaximumOffsetMetres, 0.006), "default shake displacement is capped at 6.0 mm");

    Frontier::CameraMotionBlurComponent Blur;
    Check(Frontier::EncodeCameraMotionBlurControl(Blur) == 0u, "motion blur defaults to Off");
    Blur.Mode = Frontier::CameraMotionBlurMode::Camera;
    const uint32_t CameraControl = Frontier::EncodeCameraMotionBlurControl(Blur);
    Check((CameraControl & 3u) == 1u && (CameraControl >> 2u) > 0u, "camera-only mode and strength encode independently");
    Blur.Mode = Frontier::CameraMotionBlurMode::Scene;
    const uint32_t SceneControl = Frontier::EncodeCameraMotionBlurControl(Blur);
    Check((SceneControl & 3u) == 2u && (SceneControl & ~3u) == (CameraControl & ~3u), "full-scene mode selects the second motion-vector set");
    Blur.Mode = static_cast<Frontier::CameraMotionBlurMode>(3u);
    Check(Frontier::EncodeCameraMotionBlurControl(Blur) == 0u, "invalid blur modes safely resolve to Off");
    Blur.Mode     = Frontier::CameraMotionBlurMode::Scene;
    Blur.Strength = 2.0f;
    Check((Frontier::EncodeCameraMotionBlurControl(Blur) >> 2u) == 63u, "motion-blur strength is clamped before quantization");

    Frontier::CameraShakeComponent First;
    Frontier::CameraShakeComponent Second;
    First.AccessSettings().Seed  = 12345u;
    Second.AccessSettings().Seed = 12345u;
    (void)First.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    (void)Second.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto FirstOffset  = First.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto SecondOffset = Second.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(FirstOffset.EyeMetres == SecondOffset.EyeMetres, "identical seeds produce deterministic camera shake");
    Check(Magnitude(FirstOffset.EyeMetres) <= 0.0060001, "camera shake obeys hard 6 mm offset cap");

    // 2. Thin-lens CameraOptics round-trip
    Frontier::CameraOpticsSettings Optics{};
    Optics.FocalLengthMillimetres = 35.0f;
    Optics.SensorWidthMillimetres = 36.0f;
    Optics.ApertureFNumber        = 2.8f;
    Optics.SubjectDistanceMetres  = 10.0f;
    const auto Eval = Frontier::EvaluateCameraOptics(Optics, 16.0f / 9.0f);
    Check(Eval.HorizontalFieldOfViewDegrees > 54.0f && Eval.HorizontalFieldOfViewDegrees < 55.0f, "35 mm on 36 mm full-frame yields ~54.4 deg horizontal FOV");
    const float RecoveredFocal = Frontier::ResolveFocalLengthFromVerticalFieldOfView(
        static_cast<double>(Eval.VerticalFieldOfViewDegrees) * (3.14159265358979323846 / 180.0), Optics.SensorWidthMillimetres, 16.0f / 9.0f);
    Check(Near(RecoveredFocal, 35.0, 1.0e-3), "ResolveFocalLengthFromVerticalFieldOfView inverts EvaluateCameraOptics");

    // 3. Default.levelspace Camera records & Lens Flare parented under Camera (not Solar)
    const std::filesystem::path ProjectRoot = ArgumentCount > 1 ? std::filesystem::path(Arguments[1]) : std::filesystem::path("Projects/Everest");
    Frontier::WorldSpace World;
    std::string          Diagnostic;
    Check(World.LoadLevelSpace(ProjectRoot / "Content/Levels/Default.levelspace", &Diagnostic),
          "Default.levelspace loads cleanly: " + Diagnostic);
    const auto Cameras = World.FindAllByType("Camera");
    Check(Cameras.size() == 2, "Default.levelspace contains both Editor Camera and Vehicle Camera records");

    const Frontier::CameraRecord* EditorCam  = nullptr;
    const Frontier::CameraRecord* VehicleCam = nullptr;
    for (const auto* Entry : Cameras)
    {
        const auto* Cam = dynamic_cast<const Frontier::CameraRecord*>(Entry);
        if (!Cam) continue;
        if (Cam->QueryConfiguration().Role == Frontier::CameraRole::Editor) EditorCam = Cam;
        if (Cam->QueryConfiguration().Role == Frontier::CameraRole::Vehicle) VehicleCam = Cam;
    }
    Check(EditorCam != nullptr && EditorCam->QueryConfiguration().Active, "Editor Camera is present and active by default");
    Check(VehicleCam != nullptr && !VehicleCam->QueryConfiguration().Active, "Vehicle Camera is present on standby");

    const auto* Solar = World.FindFirstByType("Solar");
    const auto* Flare = World.FindFirstByType("OpticalFlare");
    Check(Solar != nullptr && Flare != nullptr && EditorCam != nullptr, "Solar, OpticalFlare, and Editor Camera records exist");
    if (Solar != nullptr && Flare != nullptr && EditorCam != nullptr)
    {
        Check(Flare->QueryParentToken() == EditorCam->QueryToken() && Flare->QueryParentToken() != Solar->QueryToken(),
              "OpticalFlare (Lens Flare) is parented under Editor Camera, not Solar (Sun)");
    }

    // 4. CameraRecord TOML round-trip
    const std::filesystem::path TempLevel = std::filesystem::temp_directory_path() / "FrontierCameraRoundTrip.levelspace";
    if (VehicleCam != nullptr)
    {
        auto* MutableVehicleCam = dynamic_cast<Frontier::CameraRecord*>(World.FindRecord(VehicleCam->QueryToken()));
        MutableVehicleCam->AccessConfiguration().MotionBlur.Mode     = Frontier::CameraMotionBlurMode::Scene;
        MutableVehicleCam->AccessConfiguration().MotionBlur.Strength = 0.42f;
        MutableVehicleCam->AccessConfiguration().Optics.FocalLengthMillimetres = 50.0f;
    }
    Check(World.SaveLevelSpace(TempLevel, &Diagnostic), "WorldSpace saves Camera records to TOML");
    Frontier::WorldSpace Reloaded;
    Check(Reloaded.LoadLevelSpace(TempLevel, &Diagnostic), "WorldSpace reloads Camera records from TOML");
    bool FoundReloadedVehicleCam = false;
    for (const auto* Entry : Reloaded.FindAllByType("Camera"))
    {
        const auto* Cam = dynamic_cast<const Frontier::CameraRecord*>(Entry);
        if (Cam && Cam->QueryConfiguration().Role == Frontier::CameraRole::Vehicle)
        {
            FoundReloadedVehicleCam = Cam->QueryConfiguration().MotionBlur.Mode == Frontier::CameraMotionBlurMode::Scene &&
                                      Near(Cam->QueryConfiguration().MotionBlur.Strength, 0.42, 1.0e-4) &&
                                      Near(Cam->QueryConfiguration().Optics.FocalLengthMillimetres, 50.0, 1.0e-4);
        }
    }
    Check(FoundReloadedVehicleCam, "CameraRecord preserves role, thin-lens focal length, and 3-mode motion blur across TOML round-trip");
    std::error_code Ignore;
    std::filesystem::remove(TempLevel, Ignore);

    std::cout << "=== camera effects & records: " << Checks << " checks, " << Failures << " failures\n";
    return Failures == 0 ? 0 : 1;
}
