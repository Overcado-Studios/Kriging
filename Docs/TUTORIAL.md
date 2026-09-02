# Kriging Plugin Tutorial: Click-by-Click

This is the hands-on, do-this-exact-thing companion to `QUICKSTART.md`. QUICKSTART explains
the concepts and the C++ shape of the API; this document assumes you know kriging cold but
have never used Unreal Engine, and walks every click, menu, and node.

You'll build up through five levels, each ending in something you can see on screen. Every UE
term is defined the first time it shows up. If a step says "drag from pin A to pin B," that
means: click and hold on the little colored dot (pin) on the first node, drag to the pin on the
second node, and release — a wire will connect them.

**Your machine, as assumed by this tutorial:**
- Windows, Unreal Engine 5.5 and 5.4 both installed via the Epic Games Launcher
  (`C:\Program Files\Epic Games\`)
- Visual Studio 2022 already installed (needed once, automatically, in Level 0)
- Plugin source at `C:\Users\HomePC\Documents\ClaudeCode\kriging`

We'll use UE **5.5** throughout. Everything here also works on 5.4 (see Level 4).

---

## Before you start: five words you'll see constantly

- **Content Browser** — the panel (usually bottom of the editor) that lists all your project's
  assets (Blueprints, materials, data tables, etc.), organized like a file browser.
- **Blueprint** — Unreal's visual scripting system. Instead of typing code, you place *nodes*
  (boxes representing a function or event) and connect them with wires. A "Blueprint Class" is
  an asset that holds a graph of these nodes plus any variables/components you add.
- **Node** — one visual "step" in a Blueprint graph — a function call, an event, a variable
  getter, etc. Rendered as a box with input pins on the left and output pins on the right.
- **Pin** — a small colored connector on a node's edge. Data-type pins are color-coded (e.g.
  double/float pins are green-ish, boolean pins are red, object-reference pins are blue).
  Wires only connect compatible pin types (Unreal will auto-insert a small conversion node for
  some compatible mismatches, like `int32` → `double`).
- **Compile** — the "Compile" button (top toolbar of the Blueprint editor) that checks your
  graph for errors and turns it into something runnable. Do this before pressing Play whenever
  you've changed a graph. **Save** (Ctrl+S) writes it to disk — compiling does not save it.

---

## LEVEL 0 — Setup

**Goal:** a new Unreal project with the Kriging plugin installed and enabled.
**Time:** ~10 minutes (plus a few minutes of unattended compiling).

### 0.1 Launch the editor and create a project

1. Open the **Epic Games Launcher**, go to the **Unreal Engine** tab → **Library**, and click
   **Launch** next to the **5.5** engine version. This opens the Unreal Project Browser.
2. In the Project Browser, select the **Games** category (left side), then pick the **Blank**
   template (a plain empty template with no gameplay content — the correct choice for testing a
   plugin in isolation).
3. On the right-hand "Project Defaults" panel, set:
   - **Blueprint or C++**: choose **Blueprint**. (You don't need C++ for anything in this
     tutorial — all plugin functionality is exposed to Blueprint.)
   - **Target Platform / Quality Preset / Ray Tracing**: leave at their defaults — none of this
     matters for testing kriging.
   - **Starter Content**: set to **No Starter Content**. You don't need the example furniture
     and material assets; a smaller project opens faster.
4. At the bottom, set the **project location** (a folder on disk, e.g.
   `C:\Users\HomePC\Documents\Unreal Projects\`) and the **project name**, e.g.
   `KrigingTest`. Click **Create**.
5. The editor will open after a short load. Your project now physically exists at
   `<location>\KrigingTest\` — for example
   `C:\Users\HomePC\Documents\Unreal Projects\KrigingTest\`. That folder contains
   `KrigingTest.uproject`, a `Content\` folder, `Config\`, etc. Remember this path — you'll add
   a folder next to `Content\`.
6. **Close the editor** (File → Exit, or the window's X button). You need the editor closed
   while you copy plugin files in, because Windows can lock files that the editor has open.

### 0.2 Copy the plugin in

1. In Windows Explorer, navigate to your project folder (e.g.
   `C:\Users\HomePC\Documents\Unreal Projects\KrigingTest\`).
2. Create a new folder here named exactly `Plugins` (sitting next to `Content`, `Config`,
   `KrigingTest.uproject`). If Unreal already created one, reuse it.
3. Inside `Plugins`, create a new folder named `Kriging`.
4. From `C:\Users\HomePC\Documents\ClaudeCode\kriging` (the plugin source), copy these into the
   `Plugins\Kriging\` folder you just made:
   - `Kriging.uplugin` (required — this is the file that tells Unreal "a plugin lives here")
   - the whole `Source\` folder (required — contains `KrigingCore\` and `KrigingBlueprint\`,
     the actual code)
   - Optionally `README.md` and `LICENSE` for reference — everything else at the top level
     (`Docs\`, `Tests\`, `MANIFEST.sha256`, `ThirdPartyNotices\`) is developer/reference
     material for the plugin's own repo and Unreal doesn't need it; skip it to keep the copy
     smaller.

   When you're done, `KrigingTest\Plugins\Kriging\Kriging.uplugin` should exist, and
   `KrigingTest\Plugins\Kriging\Source\KrigingBlueprint\Public\KrigingLibrary.h` should exist.
   (The nesting depth matters — see Troubleshooting below.)

### 0.3 Reopen the project and let it compile

1. Double-click `KrigingTest.uproject` (or relaunch from the Epic Games Launcher and select
   **Recent Projects**).
2. Because the plugin ships as source code (not precompiled binaries), Unreal will detect that
   two modules — `KrigingCore` and `KrigingBlueprint` — don't have compiled binaries yet, and
   show a dialog along the lines of:

   > *"KrigingTest could not be compiled. Would you like to rebuild it now?"*

   Click **Yes**.
3. What happens next: Unreal invokes its build tool (UnrealBuildTool) behind the scenes, which
   in turn needs a C++ compiler — this is what Visual Studio 2022 provides on this machine (it's
   already installed, so you don't need to do anything else). A console window will appear
   showing compile progress. **This takes a few minutes** the first time (it's compiling the
   plugin's numerical core and its Blueprint wrapper from scratch). Let it finish; don't close
   the console window.
4. When compiling succeeds, the editor opens normally.

### 0.4 Verify the plugin is enabled

1. In the menu bar, go to **Edit → Plugins**.
2. In the plugin browser's search box, type `Kriging`.
3. You should see one entry. Note: its display name is **"Kriging Core"** (not "Kriging") —
   that's the plugin's `FriendlyName`, don't be thrown by it. Confirm its checkbox is **checked**
   (enabled). If it's unchecked, check it — the editor will ask to restart; let it.

**Success state:** the editor is open, no error dialogs remain, and Edit → Plugins shows
"Kriging Core" checked under installed plugins.

> **Troubleshooting — Level 0**
> - **Plugin doesn't show up in Edit → Plugins at all:** almost always a folder-nesting
>   mistake. Unreal looks for `Plugins\<AnyName>\<PluginName>.uplugin` — one level of folder,
>   then the `.uplugin` file directly inside it. If you copied the source folder itself (so you
>   ended up with `Plugins\Kriging\kriging\Kriging.uplugin`, an extra level) it won't be found.
>   Check that `Kriging.uplugin` sits directly inside `Plugins\Kriging\`.
> - **Plugin shows up but is filtered out of view / listed oddly:** `Kriging.uplugin` declares
>   `"IsBetaVersion": true`, so the plugin browser may bucket it under a "Beta"/"Experimental"
>   category filter, or show a small "Beta" tag next to its name, instead of listing it plainly
>   alongside stable plugins. That's expected — it's still the right plugin, just labeled as
>   beta; enable it the same way.
> - **Rebuild fails / errors in the console window:** the console window remains open with the
>   full compiler output on failure — scroll up to the first `error C####` or `error :` line,
>   that's the real cause; everything after it is usually noise. If you don't get that far and
>   instead see a generated `KrigingTest.sln` open in Visual Studio, that's normal too — you can
>   build from there instead (Build → Build Solution) and then reopen the `.uproject`.
> - **"Missing modules" prompt never appears, project just fails to open:** try right-clicking
>   `KrigingTest.uproject` → **Generate Visual Studio project files**, then open the generated
>   `.sln` in Visual Studio 2022 and build the `Development Editor` configuration for `Win64`,
>   then reopen the `.uproject` normally.

---

## LEVEL 1 — First kriging result with zero data import

**Goal:** run a kriging model entirely from hardcoded numbers, with no CSV, no DataTable — just
to prove the plugin works end to end.
**Time:** ~15 minutes.

### 1.1 Create an Actor Blueprint

An **Actor** is Unreal's base class for "a thing that can exist in a level" (a placeable object).
An **Actor Blueprint** is a Blueprint Class built on top of Actor — the standard way to write a
small piece of self-contained test logic you can drop into a level.

1. In the **Content Browser** (bottom panel), right-click empty space in the file list area.
2. Choose **Blueprint Class** from the context menu.
3. In the "Pick Parent Class" dialog, pick **Actor** (it's near the top of the common list).
4. A new asset appears, its name box already in "rename" mode — type `BP_KrigingLevel1` and
   press Enter.
5. **Double-click** it to open the Blueprint editor.

### 1.2 Find the Event Graph and the BeginPlay event

The **Event Graph** is the tab (usually already open, in the middle of the Blueprint editor)
where you wire up logic in response to events. You should see a node already there named
**Event BeginPlay** — a red-bordered node that fires once, when this actor starts existing in a
running level. If you don't see it: on the left "My Blueprint" panel, under **Graphs**, make sure
`EventGraph` is open, and if there's no BeginPlay node, right-click on empty graph space, type
`begin play` into the search box that pops up, and select **Event BeginPlay**.

Everything below connects, in sequence, from this node's white output pin (the "execution" pin —
white arrow-shaped pins carry *order of execution*, as opposed to colored pins which carry
*data*).

### 1.3 Build the hardcoded sample array

We'll build 6 points on a flat 1000×1000 unit square (units are centimeters — Unreal's native
unit — but for this test they're just numbers; nothing here depends on real-world scale). The
values are chosen so the *middle* of the square has an obvious, checkable answer:

| # | Location (X, Y, Z) | Value |
|---|---------------------|-------|
| 1 | (0, 0, 0)       | 0   |
| 2 | (1000, 1000, 0) | 0   |
| 3 | (1000, 0, 0)    | 100 |
| 4 | (0, 1000, 0)    | 100 |
| 5 | (500, 0, 0)     | 50  |
| 6 | (500, 1000, 0)  | 50  |

Why these numbers: this layout is symmetric under **two independent mirror reflections** through
the query point at the center, `(500, 500)`:

- **Reflection about the line y = 500** (flipping top/bottom): `(x, y) → (x, 1000 − y)`. This
  swaps point 1 ↔ point 4, point 2 ↔ point 3, and point 5 ↔ point 6.
- **Reflection about the line x = 500** (flipping left/right): `(x, y) → (1000 − x, y)`. This
  swaps point 1 ↔ point 3 and point 2 ↔ point 4, and leaves points 5 and 6 fixed in place (they
  already sit on that line).

Check both against the table above: each reflection maps the *set of six sample locations* onto
itself, and leaves the query point at the center fixed. The values deliberately do **not** stay
put — the y = 500 flip sends point 1 (value 0) onto point 4's location (value 100) — and that's
fine: only the *locations* need to be symmetric, because ordinary kriging weights are solved from
geometry alone (sample-to-sample and sample-to-query distances, which here are plain Euclidean
distances because auto-fit leaves anisotropy at its isotropic default — `StretchY = StretchZ = 1`
in `FKrigingAnisotropySpec`), never from the values. A
symmetry of the geometry therefore forces the corresponding weights to be equal. Combining what
each reflection pins down: the first reflection alone gives you two symmetric pairs among the
corners (1↔4, 2↔3) plus 5↔6; the second reflection alone gives two different symmetric pairs among
the corners (1↔3, 2↔4). Applying both together forces *all four* corner weights into a single
group — `w1 = w2 = w3 = w4 = a` — while `w5 = w6 = b` (already forced by either reflection).
Ordinary kriging also always constrains its weights to sum to 1 (this is the constraint that
makes the estimate unbiased without needing to know the true mean in advance), so `4a + 2b = 1`
here. The estimate at the center is then
`(0)·a + (0)·a + (100)·a + (100)·a + (50)·b + (50)·b = 200a + 100b = 100·(2a + b) = 50`
— exactly 50, regardless of what `a` and `b` individually turn out to be, and therefore regardless
of the exact variogram auto-fit ends up choosing. (This depends on `Method = Ordinary`, the
default we're using — a different method wouldn't necessarily preserve the `Σw = 1` constraint in
the same way.)

Now build it in Blueprint:

1. Right-click on open graph space and search for **Make Array**. Place it. Right now it holds
   one empty element.
2. Right-click the array's single element pin and choose **Add Pin** five times, so the node has
   6 element pins (labeled `[0]` through `[5]`).
3. For each of the 6 pins, right-click on empty graph space and search for **Make
   KrigingSamplePoint** (this is Unreal's auto-generated "construct a struct from its fields"
   node for the `FKrigingSamplePoint` struct — it will have two inputs, `Location` and `Value`).
   Place 6 of these.
4. On each **Make KrigingSamplePoint** node, fill in the `Location` fields (X, Y, Z — click
   directly into the number boxes on the node to type values) and the `Value` field, matching
   the table above.
5. Drag a wire from each **Make KrigingSamplePoint** node's output pin into one of the **Make
   Array** node's 6 element input pins.

You now have one output pin, off **Make Array**, carrying `TArray<FKrigingSamplePoint>` with all
6 points.

### 1.4 Build the model with auto-fit

1. Right-click on graph space, search for `Build Kriging Model`. You'll see **Build Kriging
   Model (Auto-Fit)** — that's the display name for `UKrigingLibrary::BuildKrigingModelAuto`.
   (If nothing shows up, see the "context sensitive" note in Troubleshooting below.) Place it.
2. Drag from **Make Array**'s output pin to this node's `Samples` input pin.
3. Leave the `Settings` input at its default (right-click the pin → nothing needed; an
   unconnected struct pin just uses the struct's default-constructed value, which here means
   `Method = Ordinary`, `Transform = None`, `bPlanar = false` — all correct for this test; you
   don't need to wire anything into it).
4. Connect this node's white execution pin from **Event BeginPlay**'s output execution pin.
5. This node has two outputs: the return value (a `UKrigingModel*`, i.e. a reference to the
   built model) and an output parameter pin named `OutResult` (an `FKrigingBuildResult` struct).
   You don't strictly need `OutResult` for this test, but it's good practice: right-click it →
   **Split Struct Pin** to expose `bSuccess`, `Message`, etc. individually if you want to
   inspect them later. For now we'll skip branching on it.

Because 6 samples is well below the "roughly 20+" the header recommends for a reliable auto-fit,
expect this to fall back to the documented heuristic variogram (range = ⅓ of the sample extent,
sill = sample variance, no nugget) and report that in `OutResult.Warnings` — this is expected and
does **not** mean the test failed; `bSuccess` should still be `true` and the model still usable.

### 1.5 Query the model twice: at a sample point, and at the center

You'll place **two** `Sample Value With Uncertainty` nodes — one query at a sample point, one at
the center — and print **two lines per query** (the value, and the uncertainty), so you can see
both halves of the exactness claim on screen: four Print Strings total, firing in a fixed order.

1. Right-click, search `Sample Value With Uncertainty`, place it. Drag a wire from the model
   output pin (the return value of the build node) to this node's target/self pin (the pin on
   its left edge, usually unlabeled or labeled by the class).
2. For the `Location` input, right-click it → you can type directly into the X/Y/Z boxes that
   appear on the pin itself once you expand it, or make a **Make Vector** node. Set it to
   `(0, 0, 0)` — sample point #1, which has `Value = 0`.
3. This node returns the estimate as its return value, *and* has a separate output pin
   `OutStdDev` (the uncertainty) — both need their own Print String, since a single Print String
   only takes one input. Right-click, search `Print String`, place two of them for this query:
   wire the node's return value into the first, and its `OutStdDev` pin into the second (Unreal
   auto-inserts a "double to string" conversion on both wires — accept it). Chain them in
   sequence off the node's execution pin: value first, then StdDev.
