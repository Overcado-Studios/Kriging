#pragma once

// Boundary conversions between the Blueprint-facing structs (KrigingTypes.h)
// and kriging::portable's plain-C++ types. Kept in one place so the
// EffectiveRange<->range and Sill<->partialSill conventions (see
// KrigingTypes.h::FKrigingVariogramSpec) are each defined exactly once and
// used identically in both directions (spec -> core for building, core ->
// spec for reporting fitted variograms back to Blueprint).

#include "CoreMinimal.h"
#include "KrigingTypes.h"
#include "KrigePortableCore.h"

namespace KrigingConversions
{

using kriging::portable::Shape;

/**
 * Converts this struct's user-facing EffectiveRange into the core's bare
 * decay-constant "range" (kriging::portable::Structure::range), per shape.
 * See FKrigingVariogramSpec::EffectiveRange for the derivation and the
 * RANGE_CONVENTIONS.md note this plugin was built against.
 */
inline double EffectiveRangeToCoreRange(EKrigingVariogramShape Shape, double EffectiveRange)
{
    switch (Shape)
    {
    case EKrigingVariogramShape::Spherical:
        return EffectiveRange;
    case EKrigingVariogramShape::Exponential:
        return EffectiveRange / 3.0;
    case EKrigingVariogramShape::Gaussian:
        return EffectiveRange * 4.0 / 7.0;
    case EKrigingVariogramShape::Matern:
    case EKrigingVariogramShape::Power:
    default:
        // No closed-form effective-range multiplier exists for a general
        // Matern smoothness (CONVENTIONS.md: the Gaussian limit only holds
        // as nu -> infinity, with a_gauss = sqrt(2) * a_matern at that
        // limit and no fixed ratio below it), and Power has no sill/plateau
        // at all. Both are passed through unconverted and documented as
        // such in the property tooltip.
        return EffectiveRange;
    }
}

/** Inverse of EffectiveRangeToCoreRange - used when reporting a fitted variogram back to Blueprint. */
inline double CoreRangeToEffectiveRange(EKrigingVariogramShape Shape, double CoreRange)
{
    switch (Shape)
    {
    case EKrigingVariogramShape::Spherical:
        return CoreRange;
    case EKrigingVariogramShape::Exponential:
        return CoreRange * 3.0;
    case EKrigingVariogramShape::Gaussian:
        return CoreRange * 7.0 / 4.0;
    case EKrigingVariogramShape::Matern:
    case EKrigingVariogramShape::Power:
    default:
        return CoreRange;
    }
}

inline kriging::portable::Shape ToCoreShape(EKrigingVariogramShape Shape)
{
    switch (Shape)
    {
    case EKrigingVariogramShape::Spherical: return kriging::portable::Shape::Spherical;
    case EKrigingVariogramShape::Exponential: return kriging::portable::Shape::Exponential;
    case EKrigingVariogramShape::Gaussian: return kriging::portable::Shape::Gaussian;
    case EKrigingVariogramShape::Matern: return kriging::portable::Shape::Matern;
    case EKrigingVariogramShape::Power: return kriging::portable::Shape::Power;
    default: return kriging::portable::Shape::Spherical;
    }
}

inline EKrigingVariogramShape FromCoreShape(kriging::portable::Shape Shape)
{
    switch (Shape)
    {
    case kriging::portable::Shape::Spherical: return EKrigingVariogramShape::Spherical;
    case kriging::portable::Shape::Exponential: return EKrigingVariogramShape::Exponential;
    case kriging::portable::Shape::Gaussian: return EKrigingVariogramShape::Gaussian;
    case kriging::portable::Shape::Matern: return EKrigingVariogramShape::Matern;
    case kriging::portable::Shape::Power: return EKrigingVariogramShape::Power;
    default: return EKrigingVariogramShape::Spherical;
    }
}

inline kriging::portable::NuggetMode ToCoreNuggetMode(EKrigingNuggetMode Mode)
{
    return Mode == EKrigingNuggetMode::Exact
        ? kriging::portable::NuggetMode::Exact
        : kriging::portable::NuggetMode::Filtered;
}

inline EKrigingNuggetMode FromCoreNuggetMode(kriging::portable::NuggetMode Mode)
{
    return Mode == kriging::portable::NuggetMode::Exact
        ? EKrigingNuggetMode::Exact
        : EKrigingNuggetMode::Filtered;
}

inline kriging::portable::Anisotropy ToCoreAnisotropy(const FKrigingAnisotropySpec& Spec)
{
    kriging::portable::Anisotropy Out;
    Out.azimuthDeg = Spec.AzimuthDeg;
    Out.dipDeg = Spec.DipDeg;
    Out.plungeDeg = Spec.PlungeDeg;
    Out.ratioY = Spec.StretchY;
    Out.ratioZ = Spec.StretchZ;
    return Out;
}

inline FKrigingAnisotropySpec FromCoreAnisotropy(const kriging::portable::Anisotropy& Aniso)
{
    FKrigingAnisotropySpec Out;
    Out.AzimuthDeg = Aniso.azimuthDeg;
    Out.DipDeg = Aniso.dipDeg;
    Out.PlungeDeg = Aniso.plungeDeg;
    Out.StretchY = Aniso.ratioY;
    Out.StretchZ = Aniso.ratioZ;
    return Out;
}

/**
 * Builds a single-structure core Variogram from a Blueprint spec.
 * Sill here is the TOTAL plateau (nugget + partial sill); the core's
 * Structure::partialSill is (Sill - Nugget) clamped to be non-negative,
 * per FKrigingVariogramSpec::Sill's documented convention.
 */
inline kriging::portable::Variogram ToCoreVariogram(const FKrigingVariogramSpec& Spec)
{
    kriging::portable::Variogram Out;
    Out.nugget = FMath::Max(Spec.Nugget, 0.0);
    Out.nuggetMode = ToCoreNuggetMode(Spec.NuggetMode);

    kriging::portable::Structure Structure;
    Structure.shape = ToCoreShape(Spec.Shape);
    Structure.range = EffectiveRangeToCoreRange(Spec.Shape, Spec.EffectiveRange);
    Structure.partialSill = FMath::Max(Spec.Sill - Out.nugget, 0.0);
    Structure.maternNu = Spec.MaternSmoothness;
    Structure.powerAlpha = Spec.PowerExponent;
    Structure.anisotropy = ToCoreAnisotropy(Spec.Anisotropy);
    Out.structures.push_back(Structure);
    return Out;
}

/**
 * Reports a (possibly nested/multi-structure) core Variogram back as a single
 * Blueprint spec. Only the first structure is represented explicitly; this
 * module's Blueprint surface is intentionally single-structure (see
 * STAGING_NOTES.md "Design decisions" for why), so nested-structure fits
 * from FitVariogramWeightedLeastSquares are summarized by their dominant
 * (first, largest-partial-sill-first as returned by the core) structure with
 * the total sill still reported correctly across all structures.
 */
inline FKrigingVariogramSpec FromCoreVariogram(const kriging::portable::Variogram& Variogram)
{
    FKrigingVariogramSpec Out;
    Out.Nugget = Variogram.nugget;
    Out.NuggetMode = FromCoreNuggetMode(Variogram.nuggetMode);

    double TotalPartialSill = 0.0;
    for (const kriging::portable::Structure& Structure : Variogram.structures)
    {
        TotalPartialSill += Structure.partialSill;
    }
    Out.Sill = Variogram.nugget + TotalPartialSill;

    if (!Variogram.structures.empty())
    {
        const kriging::portable::Structure& Structure = Variogram.structures.front();
        Out.Shape = FromCoreShape(Structure.shape);
        Out.EffectiveRange = CoreRangeToEffectiveRange(Out.Shape, Structure.range);
        Out.MaternSmoothness = Structure.maternNu;
        Out.PowerExponent = Structure.powerAlpha;
        Out.Anisotropy = FromCoreAnisotropy(Structure.anisotropy);
    }
    return Out;
}

inline kriging::portable::Method ToCoreMethod(EKrigingMethod Method)
{
    switch (Method)
    {
    case EKrigingMethod::Ordinary: return kriging::portable::Method::Ordinary;
    case EKrigingMethod::Simple: return kriging::portable::Method::Simple;
    case EKrigingMethod::UniversalLinear: return kriging::portable::Method::UniversalLinear;
    case EKrigingMethod::UniversalQuadratic: return kriging::portable::Method::UniversalQuadratic;
    case EKrigingMethod::InverseDistance: return kriging::portable::Method::InverseDistance;
    default: return kriging::portable::Method::Ordinary;
    }
}

inline kriging::portable::Transform ToCoreTransform(EKrigingTransform Transform)
{
    switch (Transform)
    {
    case EKrigingTransform::None: return kriging::portable::Transform::None;
    case EKrigingTransform::Logarithmic: return kriging::portable::Transform::Logarithmic;
    case EKrigingTransform::NormalScore: return kriging::portable::Transform::NormalScore;
    default: return kriging::portable::Transform::None;
    }
}

/**
 * Builds core Settings from a Blueprint settings struct. Fields not exposed
 * on FKrigingSettings (globalSolveThreshold, solveMode, external drift, ...)
 * are left at the core's own defaults on purpose - see FKrigingSettings'
 * class comment.
 */
inline kriging::portable::Settings ToCoreSettings(const FKrigingSettings& Settings)
{
    kriging::portable::Settings Out;
    Out.method = ToCoreMethod(Settings.Method);
    Out.transform = ToCoreTransform(Settings.Transform);
    Out.planar = Settings.bPlanar;
    Out.knownMean = Settings.KnownMean;
    Out.maxNeighbours = Settings.MaxNeighbours;
    Out.searchRadiusScale = Settings.SearchRadiusScale;
    Out.sectorBalanced = Settings.bSectorBalanced;
    Out.mergeRadius = Settings.MergeRadius;
    Out.lognormalBiasCorrection = Settings.bLognormalBiasCorrection;
    return Out;
}

/** Converts Blueprint sample points to the core's Sample type, assigning originalIndex = array position (required for the core's deterministic tie-breaking - see CONVENTIONS.md "Determinism"). */
inline std::vector<kriging::portable::Sample> ToCoreSamples(const TArray<FKrigingSamplePoint>& Samples)
{
    std::vector<kriging::portable::Sample> Out;
    Out.reserve(static_cast<std::size_t>(Samples.Num()));
    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const FKrigingSamplePoint& InSample = Samples[Index];
        kriging::portable::Sample OutSample;
        OutSample.location = kriging::portable::Vec3(InSample.Location.X, InSample.Location.Y, InSample.Location.Z);
        OutSample.value = InSample.Value;
        OutSample.measurementVariance = 0.0;
        OutSample.originalIndex = Index;
        Out.push_back(OutSample);
    }
    return Out;
}

