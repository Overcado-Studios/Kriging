# Kriging Core — correctness gate

This archive is a deliberately contracted Unreal Engine plugin package. It replaces the non-compiling, over-claimed remediation snapshot with one active runtime module and one numerical implementation that can be compiled and exercised without Unreal Engine.

It is **not** the complete Fab plugin described by the original implementation specification. It is the prerequisite core gate that should have existed before editor, renderer, PCG, Landscape, asset, Cesium, or packaging work began.

## Active package surface

```text
Kriging/
├── Kriging.uplugin                     # declares KrigingCore and KrigingBlueprint
├── Source/KrigingCore/
│   ├── KrigingCore.Build.cs
│   ├── Public/                         # portable API and compatibility aliases
│   └── Private/
│       ├── KrigingCoreModule.cpp
│       ├── Portable/                   # production numerical path: model, analysis, marching cubes
│       └── Tests/                      # UE automation smoke test
├── Source/KrigingBlueprint/            # Blueprint-facing library/actor wrapper module
│   ├── KrigingBlueprint.Build.cs
│   ├── Public/
│   └── Private/
├── Tests/Standalone/                   # engine-free property and allocation gates
├── Tests/Golden/                       # PyKrige golden-value generation/comparison gate
├── Tests/Review/                       # targeted source, SciPy, and PyKrige reference checks
├── Tests/Results/                      # generated provenance and results
└── Docs/                               # authoritative status, conventions, decisions
```

The manifest does not declare modules that are missing beyond `KrigingCore` and `KrigingBlueprint`. There are no active editor, renderer, PCG, shader, content, Landscape, CSV, Cesium, or legacy source trees.

## Implemented numerical core

The active core provides:

- Simple, ordinary, linear-universal, quadratic-universal, external-drift, and inverse-distance methods.
- Spherical, exponential, Gaussian, Matérn, and power variogram structures, including up to three nested structures.
- Two-dimensional and three-dimensional geometric anisotropy.
- Exact and filtered nugget semantics plus per-sample measurement variance.
- Deterministic sorting and bounded-diameter complete-link duplicate merging.
- Centered and scaled polynomial drift bases.
- A partial-pivot LU solver for the augmented indefinite kriging system.
- Zero-ridge-first factorization with bounded escalation and reported IDW degradation.
- Dual-form value evaluation and full-system kriging variance.
- Deterministic balanced k-d tree search, sector-balanced local neighborhoods, and collision-safe factorization caching.
- None, logarithmic, and normal-score transforms with one consistent back-transform policy across query entry points.
- Bounded brute-force leave-one-out and an inverse-diagonal fast path that is checked against brute force at runtime for models of 60 samples or fewer.
- Thread-safe counters for negative-variance clamps and per-query local IDW fallback.
- Analysis subsystem: empirical variogram computation and weighted-least-squares structure fitting (`KrigePortableAnalysis.cpp`), gated by the standalone property suite; test-discrimination remediation for this subsystem is complete (mutation-verified; see `Docs/IMPLEMENTATION_STATUS.md`).
- Marching cubes iso-surface extraction over a scalar grid (`KrigePortableMarchingCubes.cpp`), gated by the standalone property suite; test-discrimination remediation for this subsystem is complete (mutation-verified; see `Docs/IMPLEMENTATION_STATUS.md`).
- A `KrigingBlueprint` module exposing a Blueprint-facing library/actor wrapper around the `KrigingCore` API (build, auto-fit, iso-surface extraction to `UProceduralMeshComponent`).

The core does **not** return stored sample values from an exact-location shortcut. Exactness tests exercise the solved system.

## Verification included in this archive

Run every engine-independent gate:

```bash
python -m pip install -r Tests/Review/requirements.txt
python Tests/Review/run_verification.py
```

The runner records UTC timestamps, platform and tool versions, exact commands, stdout/stderr, durations, the NumPy/SciPy/PyKrige versions pinned in `Tests/Review/requirements.txt` alongside whichever versions were actually installed when the gate ran (mismatches are recorded as warnings, not silently absorbed), and SHA-256 hashes of the source files it tested. Results are written to `Tests/Results/VERIFICATION.json` and `Tests/Results/VERIFICATION.txt`.

The gate performs all of the following:

