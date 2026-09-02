# Frequently Asked Questions

## Licensing and distribution

**Q: Is this free? What license?**

A: Yes, MIT. No copyleft dependencies (Boost.Math used is Boost 1.0, which is permissive; Algorithm AS241 is published academic work independently implemented; marching-cubes tables are public domain). Use it in commercial projects.

**Q: Can I ship this in a game/app/service?**

A: Yes. MIT allows private modification and redistribution as part of a closed product. Include the MIT license text (provided in the plugin) in your credits or legal notice.

**Q: Do I need to open-source my game or my variogram parameters?**

A: No. The plugin is MIT, not GPL. Your game code and your data (samples, fitted variograms) are yours.

---

## Installation and compatibility

**Q: What Unreal Engine versions does this work on?**

A: **UE 5.4–5.5 verified PASSED** (Win64: Editor, Development, Shipping; `BuildPlugin` and NullRHI automation, 5/5 `Kriging.*` tests, re-verified after the final marching-cubes fix — see `Tests/Results/ENGINE_GATES.md`). **UE 5.6–5.8 untested** (no Engine installations tested yet). If you're on UE 5.6–5.8, copy the plugin into `Plugins/` and build to verify compatibility on your machine.

**Q: Do I need Unreal Engine installed to use the core in a standalone tool?**

A: No. The numerical core (`KrigePortableCore.cpp` and related headers) is engine-free. You can embed it in a Python wrapper, a command-line tool, or any C++ project. See `Tests/Standalone/` for an example CMake build.

---

## Sample counts and performance

**Q: How many samples can it handle?**

A: The Blueprint API always goes through the core's automatic local-neighborhood (k-d tree) strategy, which selects a bounded `MaxNeighbours` per query rather than sizing a workspace to the full sample count — so it has no hard cap. Typical scenarios:
- 10–1,000 samples: build in milliseconds, query in microseconds
- 10,000+ samples: build in 1–10ms, query still microseconds (per query, with 32 neighbors by default)

The C++ core's separate global exact solver has a 512-sample cap, because that path uses a fixed-size stack workspace sized to the bound. The Blueprint layer never calls that global solver, so this cap doesn't apply there — no configuration needed.

**Q: What's typical performance?**

A: Rough estimates (not benchmarked; measure your case):
- Model build: ~10ms (100 samples), ~1ms (1,000 samples)
- Single query: ~0.1–1ms
- Batch queries (1,000 points): ~10–100ms (amortized overhead)
- LOO cross-validation: ~10–100ms (n=60)

For real-time queries, batch them: querying 1,000 points is much faster per query than querying one point 1,000 times.

**Q: Can I use this for 1M samples?**

A: For kriging with 1M samples:
- Model build: O(n log n) k-d tree + local neighborhood solves; should be fine
- Per-query memory: bounded (local solve uses stack-allocated workspace)
- Factorization cache (4,096 entries) may have low hit rate if neighborhood patterns are very diverse (non-critical)

Profile with your data. For online gameplay (frame-tick queries), 1M is challenging; for offline asset baking, reasonable. If you need massive GPU batch queries, consider GPU implementations (not in this plugin yet).

---

## Uncertainty and standard deviation

**Q: What does StdDev (standard deviation) actually mean?**

A: StdDev is the kriging system's prediction of how uncertain its estimate is, in the same units as your data.

**Example:** If you're kriging temperature and get:
- Value = 25.0°C
- StdDev = 2.0°C

Then a rough 68% confidence interval is 25 ± 2 = [23, 27]°C. (This assumes normality; kriging doesn't guarantee it, but it's a reasonable approximation.)

**Q: Does StdDev account for measurement error?**

A: Yes. The `Nugget` parameter captures local unexplained noise. Kriging then reports a StdDev that reflects both spatial uncertainty and measurement noise.

**Q: Why is StdDev zero exactly at sample locations?**

