/*==========================================================================================================================================
                                                            SESSIONSPACE.CPP
==========================================================================================================================================*/
// 🧩 Policy-driven multiplayer/solo session state machine: lowest-free-slot participant roster, ready gate, countdown, and round clock.

#include "Frontier/Engine/Session/SessionSpace.h"

#include <algorithm>

namespace Frontier
{

SessionSpace::SessionSpace(SessionPolicy InitialPolicy)
{
    ConfigurePolicy(std::move(InitialPolicy));
}

void SessionSpace::ConfigurePolicy(SessionPolicy NextPolicy)
{
    Policy             = std::move(NextPolicy);
    Policy.MaximumParticipants = std::max(1u, Policy.MaximumParticipants);
    Stage              = SessionLifecycleStage::Gathering;
    Participants.clear();
    CountdownRemaining = 0.0;
    RoundElapsed       = 0.0;
}

std::optional<uint64_t> SessionSpace::JoinParticipant(std::string_view Callsign,
                                                      std::string_view RankBadge,
                                                      bool             LocalDriver)
{
    if (Participants.size() >= Policy.MaximumParticipants)
    {
        return std::nullopt;
    }

    uint32_t ChosenSlot = Policy.MaximumParticipants;
    for (uint32_t Candidate = 0u; Candidate < Policy.MaximumParticipants; ++Candidate)
    {
        if (FindParticipantInSlot(Candidate) == nullptr)
        {
            ChosenSlot = Candidate;
            break;
        }
    }
    if (ChosenSlot >= Policy.MaximumParticipants)
    {
        return std::nullopt;
    }

    SessionParticipant Entry{};
    Entry.Identifier  = NextIdentifier++;
    Entry.SlotIndex   = ChosenSlot;
    Entry.Callsign    = Callsign.empty() ? ("DRIVER-" + std::to_string(ChosenSlot + 1u)) : std::string(Callsign);
    Entry.RankBadge   = std::string(RankBadge);
    Entry.LocalDriver = LocalDriver;
    Entry.Ready       = !Policy.RequireReadyGate;
    Participants.push_back(std::move(Entry));

    std::sort(Participants.begin(), Participants.end(),
              [](const SessionParticipant& Left, const SessionParticipant& Right) { return Left.SlotIndex < Right.SlotIndex; });
    return Entry.Identifier;
}

bool SessionSpace::RemoveParticipant(uint64_t Identifier)
{
    const auto Iterator = std::find_if(Participants.begin(), Participants.end(),
                                       [Identifier](const SessionParticipant& Entry) { return Entry.Identifier == Identifier; });
    if (Iterator == Participants.end())
    {
        return false;
    }
    Participants.erase(Iterator);
    if (Participants.empty() && Stage == SessionLifecycleStage::Countdown)
    {
        CancelCountdown();
    }
    return true;
}

bool SessionSpace::ConfigureParticipantReady(uint64_t Identifier, bool Ready)
{
    for (SessionParticipant& Entry : Participants)
    {
        if (Entry.Identifier == Identifier)
        {
            Entry.Ready = Ready;
            if (!Ready && Policy.RequireReadyGate && Stage == SessionLifecycleStage::Countdown)
            {
                CancelCountdown();
            }
            return true;
        }
    }
    return false;
}

const SessionParticipant* SessionSpace::FindParticipantInSlot(uint32_t SlotIndex) const noexcept
{
    for (const SessionParticipant& Entry : Participants)
    {
        if (Entry.SlotIndex == SlotIndex)
        {
            return &Entry;
        }
    }
    return nullptr;
}

bool SessionSpace::QueryAllReady() const noexcept
{
    if (Participants.empty())
    {
        return false;
    }
    for (const SessionParticipant& Entry : Participants)
    {
        if (!Entry.Ready)
        {
            return false;
        }
    }
    return true;
}

bool SessionSpace::BeginCountdown()
{
    if (Participants.empty())
    {
        return false;
    }
    if (Policy.RequireReadyGate && !QueryAllReady())
    {
        return false;
    }
    if (Policy.CountdownSeconds <= 0.0)
    {
        Stage              = SessionLifecycleStage::Active;
        CountdownRemaining = 0.0;
        RoundElapsed       = 0.0;
        return true;
    }
    Stage              = SessionLifecycleStage::Countdown;
    CountdownRemaining = Policy.CountdownSeconds;
    return true;
}

void SessionSpace::CancelCountdown() noexcept
{
    if (Stage == SessionLifecycleStage::Countdown)
    {
        Stage              = SessionLifecycleStage::Gathering;
        CountdownRemaining = 0.0;
    }
}

void SessionSpace::Advance(double DeltaSeconds) noexcept
{
    const double Clamped = std::max(0.0, DeltaSeconds);
    if (Stage == SessionLifecycleStage::Countdown)
    {
        CountdownRemaining = std::max(0.0, CountdownRemaining - Clamped);
        if (CountdownRemaining <= 0.0)
        {
            Stage        = SessionLifecycleStage::Active;
            RoundElapsed = 0.0;
        }
    }
    else if (Stage == SessionLifecycleStage::Active)
    {
        RoundElapsed += Clamped;
        if (Policy.RoundDurationSeconds > 0.0 && RoundElapsed >= Policy.RoundDurationSeconds)
        {
            RoundElapsed = Policy.RoundDurationSeconds;
            Stage        = SessionLifecycleStage::Concluded;
        }
    }
}

} // namespace Frontier
