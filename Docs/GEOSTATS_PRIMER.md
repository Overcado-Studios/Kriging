# Geostatistics Primer: Kriging for the Curious

## What is a variogram? (The heart of kriging)

A variogram answers: *How much do values differ based on distance?*

Imagine you place a temperature sensor at every grid point on a map. Points 10m apart are probably close in temperature. Points 10km apart might be very different. A variogram is a function that describes this decay pattern.

```
Difference
    ^
    |     .  .
    |   .        .
    | .              .
    |_____________________> Distance
    low                   high
```

At distance zero, the difference is zero (a point is identical to itself). As distance grows, differences grow. Eventually they plateau—the difference stops increasing because you're far enough away that the spatial correlation is gone.

**In the plugin, the variogram encodes:**
- How far similarity decays (the `range` parameter)
- How strongly correlated nearby values are (the `sill`)
- How much unexplained noise exists (the `nugget`)
- The *shape* of decay: spherical (sharp cutoff), exponential (gradual), Gaussian (very smooth), Matérn (flexible smoothness), or power (unbounded)

## The three parameters explained in plain words

### EffectiveRange: Where influence ends

**For Blueprint users:** Set `EffectiveRange` to the distance at which spatial correlation becomes negligible (roughly the 95% decay point). Spherical reaches its plateau exactly here; Exponential and Gaussian approach it asymptotically but are treated as exhausted at this distance.

**Why the name matters:** "EffectiveRange" removes the ambiguity that plagues many kriging tools. Other libraries (PyKrige, GSLIB) use "range" to mean different things depending on the shape—for Exponential, some mean the bare decay constant (at which correlation is ~63%), others mean the practical distance where influence ends. This plugin's Blueprint API uses EffectiveRange unambiguously: **it's always the distance where influence is essentially gone.**

**For C++ users working with the portable core:** The underlying numerical core stores a bare decay constant internally (called `Structure::range` in kriging::portable::Model). The Blueprint layer automatically converts EffectiveRange to this bare constant:
- Spherical: bare constant = EffectiveRange (identical)
- Exponential: bare constant = EffectiveRange / 3
- Gaussian: bare constant = EffectiveRange × 4 / 7
- Matérn and Power: bare constant ≈ EffectiveRange (no closed-form conversion; treated as approximate correlation length)

**Mining/O&G analogy:** If your geology survey says "similarity fades beyond 500m", that's an effective range. Feed it directly as EffectiveRange to BuildKrigingModel().

### Sill: The ceiling

`sill` (aka "partial sill" in the code) = the maximum difference between points. At large distances, two random samples differ by roughly `sill` on average. In kriging, this is the variance of the field itself, excluding noise.

**Intuition:** Temperature on Earth has a sill of ~40°C (poles vs. tropics). Ore grade in a vein might have a sill of 5–20% (rich vs. barren).

### Nugget: The unexplained noise

`nugget` = correlation not explained by distance. Two samples right next to each other might still differ because:
- Measurement error
- Sub-grid-scale variation ("I took two temperature readings 1cm apart; they differ by 0.5°C")
- True randomness in the phenomenon

**In the plugin:**
- `NuggetMode::Exact` — at a sample location, kriging returns exactly that sample's value (nugget is excluded from the fit)
- `NuggetMode::Filtered` — at a sample location, kriging returns a regularized value (nugget is included, smoothing out noise)

Choose `Exact` for precious survey data. Choose `Filtered` if your samples are noisy measurements.

## Why kriging beats inverse-distance weighting (IDW)

IDW is simple: nearby points count more, distant points count less. It works. But kriging is smarter:

| Aspect | IDW | Kriging |
|--------|-----|---------|
| **Confidence** | No idea how sure it is | Returns variance (confidence map) |
| **Spatial structure** | Ignores it—all samples treated equally | Learns correlation from distance |
| **Extrapolation** | Swings wildly outside sample bounds | Smooth, bounded by sill |
| **Multiple scales** | No way to mix short + long-range patterns | Nested structures (e.g., local + regional ore trends) |
| **Math** | Fast, simple | Slower (matrix solve), richer model |

**Real example:** You're kriging playtest death locations. IDW says "average the 5 nearest deaths." But kriging says "deaths 100m away matter more than deaths 500m away *because I learned from your data that death locations are more similar at short range*. And here's my confidence that this estimate is right."

The confidence map is gold for game dev: high confidence near clusters of data, low confidence in gaps.

## When NOT to use kriging

**Too few samples:** Kriging needs structure to learn. With 5 samples and a 100m map, there's not much structure to find. **Rule of thumb:** ≥20 samples before the model becomes stable. ≥50 before you trust it in production.

**No spatial correlation:** If your values are pure random noise with no distance dependence, kriging returns roughly the global mean everywhere and high variance everywhere. It's not wrong; it's just telling you there's no signal. (Use IDW or lookup tables instead.)

