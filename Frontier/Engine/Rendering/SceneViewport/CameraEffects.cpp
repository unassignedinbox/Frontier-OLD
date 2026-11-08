/*==========================================================================================================================================
                                                       CAMERAEFFECTS.CPP
==========================================================================================================================================*/
// 🧩 Camera-owned shake and motion-blur components shared by the viewport, vehicle camera and renderer.

#include "CameraEffects.h"

#include <algorithm>
#include <cmath>

namespace Frontier
{
namespace
{
constexpr double TwoPi = 6.28318530717958647692;
using Vector3 = std::array<double, 3>;

[[nodiscard]] bool IsFinite(const Vector3& Value) noexcept
{
    return std::isfinite(Value[0]) && std::isfinite(Value[1]) && std::isfinite(Value[2]);
}

[[nodiscard]] double Length(const Vector3& Value) noexcept
{
    return std::sqrt(Value[0] * Value[0] + Value[1] * Value[1] + Value[2] * Value[2]);
}

[[nodiscard]] Vector3 Subtract(const Vector3& Left, const Vector3& Right) noexcept
{
    return {Left[0] - Right[0], Left[1] - Right[1], Left[2] - Right[2]};
}

[[nodiscard]] Vector3 Cross(const Vector3& Left, const Vector3& Right) noexcept
{
    return {Left[1] * Right[2] - Left[2] * Right[1], Left[2] * Right[0] - Left[0] * Right[2], Left[0] * Right[1] - Left[1] * Right[0]};
}

[[nodiscard]] Vector3 Scale(const Vector3& Value, double Factor) noexcept
{
    return {Value[0] * Factor, Value[1] * Factor, Value[2] * Factor};
}

[[nodiscard]] Vector3 Add(const Vector3& Left, const Vector3& Right) noexcept
{
    return {Left[0] + Right[0], Left[1] + Right[1], Left[2] + Right[2]};
}

[[nodiscard]] Vector3 NormalizeOr(const Vector3& Value, const Vector3& Fallback) noexcept
{
    const double Magnitude = Length(Value);
    return Magnitude > 1.0e-9 && std::isfinite(Magnitude) ? Scale(Value, 1.0 / Magnitude) : Fallback;
}

[[nodiscard]] double ResolvePhase(uint32_t Seed, uint32_t Salt) noexcept
{
    uint32_t Value = Seed ^ Salt;
    Value ^= Value >> 16u;
    Value *= 0x7FEB352Du;
    Value ^= Value >> 15u;
    Value *= 0x846CA68Bu;
    Value ^= Value >> 16u;
    return TwoPi * (static_cast<double>(Value) / 4294967296.0);
}

[[nodiscard]] double ClampFinite(float Value, double Minimum, double Maximum, double Fallback) noexcept
{
    return std::isfinite(Value) ? std::clamp(static_cast<double>(Value), Minimum, Maximum) : Fallback;
}
} // namespace

CameraShakeOffset CameraShakeComponent::Advance(const std::array<double, 3>& VelocityMetresPerSecond,
                                                 const std::array<double, 3>& CameraForward,
                                                 const std::array<double, 3>& CameraUp,
                                                 double DeltaSeconds) noexcept
{
    CameraShakeOffset Result{};
    if (!Settings_.Enabled || !IsFinite(VelocityMetresPerSecond) || !IsFinite(CameraForward) || !IsFinite(CameraUp) || !std::isfinite(DeltaSeconds))
    {
        Reset();
        return Result;
    }

    const double Dt = std::clamp(DeltaSeconds, 0.0, 0.25);
    const double Amplitude = ClampFinite(Settings_.AmplitudeMetres, 0.0, 0.02, 0.0);
    const double Frequency = ClampFinite(Settings_.FrequencyHz, 1.0, 60.0, 15.0);
    const double Threshold = ClampFinite(Settings_.AccelerationThresholdMps2, 0.1, 100.0, 8.0);
    const double Attack = ClampFinite(Settings_.AttackSeconds, 0.005, 1.0, 0.04);
    const double Release = ClampFinite(Settings_.ReleaseSeconds, 0.02, 2.0, 0.24);
    const double ImpactMultiplier = ClampFinite(Settings_.ImpactMultiplier, 0.0, 6.0, 2.0);
    const double MaximumOffset = ClampFinite(Settings_.MaximumOffsetMetres, 0.0, 0.02, 0.006);

    double TargetEnvelope = 0.0;
    if (PriorVelocityValid_ && Dt > 1.0e-4)
    {
        const double Acceleration = Length(Subtract(VelocityMetresPerSecond, PriorVelocityMps_)) / Dt;
        const double ResponseRange = std::max(Threshold * 3.0, 18.0);
        TargetEnvelope = std::clamp((Acceleration - Threshold) / ResponseRange, 0.0, 1.0);
    }

    const double EnvelopeTime = TargetEnvelope > Envelope_ ? Attack : Release;
    const double EnvelopeBlend = 1.0 - std::exp(-Dt / EnvelopeTime);
    Envelope_ += (TargetEnvelope - Envelope_) * EnvelopeBlend;
    Envelope_ = std::clamp(Envelope_, 0.0, 1.0);
    ClockSeconds_ += Dt;
    PriorVelocityMps_ = VelocityMetresPerSecond;
    PriorVelocityValid_ = true;

    const double Speed = Length(VelocityMetresPerSecond);
    const double SpeedFraction = std::clamp(Speed / 35.0, 0.0, 1.0);
    const double SmoothSpeed = SpeedFraction * SpeedFraction * (3.0 - 2.0 * SpeedFraction);
    const double Displacement = std::min(MaximumOffset, Amplitude * (SmoothSpeed + ImpactMultiplier * Envelope_));
    Result.Envelope = Envelope_;
    if (Displacement <= 0.0) return Result;

    const Vector3 Forward = NormalizeOr(CameraForward, {1.0, 0.0, 0.0});
    Vector3 Right = Cross(Forward, NormalizeOr(CameraUp, {0.0, 0.0, 1.0}));
    if (Length(Right) <= 1.0e-9)
    {
        const Vector3 FallbackUp = std::abs(Forward[2]) < 0.9 ? Vector3{0.0, 0.0, 1.0} : Vector3{0.0, 1.0, 0.0};
        Right = Cross(Forward, FallbackUp);
    }
    Right = NormalizeOr(Right, {0.0, -1.0, 0.0});
    const Vector3 Up = NormalizeOr(Cross(Right, Forward), {0.0, 0.0, 1.0});

    const double Phase = TwoPi * Frequency * ClockSeconds_;
    const double NoiseRight = 0.62 * std::sin(Phase + ResolvePhase(Settings_.Seed, 0x1A2B3C4Du)) +
                              0.38 * std::sin(Phase * 1.73 + ResolvePhase(Settings_.Seed, 0x51ED270Bu));
    const double NoiseUp = 0.62 * std::sin(Phase * 1.19 + ResolvePhase(Settings_.Seed, 0x6C8E9CF5u)) +
                           0.38 * std::sin(Phase * 2.07 + ResolvePhase(Settings_.Seed, 0xB5297A4Du));
    const double NoiseMagnitude = std::sqrt(NoiseRight * NoiseRight + NoiseUp * NoiseUp);
    const double NoiseScale = NoiseMagnitude > 1.0 ? 1.0 / NoiseMagnitude : 1.0;
    const Vector3 EyeOffset = Scale(Add(Scale(Right, NoiseRight * NoiseScale), Scale(Up, NoiseUp * NoiseScale)), Displacement);
    Result.EyeMetres = EyeOffset;
    Result.TargetMetres = Scale(EyeOffset, 0.30);
    return Result;
}

void CameraShakeComponent::Reset() noexcept
{
    PriorVelocityValid_ = false;
    PriorVelocityMps_ = {};
    Envelope_ = 0.0;
    ClockSeconds_ = 0.0;
}

} // namespace Frontier
