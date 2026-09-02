# Variogram range conventions: C++ core vs. PyKrige

This note exists because "range" is not a self-explanatory parameter name --
different geostatistics libraries attach it to different points on the same
curve. It records, from primary source in both codebases, exactly what each
library's range parameter means per model shape, and the conversion used by
the PyKrige golden gate (`generate_goldens.py`, `validate_goldens.py`).

## The C++ core's convention

Source: `Source/KrigingCore/Private/Portable/KrigePortableCore.cpp`,
`EvaluateNormalizedStructure(Shape shape, double ratio, ...)`, where
`ratio = h / structure.range` (`h` = separation distance), and
`Source/KrigingCore/Public/KrigePortableCore.h`, `struct Structure { double
range; double partialSill; ... }`.

| shape       | normalized curve `g(ratio)`, `ratio = h / a`        | what `a` ("range") means |
|-------------|------------------------------------------------------|---------------------------|
| Spherical   | `ratio>=1 ? 1 : 1.5*ratio - 0.5*ratio^3`              | literal saturation distance -- `g` reaches exactly 1 at `h = a` |
| Exponential | `1 - exp(-ratio)`                                     | bare decay constant -- `g` reaches `1 - e^-1 ≈ 63%` at `h = a`, not a "practical range" |
| Gaussian    | `1 - exp(-ratio^2)`                                    | bare decay constant -- `g` reaches `1 - e^-1 ≈ 63%` at `h = a` |

Semivariogram value at distance `h` for one structure: `partialSill * g(h/a)`.
Total sill of a structure (excluding nugget) is `partialSill`.

## PyKrige's convention

Source: `pykrige/variogram_models.py` (installed copy, pykrige 1.7.3), where
`m = [psill, range_, nugget]` and `d` is the lag distance:

```python
def spherical_variogram_model(m, d):
    return psill * ((3*d)/(2*range_) - d**3/(2*range_**3)) + nugget   # d <= range_
    # else: psill + nugget

def exponential_variogram_model(m, d):
    return psill * (1.0 - np.exp(-d / (range_ / 3.0))) + nugget

def gaussian_variogram_model(m, d):
    return psill * (1.0 - np.exp(-(d**2.0) / (range_ * 4.0 / 7.0) ** 2.0)) + nugget
```

PyKrige's `range_` is a **practical/effective range** for every shape: for
spherical it coincides with the literal saturation distance (as it happens
to for the C++ core too), but for exponential and Gaussian PyKrige rescales
the internal decay constant so that `range_` also reads as an effective
range (division by `range_/3` for exponential, by `range_*4/7` for
Gaussian).

## The mapping

| shape       | `a_cpp` (C++ "range")   | `R_pykrige` (PyKrige "range") |
|-------------|-------------------------|-------------------------------|
| Spherical   | `a_cpp`                 | `R_pykrige = a_cpp`           |
| Exponential | `a_cpp`                 | `R_pykrige = 3 * a_cpp`       |
| Gaussian    | `a_cpp`                 | `R_pykrige = (7/4) * a_cpp`   |

This is not asserted on faith: `generate_goldens.py` also computes, for one
exponential and one gaussian scenario, what PyKrige produces under the
*naive* (wrong) assumption `R_pykrige = a_cpp` and records the resulting
discrepancy in `goldens.json["sensitivity"]`. Observed naive-vs-correct
deltas ranged from ~0.02 to ~0.65 on quantities with sill ~0.6-1.2 -- a
clearly detectable, not roundoff-scale, difference. `validate_goldens.py`
was also run once against a deliberately sabotaged golden file generated
with the naive mapping: every non-spherical scenario failed by 15-65%
relative to a 1e-9 tolerance, while spherical scenarios (where the naive and
correct mappings coincide) still passed. This demonstrates the mapping above
is the only one of the two that a correctly-implemented C++ core can pass.

## A second, independently discovered convention hazard: sill parameterization

PyKrige has a second, unrelated convention trap in
`pykrige/core.py::_make_variogram_parameter_list`. When
`variogram_parameters` is supplied as a **list** `[p0, range, nugget]` for
spherical/exponential/gaussian/hole-effect, PyKrige treats `p0` as the
**full sill** (partial sill + nugget) and internally computes
`partial_sill = p0 - nugget` before calling the variogram function above --
contradicting its own docstring, which labels the list's first entry
`psill`. Verified empirically: `variogram_parameters=[1.23, 4.56, 0.07]` for
`"exponential"` is reported back via `ok.variogram_model_parameters` as
`[1.16, 4.56, 0.07]` (`1.23 - 0.07 = 1.16`).

The **dict** form does not do this: `{"psill": p, "range": R, "nugget": n}`
is a true pass-through. `generate_goldens.py` uses the dict form everywhere
specifically so the C++ core's `partialSill` field maps 1:1 onto PyKrige's
`psill` without an extra, undocumented nugget correction that has nothing to
do with the range-convention hazard this gate targets.

## Nugget mode

C++ `NuggetMode::Exact` / `NuggetMode::Filtered` (see
`Source/KrigingCore/Public/KrigePortableCore.h`, `Variogram::nuggetMode`)
corresponds to PyKrige's `exact_values=True` / `exact_values=False`
constructor argument. Both mean the same thing: whether querying exactly at
a sample location snaps to that sample's raw value (`Exact` /
`exact_values=True`, nugget excluded from the coincident right-hand side) or
returns the smoothed/regularized surface value (`Filtered` /
`exact_values=False`, nugget included even at zero distance).

## What stays isotropic / single-structure and why

The golden scenarios in `generate_goldens.py` are all isotropic
(`ratioY = ratioZ = 1`, no rotation) single-structure variograms. This is a
deliberate scope reduction, not an oversight: PyKrige's anisotropy angle
convention is a separate, orthogonal hazard from range convention, and it is
already exercised by `Tests/Review/validate_reference.py` (the NumPy/SciPy
gate). Mixing anisotropy into this gate would make a future failure
ambiguous between "range convention broke" and "anisotropy angle convention
differs between libraries" -- exactly the kind of confounding this gate is
built to avoid. See `SUMMARY.md` for the full list of scope exclusions.
