# Kriging Plugin Sample Datasets

This folder contains three deterministic sample datasets and a generator script for the free Unreal kriging plugin. All datasets are reproducible: run `generate_samples.py` with the included seeds to regenerate identical CSVs.

---

## Dataset 1: Ore Body Assays (3D)

**File:** `ore_body_3d.csv`  
**Type:** Synthetic mining exploration data (3D volumetric)

### Description
Simulated drillhole assay samples from a geologically plausible ore body with two overlapping mineralized shells and background noise. Drillholes are clustered as vertical strings (8 holes, ~50 samples each) rather than randomly scattered, mimicking real exploration drilling.

### Columns
| Column | Type | Units | Notes |
|--------|------|-------|-------|
| X | float | meters | Easting (local grid) |
| Y | float | meters | Northing (local grid) |
| Z | float | meters | Depth below surface |
| Grade | float | g/t (grams per tonne) | Ore grade; lognormal-like background |

### Observed Statistics (Seed=42)
- **Total samples:** 395
- **Grade (g/t):**
  - Min: 0.010, Max: 6.615
  - Mean: 0.687, Std: 0.721
  - 50th %ile (median): 0.555
  - 75th %ile: 0.911
  - 90th %ile: 1.393
- **Domain extent:** X ∈ [195.0, 254.9], Y ∈ [296.2, 454.5], Z ∈ [0.2, 245.4] meters
- **High-grade extent (Grade ≥ 1.0 g/t):** X ∈ [196.5, 254.6], Y ∈ [297.1, 453.6], Z ∈ [4.8, 245.0] meters (86 samples)

### Sample Structure
- **Background grade** (~0.5 g/t, typical): samples throughout domain
- **Mineralized shells:** Two overlapping zones with elevated grades concentrated in the central drillhole cluster (X ∈ [196–255], Y ∈ [297–454])

This structure is ideal for testing **isosurface extraction** to visualize the ore envelope at a specific cutoff grade.

### Suggested Recipe: Ore Shell Visualization

1. **Fill a blueprint array** with the CSV data (UE DataTable import or manual parsing; see "CSV Import" note below)
   - Each row becomes a `FKrigingSamplePoint` with `Location` (FVector of X, Y, Z) and `Value` (Grade)

2. **Build the model:** Call `BuildKrigingModelAuto` (displayed as **"Build Kriging Model (Auto-Fit)"** in Blueprint)
   - Input: your sample array, default `FKrigingSettings` (Method = Ordinary, bPlanar = false for 3D)
   - Output: a `UKrigingModel` and a `FKrigingBuildResult` (check `bSuccess` and `Warnings`)

3. **Extract the ore shell at a grade cutoff:** Call `ExtractIsoSurface`
   - **IsoValue:** Use **1.0 g/t** (between background 0.687 and shell 1.393 at 90th %ile)
   - **Box:** Axis-aligned bounds (190, 290, 0) to (260, 460, 250) in world space (data-derived extent)
   - **Resolution:** (64, 64, 64) or finer depending on performance budget
   - Outputs: vertices, triangle indices, normals ready for visualization
   - ⚠️ **Note:** Isosurface extraction is **synchronous** and can stall the editor for several seconds at high resolutions (e.g., 128³). Start with (64, 64, 64) and increase iteratively.

4. **Render the mesh:** Feed vertices, triangles, and normals into a `UProceduralMeshComponent` via `CreateMeshSection`, or use the convenience node `ExtractIsoSurfaceToProceduralMesh` directly

**Expected result:** A 3D surface with **several disconnected blobs** (each representing a vertical drillhole string where grade ≥ 1.0 g/t) — this is correct behavior and reflects the sparse, clustered nature of real exploration drilling. Near-surface samples and sparse inter-hole regions appear as isolated or thinly-connected fragments, not a continuous solid.

---

## Dataset 2: Playtest Death Events (2D)

**File:** `playtest_deaths_2d.csv`  
**Type:** Synthetic game telemetry data (2D planar)

### Description
Simulated player death events on a 2D game map (2000×2000 pixels), with several mechanical "hotspots" (high-difficulty zones) superimposed over a sparse background. Useful for identifying danger zones and determining where additional playtesting is needed.

### Columns
| Column | Type | Units | Notes |
|--------|------|-------|-------|
| X | float | pixels | Horizontal map coordinate |
| Y | float | pixels | Vertical map coordinate |
| Deaths | int | count | Number of player deaths at this location |

### Observed Statistics (Seed=123)
- **Total events:** 155
- **Deaths per event:**
  - Min: 0, Max: 26
  - Mean: 6.18, Std: 7.23
  - 50th %ile (median): 2.0
  - 75th %ile: 11.0
  - 90th %ile: 18.0
- **Actual data extent:** X ∈ [6.4, 1892.2], Y ∈ [11.8, 1963.2] (sparse coverage within 2000×2000 map)

### Hotspot Locations (Embedded)
- **(400, 400):** Platformer / precision section → ~25 avg deaths
- **(1400, 600):** Hazard/lava zone → ~20 avg deaths
- **(800, 1500):** Ambush encounter → ~18 avg deaths
- **(1600, 1600):** Boss arena → ~22 avg deaths

