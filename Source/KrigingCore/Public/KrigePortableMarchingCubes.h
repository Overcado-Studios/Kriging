#pragma once

// Reconstructed header for KrigePortableMarchingCubes.cpp. Declares the
// regular-lattice scalar grid, the output triangle mesh, and the extraction
// entry point that the translation unit defines. Style follows
// KrigePortableCore.h: kriging::portable namespace, C++17, no Unreal
// dependencies. Vec3 is shared with KrigePortableCore.h.

#include "KrigePortableCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kriging::portable
{

// A regular lattice of scalar samples over an axis-aligned box, addressed as
// value = f(origin + (x, y, z) * cellSize) for 0 <= x < sizeX, etc.
struct KRIGINGCORE_API ScalarGrid3D
{
    int sizeX = 0;
    int sizeY = 0;
    int sizeZ = 0;
    Vec3 origin;
    Vec3 cellSize;
    std::vector<double> values; // length sizeX * sizeY * sizeZ, x-fastest.

    // Total lattice point count as sizeX * sizeY * sizeZ (as a 64-bit value
    // so overflow of the addressable std::size_t range can be detected).
    std::int64_t Count() const;

    // Validates dimensions (>= 2 per axis), positive cell size, that
    // values.size() matches Count(), that Count() fits in std::size_t, and
    // that every scalar value is finite. On failure, if error is non-null,
    // it is set to a human-readable diagnostic.
    bool IsValid(std::string* error = nullptr) const;

    // Reads the scalar value at lattice coordinate (x, y, z). No bounds
    // checking is performed; callers must stay within [0, sizeX) etc.
    double At(int x, int y, int z) const;
};

// A welded, indexed triangle mesh with per-vertex normals.
struct KRIGINGCORE_API IsoSurfaceMesh
{
    std::vector<Vec3> vertices;
    std::vector<Vec3> normals; // parallel to vertices.
    std::vector<std::uint32_t> indices; // triangle list, length % 3 == 0.

    void Clear();

    // Validates that vertices and normals are the same length, that the
    // index count is a multiple of three, and that every index references a
    // valid vertex. On failure, if error is non-null, it is set to a
    // human-readable diagnostic.
    bool IsValid(std::string* error = nullptr) const;
};

// Extracts an isosurface at isoValue from grid using the standard
// Lorensen-Cline marching-cubes algorithm, welding vertices shared by
// adjacent cubes (within a tolerance derived from the grid's cell size),
// computing gradient-based per-vertex normals, and orienting triangles to
// face the direction of increasing scalar value. Returns false (clearing
// outMesh and, if error is non-null, setting a diagnostic) when isoValue is
// non-finite, the grid fails validation, the resulting mesh fails
// validation, or the isovalue does not intersect the scalar field.
KRIGINGCORE_API bool ExtractMarchingCubes(const ScalarGrid3D& grid,
                                          double isoValue,
                                          IsoSurfaceMesh& outMesh,
                                          std::string* error = nullptr);

} // namespace kriging::portable
