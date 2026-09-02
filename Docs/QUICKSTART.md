# Kriging Quickstart: Blueprint Guide

## What kriging does (in 60 seconds)

You have ~50 scattered sample points on your game map—death locations from playtesting, resource spawns, climate readings. Kriging turns that tiny dataset into a smooth, continuous field across the entire map. Every point becomes an interpolated value **plus** a confidence (standard deviation) score. That confidence tells you where you have data gaps.

Think: resource placement with confidence; fog-of-war uncertainty visualization; interpolated weather fields from sparse weather stations; terrain smoothing from hand-placed control points.

## Installation

Copy the `Kriging` folder into your Unreal project's `Plugins/` directory.

**Engine compatibility:** UE 5.4–5.5 compilation via UnrealBuildTool (`BuildPlugin`) and NullRHI automation (5/5 `Kriging.*` tests) have both PASSED, re-verified after the final marching-cubes fix — see `Tests/Results/ENGINE_GATES.md` for the authoritative record. UE 5.6–5.8 remain untested.

## The workflow: points → model → query/grid/isosurface

### 1. Gather your sample points

```cpp
TArray<FKrigingSamplePoint> Samples;
Samples.Add({
    {100.0, 200.0, 0.0},  // Location (cm)
    45.0                   // Value (any units)
});
// ... add more points (typically 20+ for reliable fits)
```

### 2. Build the model (two paths)

**Recommended: Auto-fit** — Kriging learns the variogram from your samples:

```cpp
FKrigingSettings Settings;
FKrigingBuildResult Result;

UKrigingModel* Model = UKrigingLibrary::BuildKrigingModelAuto(Samples, Settings, Result);
if (!Result.bSuccess) {
    UE_LOG(LogKriging, Warning, TEXT("Build warning: %s"), *Result.Message);
}
```

Auto-fit computes an empirical variogram and fits Spherical/Exponential/Gaussian shapes (keeps the best). If it can't find a reliable fit with your sample count, it falls back to a heuristic variogram (range = 1/3 your sample extent, sill = sample variance, no nugget) and warns you via `Result.Warnings`.

**Advanced: Explicit variogram** — You specify shape, range, sill, and nugget:

```cpp
FKrigingVariogramSpec Variogram;
Variogram.Shape = EKrigingVariogramShape::Exponential;
Variogram.EffectiveRange = 500.0;  // cm: distance where influence effectively ends
Variogram.Sill = 1.0;              // total plateau (variance of your data)
Variogram.Nugget = 0.1;            // local noise

FKrigingSettings Settings;
FKrigingBuildResult Result;
UKrigingModel* Model = UKrigingLibrary::BuildKrigingModel(Samples, Variogram, Settings, Result);
```

**What's EffectiveRange?** Distance at which spatial correlation becomes negligible (~95% decay). All values are in your coordinate space (cm in Unreal). Spherical reaches its plateau exactly at this distance; Exponential and Gaussian approach it asymptotically but are treated as exhausted at this distance for practical purposes.

### 3. Query: sample anywhere on the field

```cpp
const FVector QueryLocation = {250.0, 350.0, 0.0};
double Value = Model->SampleValue(QueryLocation);

// For confidence: standard deviation of the estimate
double StdDev = 0.0;
Model->SampleValueWithUncertainty(QueryLocation, StdDev);
// StdDev = prediction uncertainty at this location (0 near samples, larger in gaps)
```

**The confidence map is the power move here.** StdDev = 0 exactly at sample locations (in exact nugget mode). StdDev grows where data is sparse. Use this to:

- Adjust NPC behavior (high StdDev = uncertain decision)
- Weight heatmap colors (high StdDev = transparent/faded)
- Flag areas needing more samples before trusting the interpolation

### 4. Evaluate on a grid or extract an isosurface (optional)

**Bake to a grid** (evaluate on a regular lattice):

```cpp
FIntVector Resolution = {128, 128, 128};
FBox SampleBox = FBox(FVector(0, 0, 0), FVector(1000, 1000, 1000));
TArray<double> GridValues;

if (UKrigingLibrary::EvaluateGrid(Model, SampleBox, Resolution, GridValues)) {
    // GridValues is a flat array: index = X + Y*ResX + Z*ResX*ResY
    // You can now store in a texture volume or sparse structure for fast runtime lookups
}
```

