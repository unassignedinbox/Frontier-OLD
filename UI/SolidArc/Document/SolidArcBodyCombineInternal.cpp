/*==========================================================================================================================================
                                                   SOLID ARC BODY COMBINE INTERNAL
==========================================================================================================================================*/
// 🧩 Mutation half of the multi-body Boolean command, split into a compact translation unit for MSVC.

#include "UI/SolidArc/Document/SolidArcBodyCombineInternal.h"

#include <cstddef>
#include <string>
#include <utility>

namespace Frontier::SolidArc::Internal
{
namespace
{
[[nodiscard]] bool HasRegeneratedBody(const SolidArcDocument& Document, const std::string& Key)
{
    for (const GeometryProcessing::RegeneratedBody& Body : Document.Regeneration.Bodies)
        if (Body.Key == Key) return true;
    return false;
}
} // namespace

std::string ApplyCombineSolidArcBodies(SolidArcDocument& Document, const std::string& Target, const std::vector<std::string>& Tools,
                                       GeometryProcessing::FeatureCombination Combination, const std::string& Caption,
                                       int FirstStepOrdinal)
{
    if (Combination != GeometryProcessing::FeatureCombination::Union &&
        Combination != GeometryProcessing::FeatureCombination::Difference &&
        Combination != GeometryProcessing::FeatureCombination::Intersection)
        return "choose Union, Difference or Intersection";
    if (Tools.empty()) return "select two or more bodies (the first is the target)";
    if (!HasRegeneratedBody(Document, Target)) return "no body named " + Target;

    for (std::size_t ToolIndex = 0; ToolIndex < Tools.size(); ++ToolIndex)
    {
        const std::string& Tool = Tools[ToolIndex];
        if (!HasRegeneratedBody(Document, Tool)) return "no body named " + Tool;
        bool Duplicate = Tool == Target;
        for (std::size_t PriorIndex = 0; PriorIndex < ToolIndex && !Duplicate; ++PriorIndex)
            Duplicate = Tools[PriorIndex] == Tool;
        if (Duplicate)
        {
            std::string Refusal = SolidArcBodyCaption(Document, Tool);
            Refusal += " is picked twice (the target cannot be a tool)";
            return Refusal;
        }
    }

    for (std::size_t ToolIndex = 0; ToolIndex < Tools.size(); ++ToolIndex)
    {
        GeometryProcessing::FeatureStep Step;
        Step.Kind        = GeometryProcessing::FeatureStepKind::Combine;
        Step.Label       = "Combine#" + std::to_string(FirstStepOrdinal + static_cast<int>(ToolIndex));
        Step.Combination = Combination;
        Step.Target      = Target;
        Step.Tool        = Tools[ToolIndex];
        Document.Captions[Step.Label] = Caption;
        Document.Tree.Steps.push_back(std::move(Step));
    }
    Document.Selection = {};
    return {};
}

} // namespace Frontier::SolidArc::Internal
