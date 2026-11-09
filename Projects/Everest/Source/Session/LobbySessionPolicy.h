/*==========================================================================================================================================
                                                         LOBBYSESSIONPOLICY.H
==========================================================================================================================================*/
// 🧩 Authored Everest session policies: 5-stand Studio Lobby showcase and 5-slot Proving Grounds driving course.

#pragma once

#include "Frontier/Engine/Session/SessionPolicy.h"

#include <array>
#include <string_view>

namespace Everest
{

struct GuestPresetCallsign
{
    std::string_view Callsign;
    std::string_view RankBadge;
};

[[nodiscard]] Frontier::SessionPolicy                  QueryLobbySessionPolicy();
[[nodiscard]] Frontier::SessionPolicy                  QueryProvingGroundSessionPolicy();
[[nodiscard]] const std::array<GuestPresetCallsign, 5>& QueryLobbyPresetCallsigns() noexcept;
[[nodiscard]] std::string_view                         QueryLobbyGuestRecordName(uint32_t SlotIndex) noexcept;

} // namespace Everest
