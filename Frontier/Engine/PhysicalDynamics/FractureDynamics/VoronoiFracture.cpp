/*==========================================================================================================================================
                                                            VORONOI FRACTURE
==========================================================================================================================================*/
// 🧩 Deterministic convex-cell generation for Fracture authoring: a box partitioned by clipped Voronoi half-spaces, with
//    paired cut-face tokens and closed-polyhedron metrics. This is geometry authoring/bake input, not a runtime stress solver.

#include "Frontier/Engine/PhysicalDynamics/FractureDynamics/VoronoiFracture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace Frontier::FractureDynamics
{

namespace
{

constexpr uint32_t MaximumCellCount = 256;
constexpr double   Pi                = 3.1415926535897932384626433832795;

using EdgeKey = std::pair<uint32_t, uint32_t>;

struct FaceMetrics
{
    Vector3 Normal{};
    Vector3 Centre{};
    double  Area = 0.0;
};

struct CutFaceReference
{
    uint32_t     CellIndex = 0;
    FaceMetrics  Metrics{};
};

double Dot(const Vector3& A, const Vector3& B) noexcept
{
    return A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
}

Vector3 Add(const Vector3& A, const Vector3& B) noexcept
{
    return { A[0] + B[0], A[1] + B[1], A[2] + B[2] };
}

Vector3 Subtract(const Vector3& A, const Vector3& B) noexcept
{
    return { A[0] - B[0], A[1] - B[1], A[2] - B[2] };
}

Vector3 Multiply(const Vector3& Value, double Scale) noexcept
{
    return { Value[0] * Scale, Value[1] * Scale, Value[2] * Scale };
}

Vector3 Cross(const Vector3& A, const Vector3& B) noexcept
{
    return { A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0] };
}

double LengthSquared(const Vector3& Value) noexcept
{
    return Dot(Value, Value);
}

double Length(const Vector3& Value) noexcept
{
    return std::sqrt(LengthSquared(Value));
}

Vector3 Normalize(const Vector3& Value) noexcept
{
    const double Magnitude = Length(Value);
    return Magnitude > 0.0 ? Multiply(Value, 1.0 / Magnitude) : Vector3{};
}

bool IsFinite(const Vector3& Value) noexcept
{
    return std::isfinite(Value[0]) && std::isfinite(Value[1]) && std::isfinite(Value[2]);
}

void SetDiagnostic(std::string* Diagnostic, std::string Message)
{
    if (Diagnostic != nullptr) *Diagnostic = std::move(Message);
}

double ClampToExtent(double Value, double HalfExtent) noexcept
{
    return std::clamp(Value, -HalfExtent, HalfExtent);
}

// 📝 SplitMix64 supplies a small deterministic authoring stream without shared state or platform-specific standard-library RNGs.
double NextUnit(uint64_t& State) noexcept
{
    State += 0x9e3779b97f4a7c15ull;
    uint64_t Mixed = State;
    Mixed = (Mixed ^ (Mixed >> 30u)) * 0xbf58476d1ce4e5b9ull;
    Mixed = (Mixed ^ (Mixed >> 27u)) * 0x94d049bb133111ebull;
    Mixed ^= Mixed >> 31u;
    return static_cast<double>(Mixed >> 11u) * (1.0 / 9007199254740992.0);
}

bool IsSeparatedFromSites(const Vector3& Candidate, const std::vector<Vector3>& Sites, double MinimumDistanceSquared) noexcept
{
    return std::all_of(Sites.begin(), Sites.end(), [&](const Vector3& Site) {
        return LengthSquared(Subtract(Candidate, Site)) > MinimumDistanceSquared;
    });
}

bool ConstructSites(const VoronoiFractureRequest& Request,
                    Vector3                       ImpactPoint,
                    std::vector<Vector3>&         Sites,
                    std::string*                  Diagnostic)
{
    Sites.clear();
    Sites.reserve(Request.CellCount);
    ImpactPoint = { ClampToExtent(ImpactPoint[0], Request.HalfExtentsMetres[0]), ClampToExtent(ImpactPoint[1], Request.HalfExtentsMetres[1]),
                    ClampToExtent(ImpactPoint[2], Request.HalfExtentsMetres[2]) };
    Sites.push_back(ImpactPoint);

    uint64_t     State         = Request.Seed ^ 0xd1b54a32d192ed03ull;
    const double MinimumExtent = std::min({ Request.HalfExtentsMetres[0], Request.HalfExtentsMetres[1], Request.HalfExtentsMetres[2] });
    const double MinimumGap    = std::max(1.0e-14, MinimumExtent * 1.0e-8);
    const double MinimumGap2   = MinimumGap * MinimumGap;
    const double Bias          = std::clamp(Request.ImpactRadialBias, 0.0, 1.0);
    const double BiasChance    = Bias * 0.72;
    const double BiasRadius    = 1.55 * MinimumExtent;
    const double RadialPower   = 1.0 / 3.0 + Bias * 0.72;

    while (Sites.size() < Request.CellCount)
    {
        bool    Accepted  = false;
        Vector3 Candidate{};
        for (uint32_t Attempt = 0; Attempt < 96 && !Accepted; ++Attempt)
        {
            const bool AroundImpact = NextUnit(State) < BiasChance;
            if (AroundImpact)
            {
                const double Z      = 1.0 - 2.0 * NextUnit(State);
                const double Angle  = 2.0 * Pi * NextUnit(State);
                const double Planar = std::sqrt(std::max(0.0, 1.0 - Z * Z));
                const double Radius = BiasRadius * std::pow(NextUnit(State), RadialPower);
                Candidate = { ImpactPoint[0] + Radius * Planar * std::cos(Angle),
                              ImpactPoint[1] + Radius * Z,
                              ImpactPoint[2] + Radius * Planar * std::sin(Angle) };
                Candidate = { ClampToExtent(Candidate[0], Request.HalfExtentsMetres[0]), ClampToExtent(Candidate[1], Request.HalfExtentsMetres[1]),
                              ClampToExtent(Candidate[2], Request.HalfExtentsMetres[2]) };
            }
            else
            {
                Candidate = { (2.0 * NextUnit(State) - 1.0) * Request.HalfExtentsMetres[0],
                              (2.0 * NextUnit(State) - 1.0) * Request.HalfExtentsMetres[1],
                              (2.0 * NextUnit(State) - 1.0) * Request.HalfExtentsMetres[2] };
            }
            Accepted = IsSeparatedFromSites(Candidate, Sites, MinimumGap2);
        }
        if (!Accepted)
        {
            SetDiagnostic(Diagnostic, "Could not produce distinct Voronoi sites for the requested cell count.");
            return false;
        }
        Sites.push_back(Candidate);
    }
    return true;
}

ConvexCell ConstructBox(uint32_t SiteIndex, const Vector3& Site, const Vector3& HalfExtents)
{
    ConvexCell Cell;
    Cell.SiteIndex = SiteIndex;
    Cell.Site      = Site;
    const double X = HalfExtents[0], Y = HalfExtents[1], Z = HalfExtents[2];
    Cell.Vertices = { { -X, -Y, -Z }, { X, -Y, -Z }, { X, Y, -Z }, { -X, Y, -Z },
                      { -X, -Y, Z },  { X, -Y, Z },  { X, Y, Z },  { -X, Y, Z } };
    Cell.Faces = { { { 0, 3, 2, 1 }, 0 }, { { 4, 5, 6, 7 }, 0 }, { { 0, 1, 5, 4 }, 0 },
                   { { 3, 7, 6, 2 }, 0 }, { { 0, 4, 7, 3 }, 0 }, { { 1, 2, 6, 5 }, 0 } };
    return Cell;
}

uint32_t AddUniqueVertex(std::vector<Vector3>& Vertices, const Vector3& Point, double ToleranceSquared)
{
    for (uint32_t Index = 0; Index < Vertices.size(); ++Index)
        if (LengthSquared(Subtract(Vertices[Index], Point)) <= ToleranceSquared) return Index;
    Vertices.push_back(Point);
    return static_cast<uint32_t>(Vertices.size() - 1u);
}

void AppendUnique(std::vector<uint32_t>& Ring, uint32_t Index)
{
    if (Ring.empty() || Ring.back() != Index) Ring.push_back(Index);
}

void CloseRing(std::vector<uint32_t>& Ring)
{
    if (Ring.size() > 1 && Ring.front() == Ring.back()) Ring.pop_back();
}

double PolygonArea(const std::vector<Vector3>& Vertices, const std::vector<uint32_t>& Ring) noexcept
{
    if (Ring.size() < 3) return 0.0;
    const Vector3 Origin = Vertices[Ring.front()];
    Vector3       AreaVector{};
    for (size_t Index = 1; Index + 1 < Ring.size(); ++Index)
    {
        const Vector3 A = Subtract(Vertices[Ring[Index]], Origin);
        const Vector3 B = Subtract(Vertices[Ring[Index + 1]], Origin);
        AreaVector      = Add(AreaVector, Cross(A, B));
    }
    return 0.5 * Length(AreaVector);
}

bool ClipHalfSpace(ConvexCell& Cell, const Vector3& Normal, double PlaneDistance, uint32_t CutToken, double Tolerance)
{
    if (Cell.Vertices.empty() || Cell.Faces.empty()) return false;
    std::vector<double> Distances(Cell.Vertices.size());
    double              Minimum = std::numeric_limits<double>::infinity();
    double              Maximum = -std::numeric_limits<double>::infinity();
    for (size_t Index = 0; Index < Cell.Vertices.size(); ++Index)
    {
        Distances[Index] = Dot(Normal, Cell.Vertices[Index]) - PlaneDistance;
        Minimum         = std::min(Minimum, Distances[Index]);
        Maximum         = std::max(Maximum, Distances[Index]);
    }
    if (Maximum <= Tolerance) return true;   // the half-space keeps the complete cell
    if (Minimum >= -Tolerance)
    {
        Cell.Vertices.clear();
        Cell.Faces.clear();
        return true;
    }

    const double ToleranceSquared = Tolerance * Tolerance;
    std::vector<Vector3> NewVertices;
    std::vector<ConvexFace> NewFaces;
    std::vector<Vector3> CapPoints;
    NewVertices.reserve(Cell.Vertices.size() + 8u);
    NewFaces.reserve(Cell.Faces.size() + 1u);
    CapPoints.reserve(16u);

    const auto AddCapPoint = [&](const Vector3& Point) {
        for (const Vector3& Existing : CapPoints)
            if (LengthSquared(Subtract(Existing, Point)) <= ToleranceSquared) return;
        CapPoints.push_back(Point);
    };

    for (const Vector3& Point : Cell.Vertices)
    {
        const double Distance = Dot(Normal, Point) - PlaneDistance;
        if (std::abs(Distance) <= Tolerance) AddCapPoint(Point);
    }

    for (const ConvexFace& Face : Cell.Faces)
    {
        if (Face.Vertices.size() < 3) continue;
        std::vector<uint32_t> Ring;
        Ring.reserve(Face.Vertices.size() + 2u);
        for (size_t Edge = 0; Edge < Face.Vertices.size(); ++Edge)
        {
            const uint32_t AIndex = Face.Vertices[Edge];
            const uint32_t BIndex = Face.Vertices[(Edge + 1u) % Face.Vertices.size()];
            const Vector3& A      = Cell.Vertices[AIndex];
            const Vector3& B      = Cell.Vertices[BIndex];
            const double   DA     = Distances[AIndex];
            const double   DB     = Distances[BIndex];
            const bool     KeepA  = DA <= 0.0;
            const bool     KeepB  = DB <= 0.0;

            if (KeepA)
            {
                const uint32_t Index = AddUniqueVertex(NewVertices, A, ToleranceSquared);
                AppendUnique(Ring, Index);
            }
            if (KeepA != KeepB)
            {
                const double Denominator = DA - DB;
                if (std::abs(Denominator) <= std::numeric_limits<double>::epsilon()) continue;
                const double   Fraction     = std::clamp(DA / Denominator, 0.0, 1.0);
                const Vector3  Intersection = Add(A, Multiply(Subtract(B, A), Fraction));
                const uint32_t Index        = AddUniqueVertex(NewVertices, Intersection, ToleranceSquared);
                AppendUnique(Ring, Index);
                AddCapPoint(Intersection);
            }
        }
        CloseRing(Ring);
        if (Ring.size() >= 3 && PolygonArea(NewVertices, Ring) > ToleranceSquared)
            NewFaces.push_back({ std::move(Ring), Face.CutToken });
    }

    if (CapPoints.size() >= 3)
    {
        Vector3 Centre{};
        for (const Vector3& Point : CapPoints) Centre = Add(Centre, Point);
        Centre = Multiply(Centre, 1.0 / static_cast<double>(CapPoints.size()));

        const Vector3 ReferenceAxis = std::abs(Normal[0]) < 0.8 ? Vector3{ 1.0, 0.0, 0.0 } : Vector3{ 0.0, 1.0, 0.0 };
        const Vector3 U             = Normalize(Cross(ReferenceAxis, Normal));
        const Vector3 V             = Cross(Normal, U);   // U × V = Normal, so increasing polar angle faces outward
        std::stable_sort(CapPoints.begin(), CapPoints.end(), [&](const Vector3& A, const Vector3& B) {
            const Vector3 DA = Subtract(A, Centre), DB = Subtract(B, Centre);
            return std::atan2(Dot(DA, V), Dot(DA, U)) < std::atan2(Dot(DB, V), Dot(DB, U));
        });

        ConvexFace Cap;
        Cap.CutToken = CutToken;
        Cap.Vertices.reserve(CapPoints.size());
        for (const Vector3& Point : CapPoints)
            AppendUnique(Cap.Vertices, AddUniqueVertex(NewVertices, Point, ToleranceSquared));
        CloseRing(Cap.Vertices);
        if (Cap.Vertices.size() >= 3 && PolygonArea(NewVertices, Cap.Vertices) > ToleranceSquared)
            NewFaces.push_back(std::move(Cap));
    }

    Cell.Vertices = std::move(NewVertices);
    Cell.Faces    = std::move(NewFaces);
    return Cell.Vertices.size() >= 4 && Cell.Faces.size() >= 4;
}

bool ComputeMetrics(ConvexCell& Cell)
{
    if (Cell.Vertices.size() < 4 || Cell.Faces.size() < 4) return false;
    const Vector3 Reference = Cell.Vertices.front();
    double        SignedVolume = 0.0;
    Vector3       WeightedCentre{};
    double        SurfaceArea = 0.0;

    for (const ConvexFace& Face : Cell.Faces)
    {
        if (Face.Vertices.size() < 3) return false;
        const Vector3 Origin = Cell.Vertices[Face.Vertices.front()];
        for (size_t Index = 1; Index + 1 < Face.Vertices.size(); ++Index)
        {
            const Vector3 A = Cell.Vertices[Face.Vertices[Index]];
            const Vector3 B = Cell.Vertices[Face.Vertices[Index + 1]];
            const Vector3 RA = Subtract(A, Reference), RB = Subtract(B, Reference), RO = Subtract(Origin, Reference);
            const double  SignedTetrahedron = Dot(RO, Cross(RA, RB)) / 6.0;
            SignedVolume += SignedTetrahedron;
            const Vector3 TetrahedronCentre = Multiply(Add(Add(Add(Reference, Origin), A), B), 0.25);
            WeightedCentre = Add(WeightedCentre, Multiply(TetrahedronCentre, SignedTetrahedron));
            SurfaceArea += 0.5 * Length(Cross(Subtract(A, Origin), Subtract(B, Origin)));
        }
    }

    if (!std::isfinite(SignedVolume) || std::abs(SignedVolume) <= std::numeric_limits<double>::min()) return false;
    if (SignedVolume < 0.0)
    {
        SignedVolume = -SignedVolume;
        WeightedCentre = Multiply(WeightedCentre, -1.0);
        for (ConvexFace& Face : Cell.Faces) std::reverse(Face.Vertices.begin(), Face.Vertices.end());
    }
    Cell.VolumeCubicMetres      = SignedVolume;
    Cell.Centroid               = Multiply(WeightedCentre, 1.0 / SignedVolume);
    Cell.SurfaceAreaSquareMetres = SurfaceArea;
    return IsFinite(Cell.Centroid) && std::isfinite(Cell.SurfaceAreaSquareMetres) && Cell.SurfaceAreaSquareMetres > 0.0;
}

bool MeasureFace(const ConvexCell& Cell, const ConvexFace& Face, FaceMetrics& Metrics) noexcept
{
    if (Face.Vertices.size() < 3) return false;
    Vector3 Centre{};
    Vector3 Normal{};
    const Vector3 Origin = Cell.Vertices[Face.Vertices.front()];
    for (uint32_t Index : Face.Vertices) Centre = Add(Centre, Cell.Vertices[Index]);
    Centre = Multiply(Centre, 1.0 / static_cast<double>(Face.Vertices.size()));
    for (size_t Index = 1; Index + 1 < Face.Vertices.size(); ++Index)
    {
        const Vector3 A = Cell.Vertices[Face.Vertices[Index]];
        const Vector3 B = Cell.Vertices[Face.Vertices[Index + 1]];
        const Vector3 CrossValue = Cross(Subtract(A, Origin), Subtract(B, Origin));
        Metrics.Area += 0.5 * Length(CrossValue);
        Normal = Add(Normal, CrossValue);
    }
    Metrics.Normal = Normalize(Normal);
    Metrics.Centre = Centre;
    return Metrics.Area > 0.0 && LengthSquared(Metrics.Normal) > 0.5 && IsFinite(Metrics.Centre);
}

bool FailValidation(std::string* Diagnostic, std::string Message)
{
    SetDiagnostic(Diagnostic, std::move(Message));
    return false;
}

bool ValidateRequest(const VoronoiFractureRequest& Request, std::string* Diagnostic)
{
    if (Request.CellCount == 0 || Request.CellCount > MaximumCellCount)
        return FailValidation(Diagnostic, "Cell count must be between 1 and 256.");
    if (!IsFinite(Request.HalfExtentsMetres) || !IsFinite(Request.ImpactPointMetres) || !std::isfinite(Request.ImpactRadialBias))
        return FailValidation(Diagnostic, "Fracture extents, impact point and radial bias must be finite.");
    for (double HalfExtent : Request.HalfExtentsMetres)
        if (HalfExtent <= 1.0e-8) return FailValidation(Diagnostic, "Every fracture half extent must be greater than 1e-8 metres.");
    if (Request.ImpactRadialBias < 0.0 || Request.ImpactRadialBias > 1.0)
        return FailValidation(Diagnostic, "Impact radial bias must be in the range 0 to 1.");
    return true;
}

uint32_t ConstructCutToken(uint32_t A, uint32_t B, uint32_t CellCount) noexcept
{
    const uint32_t Low  = std::min(A, B);
    const uint32_t High = std::max(A, B);
    return Low * CellCount + High + 1u;   // 0 remains the original boundary marker
}

bool BuildUnchecked(const VoronoiFractureRequest& Request, VoronoiFractureResult& Result, std::string* Diagnostic)
{
    Result.HalfExtentsMetres = Request.HalfExtentsMetres;
    Result.ImpactPointMetres = { ClampToExtent(Request.ImpactPointMetres[0], Request.HalfExtentsMetres[0]),
                                 ClampToExtent(Request.ImpactPointMetres[1], Request.HalfExtentsMetres[1]),
                                 ClampToExtent(Request.ImpactPointMetres[2], Request.HalfExtentsMetres[2]) };
    Result.SourceVolumeCubicMetres = 8.0 * Request.HalfExtentsMetres[0] * Request.HalfExtentsMetres[1] * Request.HalfExtentsMetres[2];
    if (!std::isfinite(Result.SourceVolumeCubicMetres) || Result.SourceVolumeCubicMetres <= 0.0)
        return FailValidation(Diagnostic, "The requested fracture volume is not finite and positive.");
    if (!ConstructSites(Request, Result.ImpactPointMetres, Result.Sites, Diagnostic)) return false;

    const double Scale     = std::max({ Request.HalfExtentsMetres[0], Request.HalfExtentsMetres[1], Request.HalfExtentsMetres[2] });
    const double Tolerance = std::max(1.0e-13, Scale * 1.0e-11);
    Result.Cells.reserve(Request.CellCount);
    for (uint32_t SiteIndex = 0; SiteIndex < Request.CellCount; ++SiteIndex)
    {
        ConvexCell Cell = ConstructBox(SiteIndex, Result.Sites[SiteIndex], Request.HalfExtentsMetres);
        for (uint32_t OtherIndex = 0; OtherIndex < Request.CellCount && !Cell.Vertices.empty(); ++OtherIndex)
        {
            if (OtherIndex == SiteIndex) continue;
            const Vector3 Delta = Subtract(Result.Sites[OtherIndex], Result.Sites[SiteIndex]);
            const double  Span  = Length(Delta);
            if (Span <= Tolerance) return FailValidation(Diagnostic, "Two Voronoi sites are too close to define a stable bisector.");
            const Vector3 Normal = Multiply(Delta, 1.0 / Span);
            const double  PlaneDistance = (LengthSquared(Result.Sites[OtherIndex]) - LengthSquared(Result.Sites[SiteIndex])) / (2.0 * Span);
            const uint32_t CutToken = ConstructCutToken(SiteIndex, OtherIndex, Request.CellCount);
            if (!ClipHalfSpace(Cell, Normal, PlaneDistance, CutToken, Tolerance)) break;
        }
        if (!ComputeMetrics(Cell))
            return FailValidation(Diagnostic, "A Voronoi site produced an empty or degenerate convex cell.");
        Result.CellVolumeCubicMetres += Cell.VolumeCubicMetres;
        Result.Cells.push_back(std::move(Cell));
    }
    Result.VolumeResidualCubicMetres = Result.CellVolumeCubicMetres - Result.SourceVolumeCubicMetres;
    return true;
}

} // namespace

