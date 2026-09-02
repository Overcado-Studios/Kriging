# Scope and specification deviations

This package is not represented as the complete implementation specification. It is a repaired prerequisite gate.

## Intentional contractions

- Only `KrigingCore` is active.
- The public C++ core uses standard-library containers and a unit-agnostic `Vec3` so the production source can be built outside Unreal.
- No UObjects, Blueprint nodes, assets, editor actors, importers, widgets, outputs, PCG nodes, Landscape APIs, renderer code, or shaders are active.
- Cesium is absent because geospatial integration was not in the stated scope.
- Grid tiling and cross-fade are absent. Per-query local solving is retained and tested.
- Tiled variance is absent rather than mathematically mislabelled.
- GPU variance and GPU values are absent rather than silently divergent.
- CPU structure evaluation is analytic rather than table-based. This trades peak bake throughput for measurable correctness in the gate.
- Eigen is not used until the engine-version dependency gate can be run. The internal LU is the only active solver and future alternatives must cross-check it.
- `ForceGlobal` is capped at 512 effective samples so fixed stack query workspaces remain bounded. Exceeding the cap fails the build loudly rather than silently truncating or degrading: `Model::Build` returns `false` and `BuildReport::message` is set to "ForceGlobal is limited to 512 effective samples by the fixed-workspace query contract. Use Automatic or ForceLocal." (`KrigePortableCore.cpp:1844`). The documented workaround is to switch `SolveMode` to `Automatic` (which falls back to local solving once the sample count exceeds `GlobalSolveThreshold`) or to `ForceLocal` directly; neither is subject to the 512-sample ceiling.
- Brute-force leave-one-out is capped at 60 samples.

## Query allocation claim

The non-degraded global kriging value and variance paths are verified allocation-free after warm-up. Local-neighborhood selection, uncached local factorization, and IDW use temporary standard containers and are not claimed allocation-free.

## Transform variance

For normal score, supplied measurement variance is treated as already being in score space. Returned variance for any transformed model remains transformed-space variance. Cross-validation labels standardized statistics accordingly.

## Engine compatibility

The source is structured as one plausible UE runtime module, but UE 5.4–5.8 compilation and `BuildPlugin` are not claimed. The engine-side M0 gate must run before adding interfaces or restoring removed subsystems.
