/*==========================================================================================================================================
                                                    SOLID ARC BODY COMBINE COMMAND
==========================================================================================================================================*/
// 🧩 Public command entry kept in a compact translation unit to avoid MSVC C1001 in SolidArcDocument.cpp.

#include "UI/SolidArc/Document/SolidArcBodyCombineInternal.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace Frontier::SolidArc
{
namespace
{
[[nodiscard]] int NextCombineStepOrdinal(const GeometryProcessing::FeatureTree& Tree)
{
    int Largest = 0;
    for (const GeometryProcessing::FeatureStep& Step : Tree.Steps)
    {
        const std::size_t Mark = Step.Label.rfind('#');
        if (Mark != std::string::npos) Largest = std::max(Largest, std::atoi(Step.Label.c_str() + Mark + 1));
    }
    for (const GeometryProcessing::FeatureModifier& Modifier : Tree.Modifiers)
    {
        const std::size_t Mark = Modifier.Label.rfind('#');
        if (Mark != std::string::npos) Largest = std::max(Largest, std::atoi(Modifier.Label.c_str() + Mark + 1));
    }
    return Largest + 1;
}

[[nodiscard]] std::string NextCombineCaption(const SolidArcDocument& Document)
{
    for (int Ordinal = 1;; ++Ordinal)
    {
        const std::string Candidate = "Combine" + std::to_string(Ordinal);
        bool Used = false;
        for (const auto& Entry : Document.Captions)
        {
            if (Entry.second == Candidate)
            {
                Used = true;
                break;
            }
        }
        if (!Used) return Candidate;
    }
}
} // namespace

SolidArcCommandOutcome CombineSolidArcBodies(SolidArcDocument& Document, const std::string& Target,
                                              const std::vector<std::string>& Tools,
                                              GeometryProcessing::FeatureCombination Combination)
{
    std::string Operands = SolidArcBodyCaption(Document, Target);
    const char* Sign = Combination == GeometryProcessing::FeatureCombination::Difference ? " − " :
                       Combination == GeometryProcessing::FeatureCombination::Intersection ? " ∩ " : " ∪ ";
    for (const std::string& Tool : Tools)
    {
        Operands += Sign;
        Operands += SolidArcBodyCaption(Document, Tool);
    }

    std::string Label = "Combine · ";
    Label += GeometryProcessing::FeatureCombinationLabel(Combination);
    Label += " (";
    Label += Operands;
    Label += ')';
    const std::string Caption = NextCombineCaption(Document);
    const int FirstStepOrdinal = NextCombineStepOrdinal(Document.Tree);
    const auto Apply = [&]() {
        return Internal::ApplyCombineSolidArcBodies(Document, Target, Tools, Combination, Caption, FirstStepOrdinal);
    };
    return Internal::RunCombineCommand(Document, Label, Apply);
}

} // namespace Frontier::SolidArc