4. Place a second `Sample Value With Uncertainty` node, same model target, `Location = (500, 500,
   0)` — the exact center. Give it its own two Print Strings the same way (value, then StdDev).
5. Chain everything in one execution sequence: Build node → query-1 node → print value(0,0,0) →
   print StdDev(0,0,0) → query-2 node → print value(500,500,0) → print StdDev(500,500,0).

Compile (top toolbar) and Save (Ctrl+S).

### 1.6 Place it and press Play

1. Drag `BP_KrigingLevel1` from the Content Browser into the 3D viewport, anywhere — it doesn't
   need to be visible; `BeginPlay` fires regardless of whether the actor has a visible mesh.
2. Click **Play** (top toolbar, or Alt+P).
3. Open the **Output Log** if it's not already visible (Window → Output Log). `Print String`
   messages also appear as on-screen text in the top-left of the viewport for a few seconds by
   default, but the Output Log keeps a permanent, scrollable record — use that if you miss the
   on-screen text.

**What to expect exactly**, in order, four lines in the Output Log:
1. Value at `(0, 0, 0)`: **0.0** — kriging is *exact* at a sample location when `NuggetMode =
   Exact` (the default), i.e. it always reproduces the input value there.
2. StdDev at `(0, 0, 0)`: **0.0** — no uncertainty at a location you actually measured.
3. Value at `(500, 500, 0)`: very close to **50.0** (per the symmetry argument in 1.3).
4. StdDev at `(500, 500, 0)`: **greater than 0** — you're now asking about a point with no data,
   so there is genuine estimation uncertainty. The exact number depends on the auto-fitted (or
   heuristic-fallback) variogram parameters, so don't expect a specific value here — just confirm
   it's nonzero.

