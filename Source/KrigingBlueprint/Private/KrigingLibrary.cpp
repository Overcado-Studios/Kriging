#include "KrigingLibrary.h"
#include "KrigingConversions.h"

#include "KrigePortableCore.h"
#include "KrigePortableAnalysis.h"
#include "KrigePortableMarchingCubes.h"

#include "ProceduralMeshComponent.h"
#include "Async/ParallelFor.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace kriging::portable;
using namespace KrigingConversions;

DEFINE_LOG_CATEGORY_STATIC(LogKrigingBlueprint, Log, All);

namespace
{
// Hard cap on lattice point count for EvaluateGrid / ExtractIsoSurface. At
// 8 bytes/double, 64M points is ~512 MB for EvaluateGrid's output alone;
// beyond this we refuse rather than silently exhausting memory. Callers
// needing more resolution should tile the box themselves.
constexpr int64 MaxGridPoints = 64ll * 1024ll * 1024ll;

bool ValidateGridRequest(const FBox& Box, const FIntVector& Resolution, int64& OutTotalPoints, FString& OutError)
{
    if (Resolution.X < 2 || Resolution.Y < 2 || Resolution.Z < 2)
    {
        OutError = TEXT("Resolution must be at least 2 along every axis.");
        return false;
    }
    const FVector Extent = Box.Max - Box.Min;
    if (Extent.X <= 0.0 || Extent.Y <= 0.0 || Extent.Z <= 0.0)
    {
        OutError = TEXT("Box must have positive extent along every axis.");
        return false;
    }
    OutTotalPoints = static_cast<int64>(Resolution.X) * static_cast<int64>(Resolution.Y) * static_cast<int64>(Resolution.Z);
    if (OutTotalPoints > MaxGridPoints)
    {
        OutError = FString::Printf(TEXT("Requested grid has %lld points, above the %lld-point safety cap. Lower Resolution or tile the box."),
            OutTotalPoints, MaxGridPoints);
        return false;
    }
    return true;
}

// Builds a ScalarGrid3D by evaluating Model over every lattice point,
// row-parallel (one job per Y*Z row of X samples) via EvaluateBatch. The
// core's Model::Evaluate/EvaluateBatch are safe to call concurrently: the
// only mutable shared state is the local-solver factorization cache, which
// is protected by its own mutex (Source/KrigingCore/Private/Portable/
// KrigePortableCore.cpp). Rows write into disjoint slices of the output, so
// no additional synchronization is needed here.
bool BuildScalarGrid(const Model& CoreModel, const FBox& Box, const FIntVector& Resolution, ScalarGrid3D& OutGrid, FString& OutError)
{
    int64 TotalPoints = 0;
    if (!ValidateGridRequest(Box, Resolution, TotalPoints, OutError))
    {
        return false;
    }

    const FVector Extent = Box.Max - Box.Min;
    OutGrid.sizeX = Resolution.X;
    OutGrid.sizeY = Resolution.Y;
    OutGrid.sizeZ = Resolution.Z;
    OutGrid.origin = Vec3(Box.Min.X, Box.Min.Y, Box.Min.Z);
    OutGrid.cellSize = Vec3(
        Extent.X / static_cast<double>(Resolution.X - 1),
        Extent.Y / static_cast<double>(Resolution.Y - 1),
        Extent.Z / static_cast<double>(Resolution.Z - 1));
    OutGrid.values.assign(static_cast<std::size_t>(TotalPoints), 0.0);

    const int32 RowCount = Resolution.Y * Resolution.Z;
    ParallelFor(RowCount, [&](int32 RowIndex)
    {
        const int32 Y = RowIndex % Resolution.Y;
        const int32 Z = RowIndex / Resolution.Y;

        std::vector<Vec3> RowPoints(static_cast<std::size_t>(Resolution.X));
        for (int32 X = 0; X < Resolution.X; ++X)
        {
            RowPoints[static_cast<std::size_t>(X)] = Vec3(
                OutGrid.origin.x + X * OutGrid.cellSize.x,
                OutGrid.origin.y + Y * OutGrid.cellSize.y,
                OutGrid.origin.z + Z * OutGrid.cellSize.z);
        }

        std::vector<double> RowValues;
        CoreModel.EvaluateBatch(RowPoints, RowValues);

        const std::size_t RowOffset = (static_cast<std::size_t>(Z) * static_cast<std::size_t>(Resolution.Y) + static_cast<std::size_t>(Y))
            * static_cast<std::size_t>(Resolution.X);
        for (int32 X = 0; X < Resolution.X; ++X)
        {
            OutGrid.values[RowOffset + static_cast<std::size_t>(X)] = RowValues[static_cast<std::size_t>(X)];
        }
    });

    std::string ValidationError;
    if (!OutGrid.IsValid(&ValidationError))
    {
        OutError = FString(UTF8_TO_TCHAR(ValidationError.c_str()));
        return false;
    }
    return true;
}

// Shared build path used by both BuildKrigingModel and BuildKrigingModelAuto
// (via BuildCoreModelForBlueprint below). Builds the core model in a plain
// shared pointer and fills the shared portion of OutResult; does NOT set
// FittedVariogram/bHasFittedVariogram, since that differs between callers.
// Touches no UObject, so this is safe to call from a background thread.
UKrigingModel::FCoreModelPtr BuildCoreModel(const TArray<FKrigingSamplePoint>& Samples, const Variogram& CoreVariogram,
    const kriging::portable::Settings& CoreSettings, FKrigingBuildResult& OutResult)
{
    std::vector<Sample> CoreSamples = ToCoreSamples(Samples);

    // The core rejects Power + Simple (an unbounded, sign-indefinite system
    // with no known-mean anchor) - fail fast with a message a non-specialist
    // can act on instead of relying on whatever the core's internal error
    // text says.
    if (CoreSettings.method == Method::Simple
        && !CoreVariogram.structures.empty()
        && CoreVariogram.structures.front().shape == Shape::Power)
    {
        OutResult.bSuccess = false;
        OutResult.Message = TEXT("The Power variogram shape has no fixed variance (no plateau), so it cannot be combined with Simple kriging's known-mean assumption. Use Ordinary or a Universal method instead, or pick a different shape.");
        OutResult.InputSampleCount = Samples.Num();
        return nullptr;
    }

    UKrigingModel::FCoreModelPtr CoreModel = MakeShared<Model, ESPMode::ThreadSafe>();
    BuildReport Report;
    const bool bBuilt = CoreModel->Build(CoreSamples, CoreVariogram, CoreSettings, Report);
    FillBuildResultFromReport(Report, Samples.Num(), OutResult);

    if (!bBuilt || !CoreModel->IsValid())
    {
        OutResult.bSuccess = false;
        return nullptr;
    }

    CrossValidationReport CrossValidation;
    if (CoreModel->CrossValidate(CrossValidation) && CrossValidation.succeeded)
    {
        OutResult.bHasCrossValidation = true;
        OutResult.CrossValidationRMSE = CrossValidation.rootMeanSquareError;
    }
    else
    {
        OutResult.bHasCrossValidation = false;
        OutResult.CrossValidationRMSE = 0.0;
    }

    return CoreModel;
}

// Heuristic fallback used only when auto-fit cannot produce a reliable
// curve (too few samples, degenerate spacing, ...). Deliberately simple and
// documented as an approximation rather than a fit: range = 1/3 of the
// largest axis-aligned extent of the sample cloud, sill = sample variance,
// zero nugget, Spherical shape (the most forgiving default).
FKrigingVariogramSpec BuildHeuristicVariogram(const TArray<FKrigingSamplePoint>& Samples)
{
    FKrigingVariogramSpec Spec;
    Spec.Shape = EKrigingVariogramShape::Spherical;
    Spec.Nugget = 0.0;
    Spec.NuggetMode = EKrigingNuggetMode::Exact;

    if (Samples.Num() < 2)
    {
        return Spec; // Keep struct defaults; nothing sensible to derive.
    }

    FVector MinExtent(std::numeric_limits<double>::max());
    FVector MaxExtent(-std::numeric_limits<double>::max());
    double Mean = 0.0;
    for (const FKrigingSamplePoint& Sample : Samples)
    {
        MinExtent = FVector(FMath::Min(MinExtent.X, Sample.Location.X), FMath::Min(MinExtent.Y, Sample.Location.Y), FMath::Min(MinExtent.Z, Sample.Location.Z));
        MaxExtent = FVector(FMath::Max(MaxExtent.X, Sample.Location.X), FMath::Max(MaxExtent.Y, Sample.Location.Y), FMath::Max(MaxExtent.Z, Sample.Location.Z));
        Mean += Sample.Value;
    }
    Mean /= Samples.Num();

    double Variance = 0.0;
    for (const FKrigingSamplePoint& Sample : Samples)
    {
        const double Delta = Sample.Value - Mean;
        Variance += Delta * Delta;
    }
    Variance /= Samples.Num();

    const FVector Extent = MaxExtent - MinExtent;
    const double MaxAxisExtent = FMath::Max3(Extent.X, Extent.Y, Extent.Z);

    Spec.EffectiveRange = MaxAxisExtent > 0.0 ? MaxAxisExtent / 3.0 : 1000.0;
    Spec.Sill = Variance > 0.0 ? Variance : 1.0;
    return Spec;
}

} // namespace

