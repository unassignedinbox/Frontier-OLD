/*==========================================================================================================================================
                                                            PHYSICSEXTENSION.CPP
==========================================================================================================================================*/
// 🧩 Level records into bodies and vehicles through the register, fixed steps inline or on the physics thread, snapshots written
//    back into records and the vehicles' live readings, and the restoration when the simulation stops.

#include "Frontier/Extensions/Physics/PhysicsExtension.h"

#include "Frontier/Engine/Scene/VehicleReadings.h"
#include "Frontier/Engine/Scene/WorldSpace.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace Frontier
{

namespace
{

std::array<double, 3> ToDouble(const float Coordinates[3])
{
    return { Coordinates[0], Coordinates[1], Coordinates[2] };
}

std::array<double, 3> Conjugated(const std::array<double, 4>& Q, const std::array<double, 3>& Vector)
{
    return RotatePhysicsVector({ -Q[0], -Q[1], -Q[2], Q[3] }, Vector);
}

bool EqualsIgnoreAsciiCase(std::string_view Left, std::string_view Right) noexcept
{
    if (Left.size() != Right.size()) return false;
    for (size_t Index = 0; Index < Left.size(); ++Index)
    {
        if (std::tolower(static_cast<unsigned char>(Left[Index])) != std::tolower(static_cast<unsigned char>(Right[Index]))) return false;
    }
    return true;
}

bool ContainsIgnoreAsciiCase(std::string_view Text, std::string_view Term) noexcept
{
    if (Term.empty() || Text.size() < Term.size()) return false;
    for (size_t Start = 0; Start + Term.size() <= Text.size(); ++Start)
    {
        bool Equal = true;
        for (size_t Offset = 0; Offset < Term.size(); ++Offset)
        {
            if (std::tolower(static_cast<unsigned char>(Text[Start + Offset])) != std::tolower(static_cast<unsigned char>(Term[Offset])))
            {
                Equal = false;
                break;
            }
        }
        if (Equal) return true;
    }
    return false;
}

PhysicsImpactSurface ResolveImpactSurface(const GeometryConfiguration& Geometry, std::string_view RecordName) noexcept
{
    bool Metal = false;
    bool Concrete = false;
    const auto Inspect = [&](std::string_view Name) {
        Metal = Metal || ContainsIgnoreAsciiCase(Name, "metal") || ContainsIgnoreAsciiCase(Name, "steel") ||
                ContainsIgnoreAsciiCase(Name, "iron") || ContainsIgnoreAsciiCase(Name, "aluminium") || ContainsIgnoreAsciiCase(Name, "aluminum");
        Concrete = Concrete || ContainsIgnoreAsciiCase(Name, "concrete") || ContainsIgnoreAsciiCase(Name, "cement");
    };
    for (const std::string& Name : Geometry.Resolved.FamilyNames) Inspect(Name);
    if (!Metal && !Concrete) Inspect(RecordName);
    if (Metal) return PhysicsImpactSurface::Metal;
    if (Concrete) return PhysicsImpactSurface::Concrete;
    return PhysicsImpactSurface::Other;
}

bool ResolveEngineShapeInfo(std::string_view ShapeName, PhysicsShape& OutShape, std::array<double, 3>& OutUnitSize) noexcept
{
    if (ShapeName.empty()) return false;
    if (EqualsIgnoreAsciiCase(ShapeName, "Sphere"))
    {
        OutShape    = PhysicsShape::Sphere;
        OutUnitSize = { 1.0, 1.0, 1.0 };
        return true;
    }
    if (EqualsIgnoreAsciiCase(ShapeName, "Cylinder"))
    {
        OutShape    = PhysicsShape::Cylinder;
        OutUnitSize = { 1.0, 1.0, 1.0 };
        return true;
    }
    if (EqualsIgnoreAsciiCase(ShapeName, "Cone"))
    {
        OutShape    = PhysicsShape::Cone;
        OutUnitSize = { 1.0, 1.0, 1.0 };
        return true;
    }
    if (EqualsIgnoreAsciiCase(ShapeName, "Torus"))
    {
        OutShape    = PhysicsShape::Torus;
        OutUnitSize = { 1.0, 1.0, 0.30 };
        return true;
    }
    if (EqualsIgnoreAsciiCase(ShapeName, "Plane"))
    {
        OutShape    = PhysicsShape::Box;
        OutUnitSize = { 1.0, 1.0, 0.01 };
        return true;
    }
    if (EqualsIgnoreAsciiCase(ShapeName, "Cube") || EqualsIgnoreAsciiCase(ShapeName, "Box"))
    {
        OutShape    = PhysicsShape::Box;
        OutUnitSize = { 1.0, 1.0, 1.0 };
        return true;
    }
    OutShape    = PhysicsShape::Box;
    OutUnitSize = { 1.0, 1.0, 1.0 };
    return true;
}

void HashCombine64(uint64_t& Hash, uint64_t Value) noexcept
{
    Hash ^= Value + 0x9E3779B97F4A7C15ull + (Hash << 6u) + (Hash >> 2u);
}

void HashFloat(uint64_t& Hash, float Value) noexcept
{
    const float Canonical = (std::abs(Value) < 1.0e-6f) ? 0.0f : Value;
    uint32_t    Bits      = 0u;
    std::memcpy(&Bits, &Canonical, sizeof(Bits));
    HashCombine64(Hash, Bits);
}

uint64_t ComputeGeometrySignature(const GeometryConfiguration& Geometry, bool PreserveMovingTransform) noexcept
{
    uint64_t Hash = 0xCBF29CE484222325ull;
    HashCombine64(Hash, Geometry.Visible ? 1u : 0u);
    HashCombine64(Hash, Geometry.Physics ? 2u : 0u);
    HashCombine64(Hash, Geometry.Fracture.Enabled ? 4u : 0u);
    for (unsigned char Ch : Geometry.ShapeName) HashCombine64(Hash, Ch);
    HashCombine64(Hash, Geometry.Resolved.Bound ? 8u : 0u);
    HashCombine64(Hash, Geometry.Resolved.Shown);
    HashCombine64(Hash, Geometry.Resolved.WorldTriangles.size());
    for (const std::string& Family : Geometry.Resolved.FamilyNames)
        for (unsigned char Ch : Family) HashCombine64(Hash, Ch);
    for (int Axis = 0; Axis < 3; ++Axis)
    {
        HashFloat(Hash, Geometry.Resolved.Extent[Axis]);
        HashFloat(Hash, Geometry.Spatial.Scale[Axis]);
    }
    if (!(PreserveMovingTransform && Geometry.Physics && !Geometry.Fracture.Enabled))
    {
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            HashFloat(Hash, Geometry.Spatial.PositionMetres[Axis]);
            HashFloat(Hash, Geometry.Spatial.OrientationDegrees[Axis]);
            HashFloat(Hash, Geometry.Resolved.BoundsMinimum[Axis]);
            HashFloat(Hash, Geometry.Resolved.BoundsMaximum[Axis]);
        }
        HashCombine64(Hash, Geometry.Modifiers.size());
        for (const auto& Mod : Geometry.Modifiers)
        {
            HashCombine64(Hash, Mod.Enabled ? 1u : 0u);
            HashCombine64(Hash, Mod.Count);
        }
    }
    return Hash;
}

uint64_t ComputeRigidBodySignature(const RigidBodyRecord& Record, bool PreserveMovingTransform) noexcept
{
    const PhysicsConfiguration& Physics = Record.QueryPhysics();
    const SpatialConfiguration& Spatial = Record.QuerySpatial();
    uint64_t                    Hash    = 0x84222325CBF29CE4ull;
    HashCombine64(Hash, Physics.Enabled ? 1u : 0u);
    HashCombine64(Hash, Physics.DynamicBody ? 2u : 0u);
    HashFloat(Hash, Physics.Mass.MassKilograms);
    HashFloat(Hash, Physics.Mass.DensityKgPerCubicMetre);
    if (!(PreserveMovingTransform && Physics.DynamicBody))
    {
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            HashFloat(Hash, Spatial.PositionMetres[Axis]);
            HashFloat(Hash, Spatial.OrientationDegrees[Axis]);
        }
    }
    return Hash;
}

} // namespace

