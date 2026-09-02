# Engine Gates — KrigingCore + KrigingBlueprint

Re-run of the real-engine gates against `KrigingCore` + `KrigingBlueprint`
after the marching-cubes remediation change: `KrigePortableMarchingCubes.cpp`
gained a degenerate-triangle guard (a defense-in-depth `IsValid()`
post-condition check plus a skip in the `ExtractMarchingCubes` emission loop,
covering the case where isosurface welding collapses two corners of a
triangle onto the same lattice-adjacent vertex). Tests/Standalone and Docs
also changed in this remediation pass; no other module source file changed
(`git diff d1d534d --stat` shows exactly one `Source/` file touched). **This
run supersedes the previous `ENGINE_GATES.md`** (committed at `d1d534d`,
"Add KrigingBlueprint module; all engine gates green on UE 5.4 + 5.5"), which
was subsequently deleted from the working tree by `run_verification.py`
(which unconditionally clears `Tests/Results/` on each run) and is recovered
here only via `git show d1d534d:Tests/Results/ENGINE_GATES.md` for reference.

- run_utc: 2026-08-25T14:57:36Z
- engine_5_5: UE 5.5 (`C:\Program Files\Epic Games\UE_5.5`), MSVC 14.38.33130 (VS2022 17.x) toolchain, Windows 10.0.22621.0 SDK
- engine_5_4: UE 5.4 (`C:\Program Files\Epic Games\UE_5.4`), MSVC 14.38.33130 (VS2022 17.x) toolchain, Windows 10.0.22621.0 SDK
- host OS: Windows (via WSL2 bridge)
- fresh plugin copy: `Kriging.uplugin`, `Source/`, `LICENSE`, `README.md`, `ThirdPartyNotices/` copied from this repo to `_krigetmp/KrigingSrc4/Kriging` (new copy — `KrigingSrc3` from the previous run was left untouched)
- Note: this task deliberately did **not** re-run the engine-free gate
  (`Tests/Review/run_verification.py`) — that script unconditionally clears
  every file in `Tests/Results/`, and this task is scoped to write only
  `Tests/Results/ENGINE_GATES.md`. The four gates below are the Unreal
  engine gates the task asked to re-run.

## Gate 1 — UBT BuildPlugin, UE 5.5

Command (wrapper `.bat`, CRLF, run via `cmd.exe /c` from `_krigetmp`):
```
"C:\Program Files\Epic Games\UE_5.5\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
  -Plugin="C:\Users\HomePC\Documents\ClaudeCode\_krigetmp\KrigingSrc4\Kriging\Kriging.uplugin" ^
  -Package="C:\Users\HomePC\Documents\ClaudeCode\_krigetmp\KrigingPkg55d" ^
  -TargetPlatforms=Win64 -Rocket
```

First pass: **BUILD SUCCESSFUL** on the first attempt. All 15 actions
compiled for Editor/Development Game/Shipping Game configurations
(`KrigePortableMarchingCubes.cpp` among them, carrying the new
degenerate-triangle guard), `UnrealEditor-KrigingCore.dll` and
`UnrealEditor-KrigingBlueprint.dll` linked without incident; no repo fix was
required this time (unlike the prior run's `FLinearColor`→`FColor` fix,
which is already in the tree from `d1d534d`).

- Log: `C:\Users\HomePC\AppData\Roaming\Unreal Engine\AutomationTool\Logs\C+Program+Files+Epic+Games+UE_5.5\`
- AutomationTool exit code: **0** ("BUILD SUCCESSFUL", ~2m15s)

Result: **PASS**.

## Gate 2 — UBT BuildPlugin, UE 5.4

Same wrapper pattern, `UE_5.4`, `-Package="...\KrigingPkg54d"`.

```
"C:\Program Files\Epic Games\UE_5.4\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
  -Plugin="C:\Users\HomePC\Documents\ClaudeCode\_krigetmp\KrigingSrc4\Kriging\Kriging.uplugin" ^
  -Package="C:\Users\HomePC\Documents\ClaudeCode\_krigetmp\KrigingPkg54d" ^
  -TargetPlatforms=Win64 -Rocket
