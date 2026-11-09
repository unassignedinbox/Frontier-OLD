/*==========================================================================================================================================
                                                  VEHICLECURVEPACKVALIDATION.CPP
==========================================================================================================================================*/
// 🧩 Versioned .vcurves packs: round-trip, partial override and transactional rejection of malformed runtime-float knots.

#include "Frontier/Engine/Scene/VehicleRecordCodec.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace Frontier;

bool Check(bool Passed, const char* Label)
{
    std::cout << (Passed ? "PASS " : "FAIL ") << Label << '\n';
    return Passed;
}

bool SameCurves(Vehicle::VehicleSpecification& A, Vehicle::VehicleSpecification& B)
{
    const auto CurvesA = Vehicle::EnumerateVehicleCurves(A);
    const auto CurvesB = Vehicle::EnumerateVehicleCurves(B);
    if (CurvesA.size() != CurvesB.size()) return false;
    for (size_t Index = 0; Index < CurvesA.size(); ++Index)
    {
        if (CurvesA[Index].Section != CurvesB[Index].Section || CurvesA[Index].Key != CurvesB[Index].Key ||
            CurvesA[Index].Target == nullptr || CurvesB[Index].Target == nullptr ||
            CurvesA[Index].Target->X != CurvesB[Index].Target->X || CurvesA[Index].Target->Y != CurvesB[Index].Target->Y)
            return false;
    }
    return true;
}

constexpr const char* PartialPack = R"TOML(format = "frontier.vehicle-curves"
version = 1
vehicle = "partial override"

[curves.engine]
torque_curve = [[0.0, 72.0], [500.0, 180.0], [1000.0, 255.0]]
future_curve = [[0.0, 0.0], [1.0, 1.0]]

[curves.future_engine]
new_map = [[0.0, 0.0], [1.0, 1.0]]
)TOML";

} // namespace

int main()
{
    using namespace Frontier;
    bool Passed = true;

    VehicleConfiguration Source;
    Source.Specification = Vehicle::ConstructControlVehicleSpecification();
    std::string Encoded, Error;
    const bool EncodedAll = EncodeVehicleCurvePack(Source, Encoded, Error);
    VehicleConfiguration RoundTrip;
    const bool DecodedAll = EncodedAll && DecodeVehicleCurvePack(Encoded, RoundTrip, Error);
    Passed &= Check(DecodedAll && SameCurves(Source.Specification, RoundTrip.Specification), "all registered curves round-trip through version-1 TOML");

    VehicleConfiguration Partial;
    Vehicle::VehicleSpecification BeforePartial = Partial.Specification;
    const bool PartialLoaded = DecodeVehicleCurvePack(PartialPack, Partial, Error);
    auto PartialCurves = Vehicle::EnumerateVehicleCurves(Partial.Specification);
    auto BeforeCurves = Vehicle::EnumerateVehicleCurves(BeforePartial);
    bool OtherCurvesPreserved = true;
    for (size_t Index = 1; Index < PartialCurves.size(); ++Index)
        OtherCurvesPreserved = OtherCurvesPreserved && PartialCurves[Index].Target->X == BeforeCurves[Index].Target->X &&
                               PartialCurves[Index].Target->Y == BeforeCurves[Index].Target->Y;
    const Vehicle::Curve& Torque = *PartialCurves.front().Target;
    Passed &= Check(PartialLoaded && OtherCurvesPreserved && Torque.X.size() == 3u && Torque.Y[1] == 180.0f,
                    "partial override updates its known curve, preserves other curves and ignores unknown curve names");

    VehicleConfiguration Collision;
    const Vehicle::VehicleSpecification CollisionBefore = Collision.Specification;
    const uint64_t CollisionRevision = Collision.Revision;
    const std::string FloatCollision = R"TOML(format = "frontier.vehicle-curves"
version = 1
[curves.engine]
torque_curve = [[1000.0, 1.0], [1000.00001, 2.0]]
)TOML";
    const bool CollisionRejected = !DecodeVehicleCurvePack(FloatCollision, Collision, Error);
    Vehicle::VehicleSpecification CollisionAfter = Collision.Specification;
    Vehicle::VehicleSpecification CollisionExpected = CollisionBefore;
    Passed &= Check(CollisionRejected && Collision.Revision == CollisionRevision && SameCurves(CollisionAfter, CollisionExpected),
                    "float-colliding knots are rejected transactionally without mutating the vehicle");

    VehicleConfiguration Unordered;
    const Vehicle::VehicleSpecification UnorderedBefore = Unordered.Specification;
    const uint64_t UnorderedRevision = Unordered.Revision;
    const std::string UnorderedPack = R"TOML(format = "frontier.vehicle-curves"
version = 1
[curves.engine]
torque_curve = [[0.0, 1.0], [1000.0, 2.0], [500.0, 3.0]]
)TOML";
    const bool UnorderedRejected = !DecodeVehicleCurvePack(UnorderedPack, Unordered, Error);
    Vehicle::VehicleSpecification UnorderedAfter = Unordered.Specification;
    Vehicle::VehicleSpecification UnorderedExpected = UnorderedBefore;
    Passed &= Check(UnorderedRejected && Unordered.Revision == UnorderedRevision && SameCurves(UnorderedAfter, UnorderedExpected),
                    "unordered knots are rejected transactionally without mutating the vehicle");

    VehicleConfiguration OutOfRange;
    const Vehicle::VehicleSpecification OutOfRangeBefore = OutOfRange.Specification;
    const uint64_t OutOfRangeRevision = OutOfRange.Revision;
    const std::string OutOfRangePack = R"TOML(format = "frontier.vehicle-curves"
version = 1
[curves.engine]
torque_curve = [[0.0, 72.0], [1000.0, 180.0]]
exhaust_flow_curve = [[0.0, 0.1], [1000.0, -0.01]]
)TOML";
    const bool RangeRejected = !DecodeVehicleCurvePack(OutOfRangePack, OutOfRange, Error);
    Vehicle::VehicleSpecification OutOfRangeAfter = OutOfRange.Specification;
    Vehicle::VehicleSpecification OutOfRangeExpected = OutOfRangeBefore;
    Passed &= Check(RangeRejected && OutOfRange.Revision == OutOfRangeRevision && SameCurves(OutOfRangeAfter, OutOfRangeExpected),
                    "authored value limits reject the whole pack without applying earlier valid overrides");

    VehicleConfiguration Future;
    const Vehicle::VehicleSpecification FutureBefore = Future.Specification;
    const uint64_t FutureRevision = Future.Revision;
    const std::string UnsupportedVersion = R"TOML(format = "frontier.vehicle-curves"
version = 2
[curves.engine]
torque_curve = [[0.0, 1.0], [1.0, 2.0]]
)TOML";
    const bool VersionRejected = !DecodeVehicleCurvePack(UnsupportedVersion, Future, Error);
    Vehicle::VehicleSpecification FutureAfter = Future.Specification;
    Vehicle::VehicleSpecification FutureExpected = FutureBefore;
    Passed &= Check(VersionRejected && Future.Revision == FutureRevision && SameCurves(FutureAfter, FutureExpected),
                    "unsupported pack versions are rejected transactionally");

    return Passed ? 0 : 1;
}
