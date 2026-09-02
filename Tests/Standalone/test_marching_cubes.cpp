#include "KrigePortableMarchingCubes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace kriging::portable;

namespace
{
int failures = 0;
void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Quantized
{
    long long x;
    long long y;
    long long z;
    bool operator==(const Quantized& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct QuantizedHash
{
    std::size_t operator()(const Quantized& value) const
    {
        std::size_t result = static_cast<std::size_t>(value.x);
        result ^= static_cast<std::size_t>(value.y) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        result ^= static_cast<std::size_t>(value.z) + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        return result;
    }
};

// Aggregate mesh-topology facts used to detect welding/table defects that a
// smooth radial-error metric alone would miss (see F3/M2/M3 in the
// marching-cubes adversarial review): degenerate (repeated-index) triangles,
// edges not shared by exactly two triangles, and vertices no surviving
// triangle references.
struct MeshTopology
{
    std::size_t degenerateTriangles = 0;
    std::size_t boundaryEdges = 0;    // edges used by exactly one triangle.
    std::size_t nonManifoldEdges = 0; // edges used by more than two triangles.
    std::size_t orphanVertices = 0;   // vertices referenced by no triangle.
    long long eulerCharacteristic = 0;
};

MeshTopology AnalyzeTopology(const IsoSurfaceMesh& mesh)
{
    MeshTopology topology;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeCounts;
    std::vector<bool> referenced(mesh.vertices.size(), false);
    const std::size_t triangleCount = mesh.indices.size() / 3U;
    auto addEdge = [&](std::uint32_t p, std::uint32_t q)
    {
        const auto key = p < q ? std::make_pair(p, q) : std::make_pair(q, p);
        ++edgeCounts[key];
    };
    for (std::size_t t = 0; t < triangleCount; ++t)
    {
        const std::uint32_t a = mesh.indices[t * 3U + 0U];
        const std::uint32_t b = mesh.indices[t * 3U + 1U];
        const std::uint32_t c = mesh.indices[t * 3U + 2U];
        if (a == b || b == c || a == c)
        {
            ++topology.degenerateTriangles;
            continue;
        }
        referenced[a] = true;
        referenced[b] = true;
        referenced[c] = true;
        addEdge(a, b);
        addEdge(b, c);
        addEdge(c, a);
    }
    for (const auto& entry : edgeCounts)
    {
        if (entry.second == 1)
        {
            ++topology.boundaryEdges;
        }
        else if (entry.second > 2)
        {
            ++topology.nonManifoldEdges;
        }
    }
    for (bool isReferenced : referenced)
    {
        if (!isReferenced)
        {
            ++topology.orphanVertices;
        }
    }
    const long long v = static_cast<long long>(mesh.vertices.size());
    const long long e = static_cast<long long>(edgeCounts.size());
    const long long f = static_cast<long long>(triangleCount - topology.degenerateTriangles);
    topology.eulerCharacteristic = v - e + f;
    return topology;
}

void CheckClosedManifold(const MeshTopology& topology, const std::string& label)
{
    Check(topology.degenerateTriangles == 0, label + ": no degenerate triangles");
    Check(topology.boundaryEdges == 0, label + ": no boundary edges (surface is closed)");
    Check(topology.nonManifoldEdges == 0, label + ": no non-manifold edges");
    Check(topology.orphanVertices == 0, label + ": no orphaned vertices");
    Check(topology.eulerCharacteristic == 2,
          label + ": Euler characteristic is 2 (topological sphere)");
}

// Returns true if at least `minimum` of the vertex's three coordinates fall
// exactly on a lattice plane (integer multiple of the cell size from the
// grid origin). A genuine marching-cubes vertex lies on a lattice *edge*, so
// two of its three coordinates are lattice-exact (the interpolation happens
// along the third axis only); a corner-transposition defect (see M1/M3) can
// instead place the point on a face or space diagonal, where at most one
// coordinate (or none) is lattice-exact.
bool CoordinateOnLattice(double value, double origin, double cellSize, double tolerance)
{
    const double units = (value - origin) / cellSize;
    const double nearest = std::round(units);
    return std::abs(units - nearest) <= tolerance;
}

int LatticeAlignedAxisCount(const ScalarGrid3D& grid, const Vec3& v, double tolerance)
{
    int count = 0;
    if (CoordinateOnLattice(v.x, grid.origin.x, grid.cellSize.x, tolerance)) ++count;
    if (CoordinateOnLattice(v.y, grid.origin.y, grid.cellSize.y, tolerance)) ++count;
    if (CoordinateOnLattice(v.z, grid.origin.z, grid.cellSize.z, tolerance)) ++count;
    return count;
}

// A tilted, anisotropic quadratic field (distinct axis coefficients plus
// off-diagonal cross terms and a small deterministic high-frequency
// perturbation), used as a second, asymmetric closed surface distinct from
// the sphere. Note what this field does and does not exercise: because
// ExtractMarchingCubes caches each shared lattice edge under whichever
// cube-local edge index reaches it first in (z, y, x) iteration order, on
// any fully interior surface -- this field included -- only edge indices 5,
// 6, and 10 ever reach AddInterpolatedVertex as a genuine first creation;
// the other nine are always satisfied from a neighboring cube's cache. So
// this field's residual and on-lattice-axis checks below exercise
// EdgeCorners[5], [6], and [10] with asymmetric corner data and give a
// second, independent closed-topology/manifold check -- but a transposition
// confined to one of the other nine EdgeCorners entries (as in mutant M1)
// would not show up here on any interior field, however asymmetric.
// RunSingleCubeAllEdgesTest, below, is what catches that class: it uses a
// single-cube grid (no neighbors, so nothing can be cached) to force every
// one of the 12 edges through a genuine creation call and checks the
// resulting vertex positions against known unit-cube geometry.
double TiltedField(double x, double y, double z)
{
    return 2.0 * x * x + 2.6 * y * y + 3.1 * z * z
        + 0.5 * x * y + 0.3 * y * z + 0.2 * x * z
        + 0.05 * std::sin(5.0 * x) * std::cos(4.0 * y) * std::sin(3.0 * z);
}

constexpr double kTiltedIsoValue = 0.5;

ScalarGrid3D BuildTiltedFieldGrid()
{
    ScalarGrid3D grid;
    grid.sizeX = 33;
    grid.sizeY = 33;
    grid.sizeZ = 33;
    grid.cellSize = {0.0625, 0.0625, 0.0625};
    grid.origin = {-1.0, -1.0, -1.0};
    grid.values.resize(static_cast<std::size_t>(grid.Count()));
    std::size_t write = 0;
    for (int z = 0; z < grid.sizeZ; ++z)
    {
        for (int y = 0; y < grid.sizeY; ++y)
        {
            for (int x = 0; x < grid.sizeX; ++x)
            {
                const double px = grid.origin.x + grid.cellSize.x * x;
                const double py = grid.origin.y + grid.cellSize.y * y;
                const double pz = grid.origin.z + grid.cellSize.z * z;
                grid.values[write++] = TiltedField(px, py, pz);
            }
        }
    }
    return grid;
}

void RunDegenerateTriangleGuardTest()
{
    // Positive control: a single well-formed triangle must validate.
    IsoSurfaceMesh clean;
    clean.vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
    clean.normals = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}};
    clean.indices = {0, 1, 2};
    std::string cleanError;
    Check(clean.IsValid(&cleanError), "a non-degenerate triangle validates: " + cleanError);

    // Negative control: the only defect is a repeated index within the
    // triangle (the F3 degenerate-triangle class); everything else about the
    // mesh (lengths, index range) is identical to the clean mesh above.
    IsoSurfaceMesh degenerate = clean;
    degenerate.indices = {0, 0, 1};
    std::string degenerateError;
    Check(!degenerate.IsValid(&degenerateError),
          "a triangle with a repeated vertex index is rejected");
}

// Exercises every one of the 12 cube edges as a genuine first-creation call
// (not a cache hit off a neighboring cube), which no continuous, fully
// interior field can do: ExtractMarchingCubes caches each shared lattice
// edge under whichever cube-local edge index visits it first in (z, y, x)
// iteration order, so for any interior surface only edge indices 5, 6, and
// 10 ever reach AddInterpolatedVertex as a fresh creation -- edges 0-4, 7-9,
// and 11 are always satisfied from a neighbor's cache. A single-cube grid
// has no neighbors, so every one of its crossing edges is necessarily a
// first creation. Corner values are assigned by coordinate-sum parity (a
// proper 2-coloring of the cube graph), so all 12 edges cross the isovalue,
// each with the same interpolation parameter t = 0.5, landing exactly on
// the 12 canonical edge midpoints of a unit cube -- letting the test check
// CornerOffset and EdgeCorners directly against known geometry without
// duplicating either table. This is what lets the test catch a defect like
// M1 (EdgeCorners entries for a "dead" edge index transposed) that a
// topology or residual check on any interior mesh cannot see.
void RunSingleCubeAllEdgesTest()
{
    ScalarGrid3D grid;
    grid.sizeX = 2;
    grid.sizeY = 2;
    grid.sizeZ = 2;
    grid.origin = {0.0, 0.0, 0.0};
    grid.cellSize = {1.0, 1.0, 1.0};
    grid.values.resize(static_cast<std::size_t>(grid.Count()));
    std::size_t write = 0;
    for (int z = 0; z < 2; ++z)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                grid.values[write++] = ((x + y + z) % 2 == 0) ? 0.0 : 1.0;
            }
        }
    }

    IsoSurfaceMesh mesh;
    std::string error;
    Check(ExtractMarchingCubes(grid, 0.5, mesh, &error), "single-cube extraction: " + error);
    Check(mesh.vertices.size() == 12U,
          "single-cube mesh creates exactly one vertex per cube edge (no unexpected welding)");

    const std::array<Vec3, 12> expected = {{
        {0.5, 0.0, 0.0}, {1.0, 0.5, 0.0}, {0.5, 1.0, 0.0}, {0.0, 0.5, 0.0},
        {0.5, 0.0, 1.0}, {1.0, 0.5, 1.0}, {0.5, 1.0, 1.0}, {0.0, 0.5, 1.0},
        {0.0, 0.0, 0.5}, {1.0, 0.0, 0.5}, {1.0, 1.0, 0.5}, {0.0, 1.0, 0.5}
    }};
    std::vector<bool> vertexMatched(mesh.vertices.size(), false);
    constexpr double kExactTolerance = 1.0e-9;
    for (const Vec3& target : expected)
    {
        bool foundMatch = false;
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            if (vertexMatched[i])
            {
                continue;
            }
            const Vec3& v = mesh.vertices[i];
            if (std::abs(v.x - target.x) <= kExactTolerance
                && std::abs(v.y - target.y) <= kExactTolerance
                && std::abs(v.z - target.z) <= kExactTolerance)
            {
                vertexMatched[i] = true;
                foundMatch = true;
                break;
            }
        }
        Check(foundMatch, "single-cube mesh has a vertex at canonical edge midpoint ("
            + std::to_string(target.x) + ", " + std::to_string(target.y) + ", "
            + std::to_string(target.z) + ")");
    }
}
}