UKrigingModel::FCoreModelPtr UKrigingLibrary::BuildCoreModelForBlueprint(const TArray<FKrigingSamplePoint>& Samples,
    bool bUseAutoFit, const FKrigingVariogramSpec& ExplicitVariogramSpec, const FKrigingSettings& Settings,
    FKrigingBuildResult& OutResult)
{
    OutResult = FKrigingBuildResult();

    FKrigingVariogramSpec EffectiveSpec = ExplicitVariogramSpec;
    if (bUseAutoFit)
    {
        FString FitMessage;
        const bool bFitSucceeded = FitVariogram(Samples, Settings.bPlanar, EffectiveSpec, FitMessage);
        if (!bFitSucceeded)
        {
            EffectiveSpec = BuildHeuristicVariogram(Samples);
        }

        OutResult.bHasFittedVariogram = true;
        if (!bFitSucceeded)
        {
            OutResult.Warnings.Add(FString::Printf(
                TEXT("Auto-fit could not find a reliable variogram (%s); used a rough heuristic instead (Spherical, range = 1/3 sample extent, sill = sample variance). Consider supplying more/better-spread samples, or use Build Kriging Model (Explicit Variogram) with your own variogram."),
                *FitMessage));
        }
    }
    else
    {
        OutResult.bHasFittedVariogram = false;
    }

    const Variogram CoreVariogram = ToCoreVariogram(EffectiveSpec);
    const kriging::portable::Settings CoreSettings = ToCoreSettings(Settings);

    UKrigingModel::FCoreModelPtr CoreModel = BuildCoreModel(Samples, CoreVariogram, CoreSettings, OutResult);

    // BuildCoreModel() may have reset OutResult.bSuccess/Message on failure,
    // but it never touches bHasFittedVariogram/FittedVariogram - set those
    // here regardless of build outcome so a failed build still reports what
    // variogram it tried.
    OutResult.FittedVariogram = EffectiveSpec;

    return CoreModel;
}

