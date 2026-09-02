#!/usr/bin/env python3
"""Generate PyKrige golden kriging predictions for the C++ range-convention gate.

Why this file exists
---------------------
Tests/Review/reference_probe.cpp + validate_reference.py already cross-check
the shipped C++ core against an *independent NumPy/SciPy re-derivation of the
same math*. That is a strong "did we implement our own formulas correctly"
gate, but it cannot catch a mistaken *convention*: if the C++ author and the
NumPy author both believe "range" means the same thing, a wrong shared belief
sails through undetected. The one way to catch that is to compare against a
genuinely independent library that made its own, possibly different,
convention choice. PyKrige (BSD-3, no GPL/LGPL exposure) is that library.

THE RANGE-CONVENTION HAZARD, RESEARCHED FROM PRIMARY SOURCE
-------------------------------------------------------------
Read directly from the installed pykrige.variogram_models source
(pykrige/variogram_models.py in this venv's site-packages), the built-in
models are, with m = [psill, range_, nugget] and d = lag distance:

    spherical:    psill * (3d/(2R) - d^3/(2R^3))   for d <= R, else psill   (+nugget)
    exponential:  psill * (1 - exp(-d / (R/3)))                            (+nugget)
    gaussian:     psill * (1 - exp(-(d / (R*4/7))^2))                      (+nugget)

So PyKrige's "range" R is the *practical/effective range*: the lag at which
the curve is (by construction) at/near the sill, regardless of shape. For
spherical this coincides with the literal shape parameter because the
spherical model already saturates exactly at its shape parameter. For
exponential and Gaussian, PyKrige *rescales* the shape parameter so that R
also reads as an effective range (~95% of sill at d=R for exponential,
~forced-to-cross-95% for Gaussian's erf-derived 4/7 constant).

THE C++ CORE'S CONVENTION, READ FROM PRIMARY SOURCE
------------------------------------------------------
Source/KrigingCore/Private/Portable/KrigePortableCore.cpp,
EvaluateNormalizedStructure(shape, ratio, ...), where ratio = h / structure.range
(see PreparedStructure::Evaluate two lines above it: "anisotropy.Distance(delta)
/ range"):

    Spherical:    ratio >= 1 ? 1.0 : 1.5*ratio - 0.5*ratio^3
    Exponential:  1 - exp(-ratio)            = 1 - exp(-h/a)
    Gaussian:     1 - exp(-ratio^2)          = 1 - exp(-(h/a)^2)

So the C++ core's "range" `a` is the bare shape/decay parameter, NOT a
rescaled effective range. For spherical it happens to equal PyKrige's R
(both use the literal saturation distance). For exponential, PyKrige's R is
3x the C++ decay parameter (because PyKrige divides d by R/3, i.e. multiplies
by 3/R = 1/a with a = R/3). For Gaussian, PyKrige's R is 7/4 of the C++ decay
parameter (PyKrige divides d by R*4/7, i.e. by a with a = R*4/7).

THE MAPPING (also written out in RANGE_CONVENTIONS.md at the repo/staging root)
--------------------------------------------------------------------------------
    shape         a_cpp (this library's "range")   ->  R_pykrige (its "range")
    spherical     a_cpp                                R_pykrige = a_cpp
    exponential   a_cpp                                R_pykrige = 3 * a_cpp
    gaussian      a_cpp                                R_pykrige = (7/4) * a_cpp

This mapping is not merely asserted: generate_goldens() also computes, for
one exponential and one gaussian scenario, what PyKrige would produce under
the *naive but wrong* mapping (R_pykrige = a_cpp, i.e. assuming both libraries
share a convention) and records the resulting discrepancy in
goldens.json["sensitivity"]. That number demonstrates the mapping above is
not an arbitrary choice that happens to pass -- the naive mapping visibly
fails, and only the corrected mapping does not.

WHAT IS DELIBERATELY OUT OF PYKRIGE GOLDEN COVERAGE (see SUMMARY.md)
----------------------------------------------------------------------
Simple kriging with known mean, UniversalQuadratic drift, ExternalDrift,
InverseDistance, Matern, Power, nested (multi-structure) variograms,
anisotropy, per-sample measurement variance, and value transforms are not
expressible with PyKrige's built-in models/kriging classes and are excluded
here. They remain covered by Tests/Review/validate_reference.py. PyKrige does
support a "custom" variogram_function hook that could reproduce Matern or
nested sums numerically, but doing so would only cross-check PyKrige's linear
solver against ours -- not its *variogram convention* against ours, which is
the one hazard this gate exists to close. So the golden set here stays on
PyKrige's built-in spherical/exponential/gaussian models.

A THIRD HAZARD, DISCOVERED EMPIRICALLY WHILE BUILDING THIS GATE (sill convention)
------------------------------------------------------------------------------------
pykrige/core.py::_make_variogram_parameter_list has a non-obvious quirk: when
variogram_parameters is passed as a *list* `[p0, range, nugget]` for
gaussian/spherical/exponential/hole-effect, PyKrige treats p0 as the *full*
sill (partial sill + nugget) and internally computes the partial sill fed
into the variogram formula as `p0 - nugget`:

    elif variogram_model in ["gaussian", "spherical", "exponential", "hole-effect"]:
        parameter_list = [
            variogram_model_parameters[0] - variogram_model_parameters[2],  # <-- subtracts nugget
            variogram_model_parameters[1],
            variogram_model_parameters[2],
        ]

This directly contradicts the same file's own docstring ("gaussian - [psill,
range, nugget]") and the variogram_models.py formula, where m[0] is used as
a bare partial sill: `psill * (1 - ...) + nugget`. Verified empirically: a
list `[1.23, 4.56, 0.07]` for "exponential" is reported back by PyKrige as
`variogram_model_parameters == [1.16, 4.56, 0.07]` (1.23 - 0.07 = 1.16) --
see assert_parameters_verbatim's dict-vs-list comparison below.

The DICT form does not do this: passing `{"psill": p, "range": R, "nugget":
n}` for these four models is a true pass-through -- `variogram_model_parameters
== [p, R, n]` unaltered. This is the form generate_goldens.py uses
everywhere, specifically so the C++ core's `partialSill` field maps 1:1 onto
PyKrige's `psill` without an extra nugget correction that has nothing to do
with the range-convention hazard this gate targets.

A FOURTH HAZARD THIS FILE GUARDS AGAINST (not a convention issue, a footgun)
-----------------------------------------------------------------------------
Read from KrigePortableCore.cpp, prepared_nugget_and_sill-equivalent logic
silently *raises* the nugget to 1e-4 * totalSill whenever a Gaussian (or
Matern nu>2.5) structure is present and the supplied nugget is below that
floor (this exists to keep the Gaussian covariance matrix from becoming
numerically singular near-perfectly-correlated columns). PyKrige has no such
guard. If a golden scenario supplied a tiny/zero nugget with a Gaussian
structure, the C++ core would silently use a different nugget than PyKrige
did, producing a real numeric mismatch that has *nothing to do* with the
range convention -- a phantom failure that would burn time. Every Gaussian
scenario below therefore uses a nugget comfortably above that floor, and
`assert_nugget_guard_inert` checks the margin numerically so a future edit
cannot silently reintroduce the phantom.
"""