A: In exact nugget mode, the kriging system is constrained to pass through each sample exactly. Therefore, at a sample location, there's no prediction error, so StdDev = 0. In filtered mode, StdDev is nonzero (it includes the nugget—the system is regularized).

**Q: What about negative StdDev?**

A: Impossible—standard deviation is always non-negative by definition. (At the C++ core level, extremely rare rounding errors can produce tiny negative variances, which are clamped to zero; the core reports the count.)

---

## Variogram and ranges

**Q: Why do EffectiveRange values differ from ranges in other tools?**

A: **Convention difference — now transparent at the Blueprint layer.** This plugin's Blueprint API uses `EffectiveRange`, which means the distance where spatial correlation becomes negligible (~95% decay). That's the same convention as PyKrige and modern kriging practice.

If you're comparing to older tools (GSLIB, early gstat) or reading C++ code (the portable core), those may use a bare decay constant where correlation at that distance is ~63%, not ~95%. The mapping is:
- Exponential: bare constant = EffectiveRange / 3
- Gaussian: bare constant = EffectiveRange × 4 / 7

**Concrete example:**
- GSLIB documentation says: "Exponential bare range parameter = 100"
- Convert to EffectiveRange: 100 × 3 = 300
- Use EffectiveRange = 300 in BuildKrigingModel()

See PROFESSIONAL_USE.md for a full external-tool mapping table.

**Q: Can I use a variogram from another tool directly?**

A: For `EffectiveRange`: if the other tool uses the same convention (practical/effective range), yes—use it directly. If it uses a bare decay constant, apply the mapping above.

For other parameters (Sill, Nugget, shape), use directly—those are standardized across kriging tools. Test with leave-one-out cross-validation to verify the conversion worked: build a model, check `Result.bHasCrossValidation` and `Result.CrossValidationRMSE`.

**Q: Can I fit a variogram automatically?**

A: **Yes!** Use `BuildKrigingModelAuto(Samples, Settings, Result)`. It computes the empirical variogram and fits Spherical/Exponential/Gaussian shapes (keeps the best fit via weighted least squares). 

Works best with 20+ well-spread samples. If auto-fit can't find a reliable curve, it falls back to a heuristic variogram and warns you in `Result.Warnings`.

**For manual fitting:**
- Use domain knowledge (geology, prior surveys)
- Compute the empirical semivariogram and fit a curve externally (PyKrige, R gstat)
- Supply the fitted parameters to `BuildKrigingModel(Samples, VariogramSpec, Settings, Result)`
- Use leave-one-out cross-validation to test your fit

---

## Transforms

**Q: When should I use log or normal-score transforms?**

A: **Log transform:** Your data are positive and log-distributed (e.g., ore grades, permeability, raindrop sizes). Log normalizes the distribution. Use if your raw data have extreme skew or if your domain theory says the phenomenon is log-normal.

**Normal-score transform:** Your data have outliers or unknown distribution. This converts data to ranks (quantiles), kriging on rank-space, then back-transforms to original quantiles. Removes outlier leverage entirely.

**Neither:** Your data are roughly symmetric and Gaussian. Most meteorological and engineering data fit this.

**Q: What are the transformed variance units?**

A: Variance is always in the transformed space. If you use log:
- Value returned is in original units (back-transformed)
- Variance returned is in log-space units
- To convert back, use: `variance_original ≈ variance_log × (value)²` (delta method)

