# Adversarial-review remediation ledger

| Review finding | Corrected disposition | Verification |
|---|---|---|
| Manifest declares missing modules and cannot load | Manifest now declares `KrigingCore` only; matching `Build.cs` and `IMPLEMENT_MODULE` exist. | Static topology guard; Unreal compile remains not run. |
| CSV caller arity mismatch | Known-broken editor/import module removed from active package instead of relabelled “present.” | Forbidden-subsystem topology guard. |
| Cesium import collapses all samples and bridge is out of scope | Cesium source, docs, dependency, coordinate mode, and public header are absent. | Active-source token and path guard. |
| HLSL `frexp` signature error | Renderer and shaders are absent; no uncompiled GPU claim remains. | Forbidden path and symbol guard. |
| Grep validator reports green on compile errors | Static checks now isolate specific function bodies and explicitly disclaim compilation. A separate GCC release build and a GCC sanitizer build compile production source; a Clang release build also runs and must pass whenever `clang++` is available, and is recorded as SKIP otherwise — the shipped `Tests/Results/VERIFICATION.json` on this environment records SKIP for that step. | `run_verification.py`. |
| Python validator restates lookup rather than calling C++ | CPU lookup removed. Numeric probe compiles `KrigePortableCore.cpp` and calls its exported evaluator directly. | Numeric report contains source hashes and compile command. |
| Result text lacks provenance | Generated result includes UTC timestamps, platform/tool versions, exact commands, stdout/stderr, durations, dependency versions, and source SHA-256. | `Tests/Results/VERIFICATION.json`. |
| Mathematical core lacks an independent matrix reference | NumPy/SciPy independently assembles and solves nine augmented systems; the C++ probe only parses input and calls shipped build/evaluation paths. | `Tests/Results/INDEPENDENT_REFERENCE.json`. |
| Exact-interpolation shortcut makes property test vacuous | Kriging `Evaluate` and `EvaluateWithVariance` have no exact lookup. Tests evaluate every sample through the solved system. | Isolated-function guard plus exactness suite. |
| Ridge conceals solved non-exactness | Factorization tries ridge zero first and reports any escalation. Exactness tests assert zero ridge in well-posed cases. | Property suite. |
| Unproven LOO is default and absolute-values inverse diagonal | Fast eligibility is narrow; diagonal must be positive; no `abs`; models up to 60 are checked against brute force at runtime. | Fast/brute tests at 20 and 60 across methods and nugget modes. |
| Standardized CV stats are misleading for transforms | Transformed models use brute force; unstandardized errors remain original-space while standard errors and standardized residuals are computed and labelled in transformed space. | Transform CV test and message check. |
| Out-of-spec subsystem promoted before M0 | Cesium, editor, renderer, PCG, output, and content surfaces are not active. | Manifest and path guards. |
| Contradictory legacy implementation ships beside active code | `Docs/LegacyDiscarded` and alternate `FImpl` source are absent. | Forbidden path guard. |
| Duplicate merging is transitive | Complete-link admission caps cluster diameter; report records maximum diameter. | 0.0/0.9/1.8 chain test. |
| Tile neighborhood selected incorrectly | Tiled grid implementation is absent until its selection rule is proved. | No tiled symbols in active source. |
| Tiled variances are averaged | No tiled variance is exposed. | No tiled symbols in active source. |
| Tiled compositing allocates huge full-grid arrays | Grid/tile subsystem absent. | Package topology. |
| Degraded global IDW scans all samples | Every IDW call selects at most `MaxNeighbours` through the k-d tree. | Isolated `IdwWorking` guard. |
| Variance path allocates per call | Global RHS and solve use fixed `std::array` workspaces. | Process-wide `operator new` allocation probe. |
| `check(!bValid)` remains in drift repointing | Repointing invalidates the model and requires rebuild; no active `check` or `assert`. | Isolated setter guard. |
| Cache hash collision can return wrong system | Hash identifies a bucket only; full sorted neighbor list must compare equal. | Isolated cache guard. |
| Cache flushes wholesale at capacity | Admission stops at 4,096; current uncached system still serves the query; no reset and no empty-bucket growth. | Isolated cache guard. |
| Brute-force LOO is unbounded | Public reference call is capped, defaults to 60, and explains refusal. | Cap test. |
| Headerless CSV ambiguities | CSV subsystem is absent rather than claimed fixed without tests. | Package topology. |
| GPU evaluator unreachable and self-test never runs | GPU subsystem is absent and status says disabled/absent. | Package topology and status matrix. |
| Dead GPU variance plumbing | Renderer/shader source absent. | Package topology. |
| Oversized lookup table uploaded per dispatch | No GPU path; no CPU lookup. | Source guard and numeric probe. |
| `TransformedKnownMean` not transformed | Simple known mean is passed through the selected transform at build and rejected if outside its domain. | Build guard test. |
| Process-global actor generation map is unsafe | Editor actor/live-bake subsystem absent. | Package topology. |
| Sentinel requirement substituted without documentation | No dispatch or sentinel claim exists in active package. | Status and deviations docs. |
| Status prose carries more weight than evidence | One status document distinguishes verified, present-unverified, absent, and not-run. Result record can be regenerated from source. | `Docs/IMPLEMENTATION_STATUS.md` and runner. |