from __future__ import annotations

import copy
import json
import math
import platform
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import pykrige
import scipy
from pykrige.ok import OrdinaryKriging
from pykrige.ok3d import OrdinaryKriging3D
from pykrige.uk import UniversalKriging
from pykrige.uk3d import UniversalKriging3D

# ---------------------------------------------------------------------------
# C++ core enum values (Source/KrigingCore/Public/KrigePortableCore.h)
# ---------------------------------------------------------------------------
SHAPE_SPHERICAL, SHAPE_EXPONENTIAL, SHAPE_GAUSSIAN, SHAPE_MATERN, SHAPE_POWER = range(5)
METHOD_SIMPLE, METHOD_ORDINARY, METHOD_UNIVERSAL_LINEAR = 0, 1, 2
NUGGET_EXACT, NUGGET_FILTERED = 0, 1

SHAPE_NAME = {SHAPE_SPHERICAL: "spherical", SHAPE_EXPONENTIAL: "exponential", SHAPE_GAUSSIAN: "gaussian"}


def cpp_range_to_pykrige_range(shape: int, a_cpp: float) -> float:
    """The mapping this whole gate exists to validate. See module docstring."""
    if shape == SHAPE_SPHERICAL:
        return a_cpp
    if shape == SHAPE_EXPONENTIAL:
        return 3.0 * a_cpp
    if shape == SHAPE_GAUSSIAN:
        return (7.0 / 4.0) * a_cpp
    raise ValueError(f"No PyKrige built-in model for shape={shape}")


