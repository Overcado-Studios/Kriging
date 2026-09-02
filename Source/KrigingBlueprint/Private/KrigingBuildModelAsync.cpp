#include "KrigingBuildModelAsync.h"
#include "KrigingLibrary.h"

#include "Async/Async.h"

UKrigingBuildModelAsync* UKrigingBuildModelAsync::BuildKrigingModelAsync(UObject* WorldContextObject,
    const TArray<FKrigingSamplePoint>& Samples, bool bUseAutoFit,
    const FKrigingVariogramSpec& ExplicitVariogramSpec, const FKrigingSettings& Settings)
{
    UKrigingBuildModelAsync* Action = NewObject<UKrigingBuildModelAsync>();
    Action->PendingSamples = Samples;
    Action->bPendingUseAutoFit = bUseAutoFit;
    Action->PendingVariogramSpec = ExplicitVariogramSpec;
    Action->PendingSettings = Settings;
    return Action;
}

void UKrigingBuildModelAsync::Activate()
{
    // Keep this action object alive across the background task: the calling
    // Blueprint graph's own reference to the object this function returned
    // is not a GC root, and nothing else is guaranteed to hold a strong
    // reference between now and OnComplete firing. Removed from root right
    // before broadcasting, from the game-thread continuation below.
    AddToRoot();

    // Copy the pending inputs by value for the background task; these are
    // plain value types (TArray<FKrigingSamplePoint>, FKrigingVariogramSpec,
    // FKrigingSettings are not UObjects), so copying them across the thread
    // boundary is safe. This UObject itself (PendingSamples etc. as members)
    // must not be touched again off the game thread.
    TArray<FKrigingSamplePoint> SamplesCopy = PendingSamples;
    const bool bUseAutoFit = bPendingUseAutoFit;
    FKrigingVariogramSpec VariogramSpecCopy = PendingVariogramSpec;
    FKrigingSettings SettingsCopy = PendingSettings;
    TWeakObjectPtr<UKrigingBuildModelAsync> WeakThis(this);

    Async(EAsyncExecution::ThreadPool, [SamplesCopy, bUseAutoFit, VariogramSpecCopy, SettingsCopy, WeakThis]()
    {
        // --- Background thread: the actual numerical work happens here. ---
        // BuildCoreModelForBlueprint runs the empirical-variogram + WLS fit
        // (when bUseAutoFit) and the kriging solve itself. It touches no
        // UObject at all - see its declaration in KrigingLibrary.h - so it
        // is safe to run off the game thread. Only the thin UKrigingModel
        // wrapper is deferred to the game thread below.
        FKrigingBuildResult Result;
        UKrigingModel::FCoreModelPtr CoreModel = UKrigingLibrary::BuildCoreModelForBlueprint(
            SamplesCopy, bUseAutoFit, VariogramSpecCopy, SettingsCopy, Result);

        AsyncTask(ENamedThreads::GameThread, [CoreModel, Result, WeakThis]()
        {
            // --- Game thread: only UObject creation happens here. ---
            UKrigingBuildModelAsync* StrongThis = WeakThis.Get();
            if (StrongThis == nullptr)
            {
                // The action was destroyed (e.g. owning world torn down)
                // before the background build finished; drop the result.
                return;
            }

            UKrigingModel* Model = nullptr;
            if (CoreModel.IsValid())
            {
                Model = NewObject<UKrigingModel>();
                Model->InitializeFromCore(CoreModel, Result);
            }

            StrongThis->OnComplete.Broadcast(Model, Result);
            StrongThis->RemoveFromRoot();
        });
    });
}