If you use normal-score:
- Value and variance are in score-space (standardized quantiles)
- Interpretation: variance = standardized uncertainty; no direct back-transformation to original-space variance (it's not well-defined for rank-based transforms)

See GEOSTATS_PRIMER.md for more detail.

---

## Drift methods

**Q: What drift should I use?**

A: In the Blueprint API, choose from `EKrigingMethod`:
- **Ordinary (default):** Assumes no large-scale trend. Best when samples are roughly spread evenly across the area.
- **Simple:** Assumes a known, fixed mean (you supply `KnownMean` in Settings). Only use if you truly know the global mean.
- **UniversalLinear:** Fits a 1st-order trend (e.g., elevation increases northward). Kriging removes the trend and interpolates residuals.
- **UniversalQuadratic:** Fits a quadratic surface. Use if the trend curves (rare in practice; risky with small sample sizes).
- **InverseDistance:** No variogram needed; simple distance-weighted average. Fast, but no uncertainty estimate. (Fallback used internally when kriging fails.)

**Note:** External drift kriging (using another measured field as a guide) is available at the C++ core level but not exposed in the Blueprint API.

**Q: How many samples do I need for each drift method?**

A: Rough minimums:
- Ordinary: ≥10 (really want ≥20–50)
- Simple: ≥20 (need enough structure to estimate variogram reliably)
- Linear universal: ≥15–20
- Quadratic universal: ≥30–50

Below these, fitting becomes unstable and leave-one-out errors will be high.

---

## 3D and 4D

**Q: Does it work in 3D?**

A: Yes, fully. Samples have XYZ coordinates, anisotropy is 3D (azimuth, dip, plunge), and evaluation is 3D. No special setup needed.

**Q: Can I do 4D kriging (space + time)?**

A: No native 4D kernel. Workaround: build one model per timestep, then interpolate between them in time (linear, cubic spline, etc.). See PROFESSIONAL_USE.md for the pattern. This is a user-layer solution; production-grade 4D kriging (cokriging with time as a dimension) would require a separate time-covariance model.

---

## Troubleshooting

**Q: Model build fails with an error about 512 samples.**

A: This is a C++ core-level limitation: the exact global solver uses a fixed-size stack workspace sized to a 512-sample bound. The Blueprint layer should handle this automatically, since it always uses the core's automatic local-neighborhood (k-d tree) strategy instead — that strategy selects a bounded `MaxNeighbours` per query rather than sizing a workspace to the full sample count, so it never hits this cap. If you encounter this error in Blueprint code, report it as a bug. For the C++ core directly, use local k-d tree solving instead of the global exact solve.

**Q: Build succeeded but Result.Message warns about ridge escalation or IDW fallback.**

A: The kriging matrix was nearly singular at some point (usually during LOO). Causes:
- Variogram parameters unrealistic (e.g., EffectiveRange >> sample spread)
- Duplicate or near-duplicate samples (merge them; see `FKrigingSettings::MergeRadius`)
- All samples collinear or degenerate configuration

Fix: add a small nugget, decrease EffectiveRange, increase MergeRadius, or collect more diverse samples. LOO results remain valid, but the model is less stable.

**Q: StdDev is very high everywhere (near-Sill).**

A: Likely causes:
- Sample count too small (kriging has little structure to learn)
- Samples widely spread (low sample density relative to EffectiveRange)
- Variogram Sill too large relative to your data variance
- Nugget too large (too much modeled noise)

Fix: collect more samples, reduce Sill to match your data variance, reduce Nugget, or decrease EffectiveRange if samples are clustered at small scales.

**Q: EvaluateGrid returns false.**

A: `EvaluateGrid` returns false if the resolution is invalid (each axis must be >= 2). Check that Resolution.X, Resolution.Y, Resolution.Z are all >= 2. Also, ensure the Model is Valid (call `Model->IsValid()` first).

**Q: ExtractIsoSurface returns false.**

A: `ExtractIsoSurface` returns false if the IsoValue does not actually intersect the field within the given Box. Check the model's value range first: sample the grid at a few points and verify that IsoValue lies within the min/max values you observe.

---

## Further help

- **QUICKSTART.md** — Step-by-step game-dev example
- **GEOSTATS_PRIMER.md** — Variogram and kriging concepts
- **PROFESSIONAL_USE.md** — Industry workflows and validation
- **RANGE_CONVENTIONS.md** — Detailed range-convention research
- **IMPLEMENTATION_STATUS.md** — What's verified vs. not-run

