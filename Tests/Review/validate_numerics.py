#!/usr/bin/env python3
"""Compile and test the shipped C++ numerical implementation.

Unlike the discarded validator, this script does not restate a lookup design in
Python. It compiles KrigePortableCore.cpp and drives the exact exported structure
function, AS241 implementation, and Bessel kernel used by the plugin module.
"""

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
from typing import Iterable

import numpy as np
import scipy
from scipy import special

ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "Source/KrigingCore/Public"
PRIVATE = ROOT / "Source/KrigingCore/Private/Portable"
PRODUCTION = PRIVATE / "KrigePortableCore.cpp"
PROBE = Path(__file__).with_name("numeric_probe.cpp")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def compile_probe(compiler: str, output: Path) -> list[str]:
    command = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-I",
        str(PUBLIC),
        "-I",
        str(PRIVATE),
        str(PROBE),
        str(PRODUCTION),
        "-o",
        str(output),
    ]
    subprocess.run(command, check=True, cwd=ROOT)
    return command


def run_probe(executable: Path, commands: Iterable[str]) -> list[float]:
    lines = list(commands)
    completed = subprocess.run(
        [str(executable)],
        input="\n".join(lines) + "\n",
        text=True,
        capture_output=True,
        check=True,
        cwd=ROOT,
    )
    values = [float(line) for line in completed.stdout.splitlines() if line.strip()]
    if len(values) != len(lines):
        raise RuntimeError(
            f"Probe returned {len(values)} values for {len(lines)} commands; "
            f"stderr={completed.stderr!r}"
        )
    return values