PhysicsExtension::PhysicsExtension() = default;

PhysicsExtension::~PhysicsExtension()
{
    Thread.Stop();
}

size_t PhysicsExtension::Begin(WorldSpace& World, const PhysicsSpaceSettings& Settings)
{
    PhysicsExtensionSettings Inline;
    Inline.Space = Settings;
    return Begin(World, Inline);
}

PhysicsExtension::RecordBinding PhysicsExtension::BuildGeometryCollider(
    WorldSpace&, SceneRecordBase& RecordBase, uint64_t Signature, WorldRegisterEntry* Entry)
{
    auto&                                    Record = static_cast<GeometryRecord&>(RecordBase);
    RecordBinding                            Binding{};
    Binding.Record                           = Record.QueryToken();
    Binding.Signature                        = Signature;
    Binding.Geometry                         = true;
    GeometryConfiguration&                   Geometry = Record.AccessConfiguration();
    const GeometryConfiguration::Resolution& Resolved = Geometry.Resolved;
    const std::array<double, 3>              Minimum  = ToDouble(Resolved.BoundsMinimum);
    const std::array<double, 3>              Maximum  = ToDouble(Resolved.BoundsMaximum);
    const std::array<double, 3>              World3   = { Maximum[0] - Minimum[0], Maximum[1] - Minimum[1], Maximum[2] - Minimum[2] };
    const bool                               Bounded  = (World3[0] > 1.0e-4 && World3[1] > 1.0e-4 && World3[2] > 1.0e-4) &&
                                                        (World3[0] > 1.0e-2 || World3[1] > 1.0e-2 || World3[2] > 1.0e-2);
    const PhysicsImpactSurface ImpactSurface = ResolveImpactSurface(Geometry, Record.QueryRecordName());
    const auto AnnotateImpactSurface = [ImpactSurface](PhysicsBodyDescriptor& Collider) { Collider.ImpactSurface = ImpactSurface; };

    PhysicsShape          PrimitiveShape = PhysicsShape::Box;
    std::array<double, 3> UnitShapeSize  = { 1.0, 1.0, 1.0 };
    const bool            IsEngineShape  = ResolveEngineShapeInfo(Geometry.ShapeName, PrimitiveShape, UnitShapeSize);

    auto AssignEntry = [&](PhysicsParticipation Part, uint32_t BodyId, std::string Remark = {})
    {
        Binding.Participation = Part;
        Binding.Body          = BodyId;
        Binding.Signature     = ComputeGeometrySignature(Geometry, Part == PhysicsParticipation::Moving);
        if (Entry != nullptr)
        {
            Entry->Physics = Part;
            Entry->Body    = BodyId;
            if (!Remark.empty()) Entry->Remark = std::move(Remark);
        }
    };

    if (!Geometry.Visible)
    {
        AssignEntry(PhysicsParticipation::Skipped, PhysicsNoBody, "hidden");
        return Binding;
    }

    if (Geometry.Fracture.Enabled)
    {
        if (Resolved.Shown > 1 && Resolved.WorldTriangles.empty())
        {
            AssignEntry(PhysicsParticipation::Held, PhysicsNoBody,
                        std::to_string(Resolved.Shown) + " copies share one transform, so they keep their place");
            return Binding;
        }
        uint32_t BodyId = PhysicsNoBody;
        if (Resolved.WorldTriangles.size() >= 3u && (Resolved.WorldTriangles.size() % 3u) == 0u)
        {
            PhysicsBodyDescriptor MeshCollider;
            MeshCollider.Record   = Record.QueryToken().Value;
            MeshCollider.Shape    = PhysicsShape::Triangles;
            MeshCollider.Motion   = PhysicsMotion::Fixed;
            MeshCollider.Friction = 0.85;
            MeshCollider.Points.reserve(Resolved.WorldTriangles.size());
            MeshCollider.Triangles.reserve(Resolved.WorldTriangles.size());
            for (size_t Index = 0u; Index < Resolved.WorldTriangles.size(); ++Index)
            {
                MeshCollider.Points.push_back(Resolved.WorldTriangles[Index]);
                MeshCollider.Triangles.push_back(static_cast<uint32_t>(Index));
            }
            AnnotateImpactSurface(MeshCollider);
            BodyId = Space->AddBody(MeshCollider);
        }
        if (BodyId == PhysicsNoBody && Bounded)
        {
            PhysicsBodyDescriptor Collider;
            Collider.Record           = Record.QueryToken().Value;
            Collider.HalfExtentMetres = { std::max(0.01, 0.5 * World3[0]), std::max(0.01, 0.5 * World3[1]), std::max(0.01, 0.5 * World3[2]) };
            Collider.PositionMetres   = { 0.5 * (Minimum[0] + Maximum[0]), 0.5 * (Minimum[1] + Maximum[1]), 0.5 * (Minimum[2] + Maximum[2]) };
            Collider.Motion           = PhysicsMotion::Fixed;
            AnnotateImpactSurface(Collider);
            BodyId                    = Space->AddBody(Collider);
        }
        if (BodyId != PhysicsNoBody)
        {
            AssignEntry(PhysicsParticipation::Fracture, BodyId, "fracture deferred: a fixed collider over its bounds");
        }
        else
        {
            AssignEntry(PhysicsParticipation::Skipped, PhysicsNoBody, "not resolved against the scene (no bounds)");
        }
        return Binding;
    }

    if (Geometry.Physics && Resolved.Shown <= 1)
    {
        const SpatialConfiguration& Spatial = Geometry.Spatial;
        std::array<double, 3>       Size    = {
            Resolved.Extent[0] * std::abs(Spatial.Scale[0]),
            Resolved.Extent[1] * std::abs(Spatial.Scale[1]),
            Resolved.Extent[2] * std::abs(Spatial.Scale[2]),
        };
        if (!(Size[0] > 1.0e-4 && Size[1] > 1.0e-4 && Size[2] > 1.0e-4))
        {
            if (IsEngineShape)
            {
                Size = {
                    UnitShapeSize[0] * std::max(1.0e-3, static_cast<double>(std::abs(Spatial.Scale[0]))),
                    UnitShapeSize[1] * std::max(1.0e-3, static_cast<double>(std::abs(Spatial.Scale[1]))),
                    UnitShapeSize[2] * std::max(1.0e-3, static_cast<double>(std::abs(Spatial.Scale[2]))),
                };
            }
            else if (Bounded)
            {
                Size = World3;
            }
            else
            {
                AssignEntry(PhysicsParticipation::Skipped, PhysicsNoBody, "not resolved against the scene (no bounds)");
                return Binding;
            }
        }
        const std::array<double, 3> Origin      = ToDouble(Spatial.PositionMetres);
        const std::array<double, 4> Orientation = ResolvePhysicsOrientation(ToDouble(Spatial.OrientationDegrees));
        const std::array<double, 3> Centre =
            (!IsEngineShape && Bounded)
                ? std::array<double, 3>{ 0.5 * (Minimum[0] + Maximum[0]), 0.5 * (Minimum[1] + Maximum[1]), 0.5 * (Minimum[2] + Maximum[2]) }
                : Origin;
        const std::array<double, 3> Offset =
            IsEngineShape ? std::array<double, 3>{ 0.0, 0.0, 0.0 }
                          : Conjugated(Orientation, { Centre[0] - Origin[0], Centre[1] - Origin[1], Centre[2] - Origin[2] });

        PhysicsBodyDescriptor Body;
        Body.Record           = Record.QueryToken().Value;
        Body.Shape            = IsEngineShape ? PrimitiveShape : PhysicsShape::Box;
        Body.HalfExtentMetres = { std::max(0.005, 0.5 * Size[0]), std::max(0.005, 0.5 * Size[1]), std::max(0.005, 0.5 * Size[2]) };
        Body.PositionMetres   = Centre;
        Body.Orientation      = Orientation;
        Body.Motion           = PhysicsMotion::Moving;
        AnnotateImpactSurface(Body);
        const uint32_t Index  = Space->AddBody(Body);
        if (Index == PhysicsNoBody)
        {
            AssignEntry(PhysicsParticipation::Skipped, PhysicsNoBody, "degenerate shape");
            return Binding;
        }
        Links.push_back({ Index, Record.QueryToken(), true, Offset, Spatial, Geometry.Dynamic });
        Geometry.Dynamic = true;
        AssignEntry(PhysicsParticipation::Moving, Index, IsEngineShape ? ("moving " + Geometry.ShapeName + " shape") : std::string{});
        return Binding;
    }

    // Static Geometry collider: exact per-instance triangle mesh first (single or multi-copy), else primitive shape, else bounds box.
    if (IsEngineShape && Resolved.Shown <= 1 && Geometry.Modifiers.empty())
    {
        const SpatialConfiguration& Spatial = Geometry.Spatial;
        std::array<double, 3>       Size    = {
            (Resolved.Extent[0] > 1.0e-4 ? Resolved.Extent[0] : UnitShapeSize[0]) * std::max(1.0e-3, static_cast<double>(std::abs(Spatial.Scale[0]))),
            (Resolved.Extent[1] > 1.0e-4 ? Resolved.Extent[1] : UnitShapeSize[1]) * std::max(1.0e-3, static_cast<double>(std::abs(Spatial.Scale[1]))),
            (Resolved.Extent[2] > 1.0e-4 ? Resolved.Extent[2] : UnitShapeSize[2]) * std::max(1.0e-3, static_cast<double>(std::abs(Spatial.Scale[2]))),
        };
        PhysicsBodyDescriptor ShapeCollider;
        ShapeCollider.Record           = Record.QueryToken().Value;
        ShapeCollider.Shape            = PrimitiveShape;
        ShapeCollider.HalfExtentMetres = { std::max(0.005, 0.5 * Size[0]), std::max(0.005, 0.5 * Size[1]), std::max(0.005, 0.5 * Size[2]) };
        ShapeCollider.PositionMetres   = ToDouble(Spatial.PositionMetres);
        ShapeCollider.Orientation      = ResolvePhysicsOrientation(ToDouble(Spatial.OrientationDegrees));
        ShapeCollider.Motion           = PhysicsMotion::Fixed;
        ShapeCollider.Friction         = 0.85;
        AnnotateImpactSurface(ShapeCollider);
        const uint32_t BodyId          = Space->AddBody(ShapeCollider);
        if (BodyId != PhysicsNoBody)
        {
            AssignEntry(PhysicsParticipation::Fixed, BodyId, "fixed " + Geometry.ShapeName + " shape collider");
            return Binding;
        }
    }

    if (Resolved.WorldTriangles.size() >= 3u && (Resolved.WorldTriangles.size() % 3u) == 0u)
    {
        PhysicsBodyDescriptor MeshCollider;
        MeshCollider.Record   = Record.QueryToken().Value;
        MeshCollider.Shape    = PhysicsShape::Triangles;
        MeshCollider.Motion   = PhysicsMotion::Fixed;
        MeshCollider.Friction = 0.85;
        MeshCollider.Points.reserve(Resolved.WorldTriangles.size());
        MeshCollider.Triangles.reserve(Resolved.WorldTriangles.size());
        for (size_t Index = 0u; Index < Resolved.WorldTriangles.size(); ++Index)
        {
            MeshCollider.Points.push_back(Resolved.WorldTriangles[Index]);
            MeshCollider.Triangles.push_back(static_cast<uint32_t>(Index));
        }
        AnnotateImpactSurface(MeshCollider);
        const uint32_t BodyId = Space->AddBody(MeshCollider);
        if (BodyId != PhysicsNoBody)
        {
            AssignEntry(PhysicsParticipation::Fixed, BodyId,
                        std::to_string(Resolved.WorldTriangles.size() / 3u) + "-facet static triangle mesh (" +
                            std::to_string(std::max(1u, Resolved.Shown)) + (Resolved.Shown == 1u ? " copy)" : " copies)"));
            return Binding;
        }
    }

    if (Resolved.Shown > 1)
    {
        AssignEntry(PhysicsParticipation::Held, PhysicsNoBody,
                    std::to_string(Resolved.Shown) + " copies share one transform, so they keep their place");
        return Binding;
    }

    if (Resolved.Shown == 1 && Bounded)
    {
        PhysicsBodyDescriptor Collider;
        Collider.Record           = Record.QueryToken().Value;
        Collider.HalfExtentMetres = { std::max(0.01, 0.5 * World3[0]), std::max(0.01, 0.5 * World3[1]), std::max(0.01, 0.5 * World3[2]) };
        Collider.PositionMetres   = { 0.5 * (Minimum[0] + Maximum[0]), 0.5 * (Minimum[1] + Maximum[1]), 0.5 * (Minimum[2] + Maximum[2]) };
        Collider.Motion           = PhysicsMotion::Fixed;
        AnnotateImpactSurface(Collider);
        const uint32_t BodyId     = Space->AddBody(Collider);
        if (BodyId != PhysicsNoBody)
        {
            AssignEntry(PhysicsParticipation::Fixed, BodyId);
            return Binding;
        }
    }

    AssignEntry(PhysicsParticipation::Skipped, PhysicsNoBody, "not resolved against the scene (no bounds)");
    return Binding;
}

