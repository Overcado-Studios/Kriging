# Professional Use: Mining, O&G, Environmental Science

This document is for practitioners who know kriging and need to know what's really implemented, how it's been validated, and what to watch for when deploying in production workflows.

## Implemented numerical core

### Drift methods

- **Simple kriging** with known mean
- **Ordinary kriging (OK)**
- **Universal kriging (UK):** Linear and quadratic polynomial drift, centered and scaled on effective sample bounds
- **External drift kriging:** One additional co-variate field (e.g., distance-to-fault); constant external drift rejected to avoid collinearity
- **Inverse-distance weighting (IDW):** Fallback method when kriging system is singular; automatic degradation reporting

### Variogram structures

- **Single structure:** Spherical, exponential, Gaussian, Matérn (any shape order), power (0 < α < 2)
- **Nested structures:** Up to three structures; cumulative sill = sum of partial sills (exact; no approximation)
- **Anisotropy:** 2D (azimuth rotation) and 3D (azimuth, dip, plunge rotations + Y/Z axis stretch); per-structure

Power model is rejected for simple kriging (unbounded). Matérn special case: ν = 0.5 reduces exactly to exponential; no hard branch to Gaussian at high ν, but internal limit checks prevent Bessel underflow.

### Nugget and measurement variance

- **Nugget modes:** Exact (sample-coincident query returns exact sample value; nugget excluded from right-hand side) and Filtered (nugget included; regular kriged value at sample location)
- **Per-sample measurement variance:** Heteroskedastic; interpreted as diagonal-only contribution to the system matrix
- **Nugget conditioning:** For Gaussian structures with very small nugget, effective nugget is raised to 1e-4 × totalSill (safeguard); exact mode maintains algebraic exactness when no measurement variance present

### Drift basis scaling

Polynomial drift coordinates are centered at the midpoint of sample bounds and scaled by half-extents (floored at 1 unit). Linear and quadratic terms remain dimensionless and near order-one even for large coordinate offsets (standard practice for numerical stability).

External drift centered by sample mean, scaled by maximum absolute deviation. Any constant external drift is rejected (linearly dependent on intercept).

### Solver

- **Linear system:** Augmented symmetric indefinite (kriging equations)
- **Factorization:** Partial-pivot LU; Cholesky rejected for indefinite systems
- **Ridge escalation:** Attempted with zero ridge first; bounded escalation to a documented conditioning floor if factorization fails
- **Exactness semantics:** Solved system exactness, not shortcut return; stable system preferred

Power structures use signed system with `-gamma` covariance; variance clamping (0-floor) is atomically reported.

### Neighborhood and tree algorithms (C++ core)

- **Global solve (C++ core exact solver):** Bounded to 512 effective samples (hard cap; loud error if exceeded) — this path uses a fixed-size stack workspace sized for that bound. The Blueprint layer never calls this global solver directly; it always goes through the core's automatic local-neighborhood strategy (static k-d tree + bounded `MaxNeighbours` per query), which has no such fixed workspace and therefore no sample-count cap.
- **Local neighborhood:** Static balanced k-d tree, squared Euclidean distance, deterministic tie-breaking by original index then sample index
- **Sector balancing:** Four quadrants (2D) or eight octants (3D); optional per query
- **Local factorization cache:** 4,096 unique neighborhoods; no whole-flush, only new-entry rejection at capacity
- **Per-sample allocation:** Non-degraded global kriging (value and uncertainty) is verified allocation-free after warm-up; local and IDW use temporary containers

### Transforms

- **Log:** Every sample shifted by (1 - minimum) to ensure positivity; measurement variance mapped by delta-method derivative; back-transform applied to returned values; variance remains in log-space
- **Normal-score:** Deterministic rank-order with original index as tie-breaker; AS241 for inverse normal quantiles; full rank table retained in model state; supplied measurement variance under normal-score interpreted as already score-space (no stable derivative at rank ties)

### Cross-validation

- **Leave-one-out (LOO):** Two paths:
  1. **Fast path** (inverse-diagonal): Eligible only for global untransformed non-power kriging, zero ridge, zero measurement variance, all relevant inverse diagonals finite and strictly positive. For n ≤ 60, runtime verification against complete rebuilds. Above n = 60, fast result returned but `verifiedAgainstBruteForce = false` in report.
  2. **Brute-force path:** Capped at n = 60 samples per design (O(n⁴) reference implementation); above 60, brute-force refused with descriptive error.

