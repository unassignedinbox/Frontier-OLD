/*==========================================================================================================================================
                                                       IMPACTSPARKVALIDATION.CPP
==========================================================================================================================================*/
// 🧩 Real Jolt hard contacts emit deterministic, bounded ballistic particles whose streak payload moves and cools over time.

#include "ImpactSparkFixture.h"
#include "Frontier/Engine/Rendering/GlobalIllumination/ImpactSparkGpuRecord.h"
#include "Frontier/Engine/Rendering/SceneViewport/ImpactSparkSequence.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{

bool Check(bool Passed, const char* Label)
{
    std::cout << (Passed ? "[PASS] " : "[FAIL] ") << Label << '\n';
    return Passed;
}

bool SameRecords(const std::vector<Frontier::GlobalIllumination::ImpactSparkGpuRecord>& A,
                 const std::vector<Frontier::GlobalIllumination::ImpactSparkGpuRecord>& B)
{
    if (A.size() != B.size()) return false;
    for (size_t Index = 0; Index < A.size(); ++Index)
        if (A[Index].HeadWidth != B[Index].HeadWidth || A[Index].TailPixelWidth != B[Index].TailPixelWidth ||
            A[Index].RadianceShutter != B[Index].RadianceShutter)
            return false;
    return true;
}

} // namespace

int main()
{
    using namespace Frontier;
    const std::vector<SceneImpactSpark> Impacts = FrontierExhibit::ConstructImpactReplay();
    bool Passed = true;
    Passed &= Check(Impacts.size() == 2, "the deterministic fixture produced real Jolt metal and concrete contacts");
    if (Impacts.size() != 2) return 1;
    Passed &= Check(Impacts[0].EventSeed != 0 && Impacts[1].EventSeed != 0 && Impacts[0].EventSeed != Impacts[1].EventSeed &&
                        Impacts[0].ClosingSpeedMps >= 4.5f && Impacts[1].ClosingSpeedMps >= 4.5f &&
                        std::abs(Impacts[0].RelativeVelocityMps[1]) > 1.0 && Impacts[0].Normal[2] > 0.7,
                    "Jolt supplies a seeded world contact, outward normal and point-relative launch velocity");
    std::cout << "Jolt contact velocity: (" << Impacts[0].RelativeVelocityMps[0] << ", " << Impacts[0].RelativeVelocityMps[1] << ", "
              << Impacts[0].RelativeVelocityMps[2] << ") m/s; closing " << Impacts[0].ClosingSpeedMps << " m/s\n";

    ImpactSparkSequence Sequence;
    Sequence.Advance(1.0 / 60.0, Impacts);
    const auto FirstFrame = Sequence.BuildGpuRecords();
    const auto FirstStats = Sequence.QueryStatistics();
    Passed &= Check(FirstStats.ImpactsLastStep == 2 && FirstStats.ActiveParticles == FirstFrame.size() &&
                        FirstFrame.size() >= 12 && FirstFrame.size() <= GlobalIllumination::ImpactSparkCapacity,
                    "one collision produces a bounded packet of moving spark particles, not one sprite");
    double MaximumStreakMetres = 0.0;
    for (const auto& Spark : FirstFrame)
    {
        const double Dx = static_cast<double>(Spark.HeadWidth[0] - Spark.TailPixelWidth[0]);
        const double Dy = static_cast<double>(Spark.HeadWidth[1] - Spark.TailPixelWidth[1]);
        const double Dz = static_cast<double>(Spark.HeadWidth[2] - Spark.TailPixelWidth[2]);
        MaximumStreakMetres = std::max(MaximumStreakMetres, std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz));
    }
    std::cout << "packet: " << FirstFrame.size() << " live filings; longest first-frame streak " << MaximumStreakMetres << " m\n";
    Passed &= Check(MaximumStreakMetres > 0.05, "the post-AA record contains a camera-independent motion segment, not a point sprite");

    const bool HasStreaks = std::all_of(FirstFrame.begin(), FirstFrame.end(), [](const auto& Spark) {
        const double Dx = static_cast<double>(Spark.HeadWidth[0] - Spark.TailPixelWidth[0]);
        const double Dy = static_cast<double>(Spark.HeadWidth[1] - Spark.TailPixelWidth[1]);
        const double Dz = static_cast<double>(Spark.HeadWidth[2] - Spark.TailPixelWidth[2]);
        return Spark.HeadWidth[3] > 0.0f && (Dx * Dx + Dy * Dy + Dz * Dz) > 1.0e-8 &&
               Spark.RadianceShutter[0] > 0.0f && Spark.RadianceShutter[1] > 0.0f &&
               Spark.RadianceShutter[2] >= 0.0f && Spark.RadianceShutter[2] < Spark.RadianceShutter[0];
    });
    Passed &= Check(HasStreaks, "GPU records carry per-particle motion tails, thin world widths and hot blackbody radiance");

    ImpactSparkSequence Repeat;
    Repeat.Advance(1.0 / 60.0, Impacts);
    Passed &= Check(SameRecords(FirstFrame, Repeat.BuildGpuRecords()), "the same Jolt contact seed reproduces the same particle packet");

    Sequence.Advance(1.0 / 60.0, {});
    const auto SecondFrame = Sequence.BuildGpuRecords();
    const bool MovedAndCooled = SecondFrame.size() == FirstFrame.size() && !SameRecords(FirstFrame, SecondFrame) &&
                                SecondFrame.front().HeadWidth != FirstFrame.front().HeadWidth &&
                                SecondFrame.front().RadianceShutter[0] < FirstFrame.front().RadianceShutter[0];
    Passed &= Check(MovedAndCooled, "ballistic integration advances the streaks and radiative cooling dims them");

    for (int Frame = 0; Frame < 24; ++Frame) Sequence.Advance(0.1, {});
    Passed &= Check(Sequence.QueryStatistics().ActiveParticles == 0 && Sequence.BuildGpuRecords().empty(),
                    "cooled or expired particles retire instead of leaving a persistent glow");

    std::vector<SceneImpactSpark> Burst(64, Impacts.front());
    for (size_t Index = 0; Index < Burst.size(); ++Index) Burst[Index].EventSeed += static_cast<uint32_t>(Index + 1);
    ImpactSparkSequence Bounded;
    Bounded.Advance(1.0 / 60.0, Burst);
    Passed &= Check(Bounded.QueryStatistics().ActiveParticles == GlobalIllumination::ImpactSparkCapacity &&
                        Bounded.BuildGpuRecords().size() == GlobalIllumination::ImpactSparkCapacity,
                    "simultaneous contacts remain within the 1024-particle GPU profile");

    return Passed ? 0 : 1;
}
