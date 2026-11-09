/*==========================================================================================================================================
                                                             PHYSICSEXTENSION.H
==========================================================================================================================================*/
// 🧩 The bridge between a level and its PhysicsSpace. A Geometry record shown once with Physics on becomes a moving box the size
//    of its resolved extent; the other objects shown once become fixed colliders; an object with several copies is held (its
//    copies share one transform, so they cannot fall apart); RigidBody records become spheres; a ground slab tops out at z = 0.
//    Each step writes the moving bodies' transforms back into their records, and the end puts every record back where the
//    simulation found it (the editor's Simulate).

#pragma once

#include "Frontier/Extensions/Physics/PhysicsSpace.h"

#include "Frontier/Engine/Scene/EngineConfigurations.h"
#include "Frontier/Engine/Scene/RecordToken.h"

#include <array>
#include <memory>
#include <vector>

namespace Frontier
{

class WorldSpace;

struct PhysicsExtensionTally
{
    size_t Moving    = 0;   // [-] bodies written back to records
    size_t Fixed     = 0;   // [-] colliders, the ground included
    size_t Held      = 0;   // [-] objects with several copies (they keep their place and are not collided against)
    size_t Skipped   = 0;   // [-] objects without a size or hidden
    double StepMilliseconds = 0.0;
};

class PhysicsExtension
{
  public:
    PhysicsExtension();
    ~PhysicsExtension();

    // 📝 Builds the space from the level and remembers every moved record's transform. Returns the moving bodies.
    size_t Begin(WorldSpace& World, const PhysicsSpaceSettings& Settings = {});
    // 📝 Steps the space (in sub-steps of at most 1/60 s) and writes the moving bodies back. Returns the records written.
    size_t Step(WorldSpace& World, double DeltaSeconds);
    // 📝 Puts every moved record back and clears the space.
    void End(WorldSpace& World);

    [[nodiscard]] bool                         QueryRunning() const noexcept { return Space != nullptr; }
    [[nodiscard]] const PhysicsSpace*          QuerySpace() const noexcept { return Space.get(); }
    [[nodiscard]] const PhysicsExtensionTally& QueryTally() const noexcept { return Tally; }

  private:
    struct Link
    {
        uint32_t              Body = 0;
        RecordToken           Record{};
        bool                  Geometry = true;               // a Geometry record, else a RigidBody record
        std::array<double, 3> CentreOffset{};                // [m] the shape's centre from the record's origin, in its own axes
        SpatialConfiguration  Saved{};                       // the transform before the simulation
        bool                  SavedDynamic = false;          // the Geometry record's Dynamic flag before the simulation
    };

    std::unique_ptr<PhysicsSpace> Space;
    std::vector<Link>             Links;
    PhysicsExtensionTally         Tally;
    double                        Remainder = 0.0;   // [s] time not yet stepped
};

} // namespace Frontier