PhysicsExtension::RecordBinding PhysicsExtension::BuildRigidBodyCollider(
    SceneRecordBase& RecordBase, uint64_t Signature, WorldRegisterEntry* Entry)
{
    auto&         Record = static_cast<RigidBodyRecord&>(RecordBase);
    RecordBinding Binding{};
    Binding.Record    = Record.QueryToken();
    Binding.Signature = Signature;
    Binding.Geometry  = false;
    if (!Record.QueryPhysics().Enabled)
    {
        Binding.Participation = PhysicsParticipation::Skipped;
        if (Entry != nullptr) Entry->Physics = PhysicsParticipation::Skipped;
        return Binding;
    }
    const PhysicsConfiguration& Physics = Record.QueryPhysics();
    const double                Volume  = Physics.Mass.MassKilograms / std::max(1.0e-3f, Physics.Mass.DensityKgPerCubicMetre);
    PhysicsBodyDescriptor       Body;
    Body.Record              = Record.QueryToken().Value;
    Body.Shape               = PhysicsShape::Sphere;
    Body.HalfExtentMetres[0] = std::clamp(std::cbrt(3.0 * Volume / (4.0 * 3.14159265358979323846)), 0.05, 50.0);
    Body.PositionMetres      = ToDouble(Record.QuerySpatial().PositionMetres);
    Body.Orientation         = ResolvePhysicsOrientation(ToDouble(Record.QuerySpatial().OrientationDegrees));
    Body.Motion              = Physics.DynamicBody ? PhysicsMotion::Moving : PhysicsMotion::Fixed;
    Body.MassKilograms       = Physics.Mass.MassKilograms;
    Body.Friction            = Physics.DynamicFriction;
    Body.Restitution         = Physics.Restitution;
    Body.LinearDamping       = Physics.LinearDamping;
    Body.LinearVelocityMps   = ToDouble(Physics.LinearVelocityMps);
    Body.AngularVelocityRads = ToDouble(Physics.AngularVelocityRadSec);
    const uint32_t Index     = Space->AddBody(Body);
    if (Index == PhysicsNoBody)
    {
        Binding.Participation = PhysicsParticipation::Skipped;
        if (Entry != nullptr) Entry->Physics = PhysicsParticipation::Skipped;
        return Binding;
    }
    Binding.Body = Index;
    if (Entry != nullptr) Entry->Body = Index;
    if (Body.Motion == PhysicsMotion::Moving)
    {
        Links.push_back({ Index, Record.QueryToken(), false, {}, Record.QuerySpatial(), false });
        Binding.Participation = PhysicsParticipation::Moving;
        if (Entry != nullptr) Entry->Physics = PhysicsParticipation::Moving;
    }
    else
    {
        Binding.Participation = PhysicsParticipation::Fixed;
        if (Entry != nullptr) Entry->Physics = PhysicsParticipation::Fixed;
    }
    Binding.Signature = ComputeRigidBodySignature(Record, Binding.Participation == PhysicsParticipation::Moving);
    return Binding;
}

