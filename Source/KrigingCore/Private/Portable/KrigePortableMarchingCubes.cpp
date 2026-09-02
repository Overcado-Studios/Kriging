#include "KrigePortableMarchingCubes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace kriging::portable
{
namespace
{
#include "KrigeMarchingCubesTables.inl"

constexpr std::array<std::array<int, 3>, 8> CornerOffset = {{
    {{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}},
    {{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}}
}};

constexpr std::array<std::array<int, 2>, 12> EdgeCorners = {{
    {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}},
    {{4, 5}}, {{5, 6}}, {{6, 7}}, {{7, 4}},
    {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}}
}};

struct WeldKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const WeldKey& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct WeldKeyHash
{
    std::size_t operator()(const WeldKey& value) const
    {
        std::size_t result = static_cast<std::size_t>(value.x);
        result ^= static_cast<std::size_t>(value.y) + 0x9e3779b97f4a7c15ULL
            + (result << 6U) + (result >> 2U);
        result ^= static_cast<std::size_t>(value.z) + 0x9e3779b97f4a7c15ULL
            + (result << 6U) + (result >> 2U);
        return result;
    }
};

using WeldMap = std::unordered_map<WeldKey, std::uint32_t, WeldKeyHash>;

Vec3 Add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Multiply(const Vec3& value, double scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

double Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

Vec3 Normalize(const Vec3& value)
{
    const double squared = Dot(value, value);
    if (!(squared > std::numeric_limits<double>::min()) || !std::isfinite(squared))
    {
        return {0.0, 0.0, 1.0};
    }
    return Multiply(value, 1.0 / std::sqrt(squared));
}

std::size_t LatticeIndex(const ScalarGrid3D& grid, int x, int y, int z)
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(grid.sizeX)
            * (static_cast<std::size_t>(y)
                + static_cast<std::size_t>(grid.sizeY) * static_cast<std::size_t>(z));
}

std::size_t XEdgeIndex(int sizeX, int x, int y)
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(sizeX - 1) * static_cast<std::size_t>(y);
}

std::size_t YEdgeIndex(int sizeX, int x, int y)
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(y);
}

std::size_t ZEdgeIndex(int sizeX, int x, int y)
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(y);
}

Vec3 PositionAt(const ScalarGrid3D& grid, int x, int y, int z)
{
    return {grid.origin.x + grid.cellSize.x * static_cast<double>(x),
            grid.origin.y + grid.cellSize.y * static_cast<double>(y),
            grid.origin.z + grid.cellSize.z * static_cast<double>(z)};
}

Vec3 GradientAt(const ScalarGrid3D& grid, int x, int y, int z)
{
    const int x0 = std::max(0, x - 1);
    const int x1 = std::min(grid.sizeX - 1, x + 1);
    const int y0 = std::max(0, y - 1);
    const int y1 = std::min(grid.sizeY - 1, y + 1);
    const int z0 = std::max(0, z - 1);
    const int z1 = std::min(grid.sizeZ - 1, z + 1);

    const double dx = grid.cellSize.x * static_cast<double>(x1 - x0);
    const double dy = grid.cellSize.y * static_cast<double>(y1 - y0);
    const double dz = grid.cellSize.z * static_cast<double>(z1 - z0);

    const double gx = dx > 0.0
        ? (grid.At(x1, y, z) - grid.At(x0, y, z)) / dx : 0.0;
    const double gy = dy > 0.0
        ? (grid.At(x, y1, z) - grid.At(x, y0, z)) / dy : 0.0;
    const double gz = dz > 0.0
        ? (grid.At(x, y, z1) - grid.At(x, y, z0)) / dz : 0.0;
    return Normalize({gx, gy, gz});
}

void FillGradientSlice(const ScalarGrid3D& grid, int z, std::vector<Vec3>& out)
{
    out.resize(static_cast<std::size_t>(grid.sizeX) * static_cast<std::size_t>(grid.sizeY));
    for (int y = 0; y < grid.sizeY; ++y)
    {
        for (int x = 0; x < grid.sizeX; ++x)
        {
            out[static_cast<std::size_t>(x)
                + static_cast<std::size_t>(grid.sizeX) * static_cast<std::size_t>(y)]
                = GradientAt(grid, x, y, z);
        }
    }
}

