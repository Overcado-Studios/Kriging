# Implementation status

This is the single authoritative status document for the package.

## Status vocabulary

| Status | Meaning |
|---|---|
| **Verified — portable gate** | The active production C++ was compiled and exercised by the shipped engine-free test runner (`Tests/Review/run_verification.py`). |
| **Verified — engine gate** | Compiled and/or run against a real Unreal Engine installation (UnrealBuildTool `BuildPlugin` and/or NullRHI automation), recorded in `Tests/Results/ENGINE_GATES.md`. |
| **Present — Unreal unverified** | Source is present but has not been exercised by an engine gate on this row's specific claim. |
| **Disabled/absent** | The subsystem is not in the active plugin tree and is not advertised. |
| **Not run — release gate** | A required engine or packaging gate has not executed. |
| **In progress** | Present and gated, but the evidence backing it is undergoing active remediation; do not read as final/hardened. |

## Active core

| Area | Status | Evidence or limitation |
|---|---|---|
| Plugin manifest topology | Verified — portable gate | Static guard requires exactly two declared modules (`KrigingCore`, `KrigingBlueprint`), each with a matching `Build.cs` and module entry point. |
| Portable production compilation | Verified — portable gate | GCC release build compiles `KrigePortableCore.cpp` with warnings as errors. A Clang release build also runs and must pass whenever `clang++` is available on the host; it is recorded as SKIP ("clang++ is unavailable") otherwise — the shipped `Tests/Results/VERIFICATION.json` on this environment records SKIP for the Clang step. |
| Unreal-facing source syntax | Verified — portable gate, not UE compatibility | `KrigingCore`'s module and automation-test translation units compile against narrow stubs, catching ordinary C++ field/arity errors. `KrigingBlueprint`'s UHT-generated includes are out of scope for this stub-syntax gate; it is instead covered by the real UBT/automation engine gates below. |
| Independent augmented-system reference | Verified — portable gate | NumPy independently assembles and solves nine scenarios; C++ values and variances agree within the recorded tolerance. |
| PyKrige golden-value gate | Verified — portable gate | `Tests/Golden/generate_goldens.py` (PyKrige 1.7.3) produced `Tests/Golden/goldens.json`; `Tests/Review/validate_goldens.py` compiles the shipped C++ core and compares its output against those recorded golden values as part of `run_verification.py` (`pykrige_golden_validation` step). PyKrige is a test-time reference only, pinned in `Tests/Review/requirements.txt`; it is not linked into or redistributed with the plugin. |
| Sanitizer execution | Verified — portable gate | GCC ASan+UBSan CTest run is recorded in `Tests/Results`. |
| Dense partial-pivot LU | Verified — portable gate | Property suite and fast/brute LOO exercise the augmented saddle-point systems. |
| Simple/ordinary/universal/external-drift kriging | Verified — portable gate | Solved exactness, partition, drift reproduction, and CV parity tests. |
| Power semivariogram branch | Verified — portable gate | Exactness, finite local evaluation, and build-combination guards. |
| Matérn kernel | Verified — portable gate | Actual shipped Bessel and exported structure evaluator are compared directly with SciPy. |
| Anisotropy | Verified — portable gate | 2D/3D identity and isotropic rotation invariance tests. |
| Duplicate merging | Verified — portable gate | Complete-link chain test and maximum cluster-diameter reporting. |
| Centered/scaled drift | Verified — portable gate | Linear and quadratic reproduction around large coordinate offsets. |
| Global dual evaluation | Verified — portable gate | Exactness, deterministic dual bytes, and allocation probe. |
| Global variance | Verified — portable gate | Non-negativity over 10,000 random queries; no per-call heap allocation after warm-up. |
| Static balanced k-d tree | Verified — portable gate | Nearest and radius results are compared against brute force in 2D and 3D. |
| Local neighborhood solver | Verified — portable gate | Full-neighborhood local/global value and variance parity. Per-query selection only. |
| Local factorization cache | Verified — portable gate | Full index-list equality after hash match; bounded admission; no whole-cache reset. |
| IDW degradation | Verified — portable gate | Candidate count is bounded by `MaxNeighbours`; local fallbacks are atomically counted and reported. |
| Log and normal-score transforms | Verified — portable gate | Exact sample round trips and transformed-space CV labelling. |
| Lognormal bias correction | Verified — portable gate | `Evaluate` and `EvaluateWithVariance` value outputs are compared directly. |
| Fast LOO | Verified — portable gate | Runtime parity with brute force at `n=20` and `n=60`, across supported drift methods and both nugget modes. |
| Large-model fast LOO | Verified — portable gate, caveated | Result explicitly leaves `verifiedAgainstBruteForce=false`; synchronous brute reference is capped at 60. |
| Analysis subsystem (empirical variograms + WLS fitting) | Verified — standalone property gate | `KrigePortableAnalysis.cpp` is compiled and exercised by the standalone property suite (engine-free). Test-discrimination remediation on this subsystem is complete: tests are brute-force-verified per bin, and declustering, Cressie-Hawkins, and directional filters are covered; reviewer mutants A1-A7 are all caught (see git log). |
| Marching cubes iso-surface extraction | Verified — standalone property gate | `KrigePortableMarchingCubes.cpp` is compiled and exercised by the standalone property suite (engine-free), with tight tolerances, topology assertions, a tilted asymmetric field, and a single-cube all-edges test; reviewer table mutants M1-M3 are all caught (see git log). Separately verified end-to-end on a real engine: `Kriging.Blueprint.IsoSurfaceExtraction` passed under NullRHI automation on UE 5.5 and UE 5.4, re-verified after the degenerate-triangle guard fix (see `Tests/Results/ENGINE_GATES.md`); `bFlipWinding` orientation is *not* covered by that NullRHI run (no rasterizer) and remains an open caveat. |
| `KrigingBlueprint` module (library/actor wrapper) | Verified — engine gate | Compiles and links against `KrigingCore` under UBT on UE 5.5 and UE 5.4 (`RunUAT BuildPlugin`); its five automation tests (`Kriging.Blueprint.AutoFitBuild`, `AutoFitHeuristicFallback`, `ExplicitBuildExactness`, `IsoSurfaceExtraction`, plus `Kriging.Core.PortableSmoke`) passed 5/5 under NullRHI automation on both engine versions. `ExternalDrift` is not exposed on this module's Blueprint surface, and `FKrigingVariogramSpec` is single-structure only (nested 1-3 structure variograms are core-only); see `Tests/Results/ENGINE_GATES.md`. |
| UE module entry point (`KrigingCore`) | Verified — engine gate | `KrigingCore.Build.cs` and `IMPLEMENT_MODULE` compile and link under UBT on UE 5.5 and UE 5.4. |
| UE automation smoke test (`Kriging.Core.PortableSmoke`) | Verified — engine gate | Passed under NullRHI automation on UE 5.5 and UE 5.4. |

