/*==========================================================================================================================================
                                                            VORONOI FRACTURE
==========================================================================================================================================*/
// 🧩 Deterministic convex-cell generation for Fracture authoring: a box partitioned by clipped Voronoi half-spaces, with
//    paired cut-face tokens and closed-polyhedron metrics. This is geometry authoring/bake input, not a runtime stress solver.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Frontier::FractureDynamics
{

using Vector3 = std::array<double, 3>;

struct ConvexFace
{
    std::vector<uint32_t> Vertices;
    uint32_t              CutToken = 0;   // 0 = original boundary; non-zero = shared cut plane
};

struct ConvexCell
{
    uint32_t             SiteIndex = 0;
    Vector3              Site{};
    std::vector<Vector3> Vertices;
    std::vector<ConvexFace> Faces;
    Vector3              Centroid{};
    double               VolumeCubicMetres = 0.0;
    double               SurfaceAreaSquareMetres = 0.0;
};

struct VoronoiFractureRequest
{
    uint32_t CellCount = 32;                          // [4, 256] in the authoring UI
    Vector3  HalfExtentsMetres = { 0.5, 0.5, 0.5 };  // local, unrotated half extents
    Vector3  ImpactPointMetres = { 0.0, 0.0, 0.0 };
    double   ImpactRadialBias = 0.65;                 // [0, 1], increases site density around the impact point
    uint64_t Seed = 1;
};

struct VoronoiFractureResult
{
    Vector3              HalfExtentsMetres{};
    Vector3              ImpactPointMetres{};
    std::vector<Vector3> Sites;
    std::vector<ConvexCell> Cells;
    double               SourceVolumeCubicMetres = 0.0;
    double               CellVolumeCubicMetres = 0.0;
    double               VolumeResidualCubicMetres = 0.0;
};

// 📝 Builds the deterministic Voronoi partition and verifies manifold closure, shared cut faces, convexity and volume coverage.
//    On failure, OutResult is unchanged and Diagnostic receives the first failure when supplied.
[[nodiscard]] bool BuildVoronoiFracture(const VoronoiFractureRequest& Request,
                                        VoronoiFractureResult&        OutResult,
                                        std::string*                  Diagnostic = nullptr);

[[nodiscard]] bool ValidateVoronoiFracture(const VoronoiFractureRequest& Request,
                                           const VoronoiFractureResult&  Result,
                                           std::string*                  Diagnostic = nullptr);

} // namespace Frontier::FractureDynamics