/**
 * Fills the shared fields of FKrigingBuildResult from a core BuildReport.
 * Does NOT set FittedVariogram/bHasFittedVariogram or cross-validation
 * fields - callers set those explicitly since they differ between the
 * auto-fit and explicit build paths.
 *
 * APPENDS to OutResult.Warnings rather than resetting it: callers (notably
 * BuildKrigingModelAuto's heuristic-fallback path in KrigingLibrary.cpp) may
 * have already populated a warning before this runs, and this must not
 * silently discard it.
 */
inline void FillBuildResultFromReport(const kriging::portable::BuildReport& Report, int32 InputSampleCount, FKrigingBuildResult& OutResult)
{
    OutResult.bSuccess = Report.succeeded;
    OutResult.Message = UTF8_TO_TCHAR(Report.message.c_str());
    OutResult.InputSampleCount = InputSampleCount;
    OutResult.EffectiveSampleCount = Report.effectiveCount;
    OutResult.MergedAwayCount = Report.mergedPointCount;
    OutResult.bDegraded = Report.degraded;
    OutResult.Warnings.Reserve(OutResult.Warnings.Num() + static_cast<int32>(Report.warnings.size()));
    for (const std::string& Warning : Report.warnings)
    {
        OutResult.Warnings.Add(FString(UTF8_TO_TCHAR(Warning.c_str())));
    }
}

} // namespace KrigingConversions
