/*==========================================================================================================================================
                                                        CAMERAEFFECTS.H
==========================================================================================================================================*/
// 🧩 Camera-owned shake and motion-blur components shared by the viewport, vehicle camera and renderer.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Frontier
{

enum class CameraMotionBlurMode : uint8_t
{
    Off    = 0,
    Camera = 1,   // [-] camera-only vectors; moving objects stay sharp
    Scene  = 2,   // [-] camera + object vectors from the surface pass
};

// 📝 The renderer exposes exactly three authoring choices: Off, Camera, and Scene. Strength is independent of the choice.
struct CameraMotionBlurComponent
{
    CameraMotionBlurMode Mode     = CameraMotionBlurMode::Off;
    float                Strength = 0.35f;   // [0,1] scales the motion vector before the bounded five-tap resolve
};

// 📝 Encoded in the low byte of RefreshConstants.PresentationControl (mode: bits 0–1, strength: bits 2–7).
[[nodiscard]] inline uint32_t EncodeCameraMotionBlurControl(const CameraMotionBlurComponent& Component) noexcept
{
    uint32_t Mode = static_cast<uint32_t>(Component.Mode);
    if (Mode > static_cast<uint32_t>(CameraMotionBlurMode::Scene)) Mode = static_cast<uint32_t>(CameraMotionBlurMode::Off);
    if (Mode == static_cast<uint32_t>(CameraMotionBlurMode::Off)) return 0u;
    const double   Strength        = std::isfinite(Component.Strength) ? std::clamp(static_cast<double>(Component.Strength), 0.0, 1.0) : 0.0;
    const uint32_t EncodedStrength = static_cast<uint32_t>(std::floor(Strength * 63.0 + 0.5));
    return Mode | (EncodedStrength << 2u);
}

struct CameraShakeSettings
{
    bool     Enabled                   = true;
    uint32_t Seed                      = 0x4A6F7921u;
    float    AmplitudeMetres           = 0.0015f;   // [m] high-speed baseline; default motion is deliberately subtle
    float    FrequencyHz               = 15.0f;     // [Hz]
    float    AccelerationThresholdMps2 = 8.0f;      // [m/s²] impacts below this do not start the transient envelope
    float    AttackSeconds             = 0.04f;     // [s]
    float    ReleaseSeconds            = 0.24f;     // [s]
    float    ImpactMultiplier          = 2.0f;      // [-] multiplies the speed amplitude by the impact envelope
    float    MaximumOffsetMetres       = 0.006f;    // [m] hard cap across both lateral and vertical displacement
};

struct CameraShakeOffset
{
    std::array<double, 3> EyeMetres{};
    std::array<double, 3> TargetMetres{};
    double                Envelope = 0.0;   // [0,1]
};

// 📝 Stateful but deterministic per camera: velocity changes feed a smoothed impact envelope; seeded harmonics produce bounded
//    lateral/vertical motion without frame-rate-dependent random noise. The solver decides which camera rigs receive it.
class CameraShakeComponent
{
  public:
    [[nodiscard]] CameraShakeSettings&       AccessSettings() noexcept { return Settings_; }
    [[nodiscard]] const CameraShakeSettings& QuerySettings() const noexcept { return Settings_; }

    [[nodiscard]] CameraShakeOffset Advance(const std::array<double, 3>& VelocityMetresPerSecond,
                                            const std::array<double, 3>& CameraForward,
                                            const std::array<double, 3>& CameraUp,
                                            double                       DeltaSeconds) noexcept;
    void Reset() noexcept;

  private:
    CameraShakeSettings   Settings_{};
    bool                  PriorVelocityValid_ = false;
    std::array<double, 3> PriorVelocityMps_{};
    double                Envelope_     = 0.0;
    double                ClockSeconds_ = 0.0;
};

} // namespace Frontier
