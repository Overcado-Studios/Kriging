#pragma once

// Reconstructed header for KrigePortableAnalysis.cpp. Declares exactly the
// types and free functions the translation unit defines and consumes:
// empirical variogram estimation (classical and Cressie-Hawkins, directional
// filtering, pair-budget subsampling, variogram maps, cell declustering) and
// weighted least-squares variogram fitting (nonnegative-sill active set,
// nested structures, shape-parameter search, golden-section range refinement).
//
// Style follows KrigePortableCore.h: kriging::portable namespace, C++17,
// no Unreal dependencies.

#include "KrigePortableCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kriging::portable
{

// ---------------------------------------------------------------------------
// Empirical variogram estimation
// ---------------------------------------------------------------------------

enum class EmpiricalEstimator : std::uint8_t { Classical, CressieHawkins };

struct DirectionalFilter
{
    bool enabled = false;
    double azimuthDeg = 0.0;
    double azimuthToleranceDeg = 22.5;
    double bandwidth = 0.0;
    double dipDeg = 0.0;
    double dipToleranceDeg = 22.5;
};

struct EmpiricalSettings
{
    int binCount = 15;
    double maximumLag = 0.0; // <= 0 means derive from sample extent / 3.
    EmpiricalEstimator estimator = EmpiricalEstimator::Classical;
    DirectionalFilter direction;
    bool computeVariogramMap = false;
    int variogramMapSize = 21; // forced odd, minimum 3.
    bool computeDeclustering = false;
    std::uint64_t pairBudget = 20000000ull;
    std::uint64_t randomSeed = 1469598103934665603ull;
};

struct EmpiricalBin
{
    double lag = 0.0;
    double semivariance = 0.0;
    std::uint64_t pairCount = 0;
    bool valid = false;
};

struct VariogramMap
{
    int size = 0;
    double maximumLag = 0.0;
    std::vector<double> semivariance;
    std::vector<std::uint64_t> pairCounts;
};

struct DeclusteringReport
{
    bool applied = false;
    bool positivelySkewed = false;
    double chosenCellSize = 0.0;
    double declusteredMean = 0.0;
    std::vector<double> sampleWeights;
};

struct EmpiricalReport
{
    bool succeeded = false;
    std::string message;
    double maximumLag = 0.0;
    std::vector<EmpiricalBin> bins;
    VariogramMap map;
    std::uint64_t totalPossiblePairs = 0;
    std::uint64_t visitedPairs = 0;
    std::uint64_t acceptedPairs = 0;
    bool pairsSubsampled = false;
    double meanBinPopulation = 0.0;
    DeclusteringReport declustering;
    std::vector<std::string> warnings;
};

// Computes the empirical semivariogram (and optionally a variogram map and a
// cell-declustering weight set) from scattered samples. Requires at least two
// samples with finite coordinates and values. Returns false with a diagnostic
// message on failure (e.g. no pairs survive the lag/direction filters).
KRIGINGCORE_API bool ComputeEmpiricalVariogram(const std::vector<Sample>& samples,
                                               bool planar,
                                               const EmpiricalSettings& settings,
                                               EmpiricalReport& outReport);

// Evaluates a (possibly nested) semivariogram model at a single lag distance.
// Returns 0 for a non-positive lag (the semivariogram is zero at the origin).
KRIGINGCORE_API double EvaluateSemivariogramAtLag(const Variogram& variogram, double lag);

// ---------------------------------------------------------------------------
// Weighted least-squares variogram fitting
// ---------------------------------------------------------------------------

struct FitStructureSettings
{
    Shape shape = Shape::Spherical;
    Anisotropy anisotropy;
    double initialMaternNu = 1.5;
    double initialPowerAlpha = 1.0;
};

struct FitSettings
{
    std::vector<FitStructureSettings> structures; // one to three entries.
    bool fitNugget = true;
    NuggetMode nuggetMode = NuggetMode::Exact;
    bool searchShapeParameters = true;
    int singleRangeGrid = 24;
    int tripleRangeGrid = 8;
    int goldenPasses = 3;
};

struct FitReport
{
    bool succeeded = false;
    std::string message;
    Variogram variogram;
    double weightedSse = 0.0;
    double weightedR2 = 0.0;
    bool rangeExceedsMaximumLag = false;
    bool nuggetDominates = false;
    std::vector<std::string> warnings;
};

// Fits a nested variogram model (1-3 structures) to an empirical variogram via
// weighted least squares with a nonnegative-sill active-set search over
// per-structure range (log-spaced grid + golden-section refinement) and, when
// enabled, Matern nu / power alpha. Requires a successful empirical variogram
// with an inferred sample count of at least 20 and a mean bin population of
// at least 10; returns false with a diagnostic message otherwise.
KRIGINGCORE_API bool FitVariogramWeightedLeastSquares(const EmpiricalReport& empirical,
                                                       const FitSettings& settings,
                                                       FitReport& outReport);

} // namespace kriging::portable