**Success state:** Output Log shows four lines in that order: `0.000000`, `0.000000`, a number
close to `50.000000`, and a fourth number greater than `0.000000`.

> **Troubleshooting — Level 1**
> - **Can't find "Build Kriging Model" or "Make KrigingSamplePoint" in the search:** the node
>   palette search is *context-sensitive* by default — it hides nodes it thinks aren't relevant
>   at the pin you dragged from. If you're searching from empty graph space this shouldn't be an
>   issue, but if it is, uncheck **Context Sensitive** (top-left checkbox of the search popup)
>   and search again.
> - **Play shows nothing in the Output Log:** confirm you actually dragged the Blueprint into the
>   viewport (an actor that exists only in the Content Browser never runs — it must be placed in
>   the level) and that you compiled and saved after your last edit. Also confirm the Output Log
>   window is open and its verbosity filter isn't hiding `Log`-level messages.
> - **Center query doesn't look like ~50:** double check every one of the 6 coordinate/value
>   pairs against the table in 1.3 — a single transposed coordinate breaks the symmetry the
>   argument depends on.

---

## LEVEL 2 — Death heatmap from CSV

**Goal:** import `playtest_deaths_2d.csv`, krige a 2D field over the whole map, and see the four
documented hotspots as visibly denser/hotter regions of debug points.
**Time:** ~30 minutes.