void PhysicsExtension::RecomputeTally()
{
    const double PrevStepHz   = Tally.StepHz;
    const bool   PrevThreaded = Tally.Threaded;
    const double PrevStepMs   = Tally.StepMilliseconds;
    Tally                     = {};
    Tally.StepHz              = PrevStepHz;
    Tally.Threaded            = PrevThreaded;
    Tally.StepMilliseconds    = PrevStepMs;
    if (Space && Space->QueryBodyCount() > 0) ++Tally.Fixed;   // ground slab
    for (const RecordBinding& Binding : Bindings)
    {
        switch (Binding.Participation)
        {
            case PhysicsParticipation::Moving:   ++Tally.Moving; break;
            case PhysicsParticipation::Fixed:    ++Tally.Fixed; break;
            case PhysicsParticipation::Held:     ++Tally.Held; break;
            case PhysicsParticipation::Fracture: ++Tally.Fracture; break;
            case PhysicsParticipation::Skipped:  ++Tally.Skipped; break;
            default: break;
        }
    }
    for (const WorldRegisterEntry& Entry : Register.QueryEntries())
    {
        if (Entry.Type == "Vehicle" && Entry.Physics == PhysicsParticipation::Skipped) ++Tally.Skipped;
    }
    Tally.Vehicles = Vehicles.size();
}

void PhysicsExtension::StartPhysicsThread()
{
    if (!Threaded || !Space) return;
    PhysicsThreadConfiguration Configuration;
    Configuration.StepHz              = Tally.StepHz > 0.0 ? Tally.StepHz : (1.0 / StepSeconds);
    Configuration.MaximumCatchUpSteps = ActiveSettings.MaximumCatchUpSteps;
    Threaded = Thread.Start(
        Configuration, [this](uint64_t, double Seconds) { StepOnce(Seconds); },
        [this](uint64_t)
        {
            FillSnapshot(Snapshots.AccessProduced());
            Snapshots.Publish();
        });
    Tally.Threaded = Threaded;
}

