/*==========================================================================================================================================
                                                        LOBBYSESSIONPOLICY.CPP
==========================================================================================================================================*/
// 🧩 Authored Everest session policies: 5-stand Studio Lobby showcase (Stand placement, brake hold) and 5-station Proving Grounds
//    training course (TerrainEquilibrium 4-corner slope placement, brake hold, including 12.68 deg uphill Jump Ramp station).

#include "Session/LobbySessionPolicy.h"

namespace Everest
{

const std::array<GuestPresetCallsign, 5>& QueryLobbyPresetCallsigns() noexcept
{
    static constexpr std::array<GuestPresetCallsign, 5> Callsigns = {{
        {"FALCON-1", "GOLD II"},
        {"KESTREL", "PLATINUM I"},
        {"HARRIER", "DIAMOND III"},
        {"MERLIN", "SILVER I"},
        {"PEREGRINE", "GOLD IV"},
    }};
    return Callsigns;
}

std::string_view QueryLobbyGuestRecordName(uint32_t SlotIndex) noexcept
{
    switch (SlotIndex)
    {
        case 1u: return "Lobby Slot 2";
        case 2u: return "Lobby Slot 3";
        case 3u: return "Lobby Slot 4";
        case 4u: return "Lobby Slot 5";
        default: return {};
    }
}

Frontier::SessionPolicy QueryLobbySessionPolicy()
{
    Frontier::SessionPolicy Policy{};
    Policy.Identifier              = "everest.lobby";
    Policy.Title                   = "Studio Lobby";
    Policy.LevelRelativePath       = "Content/Levels/Lobby.levelspace";
    Policy.EnvironmentArchive      = "Lobby";
    Policy.MaximumParticipants     = 5u;
    Policy.Placement               = Frontier::SessionPlacementKind::Stand;
    Policy.PhysicsSimulated        = false;
    Policy.EquilibriumPlacement    = false;
    Policy.EngageBrakesOnPlacement = true;
    Policy.RequireReadyGate        = false;
    Policy.CountdownSeconds        = 3.0;
    Policy.RoundDurationSeconds    = 0.0;
    Policy.Slots                   = {
        {{0.00f, 0.00f, 0.00f}, 0.0000f, "Turntable Hero"},
        {{7.42f, 7.42f, 0.53f}, -115.1645f, "Stand NE"},
        {{-7.42f, 7.42f, 0.53f}, -25.2101f, "Stand NW"},
        {{-7.42f, -7.42f, 0.53f}, 25.2101f, "Stand SW"},
        {{7.42f, -7.42f, 0.53f}, 115.1645f, "Stand SE"},
    };
    return Policy;
}

Frontier::SessionPolicy QueryProvingGroundSessionPolicy()
{
    Frontier::SessionPolicy Policy{};
    Policy.Identifier              = "everest.proving_ground";
    Policy.Title                   = "Proving Grounds";
    Policy.LevelRelativePath       = "Content/Levels/ProvingGroundVehicle.levelspace";
    Policy.EnvironmentArchive      = "ProvingGround";
    Policy.MaximumParticipants     = 5u;
    Policy.Placement               = Frontier::SessionPlacementKind::TerrainEquilibrium;
    Policy.PhysicsSimulated        = true;
    Policy.EquilibriumPlacement    = true;
    Policy.EngageBrakesOnPlacement = true;
    Policy.RequireReadyGate        = false;
    Policy.CountdownSeconds        = 0.0;
    Policy.RoundDurationSeconds    = 0.0;
    Policy.Slots                   = {
        {{-40.0f, 0.0f, 0.0f}, 0.0f, "Launch Straight"},
        {{23.6f, 0.0f, 0.36f}, 0.0f, "Slope Ramp (12.7 deg)"},
        {{-30.0f, 17.5f, 0.0f}, 0.0f, "Skidpad Apron"},
        {{78.0f, -35.0f, 0.0f}, 0.0f, "Slalom Approach"},
        {{34.0f, 24.0f, 0.0f}, -90.0f, "Service Hangar"},
    };
    return Policy;
}

} // namespace Everest
