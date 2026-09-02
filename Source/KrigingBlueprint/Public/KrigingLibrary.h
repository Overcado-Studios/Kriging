#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "KrigingTypes.h"
#include "KrigingModel.h"
#include "KrigingLibrary.generated.h"

class UProceduralMeshComponent;

/**
 * Entry points for building and querying kriging models from Blueprint.
 *
 * Start with BuildKrigingModelAuto - the one-node path that needs no
 * geostatistics knowledge at all. Reach for BuildKrigingModel plus
 * FitVariogram only once you want explicit control over the variogram.
 */
UCLASS()
class KRIGINGBLUEPRINT_API UKrigingLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * THE flagship one-node path. Builds a kriging model from raw samples with
     * no variogram knowledge required: automatically computes an empirical
     * variogram from your samples and fits a model curve to it (weighted
     * least squares), then builds the kriging model from that fit.
     *
     * Needs a reasonable number of well-spread samples to fit reliably
     * (roughly 20+, more for noisy/clustered data). If auto-fit cannot find
     * a reliable curve, this falls back to a rough heuristic variogram
     * (range = 1/3 of your sample extent, sill = sample variance, no
     * nugget) and says so in OutResult.Warnings - it still tries to give you
     * a usable model rather than nothing, but check bSuccess and the
     * warnings if the result looks wrong.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Build", meta = (DisplayName = "Build Kriging Model (Auto-Fit)"))
    static UKrigingModel* BuildKrigingModelAuto(const TArray<FKrigingSamplePoint>& Samples,
        const FKrigingSettings& Settings, FKrigingBuildResult& OutResult);

    /**
     * Explicit-control path: builds a kriging model from raw samples using a
     * variogram you specify yourself. Use this once you understand your data
     * well enough to choose (or have fit externally) a specific shape, range,
     * and sill.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Build", meta = (DisplayName = "Build Kriging Model (Explicit Variogram)"))
    static UKrigingModel* BuildKrigingModel(const TArray<FKrigingSamplePoint>& Samples,
        const FKrigingVariogramSpec& VariogramSpec, const FKrigingSettings& Settings, FKrigingBuildResult& OutResult);

    /**
     * Standalone variogram fitting for power users: computes the empirical
     * variogram from Samples and fits a model curve to it, without building
     * a full kriging model. Useful for inspecting/tuning the fit before
     * committing to BuildKrigingModel, or for fitting once and reusing the
     * same spec across many builds.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Build", meta = (DisplayName = "Fit Variogram From Samples"))
    static bool FitVariogram(const TArray<FKrigingSamplePoint>& Samples, bool bPlanar,
        FKrigingVariogramSpec& OutVariogramSpec, FString& OutMessage);

    /**
     * Evaluates Model on every point of a regular lattice spanning Box, with
     * Resolution points along each axis (each axis must be >= 2). Returns a
     * flat array of length Resolution.X * Resolution.Y * Resolution.Z.
     *
     * Index math (matches the layout used internally and by ExtractIsoSurface):
     *   Index = X + Y * Resolution.X + Z * Resolution.X * Resolution.Y
     * for 0 <= X < Resolution.X, 0 <= Y < Resolution.Y, 0 <= Z < Resolution.Z.
     *
     * Memory: the output is Resolution.X * Resolution.Y * Resolution.Z doubles
     * (8 bytes each) - e.g. 128 x 128 x 128 is ~16.8M points = ~134 MB. This
     * function refuses to run above a hard cap (~64M points, ~512 MB) rather
     * than silently exhausting memory; lower Resolution or split the box into
     * tiles instead.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Evaluate")
    static bool EvaluateGrid(UKrigingModel* Model, const FBox& Box, const FIntVector& Resolution,
        TArray<double>& OutValues);

    /**
     * Extracts an isosurface (a surface of constant value, e.g. "everywhere
     * the estimated ore grade equals 0.5") from Model over a regular lattice
     * spanning Box at Resolution points per axis, returning raw geometry you
     * can feed into any mesh system. See ExtractIsoSurfaceToProceduralMesh
     * for a ready-to-use ProceduralMeshComponent helper.
     *
     * Returns false (with empty output arrays) if IsoValue does not actually
     * intersect the sampled field within Box - check the model's / grid's
     * value range first if this happens unexpectedly.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Evaluate")
    static bool ExtractIsoSurface(UKrigingModel* Model, double IsoValue, const FBox& Box,
        const FIntVector& Resolution, TArray<FVector>& OutVertices, TArray<int32>& OutTriangles,
        TArray<FVector>& OutNormals);

    /**
     * Convenience wrapper around ExtractIsoSurface that writes the resulting
     * mesh directly into a UProceduralMeshComponent section via
     * CreateMeshSection. bFlipWinding is exposed because the core's winding
     * (oriented to face the direction of increasing scalar value) is not
     * guaranteed to already match Unreal's front-face convention for every
     * isovalue/gradient sign combination - if your mesh renders inside-out,
     * toggle it.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Evaluate")
    static bool ExtractIsoSurfaceToProceduralMesh(UKrigingModel* Model, double IsoValue, const FBox& Box,
        const FIntVector& Resolution, UProceduralMeshComponent* TargetComponent, int32 SectionIndex,
        bool bCreateCollision, bool bFlipWinding);

    // --- Non-Blueprint C++ surface, used by UKrigingBuildModelAsync. ---

    /**
     * Performs the actual fit-or-explicit-variogram plus kriging solve with
     * no UObject construction or access at all, so it is safe to call from
     * a background thread. BuildKrigingModelAuto/BuildKrigingModel above are
     * thin, game-thread-only wrappers around this that additionally
     * NewObject<UKrigingModel>() the result.
     */
    static UKrigingModel::FCoreModelPtr BuildCoreModelForBlueprint(const TArray<FKrigingSamplePoint>& Samples,
        bool bUseAutoFit, const FKrigingVariogramSpec& ExplicitVariogramSpec, const FKrigingSettings& Settings,
        FKrigingBuildResult& OutResult);
};