size_t PhysicsExtension::Begin(WorldSpace& World, const PhysicsExtensionSettings& Settings)
{
    if (Space) End(World);
    ActiveSettings = Settings;
    Space          = std::make_unique<PhysicsSpace>(Settings.Space);
    Links.clear();
    Bindings.clear();
    Vehicles.clear();
    Tally        = {};
    Remainder    = 0.0;
    StepCount    = 0;
    Pending      = {};
    Applied      = {};
    AppliedReset = 0;
    Latest       = {};
    Register.Survey(World);

    // ① the ground: a 4 km slab whose top is z = 0
    PhysicsBodyDescriptor Ground;
    Ground.Shape            = PhysicsShape::Box;
    Ground.HalfExtentMetres = { 2000.0, 2000.0, 1.0 };
    Ground.PositionMetres   = { 0.0, 0.0, -1.0 };
    Ground.Motion           = PhysicsMotion::Fixed;
    Ground.Friction         = 0.8;
    (void)Space->AddBody(Ground);

    // ② Geometry records: environment triangle meshes, engine shapes, fracturable colliders, and moving rigid bodies
    for (WorldRegisterEntry& Entry : Register.AccessEntries())
    {
        auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Entry.Token));
        if (Record == nullptr) continue;
        const uint64_t Signature = ComputeGeometrySignature(Record->QueryConfiguration(), false);
        Bindings.push_back(BuildGeometryCollider(World, *Record, Signature, &Entry));
    }

    // ③ RigidBody records: spheres of their volume
    for (WorldRegisterEntry& Entry : Register.AccessEntries())
    {
        auto* Record = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Entry.Token));
        if (Record == nullptr) continue;
        const uint64_t Signature = ComputeRigidBodySignature(*Record, false);
        Bindings.push_back(BuildRigidBodyCollider(*Record, Signature, &Entry));
    }

    // ④ Vehicles (the fixed colliders are in and indexed, so a chassis finds its road or ramp on 4-corner equilibrium probes)
    Space->Prepare();
    for (WorldRegisterEntry& Entry : Register.AccessEntries())
        if (dynamic_cast<VehicleRecord*>(World.FindRecord(Entry.Token)) != nullptr) AddVehicle(World, Entry.Token, &Entry);
    Space->Prepare();
    RecomputeTally();

    // ⑤ the register document beside the level
    if (Settings.WriteRegister && !World.QueryLoadedLevelPath().empty())
        (void)Register.WriteMarkdown(WorldRegister::ResolveDocumentPath(World.QueryLoadedLevelPath()));

    // ⑥ the step rate, substepping, and the physics thread when asked for
    double AuthoredVehicleHz = 0.0;
    int    AuthoredCollision = Settings.CollisionSteps;
    int    AuthoredSubsteps  = Settings.SolverSubsteps;
    for (const VehicleLink& Link : Vehicles)
    {
        if (const auto* Record = dynamic_cast<const VehicleRecord*>(World.FindRecord(Link.Record)))
        {
            const VehicleConfiguration& Configuration = Record->QueryConfiguration();
            AuthoredVehicleHz = std::max(AuthoredVehicleHz, static_cast<double>(Configuration.PhysicsStepHz));
            AuthoredCollision = std::max(AuthoredCollision, Configuration.CollisionSteps);
            AuthoredSubsteps  = std::max(AuthoredSubsteps, Configuration.SolverSubsteps);
        }
    }
    const double Rate = Settings.StepHz > 0.0 ? Settings.StepHz : (Vehicles.empty() ? 60.0 : (AuthoredVehicleHz > 0.0 ? AuthoredVehicleHz : 240.0));
    StepSeconds       = 1.0 / Rate;
    CollisionSteps    = std::clamp(AuthoredCollision, 1, 8);
    SolverSubsteps    = std::clamp(AuthoredSubsteps, 1, 8);
    Tally.StepHz      = Rate;
    Threaded          = Settings.Threaded;
    Tally.Threaded    = Threaded;
    FillSnapshot(Latest);
    (void)ApplySnapshot(World, Latest);
    if (Threaded) StartPhysicsThread();
    return Tally.Moving + Tally.Vehicles;
}

void PhysicsExtension::AddVehicle(WorldSpace& World, RecordToken Token, WorldRegisterEntry* Entry)
{
    auto* Record = dynamic_cast<VehicleRecord*>(World.FindRecord(Token));
    if (Record == nullptr) return;
    VehicleConfiguration& Configuration = Record->AccessConfiguration();
    if (!Configuration.Physics || !Configuration.Visible)
    {
        if (Entry)
        {
            Entry->Physics = PhysicsParticipation::Skipped;
            Entry->Remark  = Configuration.Visible ? "Physics off" : "hidden";
        }
        ++Tally.Skipped;
        return;
    }
    Vehicle::VehicleSolverConfiguration Solver = Vehicle::ResolveSolverConfiguration(Configuration.Specification);
    // 📝 The tyres it encloses set their corners: cold pressure, compound grip, and the compounds' window for the thermal grip.
    std::vector<RecordToken> Tyres;
    std::array<bool, 4>      CornerFitted{ true, true, true, true };
    Solver.CornerPressures.assign(4, Solver.Tyre.InflationPressure);
    Solver.CornerGrip.assign(4, 1.0f);
    double WindowLow = 0.0, WindowHigh = 0.0;
    int    Fitted    = 0;
    for (const SceneRecordBase* Candidate : World.FindAllByType("Tyre"))
    {
        const auto* Tyre = dynamic_cast<const TyreRecord*>(Candidate);
        if (Tyre == nullptr || Candidate->QueryParentToken() != Token) continue;
        const TyreConfiguration& Fit = Tyre->QueryConfiguration();
        const uint32_t           CornerIndex = Fit.Corner & 3u;
        Tyres.push_back(Candidate->QueryToken());
        if (WorldRegisterEntry* TyreEntry = Register.Find(Candidate->QueryToken()))
            TyreEntry->Physics = Fit.Enclosed ? PhysicsParticipation::Tyre : PhysicsParticipation::Skipped;
        if (!Fit.Enclosed)
        {
            CornerFitted[CornerIndex] = false;
            continue;
        }
        CornerFitted[CornerIndex] = true;
        const TyreCompound& Compound = ResolveTyreCompound(Fit.Compound);
        Solver.CornerPressures[CornerIndex] = Fit.ColdPressure;
        Solver.CornerGrip[CornerIndex]      = Compound.GripScale;
        WindowLow += Compound.WindowLow_C + 273.15;
        WindowHigh += Compound.WindowHigh_C + 273.15;
        ++Fitted;
    }
    if (Fitted > 0)
    {
        Solver.TyreThermal.ColdWindowTemperature = WindowLow / Fitted;
        Solver.TyreThermal.OptimumTemperature    = WindowHigh / Fitted;
        Solver.TyreThermal.GripFadeTemperature   = std::max(Solver.TyreThermal.GripFadeTemperature, Solver.TyreThermal.OptimumTemperature + 40.0);
    }
    VehicleLink Link;
    Link.Physics        = std::make_unique<PhysicsVehicle>();
    Link.Record         = Token;
    Link.Tyres          = Tyres;
    Link.Saved          = Configuration.Spatial;
    Link.Possessed      = Configuration.Possessed;
    Link.CentreHeight   = Configuration.Specification.Geometry.CoMHeight;
    Link.Carcass        = Solver.Carcass;
    Link.CornerFitted   = CornerFitted;
    Link.SolverSubsteps = std::max(1, Configuration.SolverSubsteps);
    PhysicsVehiclePlacementOptions PlacementOptions;
    PlacementOptions.EquilibriumPlacement    = Configuration.EquilibriumPlacement;
    PlacementOptions.EngageBrakesOnPlacement = Configuration.EngageBrakesOnPlacement;
    std::string Error;
    if (!Link.Physics->Construct(*Space, Token, Configuration.Specification, Solver, ToDouble(Configuration.Spatial.PositionMetres),
                                 Configuration.Spatial.OrientationDegrees[2], &Error, PlacementOptions))
    {
        if (Entry)
        {
            Entry->Physics = PhysicsParticipation::Skipped;
            Entry->Remark  = Error;
        }
        ++Tally.Skipped;
        return;
    }
    if (Entry)
    {
        Entry->Physics = PhysicsParticipation::Vehicle;
        Entry->Body    = Link.Physics->QueryBody();
        Entry->Remark  = std::string("Project Drive on a Jolt chassis, carcass ") + Vehicle::TyreCarcassName(Solver.Carcass) + ", " +
                         std::to_string(Fitted) + " tyre records fitted";
    }
    Vehicles.push_back(std::move(Link));
    ++Tally.Vehicles;
}

