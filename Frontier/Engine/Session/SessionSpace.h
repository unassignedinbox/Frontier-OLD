/*==========================================================================================================================================
                                                             SESSIONSPACE.H
==========================================================================================================================================*/
// 🧩 Policy-driven multiplayer/solo session state machine: lowest-free-slot participant roster, ready gate, countdown, and round clock.

#pragma once

#include "Frontier/Engine/Session/SessionPolicy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Frontier
{

enum class SessionLifecycleStage : uint8_t
{
    Gathering = 0,   // [-] - Participants joining/configuring in Lobby or pre-grid
    Countdown = 1,   // [-] - Launch countdown ticking toward active session
    Active    = 2,   // [-] - Driving / active session underway
    Concluded = 3    // [-] - Timed round completed
};

struct SessionParticipant
{
    uint64_t    Identifier  = 0u;      // [-] - Monotonic session participant handle
    uint32_t    SlotIndex   = 0u;      // [-] - Claimed stand / grid slot in [0, MaximumParticipants)
    std::string Callsign    = "DRIVER";
    std::string RankBadge   = "UNRANKED";
    bool        LocalDriver = false;   // [-] - True for the local host/player driver
    bool        Ready       = false;   // [-] - Ready-gate acknowledgement
};

class SessionSpace
{
public:
    SessionSpace() = default;
    explicit SessionSpace(SessionPolicy InitialPolicy);

    void ConfigurePolicy(SessionPolicy NextPolicy);

    // Claims the lowest free slot in [0, MaximumParticipants) and returns the new participant's Identifier.
    [[nodiscard]] std::optional<uint64_t> JoinParticipant(std::string_view Callsign,
                                                          std::string_view RankBadge = {},
                                                          bool             LocalDriver = false);
    bool                                  RemoveParticipant(uint64_t Identifier);
    bool                                  ConfigureParticipantReady(uint64_t Identifier, bool Ready);

    bool BeginCountdown();
    void CancelCountdown() noexcept;
    void Advance(double DeltaSeconds) noexcept;

    [[nodiscard]] const SessionPolicy&                  QueryPolicy() const noexcept { return Policy; }
    [[nodiscard]] SessionLifecycleStage                 QueryStage() const noexcept { return Stage; }
    [[nodiscard]] const std::vector<SessionParticipant>& QueryParticipants() const noexcept { return Participants; }
    [[nodiscard]] uint32_t                              QueryParticipantCount() const noexcept
    {
        return static_cast<uint32_t>(Participants.size());
    }
    [[nodiscard]] const SessionParticipant*             FindParticipantInSlot(uint32_t SlotIndex) const noexcept;
    [[nodiscard]] bool                                  QueryAllReady() const noexcept;
    [[nodiscard]] double                                QueryCountdownRemainingSeconds() const noexcept { return CountdownRemaining; }
    [[nodiscard]] double                                QueryRoundElapsedSeconds() const noexcept { return RoundElapsed; }

private:
    SessionPolicy                   Policy{};
    SessionLifecycleStage           Stage              = SessionLifecycleStage::Gathering;
    std::vector<SessionParticipant> Participants{};
    uint64_t                        NextIdentifier     = 1u;
    double                          CountdownRemaining = 0.0;
    double                          RoundElapsed       = 0.0;
};

} // namespace Frontier
