#include "KrigingLibrary.h"
#include "KrigingModel.h"
#include "KrigingTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

#include <cmath>

namespace
{
// Small, deterministic 7x7 planar grid with a smooth (non-flat, so
// "reasonable values between samples" is a meaningful check) field, spaced
// one unit apart so a zero MergeRadius is unambiguous and no two samples
// coincide.
TArray<FKrigingSamplePoint> MakeExplicitTestSamples()
{
    TArray<FKrigingSamplePoint> Samples;
    for (int32 Y = -3; Y <= 3; ++Y)
    {
        for (int32 X = -3; X <= 3; ++X)
        {
            FKrigingSamplePoint Sample;
            Sample.Location = FVector(static_cast<double>(X), static_cast<double>(Y), 0.0);
            Sample.Value = 2.0 + 0.4 * X - 0.2 * Y + 0.05 * X * Y;
            Samples.Add(Sample);
        }
    }
    return Samples;
}

// Denser 8x8 grid for the auto-fit path: FitVariogramWeightedLeastSquares
// requires an inferred sample count of at least 20 and a mean empirical-bin
// population of at least 10 (see KrigePortableAnalysis.h /
// Docs/CONVENTIONS.md), so this is deliberately larger than the explicit test's.
TArray<FKrigingSamplePoint> MakeAutoFitTestSamples()
{
    TArray<FKrigingSamplePoint> Samples;
    for (int32 Y = 0; Y < 8; ++Y)
    {
        for (int32 X = 0; X < 8; ++X)
        {
            FKrigingSamplePoint Sample;
            Sample.Location = FVector(static_cast<double>(X), static_cast<double>(Y), 0.0);
            // Smooth radial bowl centered on the grid - no linear trend, so
            // Ordinary kriging's constant-local-mean assumption is a
            // reasonable fit and the empirical variogram is well-behaved.
            const double Dx = X - 3.5;
            const double Dy = Y - 3.5;
            Sample.Value = 5.0 + 0.15 * (Dx * Dx + Dy * Dy);
            Samples.Add(Sample);
        }
    }
    return Samples;
}

// Samples for the isosurface test: a radially symmetric "bump" so the
// requested IsoValue is guaranteed to intersect a roughly sphere-shaped
// level set. Inverse Distance is used (no variogram fitting concerns) since
// this test is about marching-cubes extraction, not the kriging solve.
TArray<FKrigingSamplePoint> MakeIsoSurfaceTestSamples()
{
    TArray<FKrigingSamplePoint> Samples;
    for (int32 Z = -4; Z <= 4; Z += 2)
    {
        for (int32 Y = -4; Y <= 4; Y += 2)
        {
            for (int32 X = -4; X <= 4; X += 2)
            {
                FKrigingSamplePoint Sample;
                Sample.Location = FVector(static_cast<double>(X), static_cast<double>(Y), static_cast<double>(Z));
                Sample.Value = -static_cast<double>(X * X + Y * Y + Z * Z);
                Samples.Add(Sample);
            }
        }
    }
    return Samples;
}
// Deliberately too few/too sparse samples to clear
// FitVariogramWeightedLeastSquares' gate (>= 20 inferred samples, mean bin
// population >= 10 - see KrigePortableAnalysis.h / Docs/CONVENTIONS.md) so
// this exercises BuildKrigingModelAuto's heuristic-fallback path.
TArray<FKrigingSamplePoint> MakeSparseAutoFitFallbackSamples()
{
    TArray<FKrigingSamplePoint> Samples;
    for (int32 Index = 0; Index < 8; ++Index)
    {
        FKrigingSamplePoint Sample;
        Sample.Location = FVector(static_cast<double>(Index), 0.0, 0.0);
        Sample.Value = 1.0 + 0.3 * Index;
        Samples.Add(Sample);
    }
    return Samples;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FKrigingBlueprintExplicitBuildTest,
    "Kriging.Blueprint.ExplicitBuildExactness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKrigingBlueprintExplicitBuildTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TArray<FKrigingSamplePoint> Samples = MakeExplicitTestSamples();

    FKrigingVariogramSpec VariogramSpec;
    VariogramSpec.Shape = EKrigingVariogramShape::Spherical;
    VariogramSpec.EffectiveRange = 8.0;
    VariogramSpec.Sill = 1.0;
    VariogramSpec.Nugget = 0.0;
    VariogramSpec.NuggetMode = EKrigingNuggetMode::Exact;

    FKrigingSettings Settings;
    Settings.Method = EKrigingMethod::Ordinary;
    Settings.MergeRadius = 0.0; // Samples are 1 unit apart; nothing should merge.

    FKrigingBuildResult Result;
    UKrigingModel* KrigingModel = UKrigingLibrary::BuildKrigingModel(Samples, VariogramSpec, Settings, Result);

    if (!TestTrue(TEXT("Explicit build succeeds"), Result.bSuccess))
    {
        AddError(FString::Printf(TEXT("Build failed: %s"), *Result.Message));
        return false;
    }
    if (!TestNotNull(TEXT("Model is non-null on success"), KrigingModel))
    {
        return false;
    }
    TestTrue(TEXT("Model reports itself valid"), KrigingModel->IsValid());
    TestEqual(TEXT("No samples were merged away"), Result.MergedAwayCount, 0);
    TestFalse(TEXT("Explicit build result is not marked as auto-fit"), Result.bHasFittedVariogram);

    // Exactness at every sample location (zero nugget, Exact mode, no
    // measurement variance, no merging - CONVENTIONS.md "Nugget, measurement
    // variance, and exactness").
    double MaxAbsoluteError = 0.0;
    double MaxStdDevAtSample = 0.0;
    for (const FKrigingSamplePoint& Sample : Samples)
    {
        const double Estimated = KrigingModel->SampleValue(Sample.Location);
        MaxAbsoluteError = FMath::Max(MaxAbsoluteError, std::abs(Estimated - Sample.Value));

        double StdDev = 0.0;
        KrigingModel->SampleValueWithUncertainty(Sample.Location, StdDev);
        MaxStdDevAtSample = FMath::Max(MaxStdDevAtSample, StdDev);
    }
    TestTrue(TEXT("Model reproduces every sample value to within 1e-6"), MaxAbsoluteError <= 1.0e-6);
    // Checked on the variance scale (sigma^2 = C(0) - b^T A^-1 b, a near-total
    // cancellation of O(1) terms at a sample point) rather than std-dev scale:
    // std-dev ~1e-6 corresponds to a variance of only ~1e-12, which is right at
    // the edge of what double-precision cancellation can guarantee. Squaring
    // back to the variance scale keeps this a meaningful "zero-ish" check
    // without being a coin flip on rounding.
    TestTrue(TEXT("Uncertainty is ~0 (variance-scale) at sample locations with exact nugget"),
        MaxStdDevAtSample * MaxStdDevAtSample <= 1.0e-9);

    // Sanity check between samples: a query at the centroid of four
    // neighboring samples should land near their average, not diverge.
    const FVector BetweenSamples(0.5, 0.5, 0.0);
    const double Estimated = KrigingModel->SampleValue(BetweenSamples);
    TestTrue(TEXT("Value between samples is finite"), std::isfinite(Estimated));
    TestTrue(TEXT("Value between samples is in a sane range"), Estimated > -10.0 && Estimated < 10.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FKrigingBlueprintAutoFitBuildTest,
    "Kriging.Blueprint.AutoFitBuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKrigingBlueprintAutoFitBuildTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TArray<FKrigingSamplePoint> Samples = MakeAutoFitTestSamples();

    FKrigingSettings Settings;
    Settings.Method = EKrigingMethod::Ordinary;
    Settings.MergeRadius = 0.0; // Grid spacing is 1 unit; nothing should merge.

    FKrigingBuildResult Result;
    UKrigingModel* KrigingModel = UKrigingLibrary::BuildKrigingModelAuto(Samples, Settings, Result);

    if (!TestTrue(TEXT("Auto-fit build succeeds"), Result.bSuccess))
    {
        AddError(FString::Printf(TEXT("Build failed: %s"), *Result.Message));
        return false;
    }
    if (!TestNotNull(TEXT("Model is non-null on success"), KrigingModel))
    {
        return false;
    }
    TestTrue(TEXT("Auto-fit build result is marked as auto-fit"), Result.bHasFittedVariogram);
    TestTrue(TEXT("Fitted effective range is positive"), Result.FittedVariogram.EffectiveRange > 0.0);
    TestTrue(TEXT("Fitted sill is positive"), Result.FittedVariogram.Sill > 0.0);

    // Auto-fit does not give us control over the exact variogram (fitNugget
    // is on), so this checks weaker, model-shape properties rather than
    // exactness: every sample estimate should be finite and inside a loose
    // envelope around the observed data range.
    double MinValue = TNumericLimits<double>::Max();
    double MaxValue = TNumericLimits<double>::Lowest();
    for (const FKrigingSamplePoint& Sample : Samples)
    {
        MinValue = FMath::Min(MinValue, Sample.Value);
        MaxValue = FMath::Max(MaxValue, Sample.Value);
    }
    const double Margin = 0.25 * (MaxValue - MinValue);

    for (const FKrigingSamplePoint& Sample : Samples)
    {
        const double Estimated = KrigingModel->SampleValue(Sample.Location);
        if (!TestTrue(TEXT("Auto-fit estimate at a sample is finite"), std::isfinite(Estimated)))
        {
            return false;
        }
        TestTrue(TEXT("Auto-fit estimate at a sample is within a loose envelope of the data range"),
            Estimated >= MinValue - Margin && Estimated <= MaxValue + Margin);
    }

    if (Result.bHasCrossValidation)
    {
        TestTrue(TEXT("Reported cross-validation RMSE is finite and non-negative"),
            std::isfinite(Result.CrossValidationRMSE) && Result.CrossValidationRMSE >= 0.0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FKrigingBlueprintAutoFitFallbackTest,
    "Kriging.Blueprint.AutoFitHeuristicFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKrigingBlueprintAutoFitFallbackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Pins the fix for a real bug caught during self-review: the heuristic
    // fallback's warning was being added to OutResult.Warnings and then
    // silently discarded when FillBuildResultFromReport (called afterwards
    // by BuildCoreModel) reset the array instead of appending to it. This
    // test fails loudly if that regresses.
    const TArray<FKrigingSamplePoint> Samples = MakeSparseAutoFitFallbackSamples();

    FKrigingSettings Settings;
    Settings.MergeRadius = 0.0;

    FKrigingBuildResult Result;
    UKrigingModel* KrigingModel = UKrigingLibrary::BuildKrigingModelAuto(Samples, Settings, Result);

    if (!TestTrue(TEXT("Auto-fit build still succeeds via the heuristic fallback"), Result.bSuccess))
    {
        AddError(FString::Printf(TEXT("Build failed: %s"), *Result.Message));
        return false;
    }
    TestNotNull(TEXT("Model is non-null on success"), KrigingModel);
    TestTrue(TEXT("Result is marked as auto-fit even when it fell back to the heuristic"), Result.bHasFittedVariogram);
    TestTrue(TEXT("Fallback's fitted range is positive"), Result.FittedVariogram.EffectiveRange > 0.0);
    TestTrue(TEXT("A warning about the heuristic fallback survives to the caller"), Result.Warnings.Num() > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FKrigingBlueprintIsoSurfaceTest,
    "Kriging.Blueprint.IsoSurfaceExtraction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKrigingBlueprintIsoSurfaceTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TArray<FKrigingSamplePoint> Samples = MakeIsoSurfaceTestSamples();

    FKrigingVariogramSpec VariogramSpec; // Unused by InverseDistance, but required by the call signature.
    FKrigingSettings Settings;
    Settings.Method = EKrigingMethod::InverseDistance;
    Settings.MergeRadius = 0.0;

    FKrigingBuildResult Result;
    UKrigingModel* KrigingModel = UKrigingLibrary::BuildKrigingModel(Samples, VariogramSpec, Settings, Result);

    if (!TestTrue(TEXT("Inverse-distance build for the isosurface test succeeds"), Result.bSuccess))
    {
        AddError(FString::Printf(TEXT("Build failed: %s"), *Result.Message));
        return false;
    }
    if (!TestNotNull(TEXT("Model is non-null on success"), KrigingModel))
    {
        return false;
    }

    // Field ranges from 0 at the center to -48 at the sampled corners; -8.0
    // sits well inside that range, so it is guaranteed to intersect a
    // roughly sphere-shaped level set around the origin.
    const FBox Box(FVector(-4.0, -4.0, -4.0), FVector(4.0, 4.0, 4.0));
    const FIntVector Resolution(17, 17, 17);
    constexpr double IsoValue = -8.0;

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    const bool bExtracted = UKrigingLibrary::ExtractIsoSurface(KrigingModel, IsoValue, Box, Resolution, Vertices, Triangles, Normals);

    if (!TestTrue(TEXT("Isosurface extraction succeeds for a value inside the field's range"), bExtracted))
    {
        return false;
    }
    TestTrue(TEXT("Extracted mesh has at least one vertex"), Vertices.Num() > 0);
    TestEqual(TEXT("Every vertex has a parallel normal"), Normals.Num(), Vertices.Num());
    TestTrue(TEXT("Triangle index count is a multiple of three"), Triangles.Num() % 3 == 0);
    TestTrue(TEXT("Extracted mesh has at least one triangle"), Triangles.Num() >= 3);

    // Coherence: every triangle index must reference a valid vertex.
    bool bAllIndicesValid = true;
    for (int32 Index : Triangles)
    {
        if (Index < 0 || Index >= Vertices.Num())
        {
            bAllIndicesValid = false;
            break;
        }
    }
    TestTrue(TEXT("All triangle indices reference valid vertices"), bAllIndicesValid);

    // Sphere-ish check: the field is radially symmetric by construction, and
    // the isosurface's underlying scalar field is Inverse-Distance
    // interpolated (a piecewise approximation, not an exact quadratic
    // between the sparse samples), so this deliberately checks a loose band
    // around the expected radius sqrt(-IsoValue) rather than a tight one -
    // it is a coherence/gross-error guard ("is this a roughly sphere-shaped
    // blob centered near the origin", not "is this a mathematically perfect
    // sphere").
    const double ExpectedRadius = std::sqrt(-IsoValue);
    bool bAllNearExpectedRadius = true;
    for (const FVector& Vertex : Vertices)
    {
        const double Radius = Vertex.Size();
        if (Radius < 0.3 * ExpectedRadius || Radius > 2.5 * ExpectedRadius)
        {
            bAllNearExpectedRadius = false;
            break;
        }
    }
    TestTrue(TEXT("Extracted vertices lie in a loose band around the expected spherical radius"), bAllNearExpectedRadius);

    return true;
}
#endif
