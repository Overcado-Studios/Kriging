#!/usr/bin/env python3
"""Run every engine-independent release gate and record reproducible provenance."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
from importlib import metadata as importlib_metadata
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
RESULTS = ROOT / "Tests/Results"
REVIEW = ROOT / "Tests/Review"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_manifest() -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT)
        if relative.parts[:2] == ("Tests", "Results"):
            continue
        if "__pycache__" in relative.parts or relative.suffix == ".pyc":
            continue
        if relative.name == "MANIFEST.sha256":
            continue
        result[str(relative)] = sha256(path)
    return result


def version(command: list[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            text=True,
            capture_output=True,
            check=True,
            cwd=ROOT,
            timeout=30,
        )
        output = completed.stdout.strip() or completed.stderr.strip()
        return output.splitlines()[0] if output else "unknown"
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return "unavailable"


def run_step(name: str, command: list[str], *, cwd: Path = ROOT,
             timeout_seconds: int = 300) -> dict[str, Any]:
    print(f"[verification] start {name}", file=sys.stderr, flush=True)
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
        )
        result = {
            "name": name,
            "status": "PASS" if completed.returncode == 0 else "FAIL",
            "return_code": completed.returncode,
            "duration_seconds": time.perf_counter() - started,
            "command": command,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else (error.stderr or "")
        result = {
            "name": name,
            "status": "FAIL",
            "return_code": None,
            "duration_seconds": time.perf_counter() - started,
            "command": command,
            "stdout": stdout,
            "stderr": stderr + f"\nTimed out after {timeout_seconds} seconds.",
        }
    except OSError as error:
        result = {
            "name": name,
            "status": "FAIL",
            "return_code": None,
            "duration_seconds": time.perf_counter() - started,
            "command": command,
            "stdout": "",
            "stderr": f"Could not execute command: {error}",
        }
    print(
        f"[verification] {result['status']} {name} "
        f"({result['duration_seconds']:.3f}s)",
        file=sys.stderr,
        flush=True,
    )
    return result


def run_isolated_cmake_gate(report: dict[str, Any], name: str,
                            compiler: str, sanitizer: bool = False) -> None:
    output = RESULTS / f"{name.upper()}.json"
    command = [
        sys.executable,
        str(REVIEW / "run_cmake_gate.py"),
        "--name", name,
        "--compiler", compiler,
        "--output", str(output),
    ]
    if sanitizer:
        command.append("--sanitizer")
    invocation = run_step(f"{name}:isolated_process", command, timeout_seconds=600)

    if output.is_file():
        gate = json.loads(output.read_text(encoding="utf-8"))
        report.setdefault("cmake_gates", {})[name] = gate
        report["steps"].extend(gate.get("steps", []))
        if invocation["status"] != gate.get("status"):
            report["steps"].append({
                "name": f"{name}:evidence_consistency",
                "status": "FAIL",
                "reason": (
                    "isolated process return status disagrees with its JSON report: "
                    f"process={invocation['status']} report={gate.get('status')}"
                ),
            })
    else:
        report["steps"].append(invocation)


def parse_pinned_requirements(path: Path) -> dict[str, str]:
    """Parse simple `package==version` pins from a requirements file.

    Only exact `==` pins are recognised; anything else (comments, extras,
    unpinned or range-constrained entries) is ignored rather than guessed at.
    """
    pinned: dict[str, str] = {}
    if not path.is_file():
        return pinned
    pattern = re.compile(r"^([A-Za-z0-9_.-]+)\s*==\s*([A-Za-z0-9_.!+-]+)\s*$")
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        match = pattern.match(line)
        if match:
            pinned[match.group(1).lower()] = match.group(2)
    return pinned


def installed_version(package: str) -> str:
    try:
        return importlib_metadata.version(package)
    except importlib_metadata.PackageNotFoundError:
        return "not installed"


def check_pinned_versions(report: dict[str, Any]) -> None:
    """Record whether the interpreter running this gate actually has the
    package versions pinned in requirements.txt, rather than silently
    reporting results that were produced with different versions.

    This never fails the gate by itself (a version drift is not a
    correctness failure), but any mismatch is recorded as a clearly-labelled
    warning so a passing report can never be misread as having used the
    pinned versions when it did not.
    """
    requirements_path = REVIEW / "requirements.txt"
    pinned = parse_pinned_requirements(requirements_path)
    installed = {name: installed_version(name) for name in pinned}
    mismatches = {
        name: {"pinned": pinned[name], "installed": installed[name]}
        for name in pinned
        if installed[name] != pinned[name]
    }
    matched = not mismatches
    report["pinned_versions"] = {
        "requirements_file": str(requirements_path.relative_to(ROOT)),
        "pinned": pinned,
        "installed": installed,
        "pinned_versions_matched": matched,
        "mismatches": mismatches,
    }
    if not matched:
        detail = "; ".join(
            f"{name}: pinned {info['pinned']}, installed {info['installed']}"
            for name, info in sorted(mismatches.items())
        )
        warning = (
            "WARNING: this run's package versions do NOT match the pins in "
            f"{requirements_path.relative_to(ROOT)}; results below were NOT "
            f"produced with the pinned versions ({detail})."
        )
        report.setdefault("warnings", []).append(warning)
        print(f"[verification] {warning}", file=sys.stderr, flush=True)


def main() -> int:
    RESULTS.mkdir(parents=True, exist_ok=True)
    for old in RESULTS.glob("*"):
        if old.is_file():
            old.unlink()

    report: dict[str, Any] = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version,
        "tools": {
            "gcc": version(["c++", "--version"]),
            "clang": version(["clang++", "--version"]),
            "cmake": version(["cmake", "--version"]),
            "ctest": version(["ctest", "--version"]),
        },
        "source_sha256_before_run": source_manifest(),
        "steps": [],
    }
    check_pinned_versions(report)

    # Build gates run in separate child processes. Besides isolating compiler and
    # sanitizer environments, this prevents one CTest invocation from inheriting
    # process state from a previous numerical-reference validator.
    # Run the sanitizer gate first. In constrained CI containers it is the
    # most memory-sensitive child; completing it before release compiler caches
    # accumulate makes the runner materially more reliable without changing the
    # commands or acceptance threshold.
    run_isolated_cmake_gate(report, "gcc_asan_ubsan", "c++", sanitizer=True)
    run_isolated_cmake_gate(report, "gcc_release", "c++")
    if shutil.which("clang++"):
        run_isolated_cmake_gate(report, "clang_release", "clang++")
    else:
        report["steps"].append({
            "name": "clang_release",
            "status": "SKIP",
            "reason": "clang++ is unavailable",
        })

    static_step = run_step(
        "targeted_static_guards",
        [sys.executable, str(REVIEW / "validate_static.py")],
    )
    report["steps"].append(static_step)
    if static_step["stdout"].strip():
        try:
            static_report = json.loads(static_step["stdout"])
        except json.JSONDecodeError:
            static_report = {"status": "UNPARSEABLE", "raw": static_step["stdout"]}
        # validate_static.py asserts that requirements.txt is pinned exactly
        # as text and, on that basis, records a checklist line claiming the
        # numeric/reference validators "exercise shipped C++ with pinned
        # references". That checklist line is about the requirements FILE,
        # not about which versions were actually installed when this run's
        # validators executed. Stamp the installed-vs-pinned mismatch (if
        # any) into this same report so it can never be read as evidence
        # that the pinned versions were used.
        if not report.get("pinned_versions", {}).get("pinned_versions_matched", True):
            static_report["pinned_versions_matched"] = False
            static_report["pinned_versions_warning"] = report["warnings"][-1]
        (RESULTS / "STATIC_VALIDATION.json").write_text(
            json.dumps(static_report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    numeric_path = RESULTS / "NUMERIC_VALIDATION.json"
    numeric_step = run_step(
        "special_function_reference_validation",
        [sys.executable, str(REVIEW / "validate_numerics.py"),
         "--compiler", "c++", "--output", str(numeric_path)],
    )
    report["steps"].append(numeric_step)
    if numeric_path.is_file():
        report["numeric_report"] = json.loads(numeric_path.read_text(encoding="utf-8"))

    reference_path = RESULTS / "INDEPENDENT_REFERENCE.json"
    reference_step = run_step(
        "independent_numpy_matrix_validation",
        [sys.executable, str(REVIEW / "validate_reference.py"),
         "--compiler", "c++", "--output", str(reference_path)],
    )
    report["steps"].append(reference_step)
    if reference_path.is_file():
        report["independent_reference_report"] = json.loads(
            reference_path.read_text(encoding="utf-8"))

    golden_path = RESULTS / "GOLDEN_VALIDATION.json"
    golden_step = run_step(
        "pykrige_golden_validation",
        [sys.executable, str(REVIEW / "validate_goldens.py"),
         "--compiler", "c++", "--output", str(golden_path)],
    )
    report["steps"].append(golden_step)
    if golden_path.is_file():
        report["pykrige_golden_report"] = json.loads(
            golden_path.read_text(encoding="utf-8"))

    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    failing = [step for step in report["steps"] if step.get("status") == "FAIL"]
    report["status"] = "PASS" if not failing else "FAIL"
    report["limitations"] = [
        "No Unreal Engine installation was available; UnrealBuildTool, UnrealHeaderTool, BuildPlugin, and the UE automation smoke test were not run.",
        "This evidence validates the active portable numerical core, not removed editor, renderer, PCG, content, Cesium, Landscape, or packaging subsystems.",
    ]

    verification_path = RESULTS / "VERIFICATION.json"
    verification_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    text_lines = [
        f"status: {report['status']}",
        f"started_utc: {report['started_utc']}",
        f"finished_utc: {report['finished_utc']}",
        f"platform: {report['platform']}",
        f"gcc: {report['tools']['gcc']}",
        f"clang: {report['tools']['clang']}",
        f"cmake: {report['tools']['cmake']}",
        "",
        "pinned package versions:",
    ]
    pinned_versions = report.get("pinned_versions", {})
    text_lines.append(
        f"- pinned_versions_matched: {pinned_versions.get('pinned_versions_matched')}"
    )
    for name in sorted(pinned_versions.get("pinned", {})):
        text_lines.append(
            f"  - {name}: pinned={pinned_versions['pinned'][name]} "
            f"installed={pinned_versions.get('installed', {}).get(name)}"
        )
    for warning in report.get("warnings", []):
        text_lines.append(f"- {warning}")
    text_lines.extend([
        "",
        "steps:",
    ])
    for step in report["steps"]:
        duration = step.get("duration_seconds")
        suffix = f" ({duration:.3f}s)" if isinstance(duration, (int, float)) else ""
        text_lines.append(f"- {step['status']}: {step['name']}{suffix}")
    numeric = report.get("numeric_report", {}).get("metrics", {})
    if numeric:
        text_lines.extend([
            "",
            "special-function metrics:",
            f"- Bessel max relative error: {numeric['bessel_max_relative_error']:.6e}",
            f"- Structure max absolute error: {numeric['structure_max_absolute_error']:.6e}",
            f"- AS241 max absolute error: {numeric['as241_max_absolute_error']:.6e}",
        ])
    independent = report.get("independent_reference_report", {}).get("metrics", {})
    if independent:
        text_lines.extend([
            "",
            "independent matrix-reference metrics:",
            f"- Maximum scaled value error: {independent['maximum_scaled_value_error']:.6e}",
            f"- Maximum scaled variance error: {independent['maximum_scaled_variance_error']:.6e}",
        ])
    pykrige_golden = report.get("pykrige_golden_report", {}).get("metrics", {})
    if pykrige_golden:
        text_lines.extend([
            "",
            "independent PyKrige golden-file metrics:",
            f"- Maximum scaled value error: {pykrige_golden['maximum_scaled_value_error']:.6e}",
            f"- Maximum scaled variance error: {pykrige_golden['maximum_scaled_variance_error']:.6e}",
        ])
    text_lines.extend(["", "limitations:"] + [f"- {item}" for item in report["limitations"]])
    (RESULTS / "VERIFICATION.txt").write_text(
        "\n".join(text_lines) + "\n", encoding="utf-8"
    )

    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