def pykrige_model_name(shape: int) -> str:
    return SHAPE_NAME[shape]


# ---------------------------------------------------------------------------
# Self-tests: verify PyKrige's own behavior empirically rather than trusting
# a reading of its source for the parts that matter most (parameter pass-
# through fidelity, and 3D coordinate/drift-column ordering in ok3d/uk3d).
# ---------------------------------------------------------------------------

def assert_parameters_verbatim() -> None:
    """PyKrige re-derives variogram_model_parameters via _initialize_variogram_model
    even when parameters are supplied explicitly. Two things must hold:
    (1) the DICT form {"psill": ..., "range": ..., "nugget": ...} -- what
    generate_goldens.py uses throughout -- is a true pass-through with no
    reordering, refitting, or rescaling; and (2) the documented quirk where
    the LIST form [p0, range, nugget] instead treats p0 as *full* sill and
    subtracts nugget is still present in this installed pykrige version (if
    a future pykrige release fixes/changes this, we want this self-test to
    fail loudly rather than have generate_goldens.py silently double-subtract
    the nugget)."""
    psill, rng, nugget = 1.23, 4.56, 0.07
    xs = np.array([0.0, 1.0, 2.0, 3.0, 4.0])
    ys = np.array([0.0, 1.0, 0.0, 1.0, 0.0])
    vs = np.array([1.0, 2.0, 1.5, 2.5, 1.1])

    ok_dict = OrdinaryKriging(
        xs, ys, vs, variogram_model="exponential",
        variogram_parameters={"psill": psill, "range": rng, "nugget": nugget},
        verbose=False, enable_plotting=False,
    )
    got_dict = list(ok_dict.variogram_model_parameters)
    expected = [psill, rng, nugget]
    if any(abs(g - e) > 1e-12 for g, e in zip(got_dict, expected)):
        raise AssertionError(
            f"PyKrige dict-form parameters were altered: got {got_dict}, expected {expected} "
            "(generate_goldens.py relies on the dict form being a pure pass-through)"
        )

    ok_list = OrdinaryKriging(
        xs, ys, vs, variogram_model="exponential",
        variogram_parameters=[psill, rng, nugget], verbose=False, enable_plotting=False,
    )
    got_list = list(ok_list.variogram_model_parameters)
    expected_list_quirk = [psill - nugget, rng, nugget]
    if any(abs(g - e) > 1e-12 for g, e in zip(got_list, expected_list_quirk)):
        raise AssertionError(
            f"The documented list-form full-sill-minus-nugget quirk no longer reproduces: "
            f"got {got_list}, expected {expected_list_quirk}. Re-derive the mapping comment "
            "in the module docstring against the installed pykrige source before trusting goldens."
        )


def assert_universal_linear_exact_on_linear_field_2d() -> None:
    """Universal kriging with a linear drift must reproduce an *exactly linear*
    field to numerical precision at arbitrary query points, independent of the
    variogram used for the residual term. This is a property of the BLUE, and
    it is exactly the kind of gross error an axis-order mistake would break."""
    rng = np.random.default_rng(12345)
    n = 20
    x = rng.uniform(-5, 5, n)
    y = rng.uniform(-5, 5, n)
    a, bx, by = 3.7, 1.3, -0.9
    v = a + bx * x + by * y
    uk = UniversalKriging(
        x, y, v, variogram_model="spherical",
        variogram_parameters={"psill": 1.0, "range": 5.0, "nugget": 0.0},
        drift_terms=["regional_linear"], exact_values=True, verbose=False, enable_plotting=False,
    )
    qx = np.array([1.1, -3.3, 4.9, 0.0])
    qy = np.array([2.2, -1.1, -4.4, 0.0])
    pred, _ = uk.execute("points", qx, qy)
    expected = a + bx * qx + by * qy
    err = np.max(np.abs(np.asarray(pred) - expected))
    if err > 1e-8:
        raise AssertionError(f"UniversalKriging (2D) failed to reproduce a linear field, max err={err:.3e}")