const Vec3& GradientFromSlice(const std::vector<Vec3>& slice, int sizeX, int x, int y)
{
    return slice[static_cast<std::size_t>(x)
        + static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(y)];
}

std::uint32_t AddInterpolatedVertex(const ScalarGrid3D& grid,
                                    int cubeX,
                                    int cubeY,
                                    int cubeZ,
                                    int edge,
                                    double isoValue,
                                    const std::array<double, 8>& values,
                                    const std::vector<Vec3>& lowerGradients,
                                    const std::vector<Vec3>& upperGradients,
                                    double weldTolerance,
                                    WeldMap& weldMap,
                                    IsoSurfaceMesh& mesh)
{
    const int cornerA = EdgeCorners[static_cast<std::size_t>(edge)][0];
    const int cornerB = EdgeCorners[static_cast<std::size_t>(edge)][1];
    const auto& offsetA = CornerOffset[static_cast<std::size_t>(cornerA)];
    const auto& offsetB = CornerOffset[static_cast<std::size_t>(cornerB)];
    const int ax = cubeX + offsetA[0];
    const int ay = cubeY + offsetA[1];
    const int az = cubeZ + offsetA[2];
    const int bx = cubeX + offsetB[0];
    const int by = cubeY + offsetB[1];
    const int bz = cubeZ + offsetB[2];

    const double valueA = values[static_cast<std::size_t>(cornerA)];
    const double valueB = values[static_cast<std::size_t>(cornerB)];
    const double denominator = valueB - valueA;
    double t = 0.5;
    if (std::abs(denominator) > std::numeric_limits<double>::epsilon())
    {
        t = (isoValue - valueA) / denominator;
    }
    t = std::clamp(t, 0.0, 1.0);

    const Vec3 positionA = PositionAt(grid, ax, ay, az);
    const Vec3 positionB = PositionAt(grid, bx, by, bz);
    const Vec3& gradientA = offsetA[2] == 0
        ? GradientFromSlice(lowerGradients, grid.sizeX, ax, ay)
        : GradientFromSlice(upperGradients, grid.sizeX, ax, ay);
    const Vec3& gradientB = offsetB[2] == 0
        ? GradientFromSlice(lowerGradients, grid.sizeX, bx, by)
        : GradientFromSlice(upperGradients, grid.sizeX, bx, by);

    const Vec3 position = Add(positionA, Multiply(Subtract(positionB, positionA), t));
    const Vec3 normal = Normalize(Add(Multiply(gradientA, 1.0 - t), Multiply(gradientB, t)));
    const WeldKey key{std::llround((position.x - grid.origin.x) / weldTolerance),
                      std::llround((position.y - grid.origin.y) / weldTolerance),
                      std::llround((position.z - grid.origin.z) / weldTolerance)};
    const auto existing = weldMap.find(key);
    if (existing != weldMap.end())
    {
        const std::uint32_t index = existing->second;
        mesh.normals[index] = Normalize(Add(mesh.normals[index], normal));
        return index;
    }
    mesh.vertices.push_back(position);
    mesh.normals.push_back(normal);
    const std::uint32_t index = static_cast<std::uint32_t>(mesh.vertices.size() - 1U);
    weldMap.emplace(key, index);
    return index;
}
}

std::int64_t ScalarGrid3D::Count() const
{
    return static_cast<std::int64_t>(sizeX)
        * static_cast<std::int64_t>(sizeY)
        * static_cast<std::int64_t>(sizeZ);
}

bool ScalarGrid3D::IsValid(std::string* error) const
{
    auto fail = [&](const char* message)
    {
        if (error) *error = message;
        return false;
    };
    if (sizeX < 2 || sizeY < 2 || sizeZ < 2)
    {
        return fail("A marching-cubes grid requires at least two lattice points per axis.");
    }
    if (!(cellSize.x > 0.0 && cellSize.y > 0.0 && cellSize.z > 0.0))
    {
        return fail("Grid cell sizes must be positive.");
    }
    if (Count() <= 0 || static_cast<std::uint64_t>(Count())
        > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return fail("Grid dimensions overflow addressable memory.");
    }
    if (values.size() != static_cast<std::size_t>(Count()))
    {
        return fail("Scalar array length does not match grid dimensions.");
    }
    for (double value : values)
    {
        if (!std::isfinite(value))
        {
            return fail("Scalar field contains a non-finite value.");
        }
    }
    return true;
}