UKrigingModel* UKrigingLibrary::BuildKrigingModelAuto(const TArray<FKrigingSamplePoint>& Samples,
    const FKrigingSettings& Settings, FKrigingBuildResult& OutResult)
{
    UKrigingModel::FCoreModelPtr CoreModel = BuildCoreModelForBlueprint(
        Samples, /*bUseAutoFit=*/true, FKrigingVariogramSpec(), Settings, OutResult);
    if (!CoreModel.IsValid())
    {
        return nullptr;
    }

    UKrigingModel* Wrapper = NewObject<UKrigingModel>();
    Wrapper->InitializeFromCore(CoreModel, OutResult);
    return Wrapper;
}

UKrigingModel* UKrigingLibrary::BuildKrigingModel(const TArray<FKrigingSamplePoint>& Samples,
    const FKrigingVariogramSpec& VariogramSpec, const FKrigingSettings& Settings, FKrigingBuildResult& OutResult)
{
    UKrigingModel::FCoreModelPtr CoreModel = BuildCoreModelForBlueprint(
        Samples, /*bUseAutoFit=*/false, VariogramSpec, Settings, OutResult);
    if (!CoreModel.IsValid())
    {
        return nullptr;
    }

    UKrigingModel* Wrapper = NewObject<UKrigingModel>();
    Wrapper->InitializeFromCore(CoreModel, OutResult);
    return Wrapper;
}

