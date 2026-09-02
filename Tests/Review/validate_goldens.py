#!/usr/bin/env python3
"""Compile golden_probe.cpp against the shipped C++ core, run it against the
scenarios frozen in goldens.json, and compare its output to PyKrige's
independently-computed predictions.

This is the "gate" half of the PyKrige cross-validation: generate_goldens.py
is run offline (its output, goldens.json, is checked in as a frozen golden
file) so that this validator does not need PyKrige installed to run in CI --
only the C++ compiler and stdlib json are required, matching the posture of
validate_reference.py in spirit (independent-library comparison) while
keeping the fast/CI-friendly path dependency-light.

Tolerance rationale (documented, not tuned to pass)
----------------------------------------------------
Both sides solve the same well-posed linear kriging system (same sill,
mapped range, nugget, drift terms) but with different linear-algebra
implementations: PyKrige does scipy.linalg.inv(...) followed by a matmul,
the C++ core does a partial-pivoting LU factor-and-solve. These are
different roundoff paths for the same mathematics, so we expect agreement
at a small multiple of double-precision epsilon on well-conditioned systems,
not bit-identical results. We reuse the scaled_error metric from
Tests/Review/validate_reference.py (|a-b| / max(1, |a|, |b|)) specifically
because kriging variance is often close to zero at/near sample points, where
a naive relative error blows up on noise; scaling by max(1, ...) keeps the
tolerance meaningful for both near-zero and O(1) quantities.

value tolerance:    1e-9
variance tolerance: 1e-9

These sit near validate_reference.py's 2e-9 even though the two sides here
are *not* evaluating the identical system-assembly code path (unlike the
NumPy reference, which literally re-derives the same augmented matrix) --
they are two independently engineered kriging implementations (PyKrige does
scipy.linalg.inv(...) then a matmul; the C++ core does a partial-pivoting
LU factor-and-solve) agreeing only at the level of "same mathematical
kriging problem, correctly mapped range convention". The tolerance was set
by running the suite once, observing the actual maximum scaled error on
these well-conditioned, isotropic, single-structure, 15-34-sample scenarios
(3.3e-14 for value, 1.1e-14 for variance -- essentially double-precision
roundoff, consistent with two clean but different O(n^3) solves of the same
well-conditioned system), and leaving about four orders of magnitude of
headroom above that observed maximum -- not by widening the tolerance until
a failure disappeared. The observed maximum for each run is recorded in the
output JSON ("metrics") so any future drift toward the tolerance is visible
even on a run that still passes.

If a genuine range-convention (or other systematic) bug exists, the
resulting error is of a completely different character than roundoff: it
scales with the sill/variance magnitude (typically 0.05-0.6 in these
scenarios, per goldens.json["sensitivity"]), i.e. 5+ orders of magnitude
above this tolerance. There is no realistic roundoff accumulation on
15-34-sample double-precision dense solves that closes that gap, so this
tolerance has a wide, safe margin between "numerically clean pass" and "the
thing this gate exists to catch".
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

VALUE_TOLERANCE = 1.0e-9
VARIANCE_TOLERANCE = 1.0e-9


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def scaled_error(actual: float, expected: float) -> float:
    return abs(actual - expected) / max(1.0, abs(actual), abs(expected))


def compile_probe(compiler: str, root: Path, probe: Path, production: Path,
                   public_dir: Path, private_dir: Path, output: Path) -> list[str]:
    command = [
        compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
        "-I", str(public_dir), "-I", str(private_dir),
        str(probe), str(production), "-o", str(output),
    ]
    subprocess.run(command, check=True, cwd=root)
    return command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_staging = Path(__file__).resolve().parent
    parser.add_argument("--compiler", default="c++",
                         help="C++ compiler to invoke (default: c++; set CXX in the "
                              "environment and pass --compiler \"$CXX\" to override)")
    parser.add_argument("--goldens", type=Path,
                         default=default_staging.parent / "Golden" / "goldens.json",
                         help="Path to the frozen goldens.json produced by generate_goldens.py")
    parser.add_argument("--repo-root", type=Path,
                         default=None,
                         help="Path to the kriging repo root (contains Source/KrigingCore). "
                              "Defaults to the post-integration layout Tests/Review/../.. "
                              "relative to this file; pass explicitly when running from staging.")
    parser.add_argument("--probe", type=Path, default=default_staging / "golden_probe.cpp",
                         help="Path to golden_probe.cpp")
    parser.add_argument("--output", type=Path, help="Where to write the JSON report")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve() if args.repo_root else Path(__file__).resolve().parents[2]
    public_dir = repo_root / "Source/KrigingCore/Public"
    private_dir = repo_root / "Source/KrigingCore/Private/Portable"
    production = private_dir / "KrigePortableCore.cpp"
    header = public_dir / "KrigePortableCore.h"

    for required in (public_dir, private_dir, production, header):
        if not required.exists():
            print(f"error: required path does not exist: {required}", file=sys.stderr)
            print("Pass --repo-root pointing at the kriging repo checkout.", file=sys.stderr)
            return 2

    goldens = json.loads(args.goldens.read_text(encoding="utf-8"))
    scenarios = goldens["scenarios"]
    protocol_text = goldens["wire_protocol"]

    with tempfile.TemporaryDirectory(prefix="kriging_golden_") as directory:
        executable = Path(directory) / "golden_probe"
        compile_command = compile_probe(
            args.compiler, repo_root, args.probe, production, public_dir, private_dir, executable,
        )
        completed = subprocess.run(
            [str(executable)], input=protocol_text, text=True,
            capture_output=True, check=True, cwd=repo_root,
        )
        try:
            actual = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            print("error: golden_probe did not emit valid JSON", file=sys.stderr)
            print(completed.stdout[:2000], file=sys.stderr)
            raise SystemExit(2) from error

    if len(actual) != len(scenarios):
        print(f"error: probe emitted {len(actual)} model results, expected {len(scenarios)}",
              file=sys.stderr)
        return 2

    maximum_value_error = 0.0
    maximum_variance_error = 0.0
    worst_value = ""
    worst_variance = ""
    per_scenario: list[dict[str, Any]] = []
    failures: list[str] = []

    for scenario, model_result in zip(scenarios, actual):
        name = scenario["name"]
        if model_result.get("build_failed"):
            failures.append(f"{name}: C++ build failed: {model_result.get('message')}")
            continue
        if model_result.get("ridge", 0.0) != 0.0 or model_result.get("degraded"):
            failures.append(
                f"{name}: unexpectedly used ridge={model_result.get('ridge')} or "
                f"degraded={model_result.get('degraded')} on a well-conditioned golden scenario"
            )
        cpp_results = model_result.get("results", [])
        pk_results = scenario["pykrige_results"]
        if len(cpp_results) != len(pk_results):
            failures.append(f"{name}: result count mismatch: cpp={len(cpp_results)} pykrige={len(pk_results)}")
            continue

        scenario_max_value_error = 0.0
        scenario_max_variance_error = 0.0
        for index, (cpp_row, pk_row) in enumerate(zip(cpp_results, pk_results)):
            if cpp_row.get("failed"):
                failures.append(f"{name}[{index}]: C++ query evaluation failed")
                continue
            value_error = scaled_error(cpp_row["value"], pk_row["value"])
            variance_error = scaled_error(cpp_row["variance"], pk_row["variance"])
            scenario_max_value_error = max(scenario_max_value_error, value_error)
            scenario_max_variance_error = max(scenario_max_variance_error, variance_error)
            if value_error > maximum_value_error:
                maximum_value_error = value_error
                worst_value = f"{name}[{index}]"
            if variance_error > maximum_variance_error:
                maximum_variance_error = variance_error
                worst_variance = f"{name}[{index}]"

        per_scenario.append({
            "name": name,
            "shape": scenario["shape_name"],
            "method": scenario["method"],
            "volumetric": scenario["volumetric"],
            "a_cpp": scenario["a_cpp"],
            "r_pykrige": scenario["r_pykrige"],
            "max_scaled_value_error": scenario_max_value_error,
            "max_scaled_variance_error": scenario_max_variance_error,
        })

    if maximum_value_error > VALUE_TOLERANCE:
        failures.append(
            f"value mismatch: max scaled error {maximum_value_error:.3e} at {worst_value} "
            f"exceeds tolerance {VALUE_TOLERANCE:.1e}"
        )
    if maximum_variance_error > VARIANCE_TOLERANCE:
        failures.append(
            f"variance mismatch: max scaled error {maximum_variance_error:.3e} at {worst_variance} "
            f"exceeds tolerance {VARIANCE_TOLERANCE:.1e}"
        )

    status = "PASS" if not failures else "FAIL"
    report = {
        "status": status,
        "compiler": args.compiler,
        "compile_command": compile_command,
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "goldens_path": str(args.goldens),
        "goldens_generated_with_versions": goldens["versions"],
        "goldens_generated_utc": goldens["generated_utc"],
        "range_convention_mapping": goldens["range_convention_mapping"],
        "sensitivity": goldens.get("sensitivity", {}),
        "scenario_count": len(scenarios),
        "query_count": sum(len(s["queries"]) for s in scenarios),
        "per_scenario": per_scenario,
        "metrics": {
            "maximum_scaled_value_error": maximum_value_error,
            "worst_value_case": worst_value,
            "maximum_scaled_variance_error": maximum_variance_error,
            "worst_variance_case": worst_variance,
            "value_tolerance": VALUE_TOLERANCE,
            "variance_tolerance": VARIANCE_TOLERANCE,
        },
        "failures": failures,
        "source_sha256": {
            str(production.relative_to(repo_root)): sha256(production),
            str(header.relative_to(repo_root)): sha256(header),
            "golden_probe.cpp": sha256(args.probe),
            "goldens.json": sha256(args.goldens),
        },
        "notes": [
            "PyKrige independently computes ordinary/universal-linear kriging predictions "
            "and variances using its own built-in spherical/exponential/gaussian variogram "
            "models and its own dense linear solver.",
            "The C++ probe parses the same scenarios (frozen in goldens.json's wire_protocol "
            "field) and calls the shipped Model::Build / EvaluateWithVariance public API.",
            "Range parameters are translated between the two libraries' conventions before "
            "comparison; see goldens.json['range_convention_mapping'] and RANGE_CONVENTIONS.md.",
        ],
    }
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8")
    else:
        print(serialized, end="")

    if failures:
        print("FAIL:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"PASS: {len(scenarios)} scenarios, max scaled value error "
          f"{maximum_value_error:.3e}, max scaled variance error {maximum_variance_error:.3e}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