1. Targeted topology and isolated-function source guards. These are explicitly labelled as static guards, not compilation evidence.
2. Compilation of the shipped `KrigePortableCore.cpp` and direct numerical comparison with SciPy for the Bessel kernel, all exported structure functions, and AS241.
3. Independent NumPy/SciPy assembly and solution of nine fixed augmented systems, compared against the shipped C++ `Model::Build` and `EvaluateWithVariance` path across all drift methods, 2D/3D anisotropy, Matérn, power exact/filtered branches, and measurement variance.
4. GCC release build and CTest execution with warnings as errors.
5. Clang release build and CTest execution with warnings as errors, when `clang++` is available on the host; recorded as SKIP ("clang++ is unavailable") otherwise. The shipped `Tests/Results/VERIFICATION.json` on this environment records SKIP for this step.
6. GCC AddressSanitizer and UndefinedBehaviorSanitizer build and CTest execution.
7. Syntax compilation of the actual Unreal module and automation-test translation units against narrow test stubs. This catches ordinary C++ errors without being represented as Unreal compatibility evidence.
8. A process-wide allocation probe confirming zero heap allocations in non-degraded global `Evaluate` and `EvaluateWithVariance` calls after warm-up.
9. PyKrige golden-value comparison (`Tests/Golden/`): shipped C++ output is compared against recorded PyKrige reference values (`goldens.json`, generated with PyKrige 1.7.3).

The standalone property suite covers solved exactness, partition of unity, constant fields, linear and quadratic drift reproduction at large coordinate offsets, translation and rotation invariance, bit-identical anisotropy identity, local-versus-global parity at full neighborhood count, fast-versus-brute-force LOO at `n = 20` and `n = 60`, transformed cross-validation semantics, k-d tree parity against brute force, deterministic dual weights, nested additivity, variance non-negativity, duplicate-chain resistance, build guards, and bounded brute-force CV.

A checked-in green result is not accepted on trust: rerun the command above after any source change.

## Standalone build only

```bash
cmake -S Tests/Standalone -B build/kriging -DCMAKE_BUILD_TYPE=Release
cmake --build build/kriging --parallel
ctest --test-dir build/kriging --output-on-failure
```

The CMake target compiles the same `KrigePortableCore.cpp` that UnrealBuildTool sees. There is no Python restatement of a production lookup table; the CPU path evaluates structure functions analytically.

## Unreal Engine use

Copy the `Kriging` directory into a project's `Plugins` directory. The active plugin exposes a standard C++17 API in `KrigePortableCore.h`; `KrigeModel.h`, `KrigeTypes.h`, `KrigeLinearSolve.h`, `KrigeKdTree.h`, `KrigeTransform.h`, and `KrigeVariogram.h` provide provisional compatibility aliases.

A minimal C++ use looks like this:

```cpp
#include "KrigeModel.h"
#include "KrigeTypes.h"

Kriging::FKrigeModel Model;
Kriging::FKrigeVariogram Variogram;
Kriging::FKrigeSolveSettings Settings;
std::vector<Kriging::FKrigeSample> Samples;
Kriging::FKrigeBuildReport Report;

const bool bBuilt = Model.Build(Samples, Variogram, Settings, Report);
```

The package includes `Kriging.Core.PortableSmoke` and `Kriging.Blueprint.*` Unreal automation tests. `RunUAT BuildPlugin` and NullRHI automation (5/5 tests) have since run and PASSED against real UE 5.5 and UE 5.4 installations; see `Tests/Results/ENGINE_GATES.md`. UE 5.6, 5.7, and 5.8 compatibility remain **NOT RUN** gates. See `Docs/DECISIONS.md` and `Docs/IMPLEMENTATION_STATUS.md` for the full gate table.

## Deliberately absent

The following are not advertised as implemented because they are not present in the active tree:

- CSV and Data Table import.
- Editor actor, variogram widget, diagnostics, undo, live preview, or export UI.
- GPU evaluation, shaders, readback, or parity self-test.
- Tiled grid blending or tiled variance.
- Texture, volume, Landscape, and heightmap outputs. (Iso-surface extraction to a `UProceduralMeshComponent` is now present via `KrigingBlueprint`; material outputs remain absent.)
- PCG integration.
- Cesium integration or geospatial container handling.
- Serialized Unreal assets and example maps.
- Fab packaging or multi-engine build verification.

Those features may be restored only after the engine-side M0 gate passes and only with tests landing in the same change.

## User documentation

New users start in `Docs/QUICKSTART.md`, then `Docs/TUTORIAL.md`. Reference material:

- `Docs/QUICKSTART.md` — install and first build/query.
- `Docs/TUTORIAL.md` — worked walkthrough using the sample CSVs in `Samples/`.
- `Docs/GEOSTATS_PRIMER.md` — variogram and kriging theory, minimal math.
- `Docs/PROFESSIONAL_USE.md` — verified limits, gates, and troubleshooting for production use.
- `Docs/FAQ.md` — short answers to recurring questions.

## Distribution status

This is a source-level numerical-core gate under MIT, not a Fab release candidate. The complete status vocabulary and feature matrix are in `Docs/IMPLEMENTATION_STATUS.md`.
