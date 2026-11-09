/*==========================================================================================================================================
                                                       IMPACTSPARKSEQUENCE.H
==========================================================================================================================================*/
// 🧩 A hard Jolt contact emits a bounded packet of ballistic, cooling particles; their instances are world-space streaks.

#pragma once

#include "Frontier/Engine/Interface/SceneViewportPresentation.h"
#include "Frontier/Engine/Rendering/GlobalIllumination/ImpactSparkGpuRecord.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Frontier
{

struct ImpactSparkSequenceStatistics
{
    uint32_t ImpactsLastStep = 0;
    uint32_t EmittedLastStep = 0;
    uint32_t ActiveParticles = 0;
    uint64_t TotalEmitted = 0;
};

class ImpactSparkSequence
{
  public:
    // 📝 Advances existing filings, consumes only the newly drained Jolt events, and keeps at most ImpactSparkCapacity live particles.
    void Advance(double DeltaSeconds, std::span<const SceneImpactSpark> Impacts);
    [[nodiscard]] std::vector<GlobalIllumination::ImpactSparkGpuRecord> BuildGpuRecords() const;
    [[nodiscard]] const ImpactSparkSequenceStatistics& QueryStatistics() const noexcept { return Statistics_; }
    void Clear() noexcept;

  private:
    struct Particle
    {
        std::array<double, 3> Position{};
        std::array<double, 3> PreviousPosition{};
        std::array<double, 3> Velocity{};
        double Kelvin = 0.0;
        double AgeSeconds = 0.0;
        double LifeSeconds = 0.0;
        double Drag = 1.6;
        double Cooling = 1.0;
        double Size = 1.0;
        double Energy = 1.0;
        double Flicker = 0.0;
        double Frequency = 0.0;
        double Phase = 0.0;
        double PixelWidth = 1.0;
        double Shutter = 1.0;
    };

    void Emit(const SceneImpactSpark& Impact, double FrameSeconds);
    static void Integrate(Particle& Spark, double Seconds) noexcept;

    std::vector<Particle> Particles_{};
    ImpactSparkSequenceStatistics Statistics_{};
};

} // namespace Frontier