## Disabled or absent subsystems

| Subsystem | Status | Reason |
|---|---|---|
| KrigingRenderer/GPU shaders | Disabled/absent | Previous path was unreachable and uncompiled; removal is more honest than a nominal self-test. |
| Tiled grid blending and tiled variance | Disabled/absent | Blending variances from different neighborhood systems is not mathematically justified. |
| KrigingEditor and CSV import | Disabled/absent | Previous caller/definition arity and coordinate-finalization defects made the subsystem known-broken. |
| PCG | Disabled/absent | No implementation was present. |
| Landscape/heightmap/texture output assets | Disabled/absent | No verified engine-side implementation in this package. (Iso-surface extraction to a `UProceduralMeshComponent`, a different output path, is present and engine-verified — see the active-core table above.) |
| Cesium | Disabled/absent | Outside the original scope and not promoted as a public API. |
| Legacy discarded implementation | Disabled/absent | Removed to keep one source of truth. |
| Content/example maps/material function | Disabled/absent | Binary assets cannot be validated without Unreal Editor. |

## Release gates

| Gate | Status |
|---|---|
| UnrealBuildTool compile, UE 5.4 | Verified — engine gate: PASS (`RunUAT BuildPlugin`; see `Tests/Results/ENGINE_GATES.md`) |
| UnrealBuildTool compile, UE 5.5 | Verified — engine gate: PASS (`RunUAT BuildPlugin`, re-verified after the final marching-cubes fix with no repo fix required; see `Tests/Results/ENGINE_GATES.md`) |
| UnrealBuildTool compile, UE 5.6 | Not run — release gate |
| UnrealBuildTool compile, UE 5.7 | Not run — release gate |
| UnrealBuildTool compile, UE 5.8 | Not run — release gate |
| `RunUAT BuildPlugin` packaged-plugin path | Verified — engine gate: PASS on UE 5.5 and UE 5.4 |
| UE command-line automation smoke test (NullRHI) | Verified — engine gate: PASS, 5/5 `Kriging.*` tests on both UE 5.5 and UE 5.4 |
| UE command-line automation, real RHI (rendering behavior, e.g. `bFlipWinding` orientation) | Not run — release gate |
| Fab submission | Not eligible |

Evidence for the engine-gate rows above lives in `Tests/Results/ENGINE_GATES.md`, produced after the engine-free `run_verification.py` evidence in `Tests/Results/VERIFICATION.json`/`VERIFICATION.txt`. That file also carries forward known caveats (installed NumPy/SciPy versions exceeding the `requirements.txt` pins in that run's environment; the un-rasterized `bFlipWinding` risk) — read it directly rather than assuming this table's one-line summaries are exhaustive.