bool BuildVoronoiFracture(const VoronoiFractureRequest& Request, VoronoiFractureResult& OutResult, std::string* Diagnostic)
{
    if (Diagnostic != nullptr) Diagnostic->clear();
    if (!ValidateRequest(Request, Diagnostic)) return false;
    VoronoiFractureResult Candidate;
    if (!BuildUnchecked(Request, Candidate, Diagnostic)) return false;
    if (!ValidateVoronoiFracture(Request, Candidate, Diagnostic)) return false;
    OutResult = std::move(Candidate);
    return true;
}

bool ValidateVoronoiFracture(const VoronoiFractureRequest& Request, const VoronoiFractureResult& Result, std::string* Diagnostic)
{
    if (Diagnostic != nullptr) Diagnostic->clear();
    if (!ValidateRequest(Request, Diagnostic)) return false;
    if (Result.Cells.size() != Request.CellCount || Result.Sites.size() != Request.CellCount)
        return FailValidation(Diagnostic, "The fracture result does not contain the requested number of cells and sites.");

    const double Scale = std::max({ Request.HalfExtentsMetres[0], Request.HalfExtentsMetres[1], Request.HalfExtentsMetres[2] });
    const double LinearTolerance = std::max(1.0e-10, Scale * 1.0e-8);
    const double AreaTolerance = LinearTolerance * Scale;
    const double VolumeTolerance = std::max(Result.SourceVolumeCubicMetres * 2.0e-8, 1.0e-14);
    const double ExpectedVolume = 8.0 * Request.HalfExtentsMetres[0] * Request.HalfExtentsMetres[1] * Request.HalfExtentsMetres[2];
    const double ExpectedArea = 8.0 * (Request.HalfExtentsMetres[0] * Request.HalfExtentsMetres[1] +
                                       Request.HalfExtentsMetres[1] * Request.HalfExtentsMetres[2] +
                                       Request.HalfExtentsMetres[2] * Request.HalfExtentsMetres[0]);
    double SumVolume = 0.0;
    double ExteriorArea = 0.0;
    std::map<uint32_t, std::vector<CutFaceReference>> CutFaces;

    for (size_t CellNumber = 0; CellNumber < Result.Cells.size(); ++CellNumber)
    {
        const ConvexCell& Cell = Result.Cells[CellNumber];
        if (Cell.SiteIndex != CellNumber || Cell.Vertices.size() < 4 || Cell.Faces.size() < 4 || !IsFinite(Cell.Centroid) ||
            !std::isfinite(Cell.VolumeCubicMetres) || Cell.VolumeCubicMetres <= 0.0)
            return FailValidation(Diagnostic, "A fracture cell has an invalid index, topology, centroid or volume.");
        if (LengthSquared(Subtract(Cell.Site, Result.Sites[CellNumber])) > LinearTolerance * LinearTolerance)
            return FailValidation(Diagnostic, "A fracture cell's stored site does not match its site table entry.");

        std::map<EdgeKey, uint32_t> EdgeUse;
        for (const ConvexFace& Face : Cell.Faces)
        {
            if (Face.Vertices.size() < 3) return FailValidation(Diagnostic, "A fracture face has fewer than three vertices.");
            for (uint32_t Index : Face.Vertices)
                if (Index >= Cell.Vertices.size()) return FailValidation(Diagnostic, "A fracture face refers to a vertex outside its cell.");

            FaceMetrics Metrics;
            if (!MeasureFace(Cell, Face, Metrics) || Metrics.Area <= AreaTolerance * 1.0e-6)
                return FailValidation(Diagnostic, "A fracture face is degenerate.");
            const Vector3 Inward = Subtract(Cell.Centroid, Metrics.Centre);
            if (Dot(Metrics.Normal, Inward) >= -LinearTolerance)
                return FailValidation(Diagnostic, "A fracture face is not oriented outward from its convex cell.");

            const Vector3 FaceOrigin = Cell.Vertices[Face.Vertices.front()];
            for (const Vector3& Point : Cell.Vertices)
            {
                if (Dot(Metrics.Normal, Subtract(Point, FaceOrigin)) > LinearTolerance * 8.0)
                    return FailValidation(Diagnostic, "A cell vertex lies outside one of its face half-spaces; the cell is not convex.");
                if (Dot(Metrics.Normal, Subtract(Cell.Site, FaceOrigin)) > LinearTolerance * 8.0)
                    return FailValidation(Diagnostic, "A Voronoi seed lies outside its own convex cell.");
            }

            if (Face.CutToken == 0)
            {
                bool OnBoundaryPlane = false;
                for (size_t Axis = 0; Axis < 3 && !OnBoundaryPlane; ++Axis)
                {
                    for (double Sign : { -1.0, 1.0 })
                    {
                        const double Plane = Sign * Request.HalfExtentsMetres[Axis];
                        const bool AllOnPlane = std::all_of(Face.Vertices.begin(), Face.Vertices.end(), [&](uint32_t Index) {
                            return std::abs(Cell.Vertices[Index][Axis] - Plane) <= LinearTolerance * 8.0;
                        });
                        if (AllOnPlane)
                        {
                            OnBoundaryPlane = true;
                            break;
                        }
                    }
                }
                if (!OnBoundaryPlane) return FailValidation(Diagnostic, "An untagged fracture face is not on the source-box boundary.");
                ExteriorArea += Metrics.Area;
            }
            else
            {
                CutFaces[Face.CutToken].push_back({ static_cast<uint32_t>(CellNumber), Metrics });
            }

            for (size_t Edge = 0; Edge < Face.Vertices.size(); ++Edge)
            {
                const uint32_t A = Face.Vertices[Edge];
                const uint32_t B = Face.Vertices[(Edge + 1u) % Face.Vertices.size()];
                if (A == B) return FailValidation(Diagnostic, "A fracture face contains a zero-length topological edge.");
                ++EdgeUse[{ std::min(A, B), std::max(A, B) }];
            }
        }
        for (const auto& [Edge, Uses] : EdgeUse)
        {
            (void)Edge;
            if (Uses != 2) return FailValidation(Diagnostic, "A fracture cell is not a closed two-manifold (an edge is not shared by two faces).");
        }
        SumVolume += Cell.VolumeCubicMetres;
    }

    for (const auto& [Token, Faces] : CutFaces)
    {
        if (Faces.size() != 2 || Faces[0].CellIndex == Faces[1].CellIndex)
            return FailValidation(Diagnostic, "A cut-face token does not identify exactly two neighboring cells.");
        const uint32_t Encoded = Token - 1u;
        const uint32_t Low = Encoded / Request.CellCount;
        const uint32_t High = Encoded % Request.CellCount;
        if (High <= Low || std::min(Faces[0].CellIndex, Faces[1].CellIndex) != Low ||
            std::max(Faces[0].CellIndex, Faces[1].CellIndex) != High)
            return FailValidation(Diagnostic, "A cut-face token does not match the pair of cells that own it.");
        const double AreaScale = std::max({ Faces[0].Metrics.Area, Faces[1].Metrics.Area, AreaTolerance });
        if (std::abs(Faces[0].Metrics.Area - Faces[1].Metrics.Area) > AreaScale * 2.0e-7 ||
            Length(Subtract(Faces[0].Metrics.Centre, Faces[1].Metrics.Centre)) > LinearTolerance * 16.0 ||
            Dot(Faces[0].Metrics.Normal, Faces[1].Metrics.Normal) > -0.99999)
            return FailValidation(Diagnostic, "The two faces sharing a cut token do not form a matching, oppositely oriented interface.");
    }

    if (std::abs(SumVolume - ExpectedVolume) > VolumeTolerance)
        return FailValidation(Diagnostic, "Voronoi cell volumes do not close to the source volume.");
    if (std::abs(Result.SourceVolumeCubicMetres - ExpectedVolume) > VolumeTolerance ||
        std::abs(Result.CellVolumeCubicMetres - SumVolume) > VolumeTolerance ||
        std::abs(Result.VolumeResidualCubicMetres - (SumVolume - ExpectedVolume)) > VolumeTolerance)
        return FailValidation(Diagnostic, "The fracture result's volume summary does not match its cells.");
    if (std::abs(ExteriorArea - ExpectedArea) > std::max(ExpectedArea * 2.0e-7, AreaTolerance * 16.0))
        return FailValidation(Diagnostic, "Exterior fracture faces do not cover the source-box surface area.");

    SetDiagnostic(Diagnostic, {});
    return true;
}

} // namespace Frontier::FractureDynamics
