/*==========================================================================================================================================
                                                   SOLID ARC BODY COMBINE INTERNAL
==========================================================================================================================================*/
// 🧩 Split helpers for the multi-body Boolean command, kept out of SolidArcDocument.cpp to avoid MSVC C1001 in that large translation unit.

#pragma once

#include "UI/SolidArc/Document/SolidArcDocument.h"

#include <functional>

namespace Frontier::SolidArc::Internal
{

[[nodiscard]] SolidArcCommandOutcome RunCombineCommand(SolidArcDocument& Document, const std::string& Label,
                                                        const std::function<std::string()>& Apply);

[[nodiscard]] std::string ApplyCombineSolidArcBodies(SolidArcDocument& Document, const std::string& Target,
                                                      const std::vector<std::string>& Tools,
                                                      GeometryProcessing::FeatureCombination Combination,
                                                      const std::string& Caption, int FirstStepOrdinal);

} // namespace Frontier::SolidArc::Internal
