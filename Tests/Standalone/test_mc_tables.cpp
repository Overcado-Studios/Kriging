// Internal-invariant check for the reconstructed marching-cubes tables.
// Does not depend on any external reference table: for every one of the 256
// cube configurations it checks that
//   1) every edge listed in a triangle row has its two endpoint corners on
//      opposite sides of the isosurface for that configuration;
//   2) every edge whose endpoints differ (one inside, one outside) appears at
//      least once in the row;
//   3) GKrigeEdgeTable[i] is exactly the OR of (1 << edge) over the row;
//   4) the entry count before the first -1 is a multiple of three;
//   5) configurations 0 and 255 (fully outside / fully inside) are empty.
#include <cstdlib>
#include <iostream>
#include <string>

namespace kriging::portable
{
#include "KrigeMarchingCubesTables.inl"
}

namespace
{
// Duplicated from KrigePortableMarchingCubes.cpp's private EdgeCorners
// table. This is a known limitation: the production EdgeCorners array is
// not exposed by KrigePortableMarchingCubes.h (and this test may not add a
// header accessor), so this copy cannot be cross-checked against the
// production definition for a transposition that both copies happen to
// share -- if someone "fixes" a typo here and there in lockstep, this file
// alone would not catch the drift. What this table invariant check CAN
// still catch (independent of that risk) is any inconsistency between
// GKrigeEdgeTable/GKrigeTriangleTable and *whichever* corner-pair mapping
// is asserted here, for all 256 configurations.
//
// The real defense against an EdgeCorners transposition in production is
// therefore not here: it is the geometric, single-cube exhaustive test in
// test_marching_cubes.cpp (RunSingleCubeAllEdgesTest), which exercises the
// production EdgeCorners table through the public ExtractMarchingCubes API
// and checks the emitted vertex positions against the known geometry of a
// unit cube's edge midpoints -- catching exactly the class of defect this
// file's private copy cannot (see also RunDegenerateTriangleGuardTest and
// the sphere/tilted-field topology checks there for the corner-offset and
// welding classes of defect).
constexpr int EdgeCorners[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7}
};

int failures = 0;
void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
}

int main()
{
    using kriging::portable::GKrigeEdgeTable;
    using kriging::portable::GKrigeTriangleTable;

    for (int cubeIndex = 0; cubeIndex < 256; ++cubeIndex)
    {
        const int* row = GKrigeTriangleTable[cubeIndex];

        // Count entries before the first -1.
        int count = 0;
        while (count < 16 && row[count] >= 0) ++count;
        Check(count % 3 == 0, "row " + std::to_string(cubeIndex) + " length is a multiple of three");
        for (int i = count; i < 16; ++i)
        {
            Check(row[i] == -1, "row " + std::to_string(cubeIndex) + " terminated by -1 after first -1");
        }

        if (cubeIndex == 0 || cubeIndex == 255)
        {
            Check(count == 0, "config " + std::to_string(cubeIndex) + " (empty/full) has no triangles");
        }

        // Every edge in the row must cross the surface (endpoints differ).
        int usedMask = 0;
        for (int i = 0; i < count; ++i)
        {
            const int edge = row[i];
            Check(edge >= 0 && edge < 12, "row " + std::to_string(cubeIndex) + " edge index in range");
            const int cornerA = EdgeCorners[edge][0];
            const int cornerB = EdgeCorners[edge][1];
            const bool insideA = (cubeIndex & (1 << cornerA)) != 0;
            const bool insideB = (cubeIndex & (1 << cornerB)) != 0;
            Check(insideA != insideB,
                  "config " + std::to_string(cubeIndex) + " edge " + std::to_string(edge)
                  + " endpoints on opposite sides");
            usedMask |= (1 << edge);
        }

        // Every crossing edge (endpoints differ) must appear in the row.
        int expectedMask = 0;
        for (int edge = 0; edge < 12; ++edge)
        {
            const int cornerA = EdgeCorners[edge][0];
            const int cornerB = EdgeCorners[edge][1];
            const bool insideA = (cubeIndex & (1 << cornerA)) != 0;
            const bool insideB = (cubeIndex & (1 << cornerB)) != 0;
            if (insideA != insideB) expectedMask |= (1 << edge);
        }
        Check(usedMask == expectedMask,
              "config " + std::to_string(cubeIndex) + " triangle row uses exactly the crossing edges");
        Check(GKrigeEdgeTable[cubeIndex] == expectedMask,
              "config " + std::to_string(cubeIndex) + " edge table matches crossing-edge mask");
    }

    if (failures == 0)
    {
        std::cout << "Marching-cubes table invariants passed for all 256 configurations.\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