double ScalarGrid3D::At(int x, int y, int z) const
{
    return values[LatticeIndex(*this, x, y, z)];
}

void IsoSurfaceMesh::Clear()
{
    vertices.clear();
    normals.clear();
    indices.clear();
}

bool IsoSurfaceMesh::IsValid(std::string* error) const
{
    auto fail = [&](const char* message)
    {
        if (error) *error = message;
        return false;
    };
    if (vertices.size() != normals.size())
    {
        return fail("Vertex and normal counts differ.");
    }
    if (indices.size() % 3U != 0U)
    {
        return fail("Index count is not a multiple of three.");
    }
    for (std::uint32_t index : indices)
    {
        if (index >= vertices.size())
        {
            return fail("An index lies outside the vertex array.");
        }
    }
    // Post-condition: a well-formed triangle mesh never repeats a vertex
    // index within one triangle. Vertex welding (see AddInterpolatedVertex)
    // can collapse two corners of a triangle onto the same lattice-adjacent
    // point when the isosurface passes exactly through a grid vertex; the
    // emission loop in ExtractMarchingCubes already skips such triangles, so
    // this check exists as a defense-in-depth assertion that no degenerate
    // triangle ever reaches the output.
    for (std::size_t i = 0; i + 2U < indices.size(); i += 3U)
    {
        if (indices[i] == indices[i + 1U]
            || indices[i + 1U] == indices[i + 2U]
            || indices[i] == indices[i + 2U])
        {
            return fail("A triangle repeats a vertex index (degenerate triangle).");
        }
    }
    return true;
}