void PhysicsExtension::AssignVehicleInput(const PhysicsVehicleInput& Input)
{
    Pending.Input = Input;
}

void PhysicsExtension::RequestVehicleReset()
{
    ++Pending.ResetSerial;
}

std::vector<PhysicsImpactEvent> PhysicsExtension::DrainImpactEvents()
{
    return Space ? Space->DrainImpactEvents() : std::vector<PhysicsImpactEvent>{};
}

// 📝 One fixed step, on the physics thread or inline: the controls, each vehicle's forces, then the space.
void PhysicsExtension::StepOnce(double Seconds)
{
    // 📝 Threaded, the controls arrive only through the mailbox (the caller's Pending belongs to its own thread).
    if (Threaded)
    {
        VehicleCommand Incoming;
        if (Commands.Peek(Incoming)) Applied = Incoming;
    }
    else
    {
        Applied = Pending;
    }
    const VehicleCommand& Command = Applied;
    const bool Reset = Command.ResetSerial != AppliedReset;
    AppliedReset     = Command.ResetSerial;
    if (Reset)
    {
        for (VehicleLink& Link : Vehicles) Link.Physics->Reset();
    }
    const int    Substeps       = std::max(1, SolverSubsteps);
    const double SubstepSeconds = Seconds / static_cast<double>(Substeps);
    for (int Substep = 0; Substep < Substeps; ++Substep)
    {
        for (VehicleLink& Link : Vehicles)
        {
            Link.Physics->AssignInput(Link.Possessed ? Command.Input : PhysicsVehicleInput{});
            Link.Physics->Step(SubstepSeconds);
        }
        Space->Step(SubstepSeconds, CollisionSteps);
    }
    ++StepCount;
}

void PhysicsExtension::FillSnapshot(PhysicsSnapshot& Snapshot) const
{
    Snapshot.Steps            = StepCount;
    Snapshot.SimulatedSeconds = Space ? Space->QuerySimulatedSeconds() : 0.0;
    Snapshot.Bodies.resize(Links.size());
    for (size_t Index = 0; Index < Links.size(); ++Index) Snapshot.Bodies[Index] = Space->ReadBody(Links[Index].Body);
    Snapshot.Vehicles.resize(Vehicles.size());
    for (size_t Index = 0; Index < Vehicles.size(); ++Index) Vehicles[Index].Physics->ReadView(Snapshot.Vehicles[Index]);
}

void PhysicsExtension::AdvanceSteps(uint32_t Count)
{
    if (!Space || Threaded) return;
    for (uint32_t Step = 0; Step < Count; ++Step) StepOnce(StepSeconds);
    FillSnapshot(Latest);
}

