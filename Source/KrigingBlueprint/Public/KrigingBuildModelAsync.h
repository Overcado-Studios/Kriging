#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "KrigingTypes.h"
#include "KrigingModel.h"
#include "KrigingBuildModelAsync.generated.h"

/** Broadcast when an async kriging build finishes, successfully or not - check Result.bSuccess. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FKrigingBuildAsyncDelegate, UKrigingModel*, Model, FKrigingBuildResult, Result);

/**
 * Builds a kriging model off the game thread and delivers it back on the
 * game thread when done. Use this for larger sample sets / finer auto-fit
 * searches where BuildKrigingModelAuto's synchronous cost would stall a
 * frame.
 *
 * Thread-safety note: the numerical work (empirical variogram, WLS fit, and
 * the kriging solve itself) touches no UObjects and runs entirely on a
 * background task. Only the final UKrigingModel wrapper is constructed back
 * on the game thread, once the plain C++ result is ready - see
 * KrigingBuildModelAsync.cpp.
 */
UCLASS()
class KRIGINGBLUEPRINT_API UKrigingBuildModelAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /** Broadcast on the game thread once the background build completes (success or failure). */
    UPROPERTY(BlueprintAssignable)
    FKrigingBuildAsyncDelegate OnComplete;

    /**
     * Starts building a kriging model on a background thread. If
     * bUseAutoFit is true this mirrors BuildKrigingModelAuto (auto-fits the
     * variogram from Samples); otherwise it mirrors BuildKrigingModel using
     * ExplicitVariogramSpec. Bind to OnComplete to receive the finished
     * UKrigingModel and its FKrigingBuildResult.
     */
    UFUNCTION(BlueprintCallable, Category = "Kriging|Build", meta = (BlueprintInternalUseOnly = "false", WorldContext = "WorldContextObject"))
    static UKrigingBuildModelAsync* BuildKrigingModelAsync(UObject* WorldContextObject,
        const TArray<FKrigingSamplePoint>& Samples, bool bUseAutoFit,
        const FKrigingVariogramSpec& ExplicitVariogramSpec, const FKrigingSettings& Settings);

    virtual void Activate() override;

private:
    UPROPERTY()
    TArray<FKrigingSamplePoint> PendingSamples;

    UPROPERTY()
    bool bPendingUseAutoFit = false;

    UPROPERTY()
    FKrigingVariogramSpec PendingVariogramSpec;

    UPROPERTY()
    FKrigingSettings PendingSettings;
};
