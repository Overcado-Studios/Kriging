#include "KrigingModel.h"
#include "KrigePortableCore.h"

#include <cmath>

UKrigingModel::UKrigingModel() = default;
UKrigingModel::~UKrigingModel() = default;

void UKrigingModel::InitializeFromCore(FCoreModelPtr InCoreModel, const FKrigingBuildResult& InReport)
{
    CoreModel = InCoreModel;
    BuildReportCache = InReport;
}

double UKrigingModel::SampleValue(const FVector& Location) const
{
    if (!CoreModel.IsValid() || !CoreModel->IsValid())
    {
        return 0.0;
    }
    const kriging::portable::Vec3 At(Location.X, Location.Y, Location.Z);
    return CoreModel->Evaluate(At);
}

double UKrigingModel::SampleValueWithUncertainty(const FVector& Location, double& OutStdDev) const
{
    OutStdDev = 0.0;
    if (!CoreModel.IsValid() || !CoreModel->IsValid())
    {
        return 0.0;
    }

    const kriging::portable::Vec3 At(Location.X, Location.Y, Location.Z);
    double Value = 0.0;
    double Variance = 0.0;
    if (!CoreModel->EvaluateWithVariance(At, Value, Variance))
    {
        // Fall back to the value-only path; leave uncertainty unreported
        // rather than fabricate a number.
        return CoreModel->Evaluate(At);
    }

    // Guard against a rounding-negative variance; the core already clamps
    // most of these internally (see CONVENTIONS.md "Dual evaluation and
    // variance"), but sqrt() of a tiny negative double is still UB-adjacent
    // to rely on the caller never seeing, so clamp again here.
    OutStdDev = std::sqrt(FMath::Max(Variance, 0.0));
    return Value;
}

bool UKrigingModel::IsValid() const
{
    return CoreModel.IsValid() && CoreModel->IsValid();
}

FKrigingBuildResult UKrigingModel::GetBuildReport() const
{
    return BuildReportCache;
}

int32 UKrigingModel::GetEffectiveSampleCount() const
{
    return BuildReportCache.EffectiveSampleCount;
}
