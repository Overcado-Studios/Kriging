# Attribution

## Boost.Math Bessel method

`Source/KrigingCore/Private/Portable/KrigeBessel.h` adapts the reduction, Temme-series, continued-fraction, and forward-recurrence approach used by Boost.Math's `special_functions/detail/bessel_ik.hpp`. The adaptation uses independently arranged code and long-double intermediates but retains the algorithmic lineage.

Boost.Math is distributed under the Boost Software License 1.0. The license text is included at `ThirdPartyNotices/BOOST_LICENSE_1_0.txt`.

## Algorithm AS241

`InverseStandardNormalCdf` is an independent Horner-form implementation of the rational approximations published as:

Michael J. Wichura, “Algorithm AS 241: The Percentage Points of the Normal Distribution,” *Applied Statistics*, 37(3), 1988, pp. 477–484.

No external AS241 source file is redistributed.

## Marching-cubes edge/triangle tables

`Source/KrigingCore/Private/Portable/KrigeMarchingCubesTables.inl` (the
256-entry edge table and 256x16 triangle table consumed by
`KrigePortableMarchingCubes.cpp`) transcribes the standard Lorensen-Cline
marching-cubes tables (W. E. Lorensen and H. E. Cline, "Marching Cubes: A
High Resolution 3D Surface Construction Algorithm," *SIGGRAPH 1987*), in
the form widely republished as a public-domain algorithm description and
popularized by Paul Bourke
(http://paulbourke.net/geometry/polygonise/). Table data is public domain;
no external source file is redistributed and no license header is
required.

## Reference tooling

The numerical review gate records the installed NumPy, SciPy, and PyKrige versions as test-time references and compares them against the pins in `Tests/Review/requirements.txt`, warning (not silently passing) on any mismatch rather than guaranteeing the pinned versions were actually installed for a given run. None of the three are linked into or redistributed with the Unreal plugin module.