bool PhysicsExtension::SynchronizeWorld(WorldSpace& World)
{
    if (!Space) return false;

    const auto GeometryRecords  = World.FindAllByType("Geometry");
    const auto RigidBodyRecords = World.FindAllByType("RigidBody");
    const size_t ExpectedCount  = GeometryRecords.size() + RigidBodyRecords.size();

    auto FindBinding = [&](RecordToken Token) -> const RecordBinding*
    {
        for (const RecordBinding& Binding : Bindings)
        {
            if (Binding.Record == Token) return &Binding;
        }
        return nullptr;
    };

    bool NeedsSync = (ExpectedCount != Bindings.size());
    if (!NeedsSync)
    {
        for (const SceneRecordBase* Candidate : GeometryRecords)
        {
            const auto* GeometryRec = dynamic_cast<const GeometryRecord*>(Candidate);
            if (GeometryRec == nullptr) continue;
            const RecordBinding* Existing = FindBinding(Candidate->QueryToken());
            if (Existing == nullptr || !Existing->Geometry)
            {
                NeedsSync = true;
                break;
            }
            const bool MovingNow = (Existing->Participation == PhysicsParticipation::Moving);
            if (ComputeGeometrySignature(GeometryRec->QueryConfiguration(), MovingNow) != Existing->Signature)
            {
                NeedsSync = true;
                break;
            }
        }
    }
    if (!NeedsSync)
    {
        for (const SceneRecordBase* Candidate : RigidBodyRecords)
        {
            const auto* RigidRec = dynamic_cast<const RigidBodyRecord*>(Candidate);
            if (RigidRec == nullptr) continue;
            const RecordBinding* Existing = FindBinding(Candidate->QueryToken());
            if (Existing == nullptr || Existing->Geometry)
            {
                NeedsSync = true;
                break;
            }
            const bool MovingNow = (Existing->Participation == PhysicsParticipation::Moving);
            if (ComputeRigidBodySignature(*RigidRec, MovingNow) != Existing->Signature)
            {
                NeedsSync = true;
                break;
            }
        }
    }
    if (!NeedsSync) return false;

    const bool WasThreaded = Threaded;
    if (WasThreaded)
    {
        Thread.Stop();
        if (Snapshots.Acquire()) Latest = Snapshots.QueryConsumed();
        else FillSnapshot(Latest);
        (void)ApplySnapshot(World, Latest);
    }

    auto RemoveLinkedBody = [&](RecordToken Token)
    {
        for (auto It = Links.begin(); It != Links.end(); ++It)
        {
            if (It->Record == Token)
            {
                if (It->Geometry)
                {
                    if (auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Token)))
                    {
                        Record->AccessConfiguration().Dynamic = It->SavedDynamic;
                    }
                }
                Links.erase(It);
                break;
            }
        }
    };

    std::vector<RecordBinding> NextBindings;
    NextBindings.reserve(ExpectedCount);
    Register.Survey(World);

    // Update or build Geometry records
    for (const SceneRecordBase* Candidate : GeometryRecords)
    {
        auto* GeometryRec = dynamic_cast<GeometryRecord*>(World.FindRecord(Candidate->QueryToken()));
        if (GeometryRec == nullptr) continue;
        const RecordBinding* Existing  = FindBinding(Candidate->QueryToken());
        const bool           MovingNow = (Existing != nullptr && Existing->Participation == PhysicsParticipation::Moving);
        const uint64_t       Sig       = ComputeGeometrySignature(GeometryRec->QueryConfiguration(), MovingNow);
        WorldRegisterEntry*  RegEntry  = Register.Find(Candidate->QueryToken());
        if (Existing != nullptr && Existing->Geometry && Existing->Signature == Sig)
        {
            if (RegEntry != nullptr)
            {
                RegEntry->Physics = Existing->Participation;
                RegEntry->Body    = Existing->Body;
            }
            NextBindings.push_back(*Existing);
            continue;
        }
        if (Existing != nullptr)
        {
            if (Existing->Body != PhysicsNoBody) Space->RemoveBody(Existing->Body);
            if (Existing->Participation == PhysicsParticipation::Moving) RemoveLinkedBody(Existing->Record);
        }
        const uint64_t FreshSig = ComputeGeometrySignature(GeometryRec->QueryConfiguration(), false);
        NextBindings.push_back(BuildGeometryCollider(World, *GeometryRec, FreshSig, RegEntry));
    }

    // Update or build RigidBody records
    for (const SceneRecordBase* Candidate : RigidBodyRecords)
    {
        auto* RigidRec = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Candidate->QueryToken()));
        if (RigidRec == nullptr) continue;
        const RecordBinding* Existing  = FindBinding(Candidate->QueryToken());
        const bool           MovingNow = (Existing != nullptr && Existing->Participation == PhysicsParticipation::Moving);
        const uint64_t       Sig       = ComputeRigidBodySignature(*RigidRec, MovingNow);
        WorldRegisterEntry*  RegEntry  = Register.Find(Candidate->QueryToken());
        if (Existing != nullptr && !Existing->Geometry && Existing->Signature == Sig)
        {
            if (RegEntry != nullptr)
            {
                RegEntry->Physics = Existing->Participation;
                RegEntry->Body    = Existing->Body;
            }
            NextBindings.push_back(*Existing);
            continue;
        }
        if (Existing != nullptr)
        {
            if (Existing->Body != PhysicsNoBody) Space->RemoveBody(Existing->Body);
            if (Existing->Participation == PhysicsParticipation::Moving) RemoveLinkedBody(Existing->Record);
        }
        const uint64_t FreshSig = ComputeRigidBodySignature(*RigidRec, false);
        NextBindings.push_back(BuildRigidBodyCollider(*RigidRec, FreshSig, RegEntry));
    }

    // Remove bodies for deleted records
    for (const RecordBinding& OldBinding : Bindings)
    {
        bool StillPresent = false;
        for (const RecordBinding& NewBinding : NextBindings)
        {
            if (NewBinding.Record == OldBinding.Record)
            {
                StillPresent = true;
                break;
            }
        }
        if (!StillPresent)
        {
            if (OldBinding.Body != PhysicsNoBody) Space->RemoveBody(OldBinding.Body);
            if (OldBinding.Participation == PhysicsParticipation::Moving) RemoveLinkedBody(OldBinding.Record);
        }
    }

    // Preserve Vehicle / Tyre entries in Register
    for (const VehicleLink& Veh : Vehicles)
    {
        if (WorldRegisterEntry* VehEntry = Register.Find(Veh.Record))
        {
            VehEntry->Physics = PhysicsParticipation::Vehicle;
            VehEntry->Body    = Veh.Physics->QueryBody();
        }
        for (const RecordToken& TyreTok : Veh.Tyres)
        {
            if (WorldRegisterEntry* TyreEntry = Register.Find(TyreTok)) TyreEntry->Physics = PhysicsParticipation::Tyre;
        }
    }

    Bindings = std::move(NextBindings);
    Space->Prepare();
    RecomputeTally();
    if (ActiveSettings.WriteRegister && !World.QueryLoadedLevelPath().empty())
        (void)Register.WriteMarkdown(WorldRegister::ResolveDocumentPath(World.QueryLoadedLevelPath()));
    FillSnapshot(Latest);
    if (WasThreaded) StartPhysicsThread();
    return true;
}

size_t PhysicsExtension::Step(WorldSpace& World, double DeltaSeconds)
{
    if (!Space) return 0;
    const auto Start = std::chrono::steady_clock::now();
    (void)SynchronizeWorld(World);
    if (Threaded)
    {
        Commands.Write(Pending);
        if (Snapshots.Acquire()) Latest = Snapshots.QueryConsumed();
    }
    else
    {
        Remainder += std::clamp(DeltaSeconds, 0.0, 0.25);
        bool Stepped = false;
        while (Remainder >= StepSeconds)
        {
            StepOnce(StepSeconds);
            Remainder -= StepSeconds;
            Stepped = true;
        }
        if (Stepped) FillSnapshot(Latest);
    }
    const size_t Written = ApplySnapshot(World, Latest);
    Tally.StepMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
    return Written;
}

