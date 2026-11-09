/*==========================================================================================================================================
                                                  FRACTURE GEOMETRY VALIDATION
==========================================================================================================================================*/
// 🧩 The native Fracture authoring kernel: deterministic Voronoi sites, closed convex cells, shared cut-token interfaces,
//    convexity, source-boundary coverage and volume conservation. This does not claim to validate the runtime stress solver.

#include "Frontier/Engine/PhysicalDynamics/FractureDynamics/VoronoiFracture.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Frontier::FractureDynamics;

namespace
{

int Failures = 0;
int Assertions = 0;

void Expect(bool Condition, const std::string& Claim)
{
    ++Assertions;
    if (!Condition)
    {
        ++Failures;
        std::printf("  \xF0\x9F\x94\xB4 %s\n", Claim.c_str());
    }
}

bool SameVector(const Vector3& A, const Vector3& B)
{
    return A[0] == B[0] && A[1] == B[1] && A[2] == B[2];
}

bool SameResult(const VoronoiFractureResult& A, const VoronoiFractureResult& B)
{
    if (A.Cells.size() != B.Cells.size() || A.Sites.size() != B.Sites.size() || A.SourceVolumeCubicMetres != B.SourceVolumeCubicMetres ||
        A.CellVolumeCubicMetres != B.CellVolumeCubicMetres || A.VolumeResidualCubicMetres != B.VolumeResidualCubicMetres)
        return false;
    for (size_t Index = 0; Index < A.Sites.size(); ++Index)
        if (!SameVector(A.Sites[Index], B.Sites[Index])) return false;
    for (size_t CellIndex = 0; CellIndex < A.Cells.size(); ++CellIndex)
    {
        const ConvexCell& Left = A.Cells[CellIndex];
        const ConvexCell& Right = B.Cells[CellIndex];
        if (Left.SiteIndex != Right.SiteIndex || !SameVector(Left.Site, Right.Site) || !SameVector(Left.Centroid, Right.Centroid) ||
            Left.VolumeCubicMetres != Right.VolumeCubicMetres || Left.SurfaceAreaSquareMetres != Right.SurfaceAreaSquareMetres ||
            Left.Vertices.size() != Right.Vertices.size() || Left.Faces.size() != Right.Faces.size())
            return false;
        for (size_t Vertex = 0; Vertex < Left.Vertices.size(); ++Vertex)
            if (!SameVector(Left.Vertices[Vertex], Right.Vertices[Vertex])) return false;
        for (size_t Face = 0; Face < Left.Faces.size(); ++Face)
            if (Left.Faces[Face].CutToken != Right.Faces[Face].CutToken || Left.Faces[Face].Vertices != Right.Faces[Face].Vertices) return false;
    }
    return true;
}

double MeanSiteDistance(const VoronoiFractureResult& Result, const Vector3& Impact)
{
    if (Result.Sites.size() < 2) return 0.0;
    double Sum = 0.0;
    for (size_t Index = 1; Index < Result.Sites.size(); ++Index)
    {
        const double X = Result.Sites[Index][0] - Impact[0];
        const double Y = Result.Sites[Index][1] - Impact[1];
        const double Z = Result.Sites[Index][2] - Impact[2];
        Sum += std::sqrt(X * X + Y * Y + Z * Z);
    }
    return Sum / static_cast<double>(Result.Sites.size() - 1u);
}

} // namespace

