/*==========================================================================================================================================
                                                          FLUIDSPACECODEC.H
==========================================================================================================================================*/
// 🧩 TOML serializer and parser for .fluidspace sidecars and upstream .gasscene.toml parity documents (SceneTomlCodec.js 1:1).

#pragma once

#include "Engine/PhysicalDynamics/FluidDynamics/FluidSceneSpecification.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Frontier::FluidDynamics
{

inline constexpr const char* FluidSpaceExtension = ".fluidspace";
inline constexpr const char* GasSceneExtension   = ".gasscene.toml";

[[nodiscard]] std::string WriteFluidSpaceToml(const FluidSceneDocument& Scene);
bool                      ReadFluidSpaceToml(std::string_view Content, FluidSceneDocument& OutScene, std::string* OutDiagnostic = nullptr);
bool                      SaveFluidSpaceFile(const std::filesystem::path& FilePath, const FluidSceneDocument& Scene,
                                             std::string* OutDiagnostic = nullptr);
bool                      LoadFluidSpaceFile(const std::filesystem::path& FilePath, FluidSceneDocument& OutScene,
                                             std::string* OutDiagnostic = nullptr);

} // namespace Frontier::FluidDynamics
