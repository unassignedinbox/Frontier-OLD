/*==========================================================================================================================================
                                                   CAMERAEFFECTSVALIDATION.CPP
==========================================================================================================================================*/
// 🧩 CPU validation for native camera shake and motion-blur controls (Exhibits/SceneViewport/README.md).

#include "Frontier/Engine/Rendering/SceneViewport/CameraEffects.h"
#include "Frontier/Engine/Rendering/SceneViewport/VehicleCameraSolver.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace
{
using Vector3 = std::array<double, 3>;

[[nodiscard]] double Magnitude(const Vector3& Value)
{
    return std::sqrt(Value[0] * Value[0] + Value[1] * Value[1] + Value[2] * Value[2]);
}

[[nodiscard]] bool Near(double A, double B, double Tolerance = 1.0e-6)
{
    return std::abs(A - B) <= Tolerance;
}
} // namespace

int main()
{
    uint32_t Checks = 0;
    uint32_t Failures = 0;
    const auto Check = [&](bool Passed, std::string_view Name) {
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

    Frontier::CameraShakeComponent Defaults;
    const Frontier::CameraShakeSettings& DefaultSettings = Defaults.QuerySettings();
    Check(DefaultSettings.Enabled, "camera shake is enabled by default");
    Check(Near(DefaultSettings.AmplitudeMetres, 0.0015), "default shake amplitude is subtle");
    Check(Near(DefaultSettings.MaximumOffsetMetres, 0.006), "default shake displacement is capped at six millimetres");

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
    Blur.Mode = Frontier::CameraMotionBlurMode::Scene;
    Blur.Strength = 2.0f;
    Check((Frontier::EncodeCameraMotionBlurControl(Blur) >> 2u) == 63u, "motion-blur strength is clamped before quantization");

    Frontier::CameraShakeComponent Disabled;
    Disabled.AccessSettings().Enabled = false;
    const auto Zero = Disabled.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(Magnitude(Zero.EyeMetres) == 0.0 && Zero.Envelope == 0.0, "disabled shake returns no camera offset");

    Frontier::CameraShakeComponent First;
    Frontier::CameraShakeComponent Second;
    First.AccessSettings().Seed = 12345u;
    Second.AccessSettings().Seed = 12345u;
    (void)First.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    (void)Second.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto FirstOffset = First.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto SecondOffset = Second.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(FirstOffset.EyeMetres == SecondOffset.EyeMetres, "identical seeds produce deterministic motion");

    Frontier::CameraShakeComponent DifferentSeed;
    DifferentSeed.AccessSettings().Seed = 67890u;
    (void)DifferentSeed.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto DifferentOffset = DifferentSeed.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(DifferentOffset.EyeMetres != FirstOffset.EyeMetres, "changing the seed changes the deterministic phase");
    Frontier::CameraShakeComponent Baseline;
    (void)Baseline.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto BaselineOffset = Baseline.Advance({35.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(Magnitude(BaselineOffset.EyeMetres) <= 0.0015001, "high-speed baseline remains within its configured amplitude");

    Frontier::CameraShakeComponent Transient;
    Transient.AccessSettings().AccelerationThresholdMps2 = 8.0f;
    (void)Transient.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto Impact = Transient.Advance({30.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(Impact.Envelope > 0.0 && Impact.Envelope < 1.0, "acceleration triggers a smoothed, bounded attack");
    double ReleasedEnvelope = Impact.Envelope;
    for (int Frame = 0; Frame < 90; ++Frame)
        ReleasedEnvelope = Transient.Advance({30.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0).Envelope;
    Check(ReleasedEnvelope < Impact.Envelope * 0.02, "impact envelope releases smoothly back to baseline");

    Frontier::CameraShakeComponent Bounded;
    Frontier::CameraShakeSettings& BoundSettings = Bounded.AccessSettings();
    BoundSettings.AmplitudeMetres = 0.02f;
    BoundSettings.MaximumOffsetMetres = 0.006f;
    BoundSettings.AccelerationThresholdMps2 = 0.1f;
    BoundSettings.AttackSeconds = 0.005f;
    BoundSettings.ImpactMultiplier = 6.0f;
    (void)Bounded.Advance({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    const auto HardImpact = Bounded.Advance({100.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(Magnitude(HardImpact.EyeMetres) <= 0.0060001, "combined impact and speed shake obey the hard offset cap");
    Bounded.Reset();
    const auto AfterReset = Bounded.Advance({100.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.0 / 60.0);
    Check(AfterReset.Envelope == 0.0, "reset clears prior velocity and transient state");

    Frontier::VehicleSubjectKinematics Subject;
    Subject.Available = true;
    Subject.PositionMetres = {0.0, 0.0, 0.525};
    Subject.ForwardUnit = {1.0, 0.0, 0.0};
    Subject.LeftUnit = {0.0, 1.0, 0.0};
    Subject.UpUnit = {0.0, 0.0, 1.0};
    Subject.SpeedMetresPerSecond = 35.0;
    Subject.VelocityMetresPerSec = {35.0, 0.0, 0.0};
    Frontier::VehicleCameraSolver ShakenSolver;
    Frontier::VehicleCameraSolver StableSolver;
    StableSolver.AccessCameraShake().AccessSettings().Enabled = false;
    ShakenSolver.ConfigureEnvironment(false, Subject);
    StableSolver.ConfigureEnvironment(false, Subject);
    const auto ShakenPose = ShakenSolver.Advance(Subject, {}, 1.0 / 60.0);
    const auto StablePose = StableSolver.Advance(Subject, {}, 1.0 / 60.0);
    const Vector3 SolverDelta = {ShakenPose.PositionMetres[0] - StablePose.PositionMetres[0],
                                 ShakenPose.PositionMetres[1] - StablePose.PositionMetres[1],
                                 ShakenPose.PositionMetres[2] - StablePose.PositionMetres[2]};
    Check(Magnitude(SolverDelta) > 0.0001 && Magnitude(SolverDelta) <= 0.0060001,
          "VehicleCameraSolver applies the native bounded camera-shake component");

    std::cout << "=== camera effects: " << Checks << " checks, " << Failures << " failures\n";
    return Failures == 0 ? 0 : 1;
}