### 2.1 Prepare the CSV (row-name column)

Unreal's CSV → DataTable importer treats the **first column** as a unique row identifier, not
data. `playtest_deaths_2d.csv` (from the plugin's `Samples/` folder) has columns `X,Y,Deaths` —
no ID column — so it needs one prepended before Unreal will import it correctly.

1. Open `playtest_deaths_2d.csv` in Excel (or any spreadsheet/text editor).
2. Insert a new first column. Header it `RowName`. Fill it with sequential integers `0, 1, 2, …`
   down to the last data row (155 death events total, so row names `0` through `154`).
3. Save it as a new file, e.g. `playtest_deaths_2d_dt.csv`, keeping CSV format. The first three
   lines should now look like:
   ```
   RowName,X,Y,Deaths
   0,1364.703726496287,11.780066634939468,0
   1,107.64203760444535,1512.9579402919144,1
   ```

### 2.2 Define the row struct

The DataTable importer needs a Blueprint struct whose member names match the CSV's data column
headers exactly (`RowName` itself is special — it becomes each row's identifier, not a struct
field).

1. In the Content Browser, right-click → **Blueprint → Blueprint Structure** (sometimes just
   listed as "Structure" under the same submenu).
2. Name it `S_DeathEvent`. Double-click to open its editor.
3. Add three variables using the **+** button in the structure editor: `X` (type **Float**),
   `Y` (type **Float**), `Deaths` (type **Integer**). Names must match the CSV headers exactly
   (case-sensitive matching is safest — keep them capitalized exactly as `X`, `Y`, `Deaths`).
4. Compile and save the structure.

### 2.3 Import the CSV as a DataTable

1. In the Content Browser, click **Import** (or drag `playtest_deaths_2d_dt.csv` from Windows
   Explorer straight into the Content Browser).
2. In the import dialog, set **Import As** to **Data Table**, and **Data Table Row Type** (or
   similarly labeled struct picker) to your `S_DeathEvent` struct.
3. Confirm. You now have a DataTable asset (e.g. `playtest_deaths_2d_dt`) with 155 rows, each
   holding `X`, `Y`, `Deaths`.

### 2.4 Read the DataTable into an FKrigingSamplePoint array

Open (or create) an Actor Blueprint for this level — `BP_KrigingLevel2` — same steps as 1.1.
On its Event Graph, off `Event BeginPlay`:

1. Right-click, search **Get Data Table Row Names**. Place it. For its `Data Table` input,
   click the pin's dropdown and pick your `playtest_deaths_2d_dt` asset directly (no wire
   needed — DataTable reference pins let you select an asset inline).