**Extract an isosurface** (marching cubes mesh at a constant value):

```cpp
TArray<FVector> Vertices, Normals;
TArray<int32> Triangles;

if (UKrigingLibrary::ExtractIsoSurface(Model, IsoValue, SampleBox, Resolution, 
    Vertices, Triangles, Normals)) {
    // Mesh extracted; feed to ProceduralMeshComponent or write to static mesh
}

// Or directly to a ProceduralMeshComponent:
UKrigingLibrary::ExtractIsoSurfaceToProceduralMesh(
    Model, IsoValue, SampleBox, Resolution, 
    TargetComponent, SectionIndex, bCreateCollision, bFlipWinding);
```

## Typical use cases

### Heat maps (playtesting data)
- Sample = death location + frame
- Evaluate on a grid and render as a heatmap texture
- StdDev shows where deaths were clustered vs. spread out

### Resource distribution (procedural)
- Sample = hand-placed ore veins or resource concentrations
- Kriging learns the spatial pattern from your samples
- Query: how much ore *should* be here? Generate resources accordingly

### Terrain smoothing
- Sample = hand-placed elevation control points
- Kriging → smooth heightfield
- Feed to Landscape or a custom mesh material

### Climate fields
- Sample = weather station readings (temperature, humidity)
- Interpolate across the map every tick/turn
- StdDev tells you where the next weather station should be placed

## Using uncertainty (StdDev) in gameplay

```cpp
double Value = 0.0, StdDev = 0.0;
Model->SampleValueWithUncertainty(QueryLocation, StdDev);

// StdDev is in the same units as your data.
// If your samples are temperatures (°C), StdDev is in °C (not squared).

// Confidence interval (roughly 68% confidence):
double ConfidenceLow = Value - StdDev;
double ConfidenceHigh = Value + StdDev;

// Use it in UI or gameplay:
Color.A = FMath::Clamp(1.0 - StdDev / MaxStdDev, 0.0, 1.0); // fade uncertain regions
```

## Transforms: Log and normal-score

If your data is skewed (e.g., ore grades are log-distributed), tell kriging to work in log space:

```cpp
FKrigingSettings Settings;
Settings.Transform = EKrigingTransform::Logarithmic;  // kriging on log(value)
// Returned Value is already back-transformed to original units
// StdDev remains in log-space units
```

Normal-score transform: kriging on rank-order, then back-transform to original quantiles. Useful if your data have extreme outliers. Set `Settings.Transform = EKrigingTransform::NormalScore`.

## Next steps

- **Async building:** For larger sample sets, use `BuildKrigingModelAsync` to build on a background thread without stalling your game tick.
- **Cross-validation:** Check `Result.bHasCrossValidation` and `Result.CrossValidationRMSE` to gauge model fit quality.
- **3D workflows:** The model handles XYZ coordinates directly; no special setup needed for volumetric data.
- **Learn more:** `GEOSTATS_PRIMER.md` explains variograms in plain language. `PROFESSIONAL_USE.md` covers validation, 3D workflows, and industry terminology.

---

## Blueprint API reference

Key types and functions:

- `FKrigingSamplePoint` — a (Location, Value) pair
- `FKrigingVariogramSpec` — describes spatial correlation (Shape, EffectiveRange, Sill, Nugget)
- `FKrigingSettings` — kriging method (Ordinary/Simple/Universal), transform, and tuning options
- `UKrigingModel` — the built model; call `SampleValue()` / `SampleValueWithUncertainty()` / `IsValid()`
- `BuildKrigingModelAuto()` — one-node auto-fit path (recommended)
- `BuildKrigingModel()` — explicit variogram path (advanced)
- `BuildKrigingModelAsync()` — background-thread build with completion callback
- `EvaluateGrid()` — evaluate on a regular lattice for baking/visualization
- `ExtractIsoSurface()` / `ExtractIsoSurfaceToProceduralMesh()` — marching cubes mesh extraction

---

**Have a question?** Check `FAQ.md`.
