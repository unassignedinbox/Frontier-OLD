/*==========================================================================================================================================
                                                               JOLTADAPTER.H
==========================================================================================================================================*/
// 🧩 The PhysicsAdapter on Jolt: one Jolt solver with the extension's layers, every body shape the adapter offers, forces at points,
//    placement and the nearest-hit ray query. Only the Physics extension's sources include this header.

#pragma once

#include "Frontier/Extensions/Physics/JoltExtension.h"
#include "Frontier/Extensions/Physics/PhysicsAdapter.h"

#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Frontier
{

// 📝 Counts contacts and emits debounced, high-speed vehicle-to-metal/concrete impacts from Jolt's job-thread callbacks.
class JoltContactTally final : public JPH::ContactListener
{
  public:
    void ConfigureBody(uint32_t BodyIndex, uint64_t Record, PhysicsImpactSurface Surface, bool VehicleChassis);
    void RemoveBody(uint32_t BodyIndex);
    void ClearImpactEvents();
    [[nodiscard]] std::vector<PhysicsImpactEvent> DrainImpactEvents();

    void OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold& Manifold, JPH::ContactSettings& Settings) override;

    std::atomic<uint64_t> Added{ 0 };

  private:
    struct BodyMetadata
    {
        uint64_t Record = 0;
        PhysicsImpactSurface Surface = PhysicsImpactSurface::Other;
        bool VehicleChassis = false;
    };

    std::mutex Mutex;
    std::vector<BodyMetadata> Metadata;
    std::vector<PhysicsImpactEvent> PendingImpacts;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> LastImpactByPair;
    uint32_t NextImpactSeed = 1;
};

class JoltAdapter final : public PhysicsAdapter
{
  public:
    explicit JoltAdapter(const PhysicsSpaceSettings& Settings);
    ~JoltAdapter() override;

    JoltAdapter(const JoltAdapter&)            = delete;
    JoltAdapter& operator=(const JoltAdapter&) = delete;

    uint32_t AddBody(const PhysicsBodyDescriptor& Descriptor) override;
    void     RemoveBody(uint32_t Body) override;
    void     Prepare() override;
    void     Step(double Seconds, uint32_t CollisionSteps) override;

    [[nodiscard]] PhysicsBodyReading ReadBody(uint32_t Body) const override;
    void PlaceBody(uint32_t Body, const PhysicsBodyReading& Pose) override;
    void ApplyForce(uint32_t Body, const std::array<double, 3>& Force, const std::array<double, 3>& Point) override;
    void ApplyTorque(uint32_t Body, const std::array<double, 3>& Torque) override;
    [[nodiscard]] bool CastRay(const PhysicsRay& Ray, PhysicsRayHit& Hit) const override;
    void AssignMass(uint32_t Body, double MassKilograms, const std::array<double, 3>& InertiaKgm2) override;
    void Dispatch(uint32_t Count, const std::function<void(uint32_t)>& Task) override;

    [[nodiscard]] size_t      QueryAwakeCount() const override;
    [[nodiscard]] uint64_t    QueryContactsAdded() const noexcept override { return Contacts.Added.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t    QueryWorkers() const noexcept override { return Backend.QueryWorkers(); }
    [[nodiscard]] const char* QuerySolverName() const noexcept override { return "Jolt"; }
    [[nodiscard]] std::vector<PhysicsImpactEvent> DrainImpactEvents() override { return Contacts.DrainImpactEvents(); }

  private:
    [[nodiscard]] JPH::RefConst<JPH::Shape> ConstructShape(const PhysicsBodyDescriptor& Descriptor) const;

    JoltExtension            Backend;
    JPH::PhysicsSystem       Solver;
    JoltContactTally         Contacts;
    std::vector<JPH::BodyID> Bodies;        // [-] by body index
    std::vector<uint32_t>    BodyIndices;   // [-] Jolt body index → body index (PhysicsNoBody: not ours)
};

} // namespace Frontier