def assert_universal_linear_exact_on_linear_field_3d() -> None:
    """Same property, 3D, and this is the one that actually settles the
    x/y/z column-order question inside uk3d.py's matrix-assembly and
    execute() rather than trusting a source-reading of a truncated snippet."""
    rng = np.random.default_rng(54321)
    n = 25
    x = rng.uniform(-5, 5, n)
    y = rng.uniform(-5, 5, n)
    z = rng.uniform(-5, 5, n)
    a, bx, by, bz = -1.4, 0.8, 1.6, -2.2
    v = a + bx * x + by * y + bz * z
    uk3 = UniversalKriging3D(
        x, y, z, v, variogram_model="spherical",
        variogram_parameters={"psill": 1.0, "range": 6.0, "nugget": 0.0},
        drift_terms=["regional_linear"], exact_values=True, verbose=False,
    )
    qx = np.array([1.0, -2.0, 3.5, 0.0])
    qy = np.array([-1.0, 2.5, -3.5, 0.0])
    qz = np.array([0.5, -0.5, 1.5, 0.0])
    pred, _ = uk3.execute("points", qx, qy, qz)
    expected = a + bx * qx + by * qy + bz * qz
    err = np.max(np.abs(np.asarray(pred) - expected))
    if err > 1e-8:
        raise AssertionError(f"UniversalKriging3D failed to reproduce a linear field, max err={err:.3e} "
                              "(this is the diagnostic for an x/y/z axis-order mistake)")


def assert_ordinary_exact_on_constant_field() -> None:
    """Ordinary kriging must reproduce a constant field exactly (its whole
    purpose is unbiasedness for a constant/unknown mean)."""
    rng = np.random.default_rng(999)
    n = 15
    x = rng.uniform(-5, 5, n)
    y = rng.uniform(-5, 5, n)
    v = np.full(n, 7.25)
    ok = OrdinaryKriging(
        x, y, v, variogram_model="gaussian",
        variogram_parameters={"psill": 1.0, "range": 5.0, "nugget": 0.05},
        exact_values=True, verbose=False, enable_plotting=False,
    )
    pred, _ = ok.execute("points", np.array([0.0, 3.0]), np.array([0.0, -3.0]))
    if np.max(np.abs(np.asarray(pred) - 7.25)) > 1e-8:
        raise AssertionError("OrdinaryKriging failed to reproduce a constant field")


def run_self_tests() -> list[str]:
    tests = [
        assert_parameters_verbatim,
        assert_universal_linear_exact_on_linear_field_2d,
        assert_universal_linear_exact_on_linear_field_3d,
        assert_ordinary_exact_on_constant_field,
    ]
    passed = []
    for test in tests:
        test()
        passed.append(test.__name__)
    return passed


# ---------------------------------------------------------------------------
# Nugget-guard-inertness check (see module docstring, second hazard)
# ---------------------------------------------------------------------------

def assert_nugget_guard_inert(shape: int, nugget: float, sill: float, matern_nu: float = 1.5) -> None:
    sensitive = shape == SHAPE_GAUSSIAN or (shape == SHAPE_MATERN and matern_nu > 2.5)
    if not sensitive:
        return
    total = nugget + sill
    floor = 1.0e-4 * total
    margin = nugget / floor if floor > 0 else math.inf
    if nugget < floor:
        raise AssertionError(
            f"Nugget {nugget} is below the C++ core's Gaussian conditioning floor {floor:.6g} "
            f"(1e-4 * totalSill={total}); this scenario would silently compare against a "
            f"different effective nugget than PyKrige used, masquerading as a range-convention bug."
        )
    if margin < 10.0:
        raise AssertionError(
            f"Nugget {nugget} clears the Gaussian conditioning floor {floor:.6g} by only "
            f"{margin:.1f}x; require >=10x margin so the guard stays inert under minor edits."
        )