def relative_error(actual: float, expected: float) -> float:
    return abs(actual - expected) / max(abs(expected), np.finfo(float).tiny)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kriging_numeric_") as directory:
        executable = Path(directory) / "numeric_probe"
        compile_command = compile_probe(args.compiler, executable)

        nus = [0.1, 0.5, 0.99999, 1.0, 1.00001, 1.5, 2.5, 5.0, 10.0]
        xs = [1.0e-8, 1.0e-6, 1.0e-3, 0.1, 1.0,
              1.999999, 2.0, 2.000001, 5.0, 20.0, 50.0]
        bessel_cases = [(nu, x) for nu in nus for x in xs]
        bessel_actual = run_probe(
            executable, (f"B {nu:.17g} {x:.17g}" for nu, x in bessel_cases)
        )
        bessel_errors = []
        for actual, (nu, x) in zip(bessel_actual, bessel_cases):
            expected = float(special.kv(nu, x))
            if not math.isfinite(actual) or not math.isfinite(expected):
                raise AssertionError(f"Non-finite Bessel result at nu={nu}, x={x}")
            bessel_errors.append(relative_error(actual, expected))
        max_bessel_relative_error = max(bessel_errors)
        if max_bessel_relative_error > 2.0e-12:
            raise AssertionError(
                f"Bessel relative error {max_bessel_relative_error:.3e} exceeds 2e-12"
            )

        ratios = np.unique(np.concatenate((
            np.array([0.0, 1.0e-12, 1.0e-9, 1.0e-6, 0.01, 0.1, 0.999999,
                      1.0, 1.000001, 2.0, 5.0, 20.0, 100.0]),
            np.logspace(-8, 2, 240),
        )))
        structure_cases: list[tuple[int, float, float, float]] = []
        # Enum order: spherical, exponential, Gaussian, Matern, power.
        for ratio in ratios:
            structure_cases.extend([
                (0, float(ratio), 1.5, 1.0),
                (1, float(ratio), 1.5, 1.0),
                (2, float(ratio), 1.5, 1.0),
                (4, float(ratio), 1.5, 0.37),
                (4, float(ratio), 1.5, 1.91),
            ])
            for nu in nus:
                structure_cases.append((3, float(ratio), nu, 1.0))
        structure_actual = run_probe(
            executable,
            (f"S {shape} {ratio:.17g} {nu:.17g} {alpha:.17g}"
             for shape, ratio, nu, alpha in structure_cases),
        )

        def expected_structure(shape: int, ratio: float, nu: float, alpha: float) -> float:
            ratio = max(0.0, ratio)
            if shape == 0:
                return 1.0 if ratio >= 1.0 else 1.5 * ratio - 0.5 * ratio**3
            if shape == 1:
                return -math.expm1(-ratio)
            if shape == 2:
                return -math.expm1(-(ratio**2))
            if shape == 4:
                return ratio**alpha
            if ratio == 0.0:
                return 0.0
            x = math.sqrt(2.0 * nu) * ratio
            correlation = (2.0 ** (1.0 - nu) / special.gamma(nu)) * (x**nu) * special.kv(nu, x)
            if not math.isfinite(float(correlation)):
                correlation = 0.0
            return min(1.0, max(0.0, 1.0 - float(correlation)))

        structure_errors = []
        for actual, case in zip(structure_actual, structure_cases):
            expected = expected_structure(*case)
            if not math.isfinite(actual):
                raise AssertionError(f"Non-finite structure result for {case}")
            structure_errors.append(abs(actual - expected))
        max_structure_absolute_error = max(structure_errors)
        if max_structure_absolute_error > 2.0e-11:
            raise AssertionError(
                f"Structure absolute error {max_structure_absolute_error:.3e} exceeds 2e-11"
            )

        probabilities = np.unique(np.concatenate((
            np.array([1.0e-15, 1.0e-12, 1.0e-9, 1.0e-6, 0.001, 0.01,
                      0.1, 0.5, 0.9, 0.99, 0.999, 1.0 - 1.0e-9,
                      1.0 - 1.0e-12, 1.0 - 1.0e-15]),
            np.linspace(1.0e-8, 1.0 - 1.0e-8, 1001),
        )))
        normal_actual = run_probe(
            executable, (f"N {probability:.17g}" for probability in probabilities)
        )
        normal_errors = [
            abs(actual - float(special.ndtri(probability)))
            for actual, probability in zip(normal_actual, probabilities)
        ]
        max_normal_absolute_error = max(normal_errors)
        if max_normal_absolute_error > 3.0e-9:
            raise AssertionError(
                f"AS241 absolute error {max_normal_absolute_error:.3e} exceeds 3e-9"
            )

        ratio = 0.8
        convention_values = run_probe(executable, [
            f"S 3 {ratio} 9.99 1",
            f"S 3 {ratio} 10 1",
            f"S 2 {ratio} 1.5 1",
            f"S 2 {ratio / math.sqrt(2.0):.17g} 1.5 1",
            f"S 3 {ratio} 0.5 1",
            f"S 1 {ratio} 1.5 1",
        ])
        matern_below, matern_at, gaussian_same, gaussian_rescaled, matern_half, exponential = convention_values
        discontinuity = abs(matern_below - matern_at)
        same_range_mismatch = abs(matern_at - gaussian_same)
        rescaled_range_mismatch = abs(matern_at - gaussian_rescaled)
        if discontinuity > 2.0e-3:
            raise AssertionError("Matérn has an artificial discontinuity near nu=10")
        if not rescaled_range_mismatch < same_range_mismatch:
            raise AssertionError("Matérn/Gaussian sqrt(2) range convention check failed")
        if abs(matern_half - exponential) > 1.0e-14:
            raise AssertionError("Matérn nu=0.5 does not match exponential exactly")

        report = {
            "status": "PASS",
            "compiler": args.compiler,
            "compile_command": compile_command,
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "numpy": np.__version__,
            "scipy": scipy.__version__,
            "source_sha256": {
                str(PRODUCTION.relative_to(ROOT)): sha256(PRODUCTION),
                str((PRIVATE / "KrigeBessel.h").relative_to(ROOT)): sha256(PRIVATE / "KrigeBessel.h"),
                str((PUBLIC / "KrigePortableCore.h").relative_to(ROOT)): sha256(PUBLIC / "KrigePortableCore.h"),
                str(PROBE.relative_to(ROOT)): sha256(PROBE),
            },
            "metrics": {
                "bessel_max_relative_error": max_bessel_relative_error,
                "structure_max_absolute_error": max_structure_absolute_error,
                "as241_max_absolute_error": max_normal_absolute_error,
                "matern_nu_9_99_to_10_discontinuity": discontinuity,
                "matern_gaussian_same_range_mismatch": same_range_mismatch,
                "matern_gaussian_rescaled_range_mismatch": rescaled_range_mismatch,
            },
            "notes": [
                "The shipped C++ analytic structure evaluator was compiled and called directly.",
                "There is no CPU lookup table in this correctness gate, so no Python lookup restatement is reported as production evidence.",
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