```

**BUILD SUCCESSFUL** on the first attempt — all 15 actions compiled and
linked (Editor, Development Game, Shipping Game) with the Visual Studio 2022
14.38.33140 toolchain; no warnings beyond the pre-existing benign `C4251`
DLL-interface notes.

- AutomationTool exit code: **0** ("BUILD SUCCESSFUL", ~2m34s)

Result: **PASS**.

## Gate 3 — Automation tests, NullRHI, UE 5.5

Host project: `_krigetmp/KrigeHost/KrigeHost.uproject` (`EngineAssociation:
5.5`, `Kriging` + `ProceduralMeshComponent` plugins enabled). Plugin
`Source/` refreshed from this repo; `Binaries/`/`Intermediate/` refreshed
from Gate 1's `KrigingPkg55d` output (source-only load under
`-unattended -nullrhi` does not trigger a compile prompt, per the prior
run's evidence — compiled binaries are required).

Command:
```
"C:\Program Files\Epic Games\UE_5.5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "C:\Users\HomePC\Documents\ClaudeCode\_krigetmp\KrigeHost\KrigeHost.uproject" ^
  -ExecCmds="Automation RunTests Kriging; Quit" ^
  -TestExit="Automation Test Queue Empty" ^
  -nullrhi -unattended -nop4 -nosplash -log ^
  -abslog="C:\Users\HomePC\Documents\ClaudeCode\_krigetmp\automation55d.log"
```

Exit code: **0**. "Found 5 automation tests based on 'Kriging'"; all 5 ran
and passed:

- Kriging.Blueprint.AutoFitBuild — Success
- Kriging.Blueprint.AutoFitHeuristicFallback — Success
- Kriging.Blueprint.ExplicitBuildExactness — Success
- Kriging.Blueprint.IsoSurfaceExtraction — Success
- Kriging.Core.PortableSmoke — Success

Non-fatal noise observed in the log, unrelated to Kriging (same category as
prior run): `LogFab`/`LogEOSSDK` login errors (offline environment, no
network credentials), `WinPixGpuCapturer.dll` failed to load
(GetLastError=126, expected without a GPU capture tool installed),
`LogPackageName`/`LogAssetRegistry` warnings about `/Temp` and untitled-map
external-actor paths from the default empty map load.

Result: **PASS** (5/5).

## Gate 4 — Automation tests, NullRHI, UE 5.4

Host project: `_krigetmp/KrigeHost54/KrigeHost54.uproject`
(`EngineAssociation: 5.4`). Plugin `Source/` refreshed from this repo;
`Binaries/`/`Intermediate/` refreshed from Gate 2's `KrigingPkg54d` output.

Command: identical pattern, `UE_5.4`'s `UnrealEditor-Cmd.exe`, log at
`_krigetmp/automation54d.log`.

Exit code: **0**. "Found 5 automation tests based on 'Kriging'"; all 5 ran
and passed:

- Kriging.Blueprint.AutoFitBuild — Success
- Kriging.Blueprint.AutoFitHeuristicFallback — Success
- Kriging.Blueprint.ExplicitBuildExactness — Success
- Kriging.Blueprint.IsoSurfaceExtraction — Success
- Kriging.Core.PortableSmoke — Success

Same category of non-fatal noise as Gate 3 (Fab/EOS login, missing PIX
capturer, `/Temp` package-path warnings from the default map).

Result: **PASS** (5/5).

## Summary

| Gate | Engine | Result |
|---|---|---|
| UBT BuildPlugin | 5.5 | PASS |
| UBT BuildPlugin | 5.4 | PASS |
| NullRHI automation (Kriging.* x5) | 5.5 | PASS (5/5) |
| NullRHI automation (Kriging.* x5) | 5.4 | PASS (5/5) |

All four gates are green on both engine versions after the
degenerate-triangle guard was added to `KrigePortableMarchingCubes.cpp`.
No source-level regressions were observed; no repo fix was needed to pass
either `BuildPlugin` run this time.

## Limitations / caveats carried forward

- This run intentionally did not repeat the engine-free gate
  (`Tests/Review/run_verification.py`) — see the note above; scope here was
  the four Unreal-engine gates only. The engine-free gate's evidence file
  (`Tests/Results/VERIFICATION.txt`/`.json` and friends) is currently absent
  from the working tree because that script clears `Tests/Results/` on each
  invocation and was last run before this remediation's test/doc changes
  landed; re-establishing that evidence is a separate task from this one.
- Automation runs used plugin `Binaries`/`Intermediate` copied straight from
  the corresponding Gate 1/2 `BuildPlugin` package output rather than having
  UBT compile the host project itself — same rationale as the previous run
  (a source-only plugin fails to load under `-unattended -nullrhi`, which
  does not prompt for a compile).
- `bFlipWinding` on `ExtractIsoSurfaceToProceduralMesh` remains unverified by
  any gate here — NullRHI has no rasterizer to show a mesh rendering
  inside-out. Carried forward from the previous run, still open.
- numpy/scipy version-pin caveat from the previous run (`requirements.txt`)
  is not re-verified here since the engine-free gate was not re-run this
  session.
- All other caveats from the prior `ENGINE_GATES.md` (Matern/Power
  variogram range-multiplier approximation, `ExternalDrift` not exposed in
  Blueprint, single-structure-only `FKrigingVariogramSpec`, the
  `Kriging.uplugin` `ProceduralMeshComponent` dependency addition) are
  unchanged by this remediation pass and remain as previously documented.