bool UKrigingLibrary::FitVariogram(const TArray<FKrigingSamplePoint>& Samples, bool bPlanar,
    FKrigingVariogramSpec& OutVariogramSpec, FString& OutMessage)
{
    OutVariogramSpec = FKrigingVariogramSpec();

    std::vector<Sample> CoreSamples = ToCoreSamples(Samples);

    EmpiricalSettings EmpSettings;
    EmpiricalReport EmpReport;
    if (!ComputeEmpiricalVariogram(CoreSamples, bPlanar, EmpSettings, EmpReport))
    {
        OutMessage = FString(UTF8_TO_TCHAR(EmpReport.message.c_str()));
        return false;
    }

    // The core's fitting function fits whatever shape(s) you hand it - it
    // does not search over shape families itself. Since the flagship
    // auto-fit path must not require the caller to know which shape to try,
    // this tries the three well-understood bounded shapes and keeps
    // whichever achieves the lowest weighted SSE. (Matern/Power are left to
    // the explicit path, where a user who wants them can ask for them
    // directly via FKrigingVariogramSpec.)
    const Shape CandidateShapes[] = { Shape::Spherical, Shape::Exponential, Shape::Gaussian };

    bool bAnySucceeded = false;
    double BestSse = std::numeric_limits<double>::max();
    FitReport BestReport;
    FString CombinedMessages;

    for (Shape Candidate : CandidateShapes)
    {
        FitSettings FitSettingsValue;
        FitStructureSettings StructureSettings;
        StructureSettings.shape = Candidate;
        FitSettingsValue.structures.push_back(StructureSettings);
        FitSettingsValue.fitNugget = true;
        FitSettingsValue.nuggetMode = NuggetMode::Exact;
        FitSettingsValue.searchShapeParameters = true;

        FitReport CandidateReport;
        if (FitVariogramWeightedLeastSquares(EmpReport, FitSettingsValue, CandidateReport) && CandidateReport.succeeded)
        {
            bAnySucceeded = true;
            if (CandidateReport.weightedSse < BestSse)
            {
                BestSse = CandidateReport.weightedSse;
                BestReport = CandidateReport;
            }
        }
        else
        {
            CombinedMessages += FString(UTF8_TO_TCHAR(CandidateReport.message.c_str())) + TEXT(" ");
        }
    }

    if (!bAnySucceeded)
    {
        OutMessage = CombinedMessages.IsEmpty() ? TEXT("Variogram fitting failed for all candidate shapes.") : CombinedMessages;
        return false;
    }

    OutVariogramSpec = FromCoreVariogram(BestReport.variogram);
    OutMessage = FString(UTF8_TO_TCHAR(BestReport.message.c_str()));
    if (BestReport.rangeExceedsMaximumLag)
    {
        OutMessage += TEXT(" (Note: fitted range exceeds the maximum lag examined - treat the range with caution.)");
    }
    if (BestReport.nuggetDominates)
    {
        OutMessage += TEXT(" (Note: the fit is nugget-dominated - your data may be mostly noise at the sampled spacing.)");
    }
    return true;
}