**Extreme outliers:** Kriging assumes data are roughly Gaussian. One extreme outlier can break the model. **Use normal-score transform** (rank-order quantiles) if your data are skewed or have outliers.

**4D (space + time) native:** The plugin does not have a built-in 4D kernel. **Workaround:** Build one model per timestep, interpolate between them in gameplay. This is a user-layer pattern, not a shipped feature.

## Anisotropy: Direction-dependent correlation

Some phenomena have correlation that depends on direction. Example: ore grades are more similar along a fault line (direction matters). Kriging handles this by rotating and stretching the distance metric.

**In the plugin:**
- 2D: azimuth angle (rotation)
- 3D: azimuth, dip, plunge (three Euler rotations) + ratioY, ratioZ (axis-stretch factors)

Set `ratio = 2` and kriging sees distances twice as far in the Y direction. Useful for elongated features (e.g., ore shoots, river channels).

## The variogram fitting problem

Your variogram's parameters (EffectiveRange, Sill, Nugget) must match your data. There are three approaches:

1. **Auto-fit** — Use `BuildKrigingModelAuto()` to automatically compute the empirical variogram and fit Spherical/Exponential/Gaussian shapes (keeps the best). Works best with 20+ well-spread samples.
2. **Guess from domain knowledge** — Mining surveys suggest Exponential with effective range ~500m? Use it. Geologists will validate.
3. **Manual fit** — Compute the empirical semivariogram (for each pair of samples, plot distance vs. difference), fit a curve by eye or non-linear optimization, then test with leave-one-out cross-validation.

**The plugin includes:**
- **Auto-fit** via `BuildKrigingModelAuto()` — computes empirical variogram and fits a model curve using weighted least squares. Falls back to a heuristic if fitting fails (warns you in the result).
- **Leave-one-out cross-validation** — measure prediction error for every sample held out once. Helps tune a manually-chosen variogram.

See `PROFESSIONAL_USE.md` for validation workflows.

## Drift: Removing trends

Some fields have a trend (e.g., elevation increases northward). Kriging can fit and remove it:

- **Ordinary kriging (OK):** No trend removal. Kriging assumes the field is stationary (no bias).
- **Universal kriging (UK):** Fit a 1st or 2nd-degree polynomial (linear or quadratic trend). Kriging works on the residual.
- **External drift:** Use another measured field as a guide (e.g., kriging ore grade using distance-to-fault as the external field).

**When to use it:** Elevation always has a trend (gravity). Climate has a trend (latitude). If you don't remove it, kriging confuses trend with local structure.

## Unit agnosticism

The plugin does not care about units. Distances are in your coordinate system (cm in Unreal). Values are in your value units. The math is the same whether you're kriging temperatures in Celsius or ore grades in %.

**Gotcha:** Large coordinate offsets. If your samples span 0–1,000,000 cm (10 km), numerical precision can suffer. **Workaround:** Subtract a large world origin from your coordinates (keep only local offsets), then use that in kriging. This is standard practice in game engines.

## Transform semantics

**Log transform:** Kriging on log(value). Useful if your data are log-distributed (e.g., ore grades, raindrop sizes). Returned values are back-transformed to original units. Variance returned is in log-space; to get variance in original units, use the delta method: `variance_original ≈ variance_log × (value)²`.

**Normal-score:** Kriging on rank-order (quantile transform). Removes outlier effects entirely. Useful if your data have extreme values. Returned values are back-transformed to original quantiles. Variance is in score-space; interpret it as a standardized uncertainty.

---

## Quick reference: Variogram model choices

| Shape | When to use | EffectiveRange convention |
|-------|------------|-----------------|
| **Spherical** | Sharp spatial cutoff (ore vein boundary) | Distance at exact plateau (all samples beyond this distance have zero correlation) |
| **Exponential** | Gradual smooth decay (temperature fields) | Distance where correlation becomes negligible (~95% decay; core uses EffectiveRange / 3 internally) |
| **Gaussian** | Very smooth, long-range influence (climate) | Distance where correlation becomes negligible (~95% decay; core uses EffectiveRange × 4 / 7 internally) |
| **Matérn** | Fine control over smoothness (academic/specialized) | Approximate correlation length; core treats as pass-through (no closed-form conversion factor) |
| **Power** | Unbounded (fractal-like) data; rare | Scale parameter only (no plateau/sill; "effective range" not meaningful) |

---

## Further reading

- **QUICKSTART.md** — Tutorial with code
- **PROFESSIONAL_USE.md** — Industry terminology, validation, 3D/4D workflows
- **FAQ.md** — Common questions
- **RANGE_CONVENTIONS.md** (in Tests/Golden/) — Detailed range-convention research and comparison with PyKrige/GSLIB