2. This outputs a `TArray<FName>` of every row's identifier (`"0"`, `"1"`, … `"154"`). Right-click
   its output pin → **For Each Loop** (this both places the loop node and wires the array into
   it automatically).
3. Right-click, search **Get Data Table Row**. Place it inside the loop body (wire the For Each
   Loop's `Loop Body` execution pin into it). Set its `Data Table` pin to the same DataTable
   asset — as soon as you assign the asset, the node's wildcard `Out Row` output pin automatically
   resolves to the `S_DeathEvent` struct type (there's no separate "row struct" input to set; the
   node infers it from the DataTable asset itself). Wire the For Each Loop's `Array Element`
   output into this node's `Row Name` input.
4. Right-click, search **Make KrigingSamplePoint**. Place it. Wire:
   - Row struct's `X` → **Make Vector**'s `X`, Row struct's `Y` → **Make Vector**'s `Y`, and a
     literal `0` (just type it, no node needed) → **Make Vector**'s `Z`. Wire **Make Vector**'s
     output into **Make KrigingSamplePoint**'s `Location` pin. (This is the "Location = (X, Y,
     Z=0)" step called out in `SAMPLES_README.md` — deaths are a 2D map, Z is unused.)
   - Row struct's `Deaths` → **Make KrigingSamplePoint**'s `Value` pin directly (Unreal
     auto-converts `int32 → double` on the wire).
5. Before wiring the loop, create the array up front as a proper Blueprint variable instead of a
   Make Array node: in the **My Blueprint** panel (left side), click **+ Variable**, name it
   `Samples`, and set its type to `Kriging Sample Point` — then click the small grid/array icon
   next to the type dropdown to make it an array. A freshly declared array variable starts empty
   automatically, so there's no separate "reset to empty" step needed.
6. Inside the loop body, after building each `FKrigingSamplePoint`, drag off your `Samples`
   variable (from the My Blueprint panel, or from a **Get** node for it already on the graph),
   and from its output pin search **Add** — this places an **Array: Add** node targeting
   `Samples`. Wire the **Make KrigingSamplePoint** output into its `Item` input. Chain this `Add`
   call off the loop body's execution pin, after the "Get Data Table Row" node.

After the For Each Loop completes (its **Completed** execution pin, a separate output at the
bottom of the loop node, fires once all 155 rows are processed), your `Samples` variable holds
all 155 points, and everything after the loop should hang off `Completed`, not `Loop Body`.

### 2.5 Build a planar model and evaluate a grid

1. Right-click, search **Build Kriging Model (Auto-Fit)**. Wire your `Samples` variable into its
   `Samples` pin.
2. For its `Settings` input, right-click the pin → **Split Struct Pin**, and set `bPlanar` to
   **true** (checkbox appears once split) — this tells kriging to treat the data as a 2D surface
   (X, Y only), which is correct for a top-down death map. Leave `Method` at its default
   (Ordinary).
3. Wire this off the loop's `Completed` pin.
4. Right-click, search **Evaluate Grid**. Wire the model output into its `Model` pin.
5. For `Box`, right-click → **Split Struct Pin** to expose `Min` and `Max` (each an `FVector`).
   Set `Min = (0, 0, -1)`, `Max = (2000, 2000, 1)` — matching the map's pixel extent, with a thin
   ±1 slab in Z (required because `EvaluateGrid` always needs a 3D box, even though `bPlanar`
   means Z is ignored for the actual kriging math).
6. For `Resolution` (an `FIntVector`), set `X = 64, Y = 64, Z = 2` — each axis must be at least 2;
   Z=2 is the minimum since we don't need depth resolution here.
7. This produces `OutValues`, a flat `TArray<double>` of length 64×64×2 = 8192, indexed as
   `Index = X + Y*64 + Z*64*64`. We only care about the Z=0 slice, so valid indices for our
   heatmap are `Index = X + Y*64` for `X, Y` each in `[0, 63]`.