bool UKrigingLibrary::EvaluateGrid(UKrigingModel* Model, const FBox& Box, const FIntVector& Resolution, TArray<double>& OutValues)
{
    OutValues.Reset();
    if (Model == nullptr || !Model->IsValid())
    {
        return false;
    }

    ScalarGrid3D Grid;
    FString Error;
    if (!BuildScalarGrid(*Model->GetCoreModel(), Box, Resolution, Grid, Error))
    {
        UE_LOG(LogKrigingBlueprint, Warning, TEXT("EvaluateGrid: %s"), *Error);
        return false;
    }

    OutValues.SetNumUninitialized(static_cast<int32>(Grid.values.size()));
    FMemory::Memcpy(OutValues.GetData(), Grid.values.data(), Grid.values.size() * sizeof(double));
    return true;
}

bool UKrigingLibrary::ExtractIsoSurface(UKrigingModel* Model, double IsoValue, const FBox& Box,
    const FIntVector& Resolution, TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector>& OutNormals)
{
    OutVertices.Reset();
    OutTriangles.Reset();
    OutNormals.Reset();

    if (Model == nullptr || !Model->IsValid())
    {
        return false;
    }

    ScalarGrid3D Grid;
    FString Error;
    if (!BuildScalarGrid(*Model->GetCoreModel(), Box, Resolution, Grid, Error))
    {
        UE_LOG(LogKrigingBlueprint, Warning, TEXT("ExtractIsoSurface: %s"), *Error);
        return false;
    }

    IsoSurfaceMesh Mesh;
    std::string ExtractError;
    if (!ExtractMarchingCubes(Grid, IsoValue, Mesh, &ExtractError))
    {
        UE_LOG(LogKrigingBlueprint, Warning, TEXT("ExtractIsoSurface: %s (isovalue may not intersect the sampled field)"),
            UTF8_TO_TCHAR(ExtractError.c_str()));
        return false;
    }

    OutVertices.Reserve(static_cast<int32>(Mesh.vertices.size()));
    OutNormals.Reserve(static_cast<int32>(Mesh.normals.size()));
    for (std::size_t Index = 0; Index < Mesh.vertices.size(); ++Index)
    {
        const Vec3& V = Mesh.vertices[Index];
        OutVertices.Add(FVector(V.x, V.y, V.z));
        const Vec3& N = Mesh.normals[Index];
        OutNormals.Add(FVector(N.x, N.y, N.z));
    }

    OutTriangles.Reserve(static_cast<int32>(Mesh.indices.size()));
    for (std::uint32_t Index : Mesh.indices)
    {
        OutTriangles.Add(static_cast<int32>(Index));
    }
    return true;
}

bool UKrigingLibrary::ExtractIsoSurfaceToProceduralMesh(UKrigingModel* Model, double IsoValue, const FBox& Box,
    const FIntVector& Resolution, UProceduralMeshComponent* TargetComponent, int32 SectionIndex,
    bool bCreateCollision, bool bFlipWinding)
{
    if (TargetComponent == nullptr)
    {
        return false;
    }

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    if (!ExtractIsoSurface(Model, IsoValue, Box, Resolution, Vertices, Triangles, Normals))
    {
        return false;
    }

    if (bFlipWinding)
    {
        for (int32 Index = 0; Index + 2 < Triangles.Num(); Index += 3)
        {
            Swap(Triangles[Index + 1], Triangles[Index + 2]);
        }
    }

    TArray<FVector2D> EmptyUVs;
    TArray<FProcMeshTangent> EmptyTangents;
    TArray<FColor> EmptyColors;
    TargetComponent->CreateMeshSection(SectionIndex, Vertices, Triangles, Normals, EmptyUVs, EmptyColors, EmptyTangents, bCreateCollision);
    return true;
}