- **Metrics:** Standardized errors (residuals / kriging standard deviation), residuals (value - prediction), and global mean squared error all reported in untransformed query space for transformed models.

### Duplicate handling

Deterministic sorted-coordinate clustering (X, Y, Z, then original index). Complete-link admission within merge radius: candidate joins only if within radius of every existing cluster member. This caps cluster diameter and avoids chaining artifacts. Values averaged; measurement variances combined as (sum of variances) / n².

## Validation and credibility

### Independent NumPy/SciPy gate

Nine fixed augmented systems assembled and solved independently:
- All drift methods (simple, ordinary, universal-linear, universal-quadratic, external-drift)
- 2D and 3D anisotropy
- Matérn and power branches
- Exact and filtered nugget modes
- Measurement variance
- C++ and NumPy/SciPy solutions compared; agreement within 1e-9 relative tolerance

**Result:** PASS. Observed maximum scaled error: 1e-14 (roundoff scale, four orders of magnitude below tolerance).

### PyKrige golden gate

Eleven isotropic single-structure scenarios:
- 2D/3D ordinary and universal kriging
- Spherical, exponential, Gaussian structures
- Exact and filtered nugget modes
- 15–34 samples per scenario, 9 query points each (2 exact sample-coincident, 4 interior interpolation, 3 moderate extrapolation)
- PyKrige validated against C++ core using correct range-convention mapping (see below)

**Range convention finding:** PyKrige's "range" parameter is a practical/effective range for every shape. The C++ core's "range" is:
- Spherical: literal saturation distance (matches PyKrige)
- Exponential: bare decay constant; PyKrige rescales by factor of 3 internally
- Gaussian: bare decay constant; PyKrige rescales by factor of 7/4 internally

The mapping is verified empirically: naive assumption (no rescaling) was tested against deliberately sabotaged goldens and failed non-spherical scenarios by 15–65%.

**Result:** PASS. Observed maximum scaled errors: value 3.32e-14, variance 1.13e-14 (tolerance 1e-9; roundoff residual from two different O(n³) solves).

### Unreal Engine compilation gates

- **UBT compile on UE 5.4, 5.5 (Win64: Editor, Development, Shipping):** Verified PASSED
- **`RunUAT BuildPlugin` packaged-plugin path (UE 5.4, 5.5):** Verified PASSED
- **UE 5.6–5.8 UBT compile:** Untested (no Engine installations available)
- **Blueprint module UBT compile + NullRHI automation test:** Verified PASSED (5/5 `Kriging.*` tests, UE 5.4 and UE 5.5)

The core module and the Blueprint module have both been compiled (`BuildPlugin`) and exercised under NullRHI automation on UE 5.4 and 5.5, re-verified after the final marching-cubes fix; see `Tests/Results/ENGINE_GATES.md` for the authoritative record.

## Current status and limits

### What ships (Blueprint layer)

- Core kriging model with build/query functions
- Auto-fit variogram fitting (Spherical/Exponential/Gaussian, with fallback heuristic)
- Grid evaluation (`EvaluateGrid()`) for lattice baking
- Isosurface extraction (`ExtractIsoSurface()`, `ExtractIsoSurfaceToProceduralMesh()`) via marching cubes
- Async background-thread build (`BuildKrigingModelAsync`)
- Leave-one-out cross-validation reporting
- Build diagnostics: sample merge count, degradation flags, warnings

### What is absent (Disabled/absent status)

- Editor UI, variogram visualization, parameter tuning dashboard
- CSV import, Data Tables, asset serialization
- GPU evaluation, shaders, readback
- Grid tiling, tiled variance, blended cross-fade
- Landscape integration, heightmap export
- PCG nodes
- Cesium integration
- Texture/volume baking (you can evaluate a grid, but no automatic texture export)

### Known limits

