/*==========================================================================================================================================
                                                      IMPACTSPARKSEQUENCE.CPP
==========================================================================================================================================*/
// 🧩 Ported impact-particle core: packets follow the contact's tangential velocity, fan off the surface, fall, drag and cool.

#include "ImpactSparkSequence.h"

#include <algorithm>
#include <cmath>

namespace Frontier
{

namespace
{

using Vector = std::array<double, 3>;
constexpr double Gravity = 9.81;              // [m/s²] z-up world
constexpr double CoolingSeconds = 0.42;       // [s] excess-temperature e-folding
constexpr double VisibleKelvin = 950.0;       // [K]
constexpr double AmbientKelvin = 293.0;        // [K]
constexpr double SparkWidthMetres = 0.0022;    // [m] fine hot filings stay hair-thin on screen
constexpr double TwoPi = 6.2831853071795864769;

Vector Add(const Vector& A, const Vector& B) noexcept
{
    return {A[0] + B[0], A[1] + B[1], A[2] + B[2]};
}

Vector Subtract(const Vector& A, const Vector& B) noexcept
{
    return {A[0] - B[0], A[1] - B[1], A[2] - B[2]};
}

Vector Multiply(const Vector& A, double Scalar) noexcept
{
    return {A[0] * Scalar, A[1] * Scalar, A[2] * Scalar};
}

double Dot(const Vector& A, const Vector& B) noexcept
{
    return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
}

double Length(const Vector& A) noexcept
{
    return std::hypot(A[0], A[1], A[2]);
}

Vector Normalize(const Vector& A, const Vector& Fallback) noexcept
{
    const double Magnitude = Length(A);
    return Magnitude > 1.0e-9 && std::isfinite(Magnitude) ? Multiply(A, 1.0 / Magnitude) : Fallback;
}

bool IsFinite(const Vector& Value) noexcept
{
    return std::isfinite(Value[0]) && std::isfinite(Value[1]) && std::isfinite(Value[2]);
}

uint32_t Hash(uint32_t Value) noexcept
{
    Value ^= Value >> 16u;
    Value *= 0x7FEB352Du;
    Value ^= Value >> 15u;
    Value *= 0x846CA68Bu;
    Value ^= Value >> 16u;
    return Value;
}

uint32_t SeedFor(const SceneImpactSpark& Impact) noexcept
{
    uint32_t State = Impact.EventSeed != 0 ? Impact.EventSeed : 0xA341316Cu;
    const auto Mix = [&State](double Value) {
        // Quantized world inputs keep hand-authored/legacy events deterministic without hashing NaN payloads.
        const double Clamped = std::clamp(Value, -1.0e6, 1.0e6);
        const int64_t Quantized = static_cast<int64_t>(std::llround(Clamped * 1000.0));
        State = Hash(State ^ static_cast<uint32_t>(Quantized));
        State = Hash(State ^ static_cast<uint32_t>(static_cast<uint64_t>(Quantized) >> 32u));
    };
    for (const double Value : Impact.PointMetres) Mix(Value);
    for (const double Value : Impact.RelativeVelocityMps) Mix(Value);
    return State != 0 ? State : 1u;
}

uint32_t RandomWord(uint32_t& State) noexcept
{
    State = State * 747796405u + 2891336453u;
    const uint32_t Word = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
    return (Word >> 22u) ^ Word;
}

double Random01(uint32_t& State) noexcept
{
    return static_cast<double>(RandomWord(State)) / 4294967296.0;
}

Vector RandomTangent(uint32_t& State, const Vector& Normal) noexcept
{
    Vector Candidate{Random01(State) * 2.0 - 1.0, Random01(State) * 2.0 - 1.0, Random01(State) * 2.0 - 1.0};
    Candidate = Subtract(Candidate, Multiply(Normal, Dot(Candidate, Normal)));
    if (Length(Candidate) < 1.0e-4)
    {
        const Vector Axis = std::abs(Normal[2]) < 0.8 ? Vector{0.0, 0.0, 1.0} : Vector{1.0, 0.0, 0.0};
        Candidate = {Normal[1] * Axis[2] - Normal[2] * Axis[1], Normal[2] * Axis[0] - Normal[0] * Axis[2],
                     Normal[0] * Axis[1] - Normal[1] * Axis[0]};
    }
    return Normalize(Candidate, {1.0, 0.0, 0.0});
}

std::array<double, 3> ResolveBlackbodyRadiance(double Kelvin) noexcept
{
    const double Hecto = std::clamp(Kelvin, 1000.0, 6600.0) / 100.0;
    const double Green = std::clamp((99.4708025861 * std::log(Hecto) - 161.1195681661) / 255.0, 0.0, 1.0);
    const double Blue = Hecto <= 19.0 ? 0.0 : std::clamp((138.5177312231 * std::log(std::max(Hecto - 10.0, 1.0)) - 305.0447927307) / 255.0,
                                                         0.0, 1.0);
    const double Ratio = Kelvin / 1500.0;
    const double Brightness = 8.0 * Ratio * Ratio * Ratio * Ratio * std::clamp((Kelvin - VisibleKelvin) / 500.0, 0.0, 1.0);
    return {Brightness, std::pow(Green, 2.2) * Brightness, std::pow(Blue, 2.2) * Brightness};
}

} // namespace

void ImpactSparkSequence::Advance(double DeltaSeconds, std::span<const SceneImpactSpark> Impacts)
{
    const double FrameSeconds = std::isfinite(DeltaSeconds) ? std::clamp(DeltaSeconds, 0.0, 0.1) : 0.0;
    Statistics_.ImpactsLastStep = 0;
    Statistics_.EmittedLastStep = 0;

    for (Particle& Spark : Particles_)
    {
        Spark.PreviousPosition = Spark.Position;
        Integrate(Spark, FrameSeconds);
    }
    std::erase_if(Particles_, [](const Particle& Spark) {
        return Spark.AgeSeconds >= Spark.LifeSeconds || Spark.Kelvin <= VisibleKelvin || !IsFinite(Spark.Position) || !IsFinite(Spark.Velocity);
    });

    for (const SceneImpactSpark& Impact : Impacts)
        Emit(Impact, FrameSeconds);

    Statistics_.ActiveParticles = static_cast<uint32_t>(Particles_.size());
    Statistics_.TotalEmitted += Statistics_.EmittedLastStep;
}

void ImpactSparkSequence::Emit(const SceneImpactSpark& Impact, double FrameSeconds)
{
    if (Particles_.size() >= GlobalIllumination::ImpactSparkCapacity || !IsFinite(Impact.PointMetres) || !IsFinite(Impact.Normal) ||
        !IsFinite(Impact.RelativeVelocityMps) || !std::isfinite(Impact.ClosingSpeedMps) || !(Impact.ClosingSpeedMps >= 4.5f))
        return;

    const Vector Normal = Normalize(Impact.Normal, {0.0, 0.0, 1.0});
    Vector RelativeVelocity = Impact.RelativeVelocityMps;
    const double ContactSpeed = Length(RelativeVelocity);
    if (!std::isfinite(ContactSpeed)) return;
    // 📝 A high but finite guard keeps malformed/outlier contact data from overflowing the integrator or count conversion.
    constexpr double MaximumContactSpeed = 120.0;
    if (ContactSpeed > MaximumContactSpeed) RelativeVelocity = Multiply(RelativeVelocity, MaximumContactSpeed / ContactSpeed);
    const Vector TangentialVelocity = Subtract(RelativeVelocity, Multiply(Normal, Dot(RelativeVelocity, Normal)));
    const double SlideSpeed = Length(TangentialVelocity);
    const double ImpactSpeed = std::clamp(std::max(static_cast<double>(Impact.ClosingSpeedMps), 0.65 * std::min(ContactSpeed, MaximumContactSpeed)),
                                          4.5, MaximumContactSpeed);
    const int RequestedCount = std::clamp(static_cast<int>(std::lround(8.0 + (ImpactSpeed - 4.5) * 2.7)), 8, 64);
    const double SurfaceGain = Impact.Surface == SceneImpactSurface::Concrete ? 0.70 : 1.0;
    const int Count = std::min(static_cast<int>(GlobalIllumination::ImpactSparkCapacity - Particles_.size()),
                               std::max(6, static_cast<int>(std::lround(RequestedCount * SurfaceGain))));
    if (Count <= 0) return;

    ++Statistics_.ImpactsLastStep;
    uint32_t State = SeedFor(Impact);
    for (int Index = 0; Index < Count; ++Index)
    {
        const double KindRoll = Random01(State);
        const int Kind = KindRoll < 0.72 ? 0 : (KindRoll < 0.94 ? 1 : 2);  // fine filings, medium streaks, heavier chips
        const double Size = Kind == 0 ? 0.25 + 0.50 * Random01(State) : (Kind == 1 ? 0.70 + 0.60 * Random01(State) : 1.30 + 0.70 * Random01(State));
        const double SpeedShare = Kind == 2 ? 0.35 + 0.40 * Random01(State) : 0.65 + 0.55 * Random01(State);
        const Vector Fan = RandomTangent(State, Normal);
        const Vector SideJitter = Multiply(RandomTangent(State, Normal), (Random01(State) - 0.5) * 0.04);
        const Vector Origin = Add(Add(Impact.PointMetres, Multiply(Normal, 0.01 + Random01(State) * 0.02)), SideJitter);
        const double Share = 0.30 + 0.45 * Random01(State);
        const double FanSpeed = 0.60 * std::max(2.0, SlideSpeed * 0.35);
        const double Kick = 1.0 + 3.0 * Random01(State);
        const Vector LaunchVelocity = Multiply(Add(Add(Multiply(TangentialVelocity, Share), Multiply(Fan, FanSpeed)), Multiply(Normal, Kick)), SpeedShare);

        Particle Spark;
        Spark.Position = Spark.PreviousPosition = Origin;
        Spark.Velocity = LaunchVelocity;
        Spark.Size = Size;
        Spark.Kelvin = Kind == 2 ? 1800.0 + 800.0 * Random01(State) : 2100.0 + 900.0 * Random01(State);
        Spark.Drag = Kind == 0 ? 2.0 + 2.5 * Random01(State) : 0.65 + 1.2 * Random01(State);
        Spark.Cooling = Kind == 0 ? 0.55 + 0.35 * Random01(State) :
                        (Kind == 1 ? 0.85 + 0.40 * Random01(State) : 1.20 + 0.50 * Random01(State));
        Spark.LifeSeconds = Kind == 0 ? 0.20 + 0.40 * Random01(State) :
                            (Kind == 1 ? 0.45 + 0.75 * Random01(State) : 0.90 + Random01(State));
        Spark.Energy = SurfaceGain * (0.35 + 0.85 * Random01(State));
        Spark.Flicker = 0.04 + 0.12 * Random01(State);
        Spark.Frequency = 10.0 + 18.0 * Random01(State);
        Spark.Phase = Random01(State) * TwoPi;
        Spark.PixelWidth = Kind == 0 ? 0.40 + 0.35 * Random01(State) :
                           (Kind == 1 ? 0.70 + 0.25 * Random01(State) : 0.90 + 0.10 * Random01(State));
        Spark.Shutter = Kind == 0 ? 0.18 + 0.45 * Random01(State) : 0.55 + 0.45 * Random01(State);

        // 📝 Emission is continuous through the physics frame: age and integrate only the spark's birth-frame remainder.
        Integrate(Spark, FrameSeconds * (0.25 + 0.75 * Random01(State)));
        if (Spark.Kelvin > VisibleKelvin && Spark.AgeSeconds < Spark.LifeSeconds && IsFinite(Spark.Position) && IsFinite(Spark.Velocity))
        {
            Particles_.push_back(Spark);
            ++Statistics_.EmittedLastStep;
        }
    }
}

void ImpactSparkSequence::Integrate(Particle& Spark, double Seconds) noexcept
{
    if (!(Seconds > 0.0)) return;
    const uint32_t Steps = std::max(1u, static_cast<uint32_t>(std::ceil(Seconds * 120.0)));
    const double StepSeconds = Seconds / Steps;
    for (uint32_t Step = 0; Step < Steps; ++Step)
    {
        Spark.AgeSeconds += StepSeconds;
        const double Damping = std::exp(-Spark.Drag * StepSeconds);
        Spark.Velocity = Multiply(Spark.Velocity, Damping);
        Spark.Velocity[2] -= Gravity * StepSeconds;
        Spark.Position = Add(Spark.Position, Multiply(Spark.Velocity, StepSeconds));
        Spark.Kelvin = AmbientKelvin + (Spark.Kelvin - AmbientKelvin) *
                       std::exp(-StepSeconds / (CoolingSeconds * Spark.Cooling * std::max(0.5, Spark.Size)));
    }
}

std::vector<GlobalIllumination::ImpactSparkGpuRecord> ImpactSparkSequence::BuildGpuRecords() const
{
    std::vector<GlobalIllumination::ImpactSparkGpuRecord> Records;
    Records.reserve(Particles_.size());
    for (const Particle& Spark : Particles_)
    {
        if (Spark.AgeSeconds >= Spark.LifeSeconds || Spark.Kelvin <= VisibleKelvin) continue;
        const double Fade = std::clamp((Spark.LifeSeconds - Spark.AgeSeconds) / 0.10, 0.0, 1.0);
        const double Ripple = 1.0 - Spark.Flicker * (0.5 + 0.5 * std::sin(Spark.AgeSeconds * Spark.Frequency * TwoPi + Spark.Phase));
        const auto Blackbody = ResolveBlackbodyRadiance(Spark.Kelvin);
        const double Gain = Spark.Energy * Fade * Ripple;

        GlobalIllumination::ImpactSparkGpuRecord Record;
        Record.HeadWidth = {static_cast<float>(Spark.Position[0]), static_cast<float>(Spark.Position[1]), static_cast<float>(Spark.Position[2]),
                            static_cast<float>(SparkWidthMetres * Spark.Size)};
        Record.TailPixelWidth = {static_cast<float>(Spark.PreviousPosition[0]), static_cast<float>(Spark.PreviousPosition[1]),
                                 static_cast<float>(Spark.PreviousPosition[2]), static_cast<float>(Spark.PixelWidth)};
        Record.RadianceShutter = {static_cast<float>(Blackbody[0] * Gain), static_cast<float>(Blackbody[1] * Gain),
                                  static_cast<float>(Blackbody[2] * Gain), static_cast<float>(Spark.Shutter)};
        Records.push_back(Record);
    }
    return Records;
}

void ImpactSparkSequence::Clear() noexcept
{
    Particles_.clear();
    Statistics_ = {};
}

} // namespace Frontier
