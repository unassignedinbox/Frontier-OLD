/*==========================================================================================================================================
                                                             SESSIONPOLICY.H
==========================================================================================================================================*/
// 🧩 Declarative session policy contract: slot transforms, placement kind (Stand vs TerrainEquilibrium), brake-hold, ready gate, and round rules.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Frontier
{

enum class SessionPlacementKind : uint8_t
{
    Stand              = 0,   // [-] - Fixed showcase turntable / plinth stands (Studio Lobby)
    TerrainEquilibrium = 1    // [-] - 4-corner slope-fitted equilibrium stations (Proving / Training Ground)
};

struct SessionSlotSpecification
{
    std::array<float, 3> PositionMetres = {0.0f, 0.0f, 0.0f};   // [m]   - Authored world slot origin (z-up)
    float                YawDegrees     = 0.0f;                 // [deg] - Authored heading about +Z
    std::string          Label          = "Slot";               // [-]   - Human-readable stand / station label
};

struct SessionPolicy
{
    std::string                           Identifier              = "session.default";
    std::string                           Title                   = "Session";
    std::string                           LevelRelativePath       = "Content/Levels/Lobby.levelspace";
    std::string                           EnvironmentArchive      = "Lobby";
    uint32_t                              MaximumParticipants     = 5u;
    SessionPlacementKind                  Placement               = SessionPlacementKind::Stand;
    bool                                  PhysicsSimulated        = false;
    bool                                  EquilibriumPlacement    = false;   // [-] - 4-corner terrain normal + static sag + radial tyre deflection
    bool                                  EngageBrakesOnPlacement = true;    // [-] - Latch service + parking brakes until throttle > 0.02
    bool                                  RequireReadyGate        = false;
    double                                CountdownSeconds        = 3.0;
    double                                RoundDurationSeconds    = 0.0;
    std::vector<SessionSlotSpecification> Slots{};
};

} // namespace Frontier