# ---------------------------------------------------------------------------
# Scenario definitions
# ---------------------------------------------------------------------------

@dataclass
class Scenario:
    name: str
    method: int  # METHOD_ORDINARY | METHOD_UNIVERSAL_LINEAR
    volumetric: bool  # False => 2D (planar=True in C++), True => 3D
    shape: int
    a_cpp: float  # the C++ core's range parameter
    sill: float
    nugget: float
    nugget_mode: int
    samples: np.ndarray  # columns x, y, z, value
    queries: np.ndarray  # columns x, y, z
    notes: str = ""


def make_samples_2d(seed: int, count: int, trend_a: float, trend_bx: float, trend_by: float,
                     wiggle: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    x = rng.uniform(-6.0, 6.0, count)
    y = rng.uniform(-6.0, 6.0, count)
    v = (trend_a + trend_bx * x + trend_by * y
         + wiggle * (np.sin(0.4 * x) + 0.3 * np.cos(0.5 * y)))
    z = np.zeros(count)
    return np.column_stack((x, y, z, v))


def make_samples_3d(seed: int, count: int, trend_a: float, trend_bx: float, trend_by: float,
                     trend_bz: float, wiggle: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    x = rng.uniform(-6.0, 6.0, count)
    y = rng.uniform(-6.0, 6.0, count)
    z = rng.uniform(-6.0, 6.0, count)
    v = (trend_a + trend_bx * x + trend_by * y + trend_bz * z
         + wiggle * (np.sin(0.35 * x) + 0.3 * np.cos(0.45 * y) + 0.2 * np.sin(0.3 * z)))
    return np.column_stack((x, y, z, v))


def make_queries(samples: np.ndarray, seed: int, volumetric: bool) -> np.ndarray:
    """Interpolation queries (two exact sample coincidences, bit-for-bit
    copies so both PyKrige's |bd|<=eps check and the C++/reference exact
    float-equality check agree on which points coincide) plus a handful of
    interior and moderate-extrapolation points. Every non-coincident query is
    kept at least ~1e-2 away from every sample so no query straddles the
    exact/filtered boundary by float noise."""
    rng = np.random.default_rng(seed)
    interior = rng.uniform(-5.0, 5.0, size=(4, 3))
    extrapolation = rng.uniform(-9.0, 9.0, size=(3, 3))
    random_queries = np.vstack((interior, extrapolation))
    if not volumetric:
        random_queries[:, 2] = 0.0
    return np.vstack((samples[0, :3].copy(), samples[1, :3].copy(), random_queries))


def build_scenarios() -> list[Scenario]:
    result: list[Scenario] = []

    # --- 2D Ordinary kriging, one scenario per built-in shape ---
    s = make_samples_2d(1, 18, trend_a=2.0, trend_bx=0.0, trend_by=0.0, wiggle=1.0)
    result.append(Scenario("ordinary_2d_spherical", METHOD_ORDINARY, False, SHAPE_SPHERICAL,
                            a_cpp=5.0, sill=1.2, nugget=0.05, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 101, False)))

    s = make_samples_2d(2, 18, trend_a=2.0, trend_bx=0.0, trend_by=0.0, wiggle=1.0)
    result.append(Scenario("ordinary_2d_exponential", METHOD_ORDINARY, False, SHAPE_EXPONENTIAL,
                            a_cpp=4.0, sill=1.0, nugget=0.05, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 102, False),
                            notes="Primary range-convention witness: a_cpp=4 maps to PyKrige R=12."))

    s = make_samples_2d(3, 18, trend_a=2.0, trend_bx=0.0, trend_by=0.0, wiggle=1.0)
    result.append(Scenario("ordinary_2d_gaussian", METHOD_ORDINARY, False, SHAPE_GAUSSIAN,
                            a_cpp=4.0, sill=1.0, nugget=0.05, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 103, False),
                            notes="Primary range-convention witness: a_cpp=4 maps to PyKrige R=7."))

    # --- 2D Ordinary kriging, exponential/gaussian with FILTERED nugget mode ---
    s = make_samples_2d(4, 18, trend_a=1.5, trend_bx=0.0, trend_by=0.0, wiggle=0.8)
    result.append(Scenario("ordinary_2d_exponential_filtered", METHOD_ORDINARY, False, SHAPE_EXPONENTIAL,
                            a_cpp=3.0, sill=0.9, nugget=0.08, nugget_mode=NUGGET_FILTERED,
                            samples=s, queries=make_queries(s, 104, False)))

    s = make_samples_2d(5, 18, trend_a=1.5, trend_bx=0.0, trend_by=0.0, wiggle=0.8)
    result.append(Scenario("ordinary_2d_gaussian_filtered", METHOD_ORDINARY, False, SHAPE_GAUSSIAN,
                            a_cpp=3.0, sill=0.9, nugget=0.08, nugget_mode=NUGGET_FILTERED,
                            samples=s, queries=make_queries(s, 105, False)))

    # --- 2D Universal (linear drift) kriging ---
    s = make_samples_2d(6, 20, trend_a=1.0, trend_bx=0.4, trend_by=-0.3, wiggle=0.3)
    result.append(Scenario("universal_linear_2d_spherical", METHOD_UNIVERSAL_LINEAR, False, SHAPE_SPHERICAL,
                            a_cpp=6.0, sill=0.7, nugget=0.03, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 106, False)))

    s = make_samples_2d(7, 20, trend_a=1.0, trend_bx=0.4, trend_by=-0.3, wiggle=0.3)
    result.append(Scenario("universal_linear_2d_exponential", METHOD_UNIVERSAL_LINEAR, False, SHAPE_EXPONENTIAL,
                            a_cpp=5.0, sill=0.6, nugget=0.03, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 107, False)))

    # --- 3D Ordinary kriging, one scenario per built-in shape ---
    s = make_samples_3d(8, 22, trend_a=2.0, trend_bx=0.0, trend_by=0.0, trend_bz=0.0, wiggle=1.0)
    result.append(Scenario("ordinary_3d_spherical", METHOD_ORDINARY, True, SHAPE_SPHERICAL,
                            a_cpp=6.0, sill=1.1, nugget=0.04, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 108, True)))

    s = make_samples_3d(9, 22, trend_a=2.0, trend_bx=0.0, trend_by=0.0, trend_bz=0.0, wiggle=1.0)
    result.append(Scenario("ordinary_3d_exponential", METHOD_ORDINARY, True, SHAPE_EXPONENTIAL,
                            a_cpp=4.5, sill=1.0, nugget=0.05, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 109, True),
                            notes="Primary range-convention witness: a_cpp=4.5 maps to PyKrige R=13.5."))

    s = make_samples_3d(10, 22, trend_a=2.0, trend_bx=0.0, trend_by=0.0, trend_bz=0.0, wiggle=1.0)
    result.append(Scenario("ordinary_3d_gaussian", METHOD_ORDINARY, True, SHAPE_GAUSSIAN,
                            a_cpp=4.5, sill=1.0, nugget=0.05, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 110, True),
                            notes="Primary range-convention witness: a_cpp=4.5 maps to PyKrige R=7.875."))

    # --- 3D Universal (linear drift) kriging ---
    s = make_samples_3d(11, 26, trend_a=0.5, trend_bx=0.3, trend_by=-0.2, trend_bz=0.25, wiggle=0.4)
    result.append(Scenario("universal_linear_3d_gaussian", METHOD_UNIVERSAL_LINEAR, True, SHAPE_GAUSSIAN,
                            a_cpp=7.0, sill=0.8, nugget=0.05, nugget_mode=NUGGET_EXACT,
                            samples=s, queries=make_queries(s, 111, True)))

    for sc in result:
        assert_nugget_guard_inert(sc.shape, sc.nugget, sc.sill)
        assert len(sc.samples) >= 15, f"{sc.name}: keep >=15 samples so PyKrige's lag binning stays quiet"

    return result