int main()
{
    RunDegenerateTriangleGuardTest();
    RunSingleCubeAllEdgesTest();

    ScalarGrid3D grid;
    grid.sizeX = 41;
    grid.sizeY = 41;
    grid.sizeZ = 41;
    grid.cellSize = {0.05, 0.05, 0.05};
    grid.origin = {-1.0, -1.0, -1.0};
    grid.values.resize(static_cast<std::size_t>(grid.Count()));
    std::size_t write = 0;
    for (int z = 0; z < grid.sizeZ; ++z)
    {
        for (int y = 0; y < grid.sizeY; ++y)
        {
            for (int x = 0; x < grid.sizeX; ++x)
            {
                const double px = grid.origin.x + grid.cellSize.x * x;
                const double py = grid.origin.y + grid.cellSize.y * y;
                const double pz = grid.origin.z + grid.cellSize.z * z;
                grid.values[write++] = std::sqrt(px * px + py * py + pz * pz);
            }
        }
    }

    IsoSurfaceMesh mesh;
    std::string error;
    Check(ExtractMarchingCubes(grid, 0.7, mesh, &error), "sphere extraction: " + error);
    Check(!mesh.vertices.empty(), "sphere has vertices");
    Check(!mesh.indices.empty(), "sphere has triangles");
    Check(mesh.IsValid(&error), "mesh validates: " + error);

    double maximumRadiusError = 0.0;
    double minimumNormalDot = 1.0;
    std::unordered_set<Quantized, QuantizedHash> unique;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        const Vec3& v = mesh.vertices[i];
        const Vec3& n = mesh.normals[i];
        const double radius = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        maximumRadiusError = std::max(maximumRadiusError, std::abs(radius - 0.7));
        if (radius > 0.0)
        {
            minimumNormalDot = std::min(minimumNormalDot,
                (v.x * n.x + v.y * n.y + v.z * n.z) / radius);
        }
        unique.insert({std::llround(v.x * 1000000000.0),
                       std::llround(v.y * 1000000000.0),
                       std::llround(v.z * 1000000000.0)});
    }
    // Tightened from the original 0.05 (~125x the observed 0.0004 error) to a
    // still-comfortable ~5x margin so a real interpolation regression trips
    // this check instead of hiding inside slack.
    Check(maximumRadiusError <= 0.002, "sphere radius within tight tolerance of the isovalue");
    Check(minimumNormalDot > 0.95, "normals face toward increasing scalar values");
    Check(unique.size() == mesh.vertices.size(), "shared grid edges are welded exactly");

    const MeshTopology sphereTopology = AnalyzeTopology(mesh);
    CheckClosedManifold(sphereTopology, "sphere mesh");

    IsoSurfaceMesh repeat;
    Check(ExtractMarchingCubes(grid, 0.7, repeat, &error), "repeat extraction");
    Check(mesh.indices == repeat.indices, "triangle indices are deterministic");
    Check(mesh.vertices.size() == repeat.vertices.size(), "vertex count deterministic");
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        Check(mesh.vertices[i].x == repeat.vertices[i].x
            && mesh.vertices[i].y == repeat.vertices[i].y
            && mesh.vertices[i].z == repeat.vertices[i].z,
            "vertex ordering is deterministic");
    }

    // Second field: an asymmetric, tilted quadratic blob whose isosurface is
    // strictly interior to the grid (verified explicitly below). It gives a
    // second, independent closed-manifold/topology check, and its residual
    // and lattice-alignment checks exercise the three edge indices (5, 6,
    // 10) that a fully interior surface ever creates fresh with asymmetric
    // corner data (see the comment on TiltedField above for why only those
    // three). Corner/edge-table transpositions confined to the other nine
    // edge indices are caught separately by RunSingleCubeAllEdgesTest, not
    // by this field.
    const ScalarGrid3D tiltedGrid = BuildTiltedFieldGrid();
    bool boundaryStrictlyOutside = true;
    for (int z = 0; z < tiltedGrid.sizeZ; ++z)
    {
        for (int y = 0; y < tiltedGrid.sizeY; ++y)
        {
            for (int x = 0; x < tiltedGrid.sizeX; ++x)
            {
                const bool onBoundary = x == 0 || y == 0 || z == 0
                    || x == tiltedGrid.sizeX - 1
                    || y == tiltedGrid.sizeY - 1
                    || z == tiltedGrid.sizeZ - 1;
                if (!onBoundary)
                {
                    continue;
                }
                if (!(tiltedGrid.At(x, y, z) > kTiltedIsoValue + 0.4))
                {
                    boundaryStrictlyOutside = false;
                }
            }
        }
    }
    Check(boundaryStrictlyOutside,
          "tilted-field isosurface stays strictly interior to the grid (no clipped boundary)");

    IsoSurfaceMesh tiltedMesh;
    Check(ExtractMarchingCubes(tiltedGrid, kTiltedIsoValue, tiltedMesh, &error),
          "tilted-field extraction: " + error);
    Check(!tiltedMesh.vertices.empty(), "tilted-field mesh has vertices");
    Check(!tiltedMesh.indices.empty(), "tilted-field mesh has triangles");
    Check(tiltedMesh.IsValid(&error), "tilted-field mesh validates: " + error);

    double tiltedMaxResidual = 0.0;
    int tiltedMinLatticeAlignedAxes = 3;
    for (const Vec3& v : tiltedMesh.vertices)
    {
        tiltedMaxResidual = std::max(tiltedMaxResidual,
            std::abs(TiltedField(v.x, v.y, v.z) - kTiltedIsoValue));
        tiltedMinLatticeAlignedAxes = std::min(tiltedMinLatticeAlignedAxes,
            LatticeAlignedAxisCount(tiltedGrid, v, 1.0e-6));
    }
    // Empirically the pristine extractor lands within a small fraction of a
    // cell's field variation of the isovalue; a wrong-corner or wrong-edge
    // table entry displaces the interpolation enough to blow well past this.
    Check(tiltedMaxResidual <= 0.02,
          "tilted-field vertices satisfy the field equation near the isovalue");
    Check(tiltedMinLatticeAlignedAxes >= 2,
          "tilted-field vertices lie on a lattice edge (>=2 axis-aligned coordinates)");

    const MeshTopology tiltedTopology = AnalyzeTopology(tiltedMesh);
    CheckClosedManifold(tiltedTopology, "tilted-field mesh");

    if (failures == 0)
    {
        std::cout << "Portable marching-cubes tests passed.\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