**Before you press Play:** this single **Evaluate Grid** call performs 64 × 64 × 2 = 8,192
kriging solves synchronously, on the game thread, inside `BeginPlay`, with no progress
indicator — the editor will look frozen for a few seconds while it runs. That's expected, not a
crash; the loop you build next in 2.6 is cheap by comparison (it only reads already-computed
values out of an array, it does no kriging itself). The plugin ships a `BuildKrigingModelAsync`
node, mentioned in `QUICKSTART.md`, specifically to avoid stalls like this in real gameplay
code — this tutorial stays synchronous throughout for simplicity.

### 2.6 Visualize with debug points

We'll drop one colored debug point per grid cell — the simplest, least error-prone visualization
available without adding a rendering system.

1. Off `Evaluate Grid`'s completion, add a **For Loop** (`First Index = 0`, `Last Index = 63`)
   nested inside another **For Loop** (same range). Each **For Loop** node exposes a pin simply
   named `Index` (not a variable you name yourself) — treat the outer loop's `Index` as "GridY"
   and the inner loop's `Index` as "GridX" in the steps below; you'll wire directly from those two
   `Index` pins.
2. Inside the innermost loop body:
   - Compute the flat index: `FlatIndex = GridX + (GridY * 64)` — wire the outer loop's `Index`
     into an **Integer × Integer** node (`* 64`), then that result plus the inner loop's `Index`
     into an **Integer + Integer** node.
   - Right-click your `OutValues` array (promote its wire to a variable first if you haven't, or
     wire directly from **Evaluate Grid**'s output pin) → **Array: Get** (Get a Copy), with
     `Index` wired from the value you just computed. This gives the kriged death estimate for
     this cell.
   - Compute the world location for this cell: `WorldX = GridX * (2000.0 / 63.0)`, `WorldY =
     GridY * (2000.0 / 63.0)`, `WorldZ = 0`. Feed these into a **Make Vector**.
   - Compute a color: use **Lerp Color** (or **Linear Interpolate** on a `LinearColor`) with
     `A` = a dark/cool color (e.g. blue, `(0,0,1)`), `B` = a hot color (e.g. red, `(1,0,0)`), and
     `Alpha` = the grid value normalized against the dataset's known range. Per
     `SAMPLES_README.md`, `Deaths` ranges from 0 to 26 in the raw data (the kriged *estimate* can
     occasionally land slightly outside that at map edges) — use `Alpha = Clamp(Value / 26.0, 0,
     1)` via a **Clamp (float)** node.
   - Right-click, search **Draw Debug Point**. Wire `World Context Object` to `Self`, `Position`
     to the **Make Vector** above, `Color` to the **Lerp Color** result, `Duration` to a literal
     `30.0` (seconds — long enough to inspect after Play starts; a larger number keeps the points
     around longer, but `-1` behaves inconsistently across engine versions, so stick to a plain
     positive literal like `30`–`60`), `Thickness` to `20.0` (a visibly large dot).
3. Chain: outer loop body → inner loop → (index math → array get → color math → draw debug
   point) all within the inner loop body, execution-pin-to-execution-pin.

Compile, save, place `BP_KrigingLevel2` in the level, and Play (the brief freeze you'll see right
after pressing Play is the **Evaluate Grid** call from 2.5, not this loop — see the note there).

**What to expect:** roughly 4,096 colored dots covering the 2000×2000 map, mostly blue/cool, with
four visibly warmer/redder clusters at approximately `(400, 400)`, `(1400, 600)`, `(800, 1500)`,
and `(1600, 1600)` — the documented hotspots (platformer section, lava zone, ambush encounter, and
boss arena respectively). The exact rendered hue at each point depends on your chosen color ramp
and the fitted variogram's smoothing, so don't expect crisp boundaries — expect a smooth gradient
that peaks visibly at those four locations and stays cool in between.

**Success state:** four visually distinct warm-colored clusters of debug points at roughly the
coordinates above, viewable by flying the Play camera above the point cloud.

> **Troubleshooting — Level 2**
> - **DataTable import complains about missing/mismatched columns:** the struct's variable
>   names must match the CSV header names exactly. Reopen `S_DeathEvent` and check spelling and
>   capitalization of `X`, `Y`, `Deaths` character-for-character against the CSV's header row.
> - **Import dialog doesn't offer your struct:** you must create and compile the Blueprint
>   Structure *before* importing the CSV — the struct picker only lists structs that already
>   exist in the project.
> - **Play shows nothing:** confirm the actor was dragged into the level (not just sitting in the
>   Content Browser) and check the Output Log for any red error text — a common cause here is
>   `Get Data Table Row`'s `Data Table` pin left unassigned, which leaves its `Out Row` pin
>   untyped and the graph fails to compile (fix the compile error first; Play won't run a broken
>   graph).
> - **Points don't disappear/reappear correctly, or seem to only show one frame:** debug draws
>   are transient by design; if you need them to persist across level reloads, this simple
>   per-BeginPlay approach isn't the right long-term tool — but for a one-off visual check it's
>   sufficient.

---

## LEVEL 3 — 3D ore body isosurface

**Goal:** import `ore_body_3d.csv`, build a 3D model, and extract a solid mesh at the 1.0 g/t
grade cutoff so you can see the ore body's shape directly in the viewport.
**Time:** ~30 minutes.

### 3.1 Prepare and import the CSV

Same row-name prepend as Level 2, but for `ore_body_3d.csv` (columns `X,Y,Z,Grade`, 395 rows, row
names `0`–`394`):

```
RowName,X,Y,Z,Grade
0,200.578,301.263,0.152,0.060
1,199.059,298.722,4.480,0.850
...
```

Create a struct `S_OreAssay` with **Float** fields `X`, `Y`, `Z`, `Grade` (matching the header
exactly, same process as 2.2), then import the augmented CSV as a DataTable using that struct
(same process as 2.3).

### 3.2 Build the actor and add a Procedural Mesh Component

The mesh output from kriging's isosurface extraction needs somewhere to live — a **Procedural
Mesh Component**, an Unreal component that can build a mesh at runtime from raw vertex/triangle
data (as opposed to a normal static mesh, which is baked in the editor). The plugin already
depends on Epic's `ProceduralMeshComponent` engine plugin (declared in `Kriging.uplugin`), so it's
enabled automatically — no extra plugin step needed.

1. Create `BP_KrigingLevel3` (Actor Blueprint, same as before) and open it.
2. In the Blueprint editor, find the **Components** panel (usually top-left). Click **+ Add**,
   type `Procedural Mesh Component` in the search box, select it.
3. It appears in the component tree as `ProceduralMesh` (or similar); leave it attached to the
   default root.

### 3.3 Read the DataTable into samples (same pattern as Level 2)

Repeat the Get Data Table Row Names → For Each Loop → Get Data Table Row → Make
KrigingSamplePoint → Array Add pattern from 2.4, but:
- Build `Location` from the row's `X`, `Y`, `Z` fields directly (this is real 3D data — no
  Z=0 override).