# ---------------------------------------------------------------------------
# PyKrige execution
# ---------------------------------------------------------------------------

def run_pykrige(scenario: Scenario, r_pykrige: float) -> list[dict[str, float]]:
    x, y, z, v = (scenario.samples[:, i] for i in range(4))
    qx, qy, qz = (scenario.queries[:, i] for i in range(3))
    model_name = pykrige_model_name(scenario.shape)
    # Dict form: a verified pure pass-through (see assert_parameters_verbatim
    # and the module docstring's sill-convention hazard). The list form would
    # silently subtract nugget from index 0 -- avoided entirely.
    params = {"psill": scenario.sill, "range": r_pykrige, "nugget": scenario.nugget}
    expected_list = [scenario.sill, r_pykrige, scenario.nugget]
    exact_values = scenario.nugget_mode == NUGGET_EXACT

    if not scenario.volumetric:
        if scenario.method == METHOD_ORDINARY:
            k = OrdinaryKriging(x, y, v, variogram_model=model_name, variogram_parameters=params,
                                 exact_values=exact_values, verbose=False, enable_plotting=False)
        elif scenario.method == METHOD_UNIVERSAL_LINEAR:
            k = UniversalKriging(x, y, v, variogram_model=model_name, variogram_parameters=params,
                                  drift_terms=["regional_linear"], exact_values=exact_values,
                                  verbose=False, enable_plotting=False)
        else:
            raise ValueError(f"Unsupported method for 2D: {scenario.method}")
        got = list(k.variogram_model_parameters)
        if any(abs(a - b) > 1e-12 for a, b in zip(got, expected_list)):
            raise AssertionError(f"{scenario.name}: PyKrige altered supplied parameters: {got} != {expected_list}")
        pred, var = k.execute("points", qx, qy)
    else:
        if scenario.method == METHOD_ORDINARY:
            k = OrdinaryKriging3D(x, y, z, v, variogram_model=model_name, variogram_parameters=params,
                                   exact_values=exact_values, verbose=False)
        elif scenario.method == METHOD_UNIVERSAL_LINEAR:
            k = UniversalKriging3D(x, y, z, v, variogram_model=model_name, variogram_parameters=params,
                                    drift_terms=["regional_linear"], exact_values=exact_values, verbose=False)
        else:
            raise ValueError(f"Unsupported method for 3D: {scenario.method}")
        got = list(k.variogram_model_parameters)
        if any(abs(a - b) > 1e-12 for a, b in zip(got, expected_list)):
            raise AssertionError(f"{scenario.name}: PyKrige altered supplied parameters: {got} != {expected_list}")
        pred, var = k.execute("points", qx, qy, qz)

    return [{"value": float(p), "variance": float(vv)} for p, vv in zip(pred, var)]