| Limit | Value | Impact | Workaround |
|-------|-------|--------|-----------|
| **Global solve cap (C++ core)** | 512 effective samples | Exact solve uses a fixed-size stack workspace sized to this bound, and the direct dense solve becomes impractical above it | Use local k-d tree solve instead — it selects a bounded `MaxNeighbours` per query rather than sizing a workspace to the full sample count, so it has no such cap (the Blueprint layer always goes through this path automatically) |
| **Brute-force LOO (C++ core)** | 60 samples max | Above 60, fast diagonal result returned with `verifiedAgainstBruteForce=false` | Use fast path for screening; refit smaller models for exact LOO |
| **Factorization cache (C++ core)** | 4,096 entries | Above capacity, new neighborhood systems skip cache insertion | Non-critical for most workflows; cache hit rate remains high for typical k-d tree selectors |
| **Single CPU** | No GPU path active | Cannot leverage CUDA/HIP for massive query batches | Vectorize at the application layer; CPU evaluate is ~1–10ms per 1,000 queries on modern hardware |

### Performance characteristics (empirical estimates, not benchmarked)

- **Model build:** ~10ms for 100 samples (global solve), ~1ms for 1,000 samples (local solve with k-d tree)
- **Query (single point):** ~0.1ms for local solve (20 neighbors), ~1ms for global solve (100 samples), batch queries 10–100× faster per query (amortized overhead)
- **Build verification:** LOO brute-force at n = 60: ~10–100ms depending on structure complexity
- **Memory:** ~100 bytes per sample; tree + cache ~1MB for typical workflows

(These are indicative only. Your performance depends on sample density, drift method, structure complexity, and hardware. Profile your actual case.)

## 3D workflows

The numerical core and Blueprint layer are both 3D-native:
- **Sampling:** `FKrigingSamplePoint` (Blueprint) / `FKrigeSample` (C++ core) stores 3D positions directly
- **Anisotropy:** 3D rotation and axis-stretch fully implemented via `FKrigingAnisotropySpec`
- **Evaluation:** `SampleValue()`, `SampleValueWithUncertainty()`, and `EvaluateGrid()` handle 3D query points natively

**Common patterns:**
- **Volumetric resource assessment:** Evaluate on a 3D grid; store in texture volume or sparse octree
- **Depth-dependent property trends:** Use `UniversalLinear` drift method (fits linear Z trend) or external drift (C++ core only)
- **Horizontal slice cross-sections:** Build once, slice at different Z values; no per-depth rebuild needed if Z trend is captured by drift

3D anisotropy is per-variogram-structure: you can mix isotropic (near-surface) and anisotropic (depth-dependent) structures in nested models (C++ core feature; Blueprint exposes single-structure variograms).

## 4D (space + time) workflows

**No native 4D kernel is shipped.** Pattern for user implementation:

```
1. For each timestep t:
   - Select samples up to time t (or a rolling window)
   - Build model M[t]
2. Query at (x, y, z, t):
   - Evaluate M[t] at (x, y, z)
   - Interpolate between M[t] and M[t+dt] in time (linear, cubic, or your blend)
   - (Optional: pre-bake M[t] to a 3D grid, then 4D interpolate the grids)
```

This is a reasonable approach for playback and interpolation. True 4D cokriging with time as a dimension would require:
- A 4D anisotropy model
- Temporal-covariance structure
- Sampling design that accounts for temporal correlation

These are not in scope. If you need true 4D kriging, consider wrapping an external library (PyKrige, gstat, GSLIB) for pre-computation, then load the result in Unreal.

## Production deployment checklist

- [ ] **Validate variogram:** Cross-check your fitted parameters against domain knowledge and LOO diagnostics (if building manually)
- [ ] **Test numerical stability:** Check `FKrigingBuildResult` for ridge escalation warnings or IDW fallback messages
- [ ] **Quantify confidence:** Do not ignore the StdDev field; use it to weight decisions and flag extrapolation regions
- [ ] **Sample count:** Blueprint layer has no hard limit — it uses the core's automatic local-neighborhood strategy (bounded `MaxNeighbours` per query), which is uncapped; if using the C++ core's global exact solver directly, keep n < 512 (its fixed-size stack workspace is sized to that bound)
- [ ] **Profile memory and speed:** Benchmark your specific sample count and structure on target hardware
- [ ] **Version your variograms:** Store variogram parameters (EffectiveRange, Sill, Nugget, Shape, Method) so results are reproducible
- [ ] **Test on target Engine:** UE 5.4–5.5 verified; UE 5.6–5.8 untested. Compile the plugin on your target version to verify

## Coordinate conventions and large-world stability