size_t PhysicsExtension::ApplySnapshot(WorldSpace& World, const PhysicsSnapshot& Snapshot)
{
    size_t Written = 0;
    for (size_t Index = 0; Index < Links.size() && Index < Snapshot.Bodies.size(); ++Index)
    {
        const Link&               Moved   = Links[Index];
        const PhysicsBodyReading& Reading = Snapshot.Bodies[Index];
        const std::array<double, 3> Turned = RotatePhysicsVector(Reading.Orientation, Moved.CentreOffset);
        const std::array<double, 3> Euler  = ResolvePhysicsEulerDegrees(Reading.Orientation);
        SpatialConfiguration*       Spatial  = nullptr;
        RevisionCounter*            Revision = nullptr;
        if (Moved.Geometry)
        {
            if (auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Moved.Record)))
            {
                Spatial  = &Record->AccessConfiguration().Spatial;
                Revision = &Record->AccessConfiguration();
            }
        }
        else if (auto* Record = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Moved.Record)))
        {
            Spatial  = &Record->AccessSpatial();
            Revision = &Record->AccessSpatial();
            for (size_t Axis = 0; Axis < 3; ++Axis) Record->AccessPhysics().LinearVelocityMps[Axis] = static_cast<float>(Reading.LinearVelocityMps[Axis]);
        }
        if (Spatial == nullptr) continue;
        for (size_t Axis = 0; Axis < 3; ++Axis)
        {
            Spatial->PositionMetres[Axis]     = static_cast<float>(Reading.PositionMetres[Axis] - Turned[Axis]);
            Spatial->OrientationDegrees[Axis] = static_cast<float>(Euler[Axis]);
        }
        Revision->AdvanceRevision();
        ++Written;
    }
    // 📝 Vehicles: the record follows the car (the ground point under its centre of mass and its heading); the live reading goes
    //    to the inspectors and the telemetry dock, under the vehicle and each tyre it encloses.
    const PhysicsThreadMeasurements Measured = Threaded ? Thread.QueryMeasurements() : PhysicsThreadMeasurements{};
    for (size_t Index = 0; Index < Vehicles.size() && Index < Snapshot.Vehicles.size(); ++Index)
    {
        const VehicleLink&        Link = Vehicles[Index];
        const PhysicsVehicleView& View = Snapshot.Vehicles[Index];
        if (auto* Record = dynamic_cast<VehicleRecord*>(World.FindRecord(Link.Record)))
        {
            const Vehicle::ChassisKinematics& Chassis = View.Telemetry.Chassis;
            const Vehicle::Vec3 Foot = Chassis.Position - Chassis.Orientation.Rotate({ 0.0f, 0.0f, static_cast<float>(Link.CentreHeight) });
            const std::array<double, 3> Euler = ResolvePhysicsEulerDegrees({ Chassis.Orientation.x, Chassis.Orientation.y, Chassis.Orientation.z, Chassis.Orientation.w });
            SpatialConfiguration& Spatial = Record->AccessConfiguration().Spatial;
            Spatial.PositionMetres[0] = Foot.x;
            Spatial.PositionMetres[1] = Foot.y;
            Spatial.PositionMetres[2] = Foot.z;
            for (size_t Axis = 0; Axis < 3; ++Axis) Spatial.OrientationDegrees[Axis] = static_cast<float>(Euler[Axis]);
            Spatial.AdvanceRevision();
            ++Written;
        }
        VehicleReading Reading;
        Reading.Simulating          = true;
        Reading.Telemetry           = View.Telemetry;
        Reading.SimulatedSeconds    = Snapshot.SimulatedSeconds;
        Reading.Steps               = Snapshot.Steps;
        Reading.StepHz              = 1.0 / StepSeconds;
        Reading.StepMicroseconds    = Measured.MeanStepMicroseconds;
        Reading.RealtimeRatio       = Threaded ? Measured.RealtimeRatio : 1.0;
        Reading.Throttle            = { View.Input.Throttle, View.Input.Brake, View.Input.Steer, View.Input.Handbrake ? 1.0f : 0.0f };
        Reading.Carcass             = Link.Carcass;
        Reading.VisualValid         = true;
        Reading.BodyTransform       = View.Body;
        Reading.WheelTransforms     = View.Wheels;
        Reading.CornerFields        = View.Fields;
        Reading.CornerFieldValid    = View.FieldValid;
        Reading.CornerFitted        = Link.CornerFitted;
        Reading.ContactAngle        = View.ContactAngle;
        Reading.Deflection          = View.Deflection;
        Reading.GaugePressure       = View.GaugePressure;
        Reading.LateralShift        = View.LateralShift;
        Reading.SteeringColumnAngle = -View.Input.Steer * 2.3561945f;
        PublishVehicleReading(Link.Record, Reading);
        for (const RecordToken& Tyre : Link.Tyres) PublishVehicleReading(Tyre, Reading);
    }
    return Written;
}

void PhysicsExtension::End(WorldSpace& World)
{
    Thread.Stop();
    for (const Link& Moved : Links)
    {
        if (Moved.Geometry)
        {
            if (auto* Record = dynamic_cast<GeometryRecord*>(World.FindRecord(Moved.Record)))
            {
                GeometryConfiguration& Geometry = Record->AccessConfiguration();
                for (size_t Axis = 0; Axis < 3; ++Axis)
                {
                    Geometry.Spatial.PositionMetres[Axis]     = Moved.Saved.PositionMetres[Axis];
                    Geometry.Spatial.OrientationDegrees[Axis] = Moved.Saved.OrientationDegrees[Axis];
                    Geometry.Spatial.Scale[Axis]              = Moved.Saved.Scale[Axis];
                }
                Geometry.Dynamic = Moved.SavedDynamic;
                Geometry.AdvanceRevision();
            }
        }
        else if (auto* Record = dynamic_cast<RigidBodyRecord*>(World.FindRecord(Moved.Record)))
        {
            SpatialConfiguration& Spatial = Record->AccessSpatial();
            for (size_t Axis = 0; Axis < 3; ++Axis)
            {
                Spatial.PositionMetres[Axis]     = Moved.Saved.PositionMetres[Axis];
                Spatial.OrientationDegrees[Axis] = Moved.Saved.OrientationDegrees[Axis];
            }
            Spatial.AdvanceRevision();
        }
    }
    for (const VehicleLink& Link : Vehicles)
    {
        if (auto* Record = dynamic_cast<VehicleRecord*>(World.FindRecord(Link.Record)))
        {
            SpatialConfiguration& Spatial = Record->AccessConfiguration().Spatial;
            for (size_t Axis = 0; Axis < 3; ++Axis)
            {
                Spatial.PositionMetres[Axis]     = Link.Saved.PositionMetres[Axis];
                Spatial.OrientationDegrees[Axis] = Link.Saved.OrientationDegrees[Axis];
            }
            Spatial.AdvanceRevision();
        }
        VehicleReading Stopped;
        PublishVehicleReading(Link.Record, Stopped);
        for (const RecordToken& Tyre : Link.Tyres) PublishVehicleReading(Tyre, Stopped);
    }
    Links.clear();
    Bindings.clear();
    Vehicles.clear();
    Space.reset();
    Remainder = 0.0;
    Threaded  = false;
    Latest    = {};
}

} // namespace Frontier