int main()
{
    std::printf("FractureGeometryValidation\n");
    const uint32_t Counts[] = { 1u, 4u, 12u, 32u, 64u, 256u };
    for (uint32_t Count : Counts)
    {
        VoronoiFractureRequest Request;
        Request.CellCount         = Count;
        Request.HalfExtentsMetres = { 1.7, 0.85, 1.2 };
        Request.ImpactPointMetres = { 0.18, 0.22, -0.31 };
        Request.ImpactRadialBias  = 0.65;
        Request.Seed              = 0x8c3c010cb4754c91ull;

        VoronoiFractureResult Result;
        std::string           Diagnostic;
        const bool            Built = BuildVoronoiFracture(Request, Result, &Diagnostic);
        Expect(Built, "build " + std::to_string(Count) + " cells (" + Diagnostic + ")");
        if (!Built) continue;
        Expect(ValidateVoronoiFracture(Request, Result, &Diagnostic), "validate " + std::to_string(Count) + " cells (" + Diagnostic + ")");
        Expect(Result.Cells.size() == Count && Result.Sites.size() == Count, "cell/site count " + std::to_string(Count));
        const double RelativeResidual = std::abs(Result.VolumeResidualCubicMetres) / Result.SourceVolumeCubicMetres;
        Expect(RelativeResidual <= 2.0e-8, "volume closure for " + std::to_string(Count) + " cells, relative residual " + std::to_string(RelativeResidual));
        std::printf("  %3u cells: %zu sites, volume %.12g / %.12g m^3, residual %.3g m^3\n", Count, Result.Sites.size(),
                    Result.CellVolumeCubicMetres, Result.SourceVolumeCubicMetres, Result.VolumeResidualCubicMetres);
    }

    // The same seed and authoring inputs replay the exact site order, face indices, tokens and metrics.
    VoronoiFractureRequest DeterministicRequest;
    DeterministicRequest.CellCount = 32;
    DeterministicRequest.HalfExtentsMetres = { 2.0, 1.0, 0.75 };
    DeterministicRequest.ImpactPointMetres = { -0.25, 0.1, 0.3 };
    DeterministicRequest.ImpactRadialBias = 0.4;
    DeterministicRequest.Seed = 90125;
    VoronoiFractureResult DeterministicA, DeterministicB;
    std::string Diagnostic;
    const bool BuiltA = BuildVoronoiFracture(DeterministicRequest, DeterministicA, &Diagnostic);
    const bool BuiltB = BuildVoronoiFracture(DeterministicRequest, DeterministicB, &Diagnostic);
    Expect(BuiltA && BuiltB && SameResult(DeterministicA, DeterministicB), "identical seed replays identical cells and face topology");

    // Radial bias is a site-density control, not a change to the source volume or clipper.
    VoronoiFractureRequest UniformRequest = DeterministicRequest;
    UniformRequest.CellCount = 64;
    UniformRequest.ImpactRadialBias = 0.0;
    VoronoiFractureRequest BiasedRequest = UniformRequest;
    BiasedRequest.ImpactRadialBias = 1.0;
    VoronoiFractureResult UniformResult, BiasedResult;
    const bool UniformBuilt = BuildVoronoiFracture(UniformRequest, UniformResult, &Diagnostic);
    const bool BiasedBuilt = BuildVoronoiFracture(BiasedRequest, BiasedResult, &Diagnostic);
    const double UniformDistance = UniformBuilt ? MeanSiteDistance(UniformResult, UniformRequest.ImpactPointMetres) : 0.0;
    const double BiasedDistance = BiasedBuilt ? MeanSiteDistance(BiasedResult, BiasedRequest.ImpactPointMetres) : 0.0;
    Expect(UniformBuilt && BiasedBuilt && BiasedDistance < UniformDistance,
           "radial bias increases site density near impact (uniform " + std::to_string(UniformDistance) + " m, biased " + std::to_string(BiasedDistance) + " m)");

    // Geometry records can have thin, anisotropic extents; this remains a watertight partition.
    VoronoiFractureRequest ThinRequest;
    ThinRequest.CellCount = 48;
    ThinRequest.HalfExtentsMetres = { 2.5, 0.14, 0.9 };
    ThinRequest.ImpactPointMetres = { 1.9, 0.12, -0.4 };
    ThinRequest.ImpactRadialBias = 0.7;
    ThinRequest.Seed = 0x5348415045ull;
    VoronoiFractureResult ThinResult;
    const bool ThinBuilt = BuildVoronoiFracture(ThinRequest, ThinResult, &Diagnostic);
    Expect(ThinBuilt && ValidateVoronoiFracture(ThinRequest, ThinResult, &Diagnostic), "anisotropic boundary impact stays closed (" + Diagnostic + ")");

    // Reject malformed inputs transactionally; an earlier good result remains intact.
    VoronoiFractureResult Kept = DeterministicA;
    VoronoiFractureRequest InvalidRequest = DeterministicRequest;
    InvalidRequest.CellCount = 0;
    Expect(!BuildVoronoiFracture(InvalidRequest, Kept, &Diagnostic) && !Diagnostic.empty() && SameResult(Kept, DeterministicA),
           "invalid request is refused without replacing the last good result");
    InvalidRequest = DeterministicRequest;
    InvalidRequest.ImpactRadialBias = 1.5;
    Expect(!BuildVoronoiFracture(InvalidRequest, Kept, &Diagnostic), "out-of-range impact bias is refused");

    std::printf("%s %d / %d assertions\n", Failures == 0 ? "\xF0\x9F\x9F\xA2" : "\xF0\x9F\x94\xB4", Assertions - Failures, Assertions);
    return Failures == 0 ? 0 : 1;
}
