/*==========================================================================================================================================
                                                           LOBBYSESSION.CPP
==========================================================================================================================================*/
// 🧩 Dynamic 1-to-5 vehicle Lobby session coordinator synchronizing SessionSpace roster slots onto Everest WorldSpace records.

#include "Session/LobbySession.h"

#if FRONTIER_EDITOR
#include "Engine/Scene/SceneDirectory.h"
#endif

#include <algorithm>

namespace Everest
{

LobbySession::LobbySession()
    : Session(QueryLobbySessionPolicy())
{
    const auto& Presets = QueryLobbyPresetCallsigns();
    if (const auto Joined = Session.JoinParticipant(Presets[0].Callsign, Presets[0].RankBadge, true))
    {
        LocalDriverIdentifier = *Joined;
    }
}

void LobbySession::ResetSolo(Frontier::WorldSpace& World)
{
    Session.ConfigurePolicy(QueryLobbySessionPolicy());
    ActiveStationIndex    = 0u;
    const auto& Presets   = QueryLobbyPresetCallsigns();
    LocalDriverIdentifier = Session.JoinParticipant(Presets[0].Callsign, Presets[0].RankBadge, true).value_or(0u);
    (void)SynchronizeWorldVisibility(World);
}

std::optional<uint64_t> LobbySession::JoinGuestDriver(Frontier::WorldSpace& World,
                                                      std::string_view      CustomCallsign,
                                                      std::string_view      CustomRankBadge)
{
    if (Session.QueryParticipantCount() >= Session.QueryPolicy().MaximumParticipants)
    {
        return std::nullopt;
    }

    uint32_t NextFreeSlot = 1u;
    for (uint32_t Slot = 1u; Slot < Session.QueryPolicy().MaximumParticipants; ++Slot)
    {
        if (Session.FindParticipantInSlot(Slot) == nullptr)
        {
            NextFreeSlot = Slot;
            break;
        }
    }
    const auto&                Presets   = QueryLobbyPresetCallsigns();
    const GuestPresetCallsign& Preset    = Presets[std::min<size_t>(NextFreeSlot, Presets.size() - 1u)];
    const std::string_view     Callsign  = CustomCallsign.empty() ? Preset.Callsign : CustomCallsign;
    const std::string_view     RankBadge = CustomRankBadge.empty() ? Preset.RankBadge : CustomRankBadge;

    const auto Identifier = Session.JoinParticipant(Callsign, RankBadge, false);
    if (Identifier.has_value())
    {
        (void)Session.ConfigureParticipantReady(*Identifier, true);
        (void)SynchronizeWorldVisibility(World);
    }
    return Identifier;
}

bool LobbySession::LeaveGuestBySlot(Frontier::WorldSpace& World, uint32_t SlotIndex)
{
    if (SlotIndex == 0u)
    {
        return false;
    }
    const Frontier::SessionParticipant* Occupant = Session.FindParticipantInSlot(SlotIndex);
    if (Occupant == nullptr)
    {
        return false;
    }
    const bool Removed = Session.RemoveParticipant(Occupant->Identifier);
    if (Removed)
    {
        (void)SynchronizeWorldVisibility(World);
    }
    return Removed;
}

bool LobbySession::LeaveLastGuest(Frontier::WorldSpace& World)
{
    const auto& Roster = Session.QueryParticipants();
    for (size_t Index = Roster.size(); Index > 0u; --Index)
    {
        const Frontier::SessionParticipant& Entry = Roster[Index - 1u];
        if (!Entry.LocalDriver && Entry.SlotIndex > 0u)
        {
            const bool Removed = Session.RemoveParticipant(Entry.Identifier);
            if (Removed)
            {
                (void)SynchronizeWorldVisibility(World);
            }
            return Removed;
        }
    }
    return false;
}

void LobbySession::ConfigureTargetDriverCount(Frontier::WorldSpace& World, uint32_t DesiredCount)
{
    const uint32_t Clamped = std::clamp(DesiredCount, 1u, Session.QueryPolicy().MaximumParticipants);
    while (Session.QueryParticipantCount() < Clamped)
    {
        if (!JoinGuestDriver(World).has_value())
        {
            break;
        }
    }
    while (Session.QueryParticipantCount() > Clamped)
    {
        if (!LeaveLastGuest(World))
        {
            break;
        }
    }
    (void)SynchronizeWorldVisibility(World);
}

bool LobbySession::DeployProvingGroundStation(Frontier::WorldSpace& World, uint32_t StationIndex)
{
    const Frontier::SessionPolicy Policy = QueryProvingGroundSessionPolicy();
    if (Policy.Slots.empty())
    {
        return false;
    }
    const uint32_t ClampedStation = std::min<uint32_t>(StationIndex, static_cast<uint32_t>(Policy.Slots.size() - 1u));
    ActiveStationIndex            = ClampedStation;

    auto* Vehicle = dynamic_cast<Frontier::VehicleRecord*>(World.FindFirstByType("Vehicle"));
    if (Vehicle == nullptr)
    {
        return false;
    }
    const Frontier::SessionSlotSpecification& Slot          = Policy.Slots[ClampedStation];
    Frontier::VehicleConfiguration&           Configuration = Vehicle->AccessConfiguration();
    Configuration.Spatial.PositionMetres[0]                 = Slot.PositionMetres[0];
    Configuration.Spatial.PositionMetres[1]                 = Slot.PositionMetres[1];
    Configuration.Spatial.PositionMetres[2]                 = Slot.PositionMetres[2];
    Configuration.Spatial.OrientationDegrees[0]             = 0.0f;
    Configuration.Spatial.OrientationDegrees[1]             = 0.0f;
    Configuration.Spatial.OrientationDegrees[2]             = Slot.YawDegrees;
    Configuration.EquilibriumPlacement                      = Policy.EquilibriumPlacement;
    Configuration.EngageBrakesOnPlacement                   = Policy.EngageBrakesOnPlacement;
    Configuration.Physics                                   = Policy.PhysicsSimulated;
    Configuration.Visible                                   = true;
    ++Configuration.Revision;
    return true;
}

bool LobbySession::ToggleLocalReady()
{
    for (const Frontier::SessionParticipant& Entry : Session.QueryParticipants())
    {
        if (Entry.Identifier == LocalDriverIdentifier)
        {
            return Session.ConfigureParticipantReady(LocalDriverIdentifier, !Entry.Ready);
        }
    }
    return false;
}

void LobbySession::Advance(Frontier::WorldSpace& World, double DeltaSeconds)
{
    Session.Advance(DeltaSeconds);
    if (World.QueryEnvironmentArchive() == "Lobby")
    {
        (void)SynchronizeWorldVisibility(World);
    }
}

bool LobbySession::SynchronizeWorldVisibility(Frontier::WorldSpace& World)
{
    if (World.QueryEnvironmentArchive() != "Lobby")
    {
        return false;
    }

    bool Changed = false;

    // ① Slot 0: Hero ControlVehicle on the centre turntable (Stand policy: authored plinth pose + brake hold).
    const Frontier::SessionPolicy& Policy       = Session.QueryPolicy();
    const bool                     HeroOccupied = (Session.FindParticipantInSlot(0u) != nullptr);
    if (auto* Vehicle = dynamic_cast<Frontier::VehicleRecord*>(World.FindFirstByType("Vehicle")))
    {
        Frontier::VehicleConfiguration& Configuration = Vehicle->AccessConfiguration();
        bool                            VehicleDirty  = false;
        if (Configuration.Visible != HeroOccupied)
        {
            Configuration.Visible = HeroOccupied;
            VehicleDirty          = true;
        }
        if (Configuration.EquilibriumPlacement != Policy.EquilibriumPlacement)
        {
            Configuration.EquilibriumPlacement = Policy.EquilibriumPlacement;
            VehicleDirty                       = true;
        }
        if (Configuration.EngageBrakesOnPlacement != Policy.EngageBrakesOnPlacement)
        {
            Configuration.EngageBrakesOnPlacement = Policy.EngageBrakesOnPlacement;
            VehicleDirty                          = true;
        }
        if (VehicleDirty)
        {
            ++Configuration.Revision;
            Changed = true;
        }
    }

    // ② Slots 1..4: Ring plinth cars ("Lobby Slot 2" .. "Lobby Slot 5") appear ONLY when that slot is occupied.
    for (const Frontier::SceneRecordBase* Candidate : World.FindAllByType("Geometry"))
    {
        if (Candidate == nullptr)
        {
            continue;
        }
        auto* Geometry = dynamic_cast<Frontier::GeometryRecord*>(World.FindRecord(Candidate->QueryToken()));
        if (Geometry == nullptr)
        {
            continue;
        }
        const std::string_view Name = Geometry->QueryRecordName();
        for (uint32_t SlotIndex = 1u; SlotIndex <= 4u; ++SlotIndex)
        {
            if (Name != QueryLobbyGuestRecordName(SlotIndex))
            {
                continue;
            }
            const bool ShouldShow = (Session.FindParticipantInSlot(SlotIndex) != nullptr);
            auto&      Config     = Geometry->AccessConfiguration();
            if (Config.Visible != ShouldShow)
            {
                Config.Visible = ShouldShow;
                ++Config.Revision;
#if FRONTIER_EDITOR
                (void)Frontier::SceneDirectory::Instance().ConfigureVisible(Geometry->QueryToken(), ShouldShow);
#endif
                Changed = true;
            }
        }
    }

    return Changed;
}

uint32_t LobbySession::QueryActiveVehicleCount() const noexcept
{
    return Session.QueryParticipantCount();
}

} // namespace Everest
