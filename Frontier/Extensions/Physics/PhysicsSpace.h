/*==========================================================================================================================================
                                                               PHYSICSSPACE.H
==========================================================================================================================================*/
// 🧩 PhysicsSpace, the world rigid bodies live in: boxes and spheres that fall under gravity, collide, come to rest and sleep,
//    stepped by Jolt behind the extension. The header holds no Jolt type, so editor, game and test code include it freely.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Frontier
{

enum class PhysicsShape : uint8_t
{
    Box,
    Sphere,
};

enum class PhysicsMotion : uint8_t
{
    Fixed,    // never moves: the ground, the scene's colliders
    Moving,   // falls, collides and is pushed
};

// 📝 One body as it enters the space (z up, metres). Orientation is a unit quaternion (x, y, z, w).
struct PhysicsBodyDescriptor
{
    uint64_t              Record              = 0;                          // [-] the level record it stands for; 0: none
    PhysicsShape          Shape               = PhysicsShape::Box;
    std::array<double, 3> HalfExtentMetres    = { 0.5, 0.5, 0.5 };          // [m] box; a sphere's radius is [0]
    std::array<double, 3> PositionMetres      = { 0.0, 0.0, 0.0 };          // [m] the shape's centre
    std::array<double, 4> Orientation         = { 0.0, 0.0, 0.0, 1.0 };     // [-]
    PhysicsMotion         Motion              = PhysicsMotion::Moving;
    double                MassKilograms       = 0.0;                        // [kg] 0: from the shape at 1000 kg/m³
    double                Friction            = 0.5;                        // [-]
    double                Restitution         = 0.2;                        // [-]
    double                LinearDamping       = 0.05;                       // [1/s]
    std::array<double, 3> LinearVelocityMps   = { 0.0, 0.0, 0.0 };          // [m/s]
    std::array<double, 3> AngularVelocityRads = { 0.0, 0.0, 0.0 };          // [rad/s]
};

struct PhysicsBodyReading
{
    std::array<double, 3> PositionMetres      = { 0.0, 0.0, 0.0 };
    std::array<double, 4> Orientation         = { 0.0, 0.0, 0.0, 1.0 };
    std::array<double, 3> LinearVelocityMps   = { 0.0, 0.0, 0.0 };
    std::array<double, 3> AngularVelocityRads = { 0.0, 0.0, 0.0 };
    bool                  Awake               = false;
};

struct PhysicsSpaceSettings
{
    std::array<double, 3> GravityMps2   = { 0.0, 0.0, -9.81 };   // [m/s²] z up
    uint32_t              MaximumBodies = 8192;
    uint32_t              Workers       = 0;                     // [-] job threads; 0: the machine's choice (up to 4)
};

class PhysicsSpace
{
  public:
    explicit PhysicsSpace(const PhysicsSpaceSettings& Settings = {});
    ~PhysicsSpace();

    PhysicsSpace(const PhysicsSpace&)            = delete;
    PhysicsSpace& operator=(const PhysicsSpace&) = delete;

    // 📝 The body's index in the space (0, 1, 2 …), or UINT32_MAX when the space is full or the shape is degenerate.
    uint32_t AddBody(const PhysicsBodyDescriptor& Descriptor);
    // 📝 Arranges the broad phase once the bodies are in (stepping works without it, only slower at first).
    void Prepare();
    // 📝 One step of DeltaSeconds, split into CollisionSteps sub-steps (more for fast bodies).
    void Step(double DeltaSeconds, uint32_t CollisionSteps = 1);

    [[nodiscard]] PhysicsBodyReading           ReadBody(uint32_t Index) const;
    [[nodiscard]] const PhysicsBodyDescriptor& QueryDescriptor(uint32_t Index) const { return Descriptors[Index]; }
    [[nodiscard]] size_t                       QueryBodyCount() const noexcept { return Descriptors.size(); }
    [[nodiscard]] size_t                       QueryAwakeCount() const;
    [[nodiscard]] uint64_t                     QueryContactsAdded() const noexcept;
    [[nodiscard]] double                       QuerySimulatedSeconds() const noexcept { return SimulatedSeconds; }
    [[nodiscard]] uint32_t                     QueryWorkers() const noexcept;

  private:
    struct Implementation;
    std::unique_ptr<Implementation>    Detail;
    std::vector<PhysicsBodyDescriptor> Descriptors;
    double                             SimulatedSeconds = 0.0;
};

// 📝 The level's Euler convention (OrientationDegrees: R = Rz(γ) · Ry(β) · Rx(α)) to and from a unit quaternion (x, y, z, w).
[[nodiscard]] std::array<double, 4> ResolvePhysicsOrientation(const std::array<double, 3>& OrientationDegrees) noexcept;
[[nodiscard]] std::array<double, 3> ResolvePhysicsEulerDegrees(const std::array<double, 4>& Orientation) noexcept;
// 📝 A vector turned by a unit quaternion.
[[nodiscard]] std::array<double, 3> RotatePhysicsVector(const std::array<double, 4>& Orientation, const std::array<double, 3>& Vector) noexcept;

} // namespace Frontier