def compute_sensitivity(scenario: Scenario) -> dict[str, Any] | None:
    """For the primary exponential/gaussian range-convention witnesses,
    record what PyKrige gives under the naive (wrong) mapping R_pykrige =
    a_cpp, i.e. assuming both libraries share a convention. This is the
    concrete evidence that the mapping in cpp_range_to_pykrige_range is not
    an arbitrary choice that happens to pass -- the naive mapping visibly
    disagrees with the correct one on the same scenario."""
    if scenario.shape not in (SHAPE_EXPONENTIAL, SHAPE_GAUSSIAN):
        return None
    correct = run_pykrige(scenario, cpp_range_to_pykrige_range(scenario.shape, scenario.a_cpp))
    naive = run_pykrige(scenario, scenario.a_cpp)  # WRONG: treats a_cpp as if it were R_pykrige
    max_value_delta = max(abs(a["value"] - b["value"]) for a, b in zip(correct, naive))
    max_variance_delta = max(abs(a["variance"] - b["variance"]) for a, b in zip(correct, naive))
    return {
        "naive_mapping_used": "R_pykrige = a_cpp (assumes shared convention; WRONG for this shape)",
        "correct_mapping_used": f"R_pykrige = {cpp_range_to_pykrige_range(scenario.shape, 1.0):.6g} * a_cpp",
        "max_abs_value_delta_between_mappings": max_value_delta,
        "max_abs_variance_delta_between_mappings": max_variance_delta,
        "interpretation": (
            "A large delta here shows the naive same-convention assumption is "
            "detectably wrong on this exact scenario; the golden values recorded "
            "for this scenario use the correct mapping only."
        ),
    }