The core is unit-agnostic and centered-coordinate-safe (no large absolute positions required). Standard practice:

1. **World origin:** Subtract your world's large offset to get local coordinates (e.g., centered at (0, 0, 0) in your working area)
2. **Kriging:** Pass local coordinates to the core
3. **Result:** Interpolated values and variances in your local coordinate system; map back to world coordinates if needed

This avoids floating-point precision loss in very-large-world titles.

## Range convention: External tool mapping

**Blueprint layer:** Use `EffectiveRange`, which means the same thing in this plugin as in PyKrige and most modern kriging tools — the distance where spatial correlation becomes negligible.

**C++ core level:** The portable core (kriging::portable::Model) stores a bare decay constant called `Structure::range`. The Blueprint layer automatically converts EffectiveRange to this:
- Spherical: bare constant = EffectiveRange
- Exponential: bare constant = EffectiveRange / 3
- Gaussian: bare constant = EffectiveRange × 4 / 7

When comparing or importing parameters from other tools:

| Tool | Shape | "Range" convention | Mapping to Blueprint EffectiveRange |
|------|-------|------|-----|
| **GSLIB** | Spherical | Literal saturation | Use directly |
| **GSLIB** | Exponential | "Practical range" (exact definition varies) | Check documentation; typically multiply our bare constant by 3 |
| **PyKrige** | Any | Effective/practical range | Use directly as EffectiveRange |
| **gstat (R)** | Exponential | Bare decay constant (1/3 of practical range) | Multiply by 3 to get EffectiveRange |
| **This plugin (Blueprint)** | Any | EffectiveRange (practical/effective range) | Direct; all shapes use consistent convention |

**IMPORTANT:** Always document the convention when sharing models. A "range = 500" parameter from GSLIB may mean different things depending on whether it's the bare constant or the practical range (which GSLIB does not always clarify). Use EffectiveRange here to avoid ambiguity.

## Troubleshooting

### "Build failed: 512 effective samples exceeded" (C++ core only)
→ This error is specific to the C++ core's exact global solver, whose fixed-size stack workspace is sized to the 512-sample bound. The Blueprint layer always uses the core's automatic local-neighborhood (k-d tree) strategy instead, which selects a bounded `MaxNeighbours` per query rather than sizing a workspace to the full sample count, so it has no such cap. If you're using the C++ core directly, use local k-d tree solving instead of the global exact solve.

### "Negative StdDev/Variance reported" (C++ core)
→ Extremely rare at the Blueprint layer. At C++ core level: rounding error in the matrix solve can produce a tiny negative variance, which is clamped to zero and counted in the build report. If this happens frequently:
- Numerically unstable variogram parameters (e.g., EffectiveRange >> sample spread)
- Degenerate sample configuration (e.g., all samples collinear)

Try: add a small Nugget, reduce EffectiveRange, or merge nearby duplicate samples.

### "IDW fallback occurred" (reported in Result.Message)
→ Kriging matrix became singular at some point (usually due to duplicate samples or degenerate local configuration). IDW was used instead. Result.Message reports which queries degraded. Fix: add Nugget, reduce EffectiveRange, or increase MergeRadius to merge duplicate samples.

### Build succeeded but Result warns about "ridge escalation"
→ The kriging matrix was nearly singular; the solver added regularization (ridge). Causes:
- Variogram parameters unrealistic (e.g., EffectiveRange >> sample spread)
- Duplicate or near-duplicate samples
- Samples collinear in local neighborhood

Try: increase MergeRadius, add small Nugget, or decrease EffectiveRange if data is clustered.

### LOO cross-validation results don't match other software
→ Check: EffectiveRange convention (bare decay constant vs. practical range), NuggetMode (Exact vs. Filtered), and whether transforms (Logarithmic, NormalScore) were applied. Different software may default differently. Use the same settings across tools for meaningful comparison.

---

## Reference

- **QUICKSTART.md** — Game-dev tutorial
- **GEOSTATS_PRIMER.md** — Variogram concepts
- **FAQ.md** — Common questions
- **RANGE_CONVENTIONS.md** (in Tests/Golden/) — Detailed convention research and PyKrige comparison
- **IMPLEMENTATION_STATUS.md** (in Docs/) — Status vocabulary and what's verified vs. not-run

