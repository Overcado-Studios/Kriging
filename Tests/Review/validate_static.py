#!/usr/bin/env python3
"""Targeted source-structure guards for the corrected core-gate package.

This is deliberately not presented as a compiler. Every assertion either
isolates one function body or validates a concrete package topology. Build and
runtime evidence is produced separately by run_verification.py.
"""

from __future__ import annotations

import ast
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "Source"
CORE_CPP = ROOT / "Source/KrigingCore/Private/Portable/KrigePortableCore.cpp"
CORE_H = ROOT / "Source/KrigingCore/Public/KrigePortableCore.h"
TEST_CPP = ROOT / "Tests/Standalone/test_core.cpp"
ALLOC_CPP = ROOT / "Tests/Standalone/allocation_probe.cpp"
CMAKE = ROOT / "Tests/Standalone/CMakeLists.txt"
NUMERIC = ROOT / "Tests/Review/validate_numerics.py"
REFERENCE = ROOT / "Tests/Review/validate_reference.py"
CMAKE_GATE = ROOT / "Tests/Review/run_cmake_gate.py"
RUNNER = ROOT / "Tests/Review/run_verification.py"


class ValidationFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationFailure(message)


def read(path: Path) -> str:
    require(path.is_file(), f"missing required file: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def function_body(text: str, signature_fragment: str) -> str:
    start = text.find(signature_fragment)
    require(start >= 0, f"function signature not found: {signature_fragment}")
    opening = text.find("{", start)
    require(opening >= 0, f"function body not found: {signature_fragment}")
    depth = 0
    in_string = False
    in_character = False
    escaped = False
    line_comment = False
    block_comment = False
    i = opening
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if c == "\n":
                line_comment = False
            i += 1
            continue
        if block_comment:
            if c == "*" and n == "/":
                block_comment = False
                i += 2
            else:
                i += 1
            continue
        if in_string:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == '"':
                in_string = False
            i += 1
            continue
        if in_character:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == "'":
                in_character = False
            i += 1
            continue
        if c == "/" and n == "/":
            line_comment = True
            i += 2
            continue
        if c == "/" and n == "*":
            block_comment = True
            i += 2
            continue
        if c == '"':
            in_string = True
        elif c == "'":
            in_character = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[opening : i + 1]
        i += 1
    raise ValidationFailure(f"unterminated function body: {signature_fragment}")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def python_function_source(text: str, function_name: str) -> str:
    try:
        tree = ast.parse(text)
    except SyntaxError as error:
        raise ValidationFailure(f"invalid Python source while locating {function_name}: {error}") from error
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == function_name:
            require(node.end_lineno is not None, f"Python function has no end line: {function_name}")
            lines = text.splitlines()
            return "\n".join(lines[node.lineno - 1 : node.end_lineno])
    raise ValidationFailure(f"Python function not found: {function_name}")


def main() -> int:
    checks: list[str] = []

    plugin = json.loads(read(ROOT / "Kriging.uplugin"))
    modules = plugin.get("Modules", [])
    expected_modules = {"KrigingCore", "KrigingBlueprint"}
    manifest_names = {m.get("Name") for m in modules}
    require(len(modules) == len(expected_modules),
            f"active manifest must declare exactly {len(expected_modules)} modules "
            f"(no duplicates), found {len(modules)} entries")
    require(manifest_names == expected_modules,
            f"active manifest must declare exactly the modules {sorted(expected_modules)}, "
            f"found {sorted(n for n in manifest_names if n)}")
    for entry_module in modules:
        require(entry_module.get("Type") == "Runtime",
                f"{entry_module.get('Name')} must be a Runtime module")
    require(plugin.get("CanContainContent") is False, "core gate must not claim content assets")
    checks.append("manifest declares exactly the KrigingCore and KrigingBlueprint runtime modules")

    module = ROOT / "Source/KrigingCore"
    require((module / "KrigingCore.Build.cs").is_file(), "KrigingCore.Build.cs missing")
    entry = read(module / "Private/KrigingCoreModule.cpp")
    require(entry.count("IMPLEMENT_MODULE") == 1
            and "KrigingCore" in entry, "module entry point is missing or ambiguous")

    bp_module = ROOT / "Source/KrigingBlueprint"
    require((bp_module / "KrigingBlueprint.Build.cs").is_file(), "KrigingBlueprint.Build.cs missing")
    bp_entry = read(bp_module / "Private/KrigingBlueprintModule.cpp")
    require(bp_entry.count("IMPLEMENT_MODULE") == 1
            and "KrigingBlueprint" in bp_entry, "KrigingBlueprint module entry point is missing or ambiguous")
    checks.append("both manifest modules have a Build.cs and an unambiguous module entry point")

    on_disk_modules = {
        path.name for path in SOURCE.iterdir()
        if path.is_dir() and (path / f"{path.name}.Build.cs").is_file()
    }
    require(on_disk_modules == expected_modules,
            f"Source/ module directories {sorted(on_disk_modules)} do not match "
            f"the manifest's declared modules {sorted(expected_modules)}")
    checks.append("every Source/ build module is declared in the manifest and vice versa")

    forbidden_paths = [
        ROOT / "Source/KrigingRenderer",
        ROOT / "Source/KrigingEditor",
        ROOT / "Source/KrigingPCG",
        ROOT / "Shaders",
        ROOT / "Content",
        ROOT / "Docs/LegacyDiscarded",
    ]
    require(not any(path.exists() for path in forbidden_paths),
            "unverified renderer/editor/PCG/shader/content/legacy subsystem is active")
    source_text = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in SOURCE.rglob("*") if path.is_file()
    )
    require("Cesium" not in source_text and "cesium" not in source_text,
            "out-of-spec Cesium code is present in the active source tree")
    checks.append("unverified and out-of-spec subsystems are absent from active tree")

    required_headers = {
        "KrigePortableCore.h", "KrigeTypes.h", "KrigeLinearSolve.h",
        "KrigeKdTree.h", "KrigeTransform.h", "KrigeVariogram.h", "KrigeModel.h",
    }
    public_headers = {path.name for path in (module / "Public").glob("*.h")}
    require(required_headers <= public_headers,
            f"public core headers missing: {sorted(required_headers - public_headers)}")
    checks.append("portable and compatibility public headers are present")

    local_names = {path.name for path in SOURCE.rglob("*.h")} | {
        path.name for path in SOURCE.rglob("*.inl")
    }
    for path in list(SOURCE.rglob("*.cpp")) + list(SOURCE.rglob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for include in re.findall(r'^\s*#include\s+"([^"]+)"', text, re.MULTILINE):
            if include.startswith("Krige"):
                require(Path(include).name in local_names,
                        f"unresolved local include {include!r} in {path.relative_to(ROOT)}")
    checks.append("all Krige-prefixed quoted includes resolve across both modules")

    core = read(CORE_CPP)
    header = read(CORE_H)
    require(not re.search(r'\b(?:check|assert)\s*\(', source_text),
            "active source contains crash-only check/assert")
    require("throw " not in source_text, "active source contains an exception throw")
    checks.append("active source avoids crash-only checks and exceptions")

    build = function_body(core, "bool Model::Build(")
    require("Impl fresh;" in build and "auto Commit" in build and "auto Fail" in build,
            "Build does not use isolated transactional state")
    require("impl_ = std::move(replacement)" in build,
            "Build does not commit through replacement state")
    require("return Commit(false)" in build,
            "failed builds do not invalidate stale state transactionally")
    checks.append("model build is transactional")

    evaluate = function_body(core, "double Evaluate(const Vec3& at) const")
    evaluate_variance = function_body(core, "bool EvaluateWithVariance(const Vec3& at")
    for body, name in ((evaluate, "Evaluate"), (evaluate_variance, "EvaluateWithVariance")):
        require("FindExact" not in body and "workingValues[" not in body,
                f"{name} contains an exact-sample value shortcut")
    require("std::array<double, MaxSystemOrder>" in core,
            "fixed query workspace is missing")
    checks.append("kriging queries solve the system and use fixed global workspace")

    merge = function_body(core, "bool MergeDuplicates(")
    require("for (std::size_t member : cluster)" in merge
            and "completeLink" in merge
            and "maxDiameter" in merge,
            "duplicate merging is not bounded-diameter complete-link clustering")
    checks.append("duplicate merging cannot collapse transitive chains")

    drift = function_body(core, "bool DriftBasis(")
    build_model_impl = build
    require("driftCenter" in drift and "driftScale" in drift
            and "fresh.driftCenter" in build_model_impl
            and "fresh.driftScale" in build_model_impl,
            "drift basis is not centered and scaled from sample bounds")
    require("Could not evaluate the drift basis" in core
            and "original index" in core,
            "external-drift failures are not descriptive")
    set_drift = function_body(core, "void Model::SetExternalDriftSampler(")
    require("valid = false" in set_drift and "rebuild" in set_drift,
            "repointing external drift does not invalidate the model")
    checks.append("drift basis and external-drift failure handling are guarded")

    normalized = function_body(core, "double EvaluateNormalizedStructure(")
    require("x >= 60.0" in normalized,
            "Matérn asymptotic underflow guard is missing")
    require(not re.search(r'maternNu\s*>=\s*10', normalized, re.IGNORECASE),
            "hard Matérn-to-Gaussian branch remains")
    require("frexp" not in core and "Lookup" not in core,
            "CPU production path still contains an unmeasured lookup remap")
    checks.append("Matérn is continuous at nu=10 and CPU structures are analytic")

    local_system = function_body(core, "std::shared_ptr<const System> LocalSystemFor(")
    require("entry.system->indices == indices" in local_system,
            "local cache trusts a hash without full index-list equality")
    require("cacheEntries < 4096" in local_system,
            "local cache has no bounded admission policy")
    require("localCache.clear" not in local_system and "localCache.Reset" not in local_system,
            "local cache flushes wholesale under load")
    require("localCache[key]" in local_system
            and local_system.find("cacheEntries < 4096") < local_system.find("localCache[key]"),
            "local cache creates empty buckets after reaching capacity")
    checks.append("local factorization cache verifies keys and avoids full resets")

    idw = function_body(core, "bool IdwWorking(")
    require("Nearest(at, settings.maxNeighbours" in idw,
            "IDW degradation scans every sample rather than bounded k-nearest candidates")
    report = function_body(core, "BuildReport Model::GetReport() const")
    require("localIdwFallbacks" in report and "degraded = true" in report,
            "per-query local IDW degradation is not surfaced")
    require("std::atomic<int> negativeVarianceClamps" in core
            and "fetch_add" in core,
            "variance clamps are not counted thread-safely")
    checks.append("degradation and variance clamps are bounded and reported")

    back_transform = function_body(core, "double BackTransform(")
    require("lognormalBiasCorrection" in back_transform,
            "bias correction is not a model-level back-transform policy")
    require("BiasNeedsVariance" in evaluate and "EvaluateWithVariance" in evaluate,
            "value-only entry point bypasses required lognormal correction variance")
    checks.append("bias correction is entry-point invariant")

    brute = function_body(core, "bool Model::CrossValidateBruteForce(")
    fast = function_body(core, "bool Model::CrossValidate(")
    require("samples.size()) > maximumSamples" in brute and "capped at" in brute,
            "brute-force LOO is unbounded")
    require("diagonal > 0.0" in fast and "sqrt(1.0 / diagonal)" in fast,
            "fast LOO hides an indefinite inverse diagonal with absolute value")
    require("std::abs(1.0 /" not in fast and "FMath::Abs" not in fast,
            "fast LOO still absolute-values its standard errors")
    require("hasMeasurementVariance" in fast and "global.ridge != 0.0" in fast,
            "fast LOO lacks measurement-variance or ridge eligibility gates")
    require("samples.size() <= 60" in fast
            and "verifiedAgainstBruteForce = true" in fast
            and "returned the brute-force reference" in fast,
            "small-model fast LOO is not runtime-checked against brute force")
    require("verifiedAgainstBruteForce remains false" in fast,
            "large-model unverified LOO is not explicitly caveated")
    checks.append("LOO fast path is eligibility-gated and checked against bounded brute force")

    require("EvaluateGridLocalTiled" not in source_text
            and not re.search(r'\b(?:CrossFade|SmoothStep)\b', source_text),
            "unproved tiled blending or averaged tiled variance remains active")
    require("KrigingRenderer" not in source_text and "FGlobalShader" not in source_text,
            "unreachable or unverified GPU path remains active")
    checks.append("tiled variance and GPU claims are removed instead of papered over")

    cmake = read(CMAKE)
    require("KrigePortableCore.cpp" in cmake and "allocation_probe.cpp" in cmake,
            "standalone gate does not compile production core and allocation probe")
    require("kriging_unreal_source_syntax" in cmake
            and "KrigeCoreAutomationTests.cpp" in cmake,
            "standalone build does not syntax-compile Unreal-facing translation units")
    tests = read(TEST_CPP)
    for token in [
        "TestExactInterpolationAndNoShortcut",
        "TestPartitionUnityAndConstantField",
        "TestDriftReproductionAndLargeCoordinates",
        "TestTranslationRotationAndAnisotropyIdentity",
        "TestLocalGlobalFullNeighbourParity",
        "TestFastBruteForceLoo",
        "TestKdTreeAgainstBruteForce",
        "TestDeterminismAndPermutationInvariance",
        "TestVarianceAndNestedAdditivity",
        "TestBoundedDuplicateMerging",
    ]:
        require(token in tests, f"standalone property suite missing {token}")
    allocation = read(ALLOC_CPP)
    require("TrackAllocations" in allocation
            and "operator new" in allocation
            and "EvaluateWithVariance" in allocation,
            "allocation probe does not instrument actual query paths")
    checks.append("standalone gate covers the load-bearing core properties")

    numeric = read(NUMERIC)
    require("KrigePortableCore.cpp" in numeric and "numeric_probe.cpp" in numeric,
            "numeric validator does not compile the shipped C++ core")
    require("build_lookup" not in numeric and "sample_lookup" not in numeric,
            "numeric validator still substitutes a Python lookup restatement")
    reference = read(REFERENCE)
    require("KrigePortableCore.cpp" in reference and "reference_probe.cpp" in reference
            and "np.linalg.solve" in reference,
            "independent reference validator does not compile C++ and assemble NumPy systems")
    requirements_lines = read(ROOT / "Tests/Review/requirements.txt").splitlines()
    pinned_requirements = {}
    for raw_line in requirements_lines:
        line = raw_line.split("#", 1)[0].strip()
        match = re.match(r"^([A-Za-z0-9_.-]+)\s*==\s*([A-Za-z0-9_.!+-]+)\s*$", line)
        if match:
            pinned_requirements[match.group(1).lower()] = match.group(2)
    required_pins = {
        "numpy": "2.3.5",
        "scipy": "1.17.0",
        "pykrige": "1.7.3",
    }
    require(all(pinned_requirements.get(name) == version
                for name, version in required_pins.items()),
            "numeric/golden reference dependencies (numpy, scipy, pykrige) are not "
            "each exactly pinned in requirements.txt")
    checks.append("numeric and independent matrix validators exercise shipped C++ with pinned references")

    cmake_gate = read(CMAKE_GATE)
    runner = read(RUNNER)
    gate_main = python_function_source(cmake_gate, "main")
    gate_step = python_function_source(cmake_gate, "run_step")
    runner_gate = python_function_source(runner, "run_isolated_cmake_gate")
    runner_manifest = python_function_source(runner, "source_manifest")
    require("build_directory = Path(tempfile.mkdtemp" in gate_main
            and '["cmake", "--build"' in gate_main
            and '["ctest", "--test-dir"' in gate_main,
            "isolated CMake gate does not configure, build, and test in a private directory")
    require("shutil.rmtree(build_directory" in gate_main
            and "subprocess.TimeoutExpired" in gate_step,
            "isolated CMake gate lacks cleanup or timeout handling")
    require("run_cmake_gate.py" in runner_gate
            and "timeout_seconds=600" in runner_gate
            and '"__pycache__" in relative.parts' in runner_manifest
            and 'relative.suffix == ".pyc"' in runner_manifest,
            "verification runner lacks isolated gates or excludes neither bytecode form")
    checks.append("CMake gates are process-isolated and verification provenance excludes generated bytecode")

    report_data = {
        "status": "PASS",
        "scope": "targeted package topology and isolated source guards; not compilation",
        "checks": checks,
        "source_sha256": {
            str(path.relative_to(ROOT)): sha256(path)
            for path in [CORE_CPP, CORE_H, TEST_CPP, ALLOC_CPP, CMAKE, NUMERIC, REFERENCE, CMAKE_GATE, RUNNER]
        },
    }
    print(json.dumps(report_data, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationFailure as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