# ---------------------------------------------------------------------------
# Wire protocol for golden_probe.cpp (mirrors reference_probe.cpp's tags)
# ---------------------------------------------------------------------------

def scenario_protocol_line(scenario: Scenario) -> list[str]:
    lines = []
    lines.append(
        f"M {scenario.method} {0 if scenario.volumetric else 1} 0.0 "
        f"{scenario.nugget:.17g} {scenario.nugget_mode} 1 "
        f"{len(scenario.samples)} {len(scenario.queries)}"
    )
    lines.append(
        "T " + " ".join(format(v, ".17g") for v in [
            scenario.shape, scenario.a_cpp, scenario.sill,
            1.5, 1.0,  # maternNu, powerAlpha: unused by spherical/exponential/gaussian
            0.0, 0.0, 0.0, 1.0, 1.0,  # isotropic: azimuth, dip, plunge, ratioY, ratioZ
        ])
    )
    for row in scenario.samples:
        x, y, z, v = row
        lines.append(f"P {x:.17g} {y:.17g} {z:.17g} {v:.17g} 0.0 0")
    for row in scenario.queries:
        x, y, z = row
        lines.append(f"Q {x:.17g} {y:.17g} {z:.17g}")
    return lines


def build_protocol(scenarios: list[Scenario]) -> str:
    lines: list[str] = []
    for scenario in scenarios:
        lines.extend(scenario_protocol_line(scenario))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    self_test_names = run_self_tests()

    scenarios = build_scenarios()
    scenario_records = []
    sensitivity = {}
    for scenario in scenarios:
        r_pk = cpp_range_to_pykrige_range(scenario.shape, scenario.a_cpp)
        results = run_pykrige(scenario, r_pk)
        record = {
            "name": scenario.name,
            "method": scenario.method,
            "volumetric": scenario.volumetric,
            "shape": scenario.shape,
            "shape_name": SHAPE_NAME[scenario.shape],
            "a_cpp": scenario.a_cpp,
            "r_pykrige": r_pk,
            "sill": scenario.sill,
            "nugget": scenario.nugget,
            "nugget_mode": scenario.nugget_mode,
            "samples": scenario.samples.tolist(),
            "queries": scenario.queries.tolist(),
            "pykrige_results": results,
            "notes": scenario.notes,
        }
        scenario_records.append(record)
        sens = compute_sensitivity(scenario)
        if sens is not None:
            sensitivity[scenario.name] = sens

    goldens = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "generator": "generate_goldens.py",
        "purpose": (
            "Cross-validate the C++ kriging core's variogram range convention "
            "against PyKrige's independent, differently-conventioned built-in "
            "spherical/exponential/gaussian models."
        ),
        "versions": {
            "python": sys.version.split()[0],
            "platform": platform.platform(),
            "numpy": np.__version__,
            "scipy": scipy.__version__,
            "pykrige": pykrige.__version__,
        },
        "range_convention_mapping": {
            "spherical": "R_pykrige = a_cpp",
            "exponential": "R_pykrige = 3 * a_cpp",
            "gaussian": "R_pykrige = (7/4) * a_cpp",
            "source": (
                "Derived from pykrige/variogram_models.py (this venv's installed copy) "
                "and Source/KrigingCore/Private/Portable/KrigePortableCore.cpp "
                "EvaluateNormalizedStructure(); see module docstring of generate_goldens.py."
            ),
        },
        "self_tests_passed": self_test_names,
        "sensitivity": sensitivity,
        "scenarios": scenario_records,
        "wire_protocol": build_protocol(scenarios),
    }

    out_path = Path(__file__).resolve().parents[1] / "Golden" / "goldens.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(goldens, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print(f"Wrote {out_path} with {len(scenario_records)} scenarios.")
    print(f"Self-tests passed: {', '.join(self_test_names)}")
    for name, sens in sensitivity.items():
        print(f"Sensitivity[{name}]: naive-vs-correct max value delta = "
              f"{sens['max_abs_value_delta_between_mappings']:.6g}, "
              f"max variance delta = {sens['max_abs_variance_delta_between_mappings']:.6g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