- Wire `Grade` → `Make KrigingSamplePoint`'s `Value` pin.

### 3.4 Build the model and extract the isosurface

1. Off the loop's `Completed` pin, place **Build Kriging Model (Auto-Fit)**, wired from your
   completed samples array. Leave `Settings` at its default — `bPlanar = false` is correct here
   (this is volumetric 3D data, not a flat surface), and `Method = Ordinary` is fine.
2. Right-click, search **Extract Iso Surface To Procedural Mesh** (this is the auto-spaced
   display name for `ExtractIsoSurfaceToProceduralMesh`). Wire:
   - `Model` ← the build node's model output
   - `Iso Value` ← literal `1.0` — this is the ore grade cutoff from `SAMPLES_README.md`, chosen
     to sit between the background grade (~0.69 g/t mean) and the shell grades (≥90th percentile
     is ~1.39 g/t)
   - `Box` → split the pin, set `Min = (190, 290, 0)`, `Max = (260, 460, 250)`. **Note:**
     `SAMPLES_README.md`'s own prose recipe suggests a much larger box (up to X=550, Y=500) —
     don't use that; it describes the shells as being "near (500, 500)" in a different coordinate
     convention than the CSV actually uses. The CSV's own data (check it yourself: `X` ranges
     194.9–254.9, `Y` ranges 296.2–454.5, `Z` ranges 0.15–245.4 across all 395 rows) is the
     source of truth, and the box above is that range with a small margin. A box built from the
     README's prose coordinates would mostly cover empty space with no samples in it, and your
     isosurface would either come back empty or hug one corner.
   - `Resolution` → start with `(32, 32, 32)` for your first attempt (32,768 evaluations — quick
     enough to confirm the whole pipeline works), then raise to `(64, 64, 64)` once you've seen a
     mesh appear (finer resolutions like 96 or 128 per axis look smoother but take much longer and
     use much more memory — see the memory note in `KrigingLibrary.h`).
   - `Target Component` ← drag your `ProceduralMesh` component from the Components panel into
     the graph (it'll offer to create a "Get" reference node) and wire that in
   - `Section Index` ← `0`
   - `Create Collision` ← leave unchecked/false (you don't need physics collision to just look at
     it)
   - `Flip Winding` ← leave unchecked/false for the first attempt
3. Chain this off the model build node's execution pin.

### 3.5 Give it a material

A freshly created procedural mesh section has no material assigned, and will render either
invisible or as the engine's default gray/checkerboard material depending on version — either
way, assign one explicitly:

1. Right-click, search **Set Material**. Wire its target to your `ProceduralMesh` component
   (same "drag component into graph" trick as above), `Material Index = 0`, and for the
   `Material` input pick any material asset in your project (if this is a truly blank project
   with no starter content, create a trivial one: Content Browser → right-click → Material →
   name it `M_Ore` → open it → wire a **Constant3Vector** color node straight into the `Base
   Color` input → compile/save — no need for anything fancier).
2. Chain this off the isosurface extraction node's execution pin.

**Two-sided tip:** marching-cubes meshes are a closed shell, so normally you view them from
outside and a one-sided (default) material is fine. But if you fly the camera *inside* the shape
(common when checking a hollow-looking isosurface), a one-sided material becomes invisible from
the inside. If you want to inspect the ore body from any angle including the inside, open your
material asset, find **Two Sided** in the Details panel, and check it.

Compile, save, place `BP_KrigingLevel3` in the level, Play (or just look at it in the viewport —
this particular actor's mesh appears as soon as `BeginPlay` runs, which also happens if you use
"Simulate" instead of "Play").

**Before you do:** even at 32³, that's 32,768 synchronous kriging solves against 395 samples in
`BeginPlay` — the editor will look frozen for several seconds to a minute or so; that's expected,
not a hang (see the same note in Level 2, and `BuildKrigingModelAsync` for the non-stalling
alternative used in real gameplay code).

**Scale reminder:** the CSV's `X`/`Y`/`Z` values are meters, and Unreal's native unit is
centimeters — but the plugin makes no unit conversion for you; whatever numbers you feed in as
`Location` become centimeters directly. So this whole ore body physically occupies only about
0.7 m × 1.7 m × 2.5 m in the level (a box roughly the size of a phone booth). After pressing Play,
you likely won't see anything at the default camera distance — select the actor in the World
Outliner and press **F** (focus-on-selection) to snap the viewport camera to it, then zoom in.

**What to expect:** the samples are drilled as vertical strings (8 holes, ~50 samples each), so
grade ≥ 1.0 g/t material is concentrated in short vertical intervals within each hole and sparse
in the gaps between holes. Given that, expect the isosurface to come back as **several separate,
elongated, roughly vertical blobs scattered through the box** — one per high-grade interval —
rather than one single smooth envelope. That's the correct, expected result for this dataset, not
a broken build: it directly reflects the "sharper near drillhole clusters, softer fades away from
data" behavior `SAMPLES_README.md` describes. If you see a dozen small disconnected lumps instead
of one clean shape, nothing is wrong.

**Success state:** one or more separate solid-looking mesh lumps visible in the viewport after
focusing the camera on the actor, distributed through the box roughly along vertical lines (the
drillholes), that you can fly/orbit the camera around. A single unified blob would actually be
the *more* surprising outcome here.

> **Troubleshooting — Level 3**
> - **Mesh looks inside-out (you see the inside faces, not the outside, from a normal exterior
>   camera angle):** toggle `Flip Winding` to `true` on **Extract Iso Surface To Procedural
>   Mesh** and re-run.
> - **Nothing appears at all:** first confirm `Extract Iso Surface To Procedural Mesh` actually
>   returned `true` (wire its return value into a **Branch** and a **Print String** on the false
>   path, or check the Output Log for a warning) — if `IsoValue` doesn't intersect the field
>   inside your box, it returns false with no mesh, per `KrigingLibrary.h`. `1.0` should intersect
>   given the documented grade range (min 0.010, max 6.615 g/t), but a mismatched `Box` (e.g. one
>   with coordinates outside where the ore actually is) can still produce an empty result.
> - **Mesh appears but is a uniform flat gray/checkerboard:** you skipped or mis-wired **Set
>   Material** — double check the target is your actual `ProceduralMesh` component reference, not
>   a copy/getter of the wrong component.
> - **DataTable import mismatch:** same as Level 2 — struct field names must match `X, Y, Z,
>   Grade` exactly.

---

## LEVEL 4 — Verify the plugin's own test suite

**Goal:** confirm, independent of everything you built above, that the plugin's shipped
automation tests pass on your machine.
**Time:** ~10 minutes.

1. In the menu bar, go to **Window → Test Automation**. This opens the **Session Frontend**
   panel, already switched to its **Automation** tab.
2. In the left-hand test tree, type `Kriging` into the filter/search box at the top of the tree.
3. You should see five tests, in two groups:
   - `Kriging.Core.PortableSmoke` (a smoke test of the underlying numerical core — no Blueprint
     involvement)
   - `Kriging.Blueprint.ExplicitBuildExactness`
   - `Kriging.Blueprint.AutoFitBuild`
   - `Kriging.Blueprint.AutoFitHeuristicFallback`
   - `Kriging.Blueprint.IsoSurfaceExtraction`
4. Check the box next to each (or check the parent `Kriging` group node to select all five at
   once).
5. Click **Start Tests** (bottom of the panel).
6. Wait for the run to finish — this should take well under a minute; these are small,
   deterministic unit-style tests, not full-engine integration tests.

**What to expect:** all five tests reporting green/pass. This has already been independently
verified on this machine, in headless (no-rendering) runs, on both UE 5.5 and UE 5.4 — you should
see the same result here in-editor.

**Success state:** the Automation tab's results column shows 5 green checkmarks, one per test
listed above, and the overall run summary reads something like "5 of 5 tests passed."

> **Troubleshooting — Level 4**
> - **Menu item not where expected:** depending on your editor layout/version, the same panel may
>   be reachable via **Window → Developer Tools → Session Frontend**, then clicking its
>   **Automation** tab manually — same destination either way.
> - **No Kriging tests listed at all:** automation tests are compiled into the plugin's modules
>   and only appear once those modules built successfully (Level 0.3) — if the editor never
>   finished its first compile cleanly, revisit Level 0's troubleshooting.
> - **A test fails:** this is not expected on this machine/engine-version combination; if it
>   happens, expand the failed test's row for the specific assertion message before concluding
>   anything about the plugin build — first check whether the compile in Level 0.3 actually
>   completed without errors, since a partially-built module can still register tests but fail
>   them.

---

You've now exercised every layer of the plugin from Blueprint: hardcoded points, DataTable-driven
2D grid evaluation, DataTable-driven 3D isosurface extraction, and the plugin's own automation
suite. For the underlying concepts (variograms, transforms, methods) behind any of the settings
you used here as defaults, see `QUICKSTART.md` and `GEOSTATS_PRIMER.md`.
