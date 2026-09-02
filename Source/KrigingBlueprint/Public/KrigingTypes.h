#pragma once

// Blueprint-reflected types for the kriging plugin.
//
// "Kriging" = smart averaging: it blends nearby known values to guess an
// unknown one, weighting closer/more-reliable samples more, and (unlike a
// plain average) it can also tell you how confident that guess is.
//
// These types deliberately do not mirror kriging::portable 1:1. Two
// conventions differ on purpose, both explained where they matter below:
//   1) EffectiveRange (not the core's bare decay constant) is exposed here,
//      because "distance beyond which samples stop mattering" is what every
//      user - geostatistician or not - means by "range". See
//      FKrigingVariogramSpec::EffectiveRange.
//   2) Sill here is the TOTAL plateau (nugget + partial sill), matching what
//      a variogram plot's y-axis shows, not the core's "partialSill" field.
//
// See Source/KrigingBlueprint/Private/KrigingConversions.h for the exact
// conversions applied at the C++ boundary, and
// /mnt/c/Users/HomePC/Documents/ClaudeCode/kriging/Docs (read-only reference
// repo) plus the range-convention note this module was built against for the
// underlying math.

#include "CoreMinimal.h"
#include "KrigingTypes.generated.h"

/**
 * Variogram shape (the curve describing how "difference between two points"
 * grows with distance between them). If you don't know which to pick, use
 * BuildKrigingModelAuto instead of picking one by hand - it fits this for you.
 */
UENUM(BlueprintType)
enum class EKrigingVariogramShape : uint8
{
    /** Smoothly saturating curve that reaches its plateau exactly at the effective range. Good general-purpose default. */
    Spherical UMETA(DisplayName = "Spherical"),
    /** Approaches its plateau gradually and never quite touches it; good for noisy, patchy data. */
    Exponential UMETA(DisplayName = "Exponential"),
    /** Very smooth near the origin (no kink); good for physically smooth fields like temperature. */
    Gaussian UMETA(DisplayName = "Gaussian"),
    /** Tunable smoothness family (see MaternSmoothness). Exponential and Gaussian are special cases of this. */
    Matern UMETA(DisplayName = "Matern"),
    /** Never levels off (no sill) - use only when you know your field genuinely keeps varying at all scales. */
    Power UMETA(DisplayName = "Power"),
};

/** How the model should treat "no other information" locations: plain average, trend-aware, etc. */
UENUM(BlueprintType)
enum class EKrigingMethod : uint8
{
    /** Assumes an unknown, locally-constant average value. Safe default for most scattered-sample use cases. */
    Ordinary UMETA(DisplayName = "Ordinary (recommended default)"),
    /** Assumes a known, fixed global average (KnownMean). Only use this if you actually know that number. */
    Simple UMETA(DisplayName = "Simple (requires known mean)"),
    /** Also fits a linear trend (a slope) across the whole domain in addition to local averaging. */
    UniversalLinear UMETA(DisplayName = "Universal (linear trend)"),
    /** Also fits a quadratic (curved) trend across the whole domain. Use for large-scale bowl/dome shapes. */
    UniversalQuadratic UMETA(DisplayName = "Universal (quadratic trend)"),
    /** Distance-weighted averaging with no statistical model at all. Fast, simple, but no uncertainty estimate. */
    InverseDistance UMETA(DisplayName = "Inverse Distance (fallback, no variogram needed)"),
};

/** Whether querying exactly at a sample snaps to that sample's raw value, or returns the smoothed surface. */
UENUM(BlueprintType)
enum class EKrigingNuggetMode : uint8
{
    /** Querying exactly at a sample location returns that sample's exact value. */
    Exact UMETA(DisplayName = "Exact at samples"),
    /** Even at a sample location, the model returns the smoothed/regularized surface value. */
    Filtered UMETA(DisplayName = "Filtered (smoothed everywhere)"),
};

/** Optional value transform applied before kriging and reversed after (useful for skewed data such as ore grades). */
UENUM(BlueprintType)
enum class EKrigingTransform : uint8
{
    /** Use raw sample values as-is. */
    None UMETA(DisplayName = "None"),
    /** Log-transform before kriging; good for strictly-positive, right-skewed data (concentrations, grades). */
    Logarithmic UMETA(DisplayName = "Logarithmic"),
    /** Rank-based normal-score transform; robust for arbitrary, heavily non-Gaussian distributions. */
    NormalScore UMETA(DisplayName = "Normal Score"),
};

/**
 * One known measurement: a location and the value observed there.
 * Example: a well log reading, a terrain probe, an ore-grade assay.
 */
USTRUCT(BlueprintType)
struct KRIGINGBLUEPRINT_API FKrigingSamplePoint
{
    GENERATED_BODY()