### Suggested Recipe: Danger Heatmap and Coverage Analysis

1. **Fill a blueprint array** with CSV data (converted to 3D sample points for kriging)
   - Each row: `FKrigingSamplePoint` with `Location` = (X, Y, **Z=0**) and `Value` = Deaths count

2. **Build a planar model:** Call `BuildKrigingModelAuto`
   - Input: sample array, **`FKrigingSettings` with `bPlanar = true`** (tells kriging to treat this as 2D surface data)
   - Method = Ordinary, other defaults OK

3. **Generate a danger heatmap:** Call `EvaluateGrid`
   - **Box:** (0, 0, **-1**) to (2000, 2000, **1**) — a thin slab in Z (kriging ignores Z when bPlanar=true, but `EvaluateGrid` requires Z extent)
   - **Resolution:** (64, 64, **2**) — each axis must be ≥ 2 per the API constraint
   - Output: a flat array of estimated death counts at each grid point
   - Convert output array to a 2D texture or heatmap visualization

4. **Identify uncertainty / coverage gaps (workaround):** 
   - `EvaluateGrid` returns **estimates only**, not uncertainty — the grid-based variance output does not exist in the shipped Blueprint surface.
   - For **coverage gaps**, compute distance-to-nearest-sample at each grid point: sparse regions (far from any event) are areas needing more playtesting.
   - Alternatively, use the model's `SampleValueWithUncertainty(Location, OutStdDev)` method **per-query** on a few key points (Blueprint node: `SampleValueWithUncertainty` on the model), which **does** return the standard deviation of the estimate.

**Expected result:** A smooth 2D heatmap showing estimated death intensity across the map, with peaks at the four hotspots and low values in safe zones.

---

## Dataset 3: Temperature with Time Dimension (4D)

**File:** `temperature_timesteps.csv`  
**Type:** Synthetic sensor network data (3D space + time)

### Description
Hourly temperature readings from a fixed grid of 80 sensors over 6 hours, with a warm front moving across the domain (simulating, e.g., weather systems or thermal diffusion). Demonstrates kriging's ability to interpolate both in space and time by treating time as a 4th coordinate.

### Columns
| Column | Type | Units | Notes |
|--------|------|-------|-------|
| X | float | meters | Sensor easting |
| Y | float | meters | Sensor northing |
| Z | float | meters | Sensor height above ground |
| Hour | int | hours (0–5) | Time index; treat as a coordinate, not metadata |
| TempC | float | °C | Temperature reading |

### Observed Statistics (Seed=456)
- **Total readings:** 480 (80 sensors × 6 hours)
- **Sensor grid:** 10 × 8 × 1 fixed locations (XYZ repeats identically for each hour)
- **Temperature (°C):**
  - Min: 12.03, Max: 32.03
  - Mean: 21.18, Std: 4.95
  - 50th %ile (median): 20.62
  - 75th %ile: 24.87
  - 90th %ile: 28.60
- **Spatial extent:** X ∈ [0, 400], Y ∈ [0, 400], Z ∈ [0, 0] meters (all sensors at ground level)
- **Temporal extent:** Hour ∈ [0, 5]

### Physical Structure
- **Warm front motion:** Starts at x = −100 at hour 0, moves rightward at ~80 m/hr
- **Front profile:** Gaussian, ~150m wide; adds up to ~12°C at the peak
- **Diurnal cycle:** Sinusoidal, ~±3°C variation over the 6-hour window
- **Spatial gradient:** Slight temperature increase with height and northing

### Suggested Recipe: Per-Timestep Models and Cross-Time Queries

**Option A: Build one model per hour (recommended for initial tests)**

1. **Filter samples by hour:** For each hour h ∈ [0, 5], extract all samples where Hour = h
   - 80 samples per hour (one from each sensor)

2. **Build a spatial model for each hour:** Call `BuildKrigingModelAuto` six times
   - Input: 80 samples (X, Y, Z, TempC) at hour h
   - Settings: Method = Ordinary, bPlanar = false (3D volumetric)

3. **Sample a point across all models:** E.g., query location (200, 200, 50) for each hour's model
   - Call `SampleValue(FVector(200, 200, 50))` on model_hour_0, model_hour_1, ..., model_hour_5
   - Output: 6 temperature estimates showing the warm front passing over that point

4. **Visualize the time series:** Plot the 6 values as a line graph; observe the sharp rise as the front arrives

**Option B: Combine space + time into a single 4D model (advanced)**

1. **Create a 4D sample array:** Treat each (X, Y, Z, Hour) as a 4D point
   - Augment each sample to: `FKrigingSamplePoint{ Location = (X, Y, Z, Hour) as custom 4D, Value = TempC }`
   - ⚠️ Note: `FVector` is only 3D, so a true 4D model requires custom C++ integration or manual coordinate encoding

**Expected result (Option A):** A temperature curve showing the warm front's passage: low at hour 0, peak around hour 2–3, decline by hour 5.

---

## CSV Import into Unreal Engine

