#!/usr/bin/env python3
"""Run one CMake/CTest gate in an isolated process and emit JSON evidence."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
STANDALONE = ROOT / "Tests/Standalone"


def tool_version(command: list[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
            timeout=30,
        )
        text = completed.stdout.strip() or completed.stderr.strip()
        return text.splitlines()[0] if text else "unknown"
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return "unavailable"


def run_step(name: str, command: list[str], environment: dict[str, str],
             timeout_seconds: int) -> dict[str, Any]:
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            env=environment,
            text=True,
            capture_output=True,
            timeout=timeout_seconds,
        )
        return {
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
        return {
            "name": name,
            "status": "FAIL",
            "return_code": None,
            "duration_seconds": time.perf_counter() - started,
            "command": command,
            "stdout": stdout,
            "stderr": stderr + f"\nTimed out after {timeout_seconds} seconds.",
        }
    except OSError as error:
        return {
            "name": name,
            "status": "FAIL",
            "return_code": None,
            "duration_seconds": time.perf_counter() - started,
            "command": command,
            "stdout": "",
            "stderr": f"Could not execute command: {error}",
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sanitizer", action="store_true")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    build_directory = Path(tempfile.mkdtemp(prefix=f"kriging_{args.name}_"))
    environment = os.environ.copy()
    environment["CXX"] = args.compiler

    configuration = "Debug" if args.sanitizer else "Release"
    configure = [
        "cmake", "-S", str(STANDALONE), "-B", str(build_directory),
        f"-DCMAKE_BUILD_TYPE={configuration}",
    ]
    if args.sanitizer:
        flags = "-fsanitize=address,undefined -fno-omit-frame-pointer"
        configure.extend([
            f"-DCMAKE_CXX_FLAGS={flags}",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
        ])
        environment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"

    report: dict[str, Any] = {
        "name": args.name,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "compiler_requested": args.compiler,
        "compiler_version": tool_version([args.compiler, "--version"]),
        "cmake_version": tool_version(["cmake", "--version"]),
        "ctest_version": tool_version(["ctest", "--version"]),
        "configuration": configuration,
        "sanitizers": ["address", "undefined"] if args.sanitizer else [],
        "steps": [],
    }

    try:
        report["steps"].append(run_step(
            f"{args.name}:configure", configure, environment, args.timeout))
        if report["steps"][-1]["status"] == "PASS":
            report["steps"].append(run_step(
                f"{args.name}:build",
                ["cmake", "--build", str(build_directory), "--parallel", "1"],
                environment,
                args.timeout,
            ))
        if report["steps"][-1]["status"] == "PASS":
            report["steps"].append(run_step(
                f"{args.name}:ctest",
                ["ctest", "--test-dir", str(build_directory), "--output-on-failure"],
                environment,
                args.timeout,
            ))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        report["status"] = (
            "PASS" if report["steps"]
            and all(step["status"] == "PASS" for step in report["steps"])
            else "FAIL"
        )
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        shutil.rmtree(build_directory, ignore_errors=True)

    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
