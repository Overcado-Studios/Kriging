#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "KrigingTypes.h"
#include "KrigingModel.generated.h"

namespace kriging::portable
{
class Model;
}

/**
 * A built kriging model ready to be sampled.
 *
 * "Kriging" = smart averaging: it blends nearby known values to estimate an
 * unknown one, and (unlike a plain average) can also tell you how confident
 * that estimate is via SampleValueWithUncertainty.
 *
 * Create instances via UKrigingLibrary::BuildKrigingModelAuto or
 * BuildKrigingModel - do not construct this directly in Blueprint.
 */
UCLASS(BlueprintType)
class KRIGINGBLUEPRINT_API UKrigingModel : public UObject
{
    GENERATED_BODY()

public:
    // Thread-safe shared ownership: the numerical build (fit + solve) can run
    // on a background task (see UKrigingBuildModelAsync), which produces this
    // pointer there, then hands it to a UKrigingModel constructed back on the
    // game thread. A plain TUniquePtr cannot cross that thread boundary by
    // capture-by-value into the completion continuation without a move-only
    // capture dance; ESPMode::ThreadSafe makes the refcount itself safe to
    // touch from both threads during that handoff.
    using FCoreModelPtr = TSharedPtr<kriging::portable::Model, ESPMode::ThreadSafe>;

    UKrigingModel();
    virtual ~UKrigingModel() override;

    /**
     * Estimates the value at Location. Only meaningful if IsValid() is true.
     * Not a Pure node: a kriging solve does real work per call, so it is not
     * free to re-evaluate on every pin pull.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Model")
    double SampleValue(const FVector& Location) const;

    /**
     * Estimates the value at Location and also reports how uncertain that
     * estimate is (OutStdDev = standard deviation, same units as your sample
     * values - roughly "how far off this particular guess could plausibly
     * be"). Near your sample points OutStdDev will be small (zero at a
     * sample if NuggetMode was Exact and there is no measurement noise);
     * far from any sample it grows.
     *
     * If Settings.Transform was not None, OutStdDev is reported in the
     * transformed (not the original) value space - see FKrigingSettings::Transform.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Model")
    double SampleValueWithUncertainty(const FVector& Location, double& OutStdDev) const;

    /** True if this model built successfully and can be sampled. */
    UFUNCTION(BlueprintPure, Category = "Kriging|Model")
    bool IsValid() const;

    /** Returns the diagnostic report captured when this model was built. */
    UFUNCTION(BlueprintPure, Category = "Kriging|Model")
    FKrigingBuildResult GetBuildReport() const;

    /** Number of samples actually used by this model after any near-duplicate merging. */
    UFUNCTION(BlueprintPure, Category = "Kriging|Model")
    int32 GetEffectiveSampleCount() const;

    // --- Non-Blueprint C++ surface used by UKrigingLibrary / the async build action. ---

    /** Takes ownership of an already-built core model plus its report. Game-thread only. */
    void InitializeFromCore(FCoreModelPtr InCoreModel, const FKrigingBuildResult& InReport);

    /** Direct, non-reflected access to the wrapped core model (for library helpers such as grid evaluation). */
    const kriging::portable::Model* GetCoreModel() const { return CoreModel.Get(); }

private:
    // Not a UPROPERTY: this is a plain C++ object, never a UObject itself,
    // and must not be visited by the GC's reference collector.
    FCoreModelPtr CoreModel;

    UPROPERTY()
    FKrigingBuildResult BuildReportCache;
};