### DataTable Route (Standard in UE)
Unreal's built-in **CSV → DataTable** importer is the recommended path for users unfamiliar with manual array construction. However:

⚠️ **Important:** Unreal's CSV importer treats the **first column as row identifiers** (unique row names), not data columns. Our CSVs do not include a row-name column.

**Solution:**
- **Prepend a row-name column** before importing:
  - Add a new first column with unique identifiers (e.g., 0, 1, 2, ..., N−1)
  - Save as, e.g., `ore_body_3d_for_datatable.csv`:
    ```
    RowName,X,Y,Z,Grade
    0,200.578,301.263,0.152,0.060
    1,199.059,298.722,4.480,0.850
    ...
    ```
- **Import into DataTable:** In UE Content Browser, right-click → Create Data Asset → DataTable
  - Select `Struct` = your custom struct (e.g., "KrigingSamplePoint" with Float X, Y, Z, Grade)
  - Reimport from the augmented CSV
- **Convert to array in Blueprint:** Drag the DataTable into your event graph, use **Get Data Table Row Names** → **Get Data Table Row** in a loop to build the sample array

### Manual Array Construction (No CSV importer needed)
If you prefer to avoid the DataTable step, construct `FKrigingSamplePoint` arrays directly in Blueprint:
- Loop over CSV rows (parsing string or reading from a file)
- For each row, create a new sample point with Location and Value
- Append to the array

---

## Build Stability & Data Quality Notes

### Auto-Fit Variogram Behavior
The `BuildKrigingModelAuto` function (displayed as **"Build Kriging Model (Auto-Fit)"** in Blueprint) automatically fits a variogram curve from your samples. 

**Drillhole data (ore_body_3d.csv):** 
- Samples are clustered vertically (typical of exploration drilling)
- Auto-fit is still reliable because variogram lags benefit from both close (downhole) and distant (cross-hole) sample pairs
- If auto-fit degrades to a fallback heuristic, check `FKrigingBuildResult.bDegraded` and read `Message` / `Warnings` for details

**Planar data (playtest_deaths_2d.csv):**
- Scattered but not deeply clustered; auto-fit is robust

**Temporal data (temperature_timesteps.csv per-hour):**
- Each hour's 80 samples are on a regular grid; auto-fit is very stable

### Node Display Names
When searching Blueprint node palettes, note that some kriging nodes have explicit display names in the header, while others use Unreal's auto-spacing of the C++ name:

| C++ Function | Display Name in Blueprint | Category |
|--------------|---------------------------|----------|
| `BuildKrigingModelAuto` | **"Build Kriging Model (Auto-Fit)"** | Kriging \| Build |
| `BuildKrigingModel` | **"Build Kriging Model (Explicit Variogram)"** | Kriging \| Build |
| `FitVariogram` | **"Fit Variogram From Samples"** | Kriging \| Build |
| `EvaluateGrid` | (auto-spaced) `Evaluate Grid` | Kriging \| Evaluate |
| `ExtractIsoSurface` | (auto-spaced) `Extract Iso Surface` | Kriging \| Evaluate |
| `ExtractIsoSurfaceToProceduralMesh` | (auto-spaced) `Extract Iso Surface To Procedural Mesh` | Kriging \| Evaluate |

When looking for uncertainty at a single point, use `SampleValueWithUncertainty` (Blueprint category: Kriging \| Model) on the `UKrigingModel` object after build.

---

## Running the Generator

To regenerate the CSVs with the same deterministic seed:

```bash
cd /path/to/Samples
python generate_samples.py
```

All three CSVs will be overwritten with identical data (seeds are hard-coded). Each dataset is suitable for immediate import into Unreal after the DataTable row-name prepend step.

---

## Troubleshooting

| Issue | Diagnosis | Solution |
|-------|-----------|----------|
| `ExtractIsoSurface` returns no mesh | IsoValue does not intersect the field in the given Box | Check the model's estimated value range with a test grid query; pick an IsoValue between min and max |
| `BuildKrigingModelAuto` returns bDegraded=true | Sample count very low (~< 20) or unusually clustered | Increase sample count or reduce cluster density; check Warnings string for specifics |
| `EvaluateGrid` fails "resolution must be >= 2 per axis" | Used a 1D or 2D resolution vector (e.g., 128, 128, 1 for 2D) | Ensure all three axes are ≥ 2, e.g., (128, 128, 2) for thin slabs |
| `SampleValueWithUncertainty` returns OutStdDev = 0 at a sample point | NuggetMode = Exact and no measurement noise in the model | This is expected; the kriging estimate has zero uncertainty directly on a measured sample |

---

## Summary

- **ore_body_3d.csv:** 395 drillhole assays → isosurface demo (ore cutoff at 1.0 g/t)
- **playtest_deaths_2d.csv:** 155 death events → heatmap + coverage analysis (use single-point queries for per-location uncertainty)
- **temperature_timesteps.csv:** 480 hourly readings (80 sensors × 6 hours) → temporal interpolation (one model per hour or advanced 4D integration)

All recipes assume the shipped Blueprint nodes and typed structures. For questions or feature requests, refer to the plugin documentation or source.