bool ExtractMarchingCubes(const ScalarGrid3D& grid,
                          double isoValue,
                          IsoSurfaceMesh& outMesh,
                          std::string* error)
{
    outMesh.Clear();
    if (!std::isfinite(isoValue))
    {
        if (error) *error = "The isovalue must be finite.";
        return false;
    }
    if (!grid.IsValid(error))
    {
        return false;
    }

    const std::size_t xEdgeCount = static_cast<std::size_t>(grid.sizeX - 1)
        * static_cast<std::size_t>(grid.sizeY);
    const std::size_t yEdgeCount = static_cast<std::size_t>(grid.sizeX)
        * static_cast<std::size_t>(grid.sizeY - 1);
    const std::size_t zEdgeCount = static_cast<std::size_t>(grid.sizeX)
        * static_cast<std::size_t>(grid.sizeY);
    constexpr std::uint32_t Invalid = std::numeric_limits<std::uint32_t>::max();

    std::vector<std::uint32_t> xLower(xEdgeCount, Invalid);
    std::vector<std::uint32_t> xUpper(xEdgeCount, Invalid);
    std::vector<std::uint32_t> yLower(yEdgeCount, Invalid);
    std::vector<std::uint32_t> yUpper(yEdgeCount, Invalid);
    std::vector<std::uint32_t> zEdges(zEdgeCount, Invalid);
    std::vector<Vec3> lowerGradients;
    std::vector<Vec3> upperGradients;
    const double weldTolerance = 1.0e-4
        * std::min({grid.cellSize.x, grid.cellSize.y, grid.cellSize.z});
    WeldMap weldMap;
    weldMap.reserve(static_cast<std::size_t>(grid.sizeX - 1)
        * static_cast<std::size_t>(grid.sizeY - 1));
    FillGradientSlice(grid, 0, lowerGradients);
    FillGradientSlice(grid, 1, upperGradients);

    auto edgeSlot = [&](int edge, int x, int y) -> std::uint32_t&
    {
        switch (edge)
        {
        case 0: return xLower[XEdgeIndex(grid.sizeX, x, y)];
        case 1: return yLower[YEdgeIndex(grid.sizeX, x + 1, y)];
        case 2: return xLower[XEdgeIndex(grid.sizeX, x, y + 1)];
        case 3: return yLower[YEdgeIndex(grid.sizeX, x, y)];
        case 4: return xUpper[XEdgeIndex(grid.sizeX, x, y)];
        case 5: return yUpper[YEdgeIndex(grid.sizeX, x + 1, y)];
        case 6: return xUpper[XEdgeIndex(grid.sizeX, x, y + 1)];
        case 7: return yUpper[YEdgeIndex(grid.sizeX, x, y)];
        case 8: return zEdges[ZEdgeIndex(grid.sizeX, x, y)];
        case 9: return zEdges[ZEdgeIndex(grid.sizeX, x + 1, y)];
        case 10: return zEdges[ZEdgeIndex(grid.sizeX, x + 1, y + 1)];
        default: return zEdges[ZEdgeIndex(grid.sizeX, x, y + 1)];
        }
    };

    std::array<double, 8> values{};
    std::array<std::uint32_t, 12> edgeVertices{};
    for (int z = 0; z < grid.sizeZ - 1; ++z)
    {
        if (z > 0)
        {
            xLower.swap(xUpper);
            yLower.swap(yUpper);
            std::fill(xUpper.begin(), xUpper.end(), Invalid);
            std::fill(yUpper.begin(), yUpper.end(), Invalid);
            std::fill(zEdges.begin(), zEdges.end(), Invalid);
            lowerGradients.swap(upperGradients);
            FillGradientSlice(grid, z + 1, upperGradients);
        }

        for (int y = 0; y < grid.sizeY - 1; ++y)
        {
            for (int x = 0; x < grid.sizeX - 1; ++x)
            {
                int cubeIndex = 0;
                for (int corner = 0; corner < 8; ++corner)
                {
                    const auto& offset = CornerOffset[static_cast<std::size_t>(corner)];
                    const double value = grid.At(x + offset[0], y + offset[1], z + offset[2]);
                    values[static_cast<std::size_t>(corner)] = value;
                    if (value < isoValue)
                    {
                        cubeIndex |= 1 << corner;
                    }
                }
                const int edgeMask = GKrigeEdgeTable[cubeIndex];
                if (edgeMask == 0)
                {
                    continue;
                }
                for (int edge = 0; edge < 12; ++edge)
                {
                    if ((edgeMask & (1 << edge)) == 0)
                    {
                        continue;
                    }
                    std::uint32_t& slot = edgeSlot(edge, x, y);
                    if (slot == Invalid)
                    {
                        slot = AddInterpolatedVertex(grid, x, y, z, edge, isoValue,
                            values, lowerGradients, upperGradients,
                            weldTolerance, weldMap, outMesh);
                    }
                    edgeVertices[static_cast<std::size_t>(edge)] = slot;
                }
                const int* triangle = GKrigeTriangleTable[cubeIndex];
                for (int index = 0; index < 16 && triangle[index] >= 0; index += 3)
                {
                    std::uint32_t a = edgeVertices[static_cast<std::size_t>(triangle[index])];
                    std::uint32_t b = edgeVertices[static_cast<std::size_t>(triangle[index + 1])];
                    std::uint32_t c = edgeVertices[static_cast<std::size_t>(triangle[index + 2])];
                    if (a == b || b == c || a == c)
                    {
                        // The isosurface passes exactly through a lattice
                        // point (t == 0 or t == 1 on an edge), so welding
                        // collapsed two corners of this triangle onto the
                        // same output vertex. Skip it: emitting a
                        // zero-area triangle would corrupt the mesh's
                        // topology (Euler characteristic, edge-manifold
                        // checks) without contributing any surface area.
                        continue;
                    }
                    const Vec3 face = Cross(Subtract(outMesh.vertices[b], outMesh.vertices[a]),
                                            Subtract(outMesh.vertices[c], outMesh.vertices[a]));
                    const Vec3 averageNormal = Add(Add(outMesh.normals[a], outMesh.normals[b]),
                                                   outMesh.normals[c]);
                    if (Dot(face, averageNormal) < 0.0)
                    {
                        std::swap(b, c);
                    }
                    outMesh.indices.push_back(a);
                    outMesh.indices.push_back(b);
                    outMesh.indices.push_back(c);
                }
            }
        }
    }

    if (!outMesh.IsValid(error))
    {
        outMesh.Clear();
        return false;
    }
    if (outMesh.indices.empty())
    {
        if (error) *error = "The isovalue does not intersect the scalar field.";
        return false;
    }
    return true;
}
}
