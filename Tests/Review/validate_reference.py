#!/usr/bin/env python3
"""Independent NumPy/SciPy kriging assembly versus the shipped C++ model."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Any

import numpy as np
import scipy
from scipy import special

ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "Source/KrigingCore/Public"
PRIVATE = ROOT / "Source/KrigingCore/Private/Portable"
PRODUCTION = PRIVATE / "KrigePortableCore.cpp"
PROBE = Path(__file__).with_name("reference_probe.cpp")

SIMPLE = 0
ORDINARY = 1
UNIVERSAL_LINEAR = 2
UNIVERSAL_QUADRATIC = 3
EXTERNAL_DRIFT = 4
SPHERICAL = 0
EXPONENTIAL = 1
GAUSSIAN = 2
MATERN = 3
POWER = 4
EXACT = 0
FILTERED = 1


@dataclass
class Structure:
    shape: int
    range: float
    sill: float
    nu: float = 1.5
    alpha: float = 1.0
    azimuth: float = 0.0
    dip: float = 0.0
    plunge: float = 0.0
    ratio_y: float = 1.0
    ratio_z: float = 1.0


@dataclass
class Scenario:
    name: str
    method: int
    planar: bool
    known_mean: float
    nugget: float
    nugget_mode: int
    structures: list[Structure]
    samples: np.ndarray  # columns x,y,z,value,variance,original_index
    queries: np.ndarray


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_probe(compiler: str, output: Path) -> list[str]:
    command = [
        compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-I", str(PUBLIC), "-I", str(PRIVATE),
        str(PROBE), str(PRODUCTION), "-o", str(output),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    return command


def rotation_x(angle: float) -> np.ndarray:
    c, s = math.cos(angle), math.sin(angle)
    return np.array([[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]])


def rotation_y(angle: float) -> np.ndarray:
    c, s = math.cos(angle), math.sin(angle)
    return np.array([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])


def rotation_z(angle: float) -> np.ndarray:
    c, s = math.cos(angle), math.sin(angle)
    return np.array([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])


def anisotropic_distance(delta: np.ndarray, structure: Structure, planar: bool) -> float:
    if structure.ratio_y == 1.0 and (planar or structure.ratio_z == 1.0):
        return float(np.linalg.norm(delta[:2] if planar else delta))
    az = -math.radians(structure.azimuth)
    dip = -math.radians(structure.dip)
    plunge = -math.radians(structure.plunge)
    rotation = rotation_z(az) if planar else rotation_z(az) @ rotation_y(dip) @ rotation_x(plunge)
    transformed = rotation @ delta
    if planar:
        return math.hypot(transformed[0], transformed[1] / structure.ratio_y)
    return math.sqrt(transformed[0] ** 2
                     + (transformed[1] / structure.ratio_y) ** 2
                     + (transformed[2] / structure.ratio_z) ** 2)


def g(structure: Structure, distance: float) -> float:
    ratio = max(0.0, distance / structure.range)
    if structure.shape == SPHERICAL:
        return 1.0 if ratio >= 1.0 else 1.5 * ratio - 0.5 * ratio**3
    if structure.shape == EXPONENTIAL:
        return -math.expm1(-ratio)
    if structure.shape == GAUSSIAN:
        return -math.expm1(-(ratio**2))
    if structure.shape == POWER:
        return ratio**structure.alpha
    if ratio == 0.0:
        return 0.0
    if structure.nu == 0.5:
        return -math.expm1(-ratio)
    x = math.sqrt(2.0 * structure.nu) * ratio
    if x >= 60.0:
        return 1.0
    correlation = (2.0 ** (1.0 - structure.nu) / special.gamma(structure.nu)) \
        * x**structure.nu * special.kv(structure.nu, x)
    return float(np.clip(1.0 - correlation, 0.0, 1.0))


def coincident(a: np.ndarray, b: np.ndarray, planar: bool) -> bool:
    return bool(a[0] == b[0] and a[1] == b[1] and (planar or a[2] == b[2]))


def prepared_nugget_and_sill(scenario: Scenario) -> tuple[float, float]:
    nugget = scenario.nugget
    total = nugget + sum(s.sill for s in scenario.structures if s.shape != POWER)
    sensitive = any(s.shape == GAUSSIAN or (s.shape == MATERN and s.nu > 2.5)
                    for s in scenario.structures)
    if sensitive and total > 0.0 and nugget < 1.0e-4 * total:
        raised = 1.0e-4 * total
        total += raised - nugget
        nugget = raised
    return nugget, total


def covariance(a: np.ndarray, b: np.ndarray, scenario: Scenario,
               nugget: float, include_nugget_at_zero: bool) -> float:
    value = 0.0
    delta = a - b
    for structure in scenario.structures:
        if structure.shape != POWER:
            value += structure.sill * (1.0 - g(
                structure, anisotropic_distance(delta, structure, scenario.planar)))
    if include_nugget_at_zero and coincident(a, b, scenario.planar):
        value += nugget
    return value


def semivariogram(a: np.ndarray, b: np.ndarray, scenario: Scenario,
                  nugget: float, include_nugget: bool) -> float:
    if coincident(a, b, scenario.planar):
        return 0.0
    value = nugget if include_nugget else 0.0
    delta = a - b
    for structure in scenario.structures:
        value += structure.sill * g(
            structure, anisotropic_distance(delta, structure, scenario.planar))
    return value


def drift_state(samples: np.ndarray, scenario: Scenario) -> dict[str, Any]:
    positions = samples[:, :3]
    minimum = positions.min(axis=0)
    maximum = positions.max(axis=0)
    center = 0.5 * (minimum + maximum)
    scale = np.maximum(1.0, 0.5 * (maximum - minimum))
    if scenario.planar:
        center[2] = 0.0
        scale[2] = 1.0
    state: dict[str, Any] = {"center": center, "scale": scale}
    if scenario.method == EXTERNAL_DRIFT:
        values = 0.35 * positions[:, 0] - 0.21 * positions[:, 1] + 0.13 * positions[:, 2]
        state["external_center"] = float(values.mean())
        state["external_scale"] = float(np.max(np.abs(values - values.mean())))
    return state


def basis(point: np.ndarray, scenario: Scenario, state: dict[str, Any]) -> np.ndarray:
    normalized = (point - state["center"]) / state["scale"]
    if scenario.planar:
        normalized[2] = 0.0
    x, y, z = normalized
    if scenario.method == SIMPLE:
        return np.empty(0)
    if scenario.method == ORDINARY:
        return np.array([1.0])
    if scenario.method == UNIVERSAL_LINEAR:
        return np.array([1.0, x, y]) if scenario.planar else np.array([1.0, x, y, z])
    if scenario.method == UNIVERSAL_QUADRATIC:
        if scenario.planar:
            return np.array([1.0, x, y, x*x, x*y, y*y])
        return np.array([1.0, x, y, z, x*x, x*y, x*z, y*y, y*z, z*z])
    external = 0.35 * point[0] - 0.21 * point[1] + 0.13 * point[2]
    return np.array([1.0, (external - state["external_center"]) / state["external_scale"]])


def reference_results(scenario: Scenario) -> list[tuple[float, float]]:
    order = np.lexsort((scenario.samples[:, 5], scenario.samples[:, 2],
                        scenario.samples[:, 1], scenario.samples[:, 0]))
    samples = scenario.samples[order].copy()
    if scenario.planar:
        samples[:, 2] = 0.0
    positions = samples[:, :3]
    values = samples[:, 3]
    measurement_variances = samples[:, 4]
    state = drift_state(samples, scenario)
    nugget, total_sill = prepared_nugget_and_sill(scenario)
    has_power = any(s.shape == POWER for s in scenario.structures)
    p = len(basis(positions[0].copy(), scenario, state))
    n = len(samples)
    matrix = np.zeros((n + p, n + p), dtype=np.float64)
    for i in range(n):
        for j in range(i, n):
            if has_power:
                filtered = scenario.nugget_mode == FILTERED
                value = -semivariogram(positions[i], positions[j], scenario,
                                       nugget, not filtered)
            else:
                value = covariance(positions[i], positions[j], scenario,
                                   nugget, True)
            matrix[i, j] = matrix[j, i] = value
        if has_power and scenario.nugget_mode == FILTERED:
            matrix[i, i] += nugget
        matrix[i, i] += measurement_variances[i]
        if p:
            row = basis(positions[i].copy(), scenario, state)
            matrix[i, n:] = row
            matrix[n:, i] = row
    extended = np.zeros(n + p)
    extended[:n] = values - scenario.known_mean if scenario.method == SIMPLE else values
    dual = np.linalg.solve(matrix, extended)

    output: list[tuple[float, float]] = []
    for query_source in scenario.queries:
        query = query_source.copy()
        if scenario.planar:
            query[2] = 0.0
        rhs = np.zeros(n + p)
        for i in range(n):
            if has_power:
                filtered = scenario.nugget_mode == FILTERED
                rhs[i] = -semivariogram(query, positions[i], scenario,
                                        nugget, not filtered)
            else:
                rhs[i] = covariance(query, positions[i], scenario, nugget,
                                    scenario.nugget_mode == EXACT)
        if p:
            rhs[n:] = basis(query, scenario, state)
        estimate = float(rhs @ dual)
        if scenario.method == SIMPLE:
            estimate += scenario.known_mean
        weights = np.linalg.solve(matrix, rhs)
        product = float(rhs @ weights)
        variance = -product if has_power else total_sill - product
        output.append((estimate, max(0.0, variance)))
    return output


def make_samples(count: int, seed: int, volumetric: bool,
                 trend: str = "mixed", measurement: bool = False) -> np.ndarray:
    rng = np.random.default_rng(seed)
    positions = rng.uniform(-7.0, 7.0, size=(count, 3))
    if not volumetric:
        positions[:, 2] = 0.0
    x, y, z = positions.T
    if trend == "linear":
        values = 2.0 + 0.4*x - 0.2*y + (0.15*z if volumetric else 0.0)
    elif trend == "quadratic":
        values = 1.0 + 0.2*x - 0.1*y + 0.03*x*x - 0.02*x*y + 0.04*y*y
        if volumetric:
            values += 0.08*z + 0.01*x*z - 0.015*y*z + 0.025*z*z
    else:
        values = np.sin(0.35*x) + 0.25*np.cos(0.41*y) + 0.07*z + 0.015*np.arange(count)
    variances = (0.002 + 0.0002*np.arange(count)) if measurement else np.zeros(count)
    originals = np.arange(count, dtype=float)
    return np.column_stack((positions, values, variances, originals))


def make_queries(samples: np.ndarray, seed: int, volumetric: bool) -> np.ndarray:
    rng = np.random.default_rng(seed)
    random_queries = rng.uniform(-8.0, 8.0, size=(7, 3))
    if not volumetric:
        random_queries[:, 2] = 0.0
    return np.vstack((samples[0, :3], samples[1, :3], random_queries))


def scenarios() -> list[Scenario]:
    result: list[Scenario] = []
    samples = make_samples(14, 1, False)
    result.append(Scenario("simple_spherical_exact", SIMPLE, True, 0.35, 0.03, EXACT,
        [Structure(SPHERICAL, 5.0, 1.1)], samples, make_queries(samples, 101, False)))

    samples = make_samples(17, 2, False, measurement=True)
    result.append(Scenario("ordinary_exponential_filtered_measurement", ORDINARY, True, 0.0, 0.08, FILTERED,
        [Structure(EXPONENTIAL, 6.5, 0.9, azimuth=31.0, ratio_y=0.7)],
        samples, make_queries(samples, 102, False)))

    samples = make_samples(20, 3, False, trend="linear")
    result.append(Scenario("universal_linear_nested_anisotropic", UNIVERSAL_LINEAR, True, 0.0, 0.02, EXACT,
        [Structure(SPHERICAL, 4.0, 0.55, azimuth=20.0, ratio_y=0.6),
         Structure(GAUSSIAN, 9.0, 0.45, azimuth=-15.0, ratio_y=0.8)],
        samples, make_queries(samples, 103, False)))

    samples = make_samples(25, 4, False, trend="quadratic")
    result.append(Scenario("universal_quadratic_matern", UNIVERSAL_QUADRATIC, True, 0.0, 0.04, EXACT,
        [Structure(MATERN, 7.0, 1.0, nu=1.5, azimuth=42.0, ratio_y=0.65)],
        samples, make_queries(samples, 104, False)))

    samples = make_samples(22, 5, True)
    result.append(Scenario("external_drift_3d_nested", EXTERNAL_DRIFT, False, 0.0, 0.03, EXACT,
        [Structure(EXPONENTIAL, 7.5, 0.6, azimuth=15.0, dip=-12.0, plunge=8.0,
                   ratio_y=0.75, ratio_z=0.55),
         Structure(MATERN, 12.0, 0.4, nu=0.8, azimuth=-25.0, dip=7.0, plunge=-11.0,
                   ratio_y=0.9, ratio_z=0.7)],
        samples, make_queries(samples, 105, True)))

    samples = make_samples(20, 6, True, trend="linear")
    result.append(Scenario("universal_linear_3d_gaussian", UNIVERSAL_LINEAR, False, 0.0, 0.05, FILTERED,
        [Structure(GAUSSIAN, 10.0, 1.0, azimuth=35.0, dip=14.0, plunge=-23.0,
                   ratio_y=0.7, ratio_z=0.45)],
        samples, make_queries(samples, 106, True)))

    for mode, name in ((EXACT, "exact"), (FILTERED, "filtered")):
        samples = make_samples(16, 70 + mode, False)
        result.append(Scenario(f"ordinary_power_{name}", ORDINARY, True, 0.0, 0.06, mode,
            [Structure(POWER, 5.5, 0.8, alpha=1.35, azimuth=27.0, ratio_y=0.72)],
            samples, make_queries(samples, 170 + mode, False)))

    samples = make_samples(34, 8, True, trend="quadratic")
    result.append(Scenario("universal_quadratic_3d", UNIVERSAL_QUADRATIC, False, 0.0, 0.04, EXACT,
        [Structure(SPHERICAL, 8.0, 0.5, azimuth=12.0, dip=18.0, plunge=6.0,
                   ratio_y=0.8, ratio_z=0.6),
         Structure(EXPONENTIAL, 15.0, 0.5)],
        samples, make_queries(samples, 108, True)))
    return result


def protocol(all_scenarios: list[Scenario]) -> str:
    lines: list[str] = []
    for scenario in all_scenarios:
        lines.append(
            f"M {scenario.method} {1 if scenario.planar else 0} "
            f"{scenario.known_mean:.17g} {scenario.nugget:.17g} "
            f"{scenario.nugget_mode} {len(scenario.structures)} "
            f"{len(scenario.samples)} {len(scenario.queries)}"
        )
        for structure in scenario.structures:
            lines.append(
                "T " + " ".join(format(value, ".17g") for value in [
                    structure.shape, structure.range, structure.sill,
                    structure.nu, structure.alpha, structure.azimuth,
                    structure.dip, structure.plunge, structure.ratio_y,
                    structure.ratio_z,
                ])
            )
        for sample in scenario.samples:
            lines.append("P " + " ".join(format(float(value), ".17g") for value in sample))
        for query in scenario.queries:
            lines.append("Q " + " ".join(format(float(value), ".17g") for value in query))
    return "\n".join(lines) + "\n"


def parse_output(text: str, all_scenarios: list[Scenario]) -> tuple[list[list[tuple[float, float]]], list[dict[str, Any]]]:
    lines = iter(line for line in text.splitlines() if line.strip())
    results: list[list[tuple[float, float]]] = []
    builds: list[dict[str, Any]] = []
    for scenario in all_scenarios:
        first = next(lines, None)
        if first is None:
            raise AssertionError(f"Missing build line for {scenario.name}")
        if first.startswith("E "):
            raise AssertionError(f"C++ build failed for {scenario.name}: {first[2:]}")
        parts = first.split()
        if len(parts) != 3 or parts[0] != "B":
            raise AssertionError(f"Malformed build line for {scenario.name}: {first}")
        ridge = float(parts[1])
        degraded = int(parts[2]) != 0
        builds.append({"name": scenario.name, "ridge": ridge, "degraded": degraded})
        if ridge != 0.0 or degraded:
            raise AssertionError(f"Reference scenario {scenario.name} unexpectedly used ridge/degradation")
        scenario_results: list[tuple[float, float]] = []
        for _ in scenario.queries:
            line = next(lines, None)
            if line is None or line.startswith("E "):
                raise AssertionError(f"C++ query failed for {scenario.name}: {line}")
            parts = line.split()
            if len(parts) != 3 or parts[0] != "R":
                raise AssertionError(f"Malformed result line for {scenario.name}: {line}")
            scenario_results.append((float(parts[1]), float(parts[2])))
        results.append(scenario_results)
    remaining = list(lines)
    if remaining:
        raise AssertionError(f"Unexpected trailing probe output: {remaining[:3]}")
    return results, builds


def scaled_error(actual: float, expected: float) -> float:
    return abs(actual - expected) / max(1.0, abs(actual), abs(expected))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    all_scenarios = scenarios()
    expected = [reference_results(scenario) for scenario in all_scenarios]
    with tempfile.TemporaryDirectory(prefix="kriging_reference_") as directory:
        executable = Path(directory) / "reference_probe"
        compile_command = compile_probe(args.compiler, executable)
        completed = subprocess.run(
            [str(executable)], input=protocol(all_scenarios), text=True,
            capture_output=True, check=True, cwd=ROOT,
        )
        actual, builds = parse_output(completed.stdout, all_scenarios)

    maximum_value_error = 0.0
    maximum_variance_error = 0.0
    worst_value = ""
    worst_variance = ""
    for scenario, actual_rows, expected_rows in zip(all_scenarios, actual, expected):
        for index, ((actual_value, actual_variance),
                    (expected_value, expected_variance)) in enumerate(zip(actual_rows, expected_rows)):
            value_error = scaled_error(actual_value, expected_value)
            variance_error = scaled_error(actual_variance, expected_variance)
            if value_error > maximum_value_error:
                maximum_value_error = value_error
                worst_value = f"{scenario.name}[{index}]"
            if variance_error > maximum_variance_error:
                maximum_variance_error = variance_error
                worst_variance = f"{scenario.name}[{index}]"
    tolerance = 2.0e-9
    if maximum_value_error > tolerance or maximum_variance_error > tolerance:
        raise AssertionError(
            f"Independent reference mismatch: value={maximum_value_error:.3e} at {worst_value}, "
            f"variance={maximum_variance_error:.3e} at {worst_variance}"
        )

    report = {
        "status": "PASS",
        "compiler": args.compiler,
        "compile_command": compile_command,
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "numpy": np.__version__,
        "scipy": scipy.__version__,
        "scenario_count": len(all_scenarios),
        "query_count": sum(len(s.queries) for s in all_scenarios),
        "builds": builds,
        "metrics": {
            "maximum_scaled_value_error": maximum_value_error,
            "worst_value_case": worst_value,
            "maximum_scaled_variance_error": maximum_variance_error,
            "worst_variance_case": worst_variance,
            "tolerance": tolerance,
        },
        "source_sha256": {
            str(PRODUCTION.relative_to(ROOT)): sha256(PRODUCTION),
            str((PUBLIC / "KrigePortableCore.h").relative_to(ROOT)): sha256(PUBLIC / "KrigePortableCore.h"),
            str(PROBE.relative_to(ROOT)): sha256(PROBE),
            str(Path(__file__).relative_to(ROOT)): sha256(Path(__file__)),
        },
        "notes": [
            "NumPy independently assembles and solves every augmented matrix.",
            "The C++ probe parses model data and calls the shipped Model::Build and EvaluateWithVariance paths.",
            "Scenarios cover all kriging drift methods, bounded structures, Matérn, power exact/filtered branches, anisotropy, 2D/3D, and measurement variance.",
        ],
    }
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8")
    else:
        print(serialized, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