    /** World- or model-local position of this measurement. Keep coordinates in a consistent, origin-shifted local space for numerical stability over large worlds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Sample")
    FVector Location = FVector::ZeroVector;

    /** The measured value at this location (any unit you like - height, concentration, temperature, ...). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Sample")
    double Value = 0.0;
};

/**
 * Anisotropy: lets the "influence distance" stretch further in some directions than others
 * (e.g. a river valley's values carry further along its length than across it).
 * Leave StretchY = StretchZ = 1 for a direction-independent (isotropic) model - the common case.
 */
USTRUCT(BlueprintType)
struct KRIGINGBLUEPRINT_API FKrigingAnisotropySpec
{
    GENERATED_BODY()

    /** Compass heading (degrees) of the direction of longest influence, measured in the XY plane from +X toward +Y. Ignored when StretchY = StretchZ = 1. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Anisotropy")
    double AzimuthDeg = 0.0;

    /** Downward tilt (degrees) of the long axis out of the XY plane. Ignored when StretchY = StretchZ = 1. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Anisotropy")
    double DipDeg = 0.0;

    /** Rotation (degrees) of the short axes about the long axis. Rarely needed outside structural-geology use cases. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Anisotropy")
    double PlungeDeg = 0.0;

    /** How much shorter the second (cross-strike) axis's influence distance is, as a fraction of the primary axis (1 = same as primary, 0.5 = half as far). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Anisotropy", meta = (ClampMin = "0.001"))
    double StretchY = 1.0;

    /** How much shorter the third (vertical/minor) axis's influence distance is, as a fraction of the primary axis. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Anisotropy", meta = (ClampMin = "0.001"))
    double StretchZ = 1.0;
};

/**
 * Describes how "similarity between two samples" fades with distance (the variogram).
 * If you don't want to think about this at all, call BuildKrigingModelAuto, which fits
 * one of these for you from your sample data.
 */
USTRUCT(BlueprintType)
struct KRIGINGBLUEPRINT_API FKrigingVariogramSpec
{
    GENERATED_BODY()

    /** The shape of the falloff curve. Spherical is a reasonable default; use auto-fit if unsure. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram")
    EKrigingVariogramShape Shape = EKrigingVariogramShape::Spherical;

    /**
     * Distance beyond which two samples are treated as essentially unrelated
     * ("effective range" / "practical range" - this is the number every user
     * intuitively means by "range").
     *
     * IMPORTANT internal note: the numerical core (kriging::portable::Structure::range)
     * stores a bare decay constant, not this effective range, and the two are NOT
     * the same number for every shape. This struct always converts, so you never
     * have to think about the difference:
     *   Spherical:    core range a = EffectiveRange            (identical)
     *   Exponential:  core range a = EffectiveRange / 3         (curve reaches ~95% of its plateau at 3a)
     *   Gaussian:     core range a = EffectiveRange * 4 / 7      (curve reaches ~95% of its plateau at (7/4)a)
     *   Matern:       core range a = EffectiveRange (pass-through; see MaternSmoothness note - no closed-form
     *                 effective-range multiplier exists for a general smoothness, so this is treated as an
     *                 approximate correlation length, not a strict 95%-plateau distance)
     *   Power:        core range a = EffectiveRange (pass-through; Power has no plateau/sill at all, so
     *                 "effective range" is not really meaningful for this shape - it is just a scale parameter)
     * See Source/KrigingBlueprint/Private/KrigingConversions.h and the RANGE_CONVENTIONS.md note this
     * plugin was built against for the derivation of the Spherical/Exponential/Gaussian multipliers.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram", meta = (ClampMin = "0.0001"))
    double EffectiveRange = 1000.0;

    /**
     * Total plateau value of the variogram (nugget + partial sill), i.e. roughly the
     * variance of your data at large separation. If unsure, leave the default and let
     * auto-fit determine it, or set it to the sample variance of your values.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram", meta = (ClampMin = "0.0"))
    double Sill = 1.0;

    /**
     * Value at zero distance - represents measurement noise / sub-grid-scale variation.
     * Must be less than or equal to Sill; the remainder (Sill - Nugget) is the "partial sill"
     * the core solves with. A nonzero nugget makes the surface not pass exactly through samples
     * unless NuggetMode is Exact.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram", meta = (ClampMin = "0.0"))
    double Nugget = 0.0;

    /** Whether querying exactly at a sample returns its raw value (Exact) or the smoothed surface (Filtered). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram")
    EKrigingNuggetMode NuggetMode = EKrigingNuggetMode::Exact;

    /**
     * Smoothness parameter, used only when Shape = Matern. Higher = smoother field.
     * 0.5 is identical to Exponential; very large values approach Gaussian.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram", meta = (ClampMin = "0.1", ClampMax = "50.0", EditCondition = "Shape == EKrigingVariogramShape::Matern"))
    double MaternSmoothness = 1.5;

    /**
     * Exponent, used only when Shape = Power. Must be strictly between 0 and 2.
     * 1.0 is a straight-line (linear) variogram.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram", meta = (ClampMin = "0.01", ClampMax = "1.99", EditCondition = "Shape == EKrigingVariogramShape::Power"))
    double PowerExponent = 1.0;

    /** Directional stretching of the influence distance. Leave StretchY = StretchZ = 1 for direction-independent behavior. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Variogram")
    FKrigingAnisotropySpec Anisotropy;
};

/**
 * Curated build controls. Anything not listed here is left at the numerical
 * core's own sensible default - this struct intentionally does not expose
 * every knob the core has, only the ones a non-specialist can reason about.
 */
USTRUCT(BlueprintType)
struct KRIGINGBLUEPRINT_API FKrigingSettings
{
    GENERATED_BODY()

    /** How the model handles the overall trend in your data. Ordinary is the right choice for almost all cases. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings")
    EKrigingMethod Method = EKrigingMethod::Ordinary;

    /** Only used when Method = Simple: the known, fixed average value of the field. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings", meta = (EditCondition = "Method == EKrigingMethod::Simple"))
    double KnownMean = 0.0;

    /** Optional transform applied before kriging and reversed after - useful for strictly-positive, skewed data. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings")
    EKrigingTransform Transform = EKrigingTransform::None;

    /**
     * If true, treats sample locations as 2D (X, Y only, Z ignored) for neighbor search and drift terms -
     * appropriate for surface/terrain-style data. Leave false for volumetric (3D) data such as ore bodies
     * or 4D (3D + time treated as a 4th sample coordinate encoded by the caller) datasets.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings")
    bool bPlanar = false;

    /**
     * Maximum number of nearby samples considered per query when the model uses local (neighborhood)
     * solving. Larger values are more accurate but slower. Only matters once your sample count is large
     * enough that the core switches away from solving one global system.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings", meta = (ClampMin = "1"))
    int32 MaxNeighbours = 32;

    /**
     * How far to search for neighbors during local solving, as a multiplier
     * on the variogram's underlying range parameter. Note this multiplies
     * the core's per-shape range parameter, not FKrigingVariogramSpec's
     * EffectiveRange - for Exponential/Gaussian shapes those differ (see
     * FKrigingVariogramSpec::EffectiveRange), so the actual search distance
     * is not simply "SearchRadiusScale x EffectiveRange" for every shape.
     * The default (1.5) is a reasonable starting point; increase it if
     * distant queries look under-supported, decrease it if local solves are
     * too slow.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings", meta = (ClampMin = "0.1"))
    double SearchRadiusScale = 1.5;

    /** If true, spreads the chosen neighbors across directions (quadrants/octants) instead of just "nearest N" - reduces directional bias from clustered sampling. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings")
    bool bSectorBalanced = true;

    /**
     * Samples within this distance of each other are merged (averaged) into one point before solving.
     * Set to 0 if you need the model to reproduce every sample's exact value independently, and you are
     * certain no two samples are coincident/near-coincident.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings", meta = (ClampMin = "0.0"))
    double MergeRadius = 1.0;

    /** Only meaningful with Transform = Logarithmic: corrects the back-transform for the log transform's statistical bias. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Kriging|Settings", meta = (EditCondition = "Transform == EKrigingTransform::Logarithmic"))
    bool bLognormalBiasCorrection = false;
};

/**
 * Result of building a model: whether it worked, why (if not), and useful diagnostics.
 * Always check bSuccess before using the returned model.
 */
USTRUCT(BlueprintType)
struct KRIGINGBLUEPRINT_API FKrigingBuildResult
{
    GENERATED_BODY()

    /** True if the model built successfully and is safe to sample. */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    bool bSuccess = false;

    /** Human-readable explanation - especially useful when bSuccess is false. */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    FString Message;

    /** Number of sample points supplied to the build call. */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    int32 InputSampleCount = 0;

    /** Number of samples actually used after near-duplicate merging (see FKrigingSettings::MergeRadius). */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    int32 EffectiveSampleCount = 0;

    /** Number of input samples that were merged away as near-duplicates of another sample. */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    int32 MergedAwayCount = 0;

    /** True if the model had to fall back to a degraded/approximate solve path (see Message for detail). */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    bool bDegraded = false;

    /** True if FittedVariogram was produced by auto-fitting rather than supplied explicitly. */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    bool bHasFittedVariogram = false;

    /** The variogram actually used to build the model. Populated for both the auto-fit and explicit build paths. */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    FKrigingVariogramSpec FittedVariogram;

    /**
     * True if CrossValidationRMSE (leave-one-out root-mean-square error) below is a real,
     * meaningful number. Cross-validation is only attempted when it is cheap for the core to
     * compute; when false, ignore CrossValidationRMSE (it is left at 0 and does NOT mean "perfect").
     */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    bool bHasCrossValidation = false;

    /**
     * Leave-one-out cross-validation root-mean-square error, in the same units as your sample
     * values: roughly "how far off is a typical prediction". Only meaningful when
     * bHasCrossValidation is true.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    double CrossValidationRMSE = 0.0;

    /** Non-fatal warnings surfaced by the numerical core (e.g. conditioning adjustments). */
    UPROPERTY(BlueprintReadOnly, Category = "Kriging|Result")
    TArray<FString> Warnings;
};
