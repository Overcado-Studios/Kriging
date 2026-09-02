// Regression tests for the reconstructed KrigePortableAnalysis.h/.cpp:
// empirical variogram estimation (classical and Cressie-Hawkins, directional
// filtering, cell declustering) and weighted least-squares variogram fitting.
//
// Unlike a pure "ballpark" smoke test, most checks here brute-force recompute
// the expected result directly from the sample data using an independent,
// literal transcription of each documented formula and compare bin-by-bin (or
// coefficient-by-coefficient) against the production output with tight
// tolerances. This is deliberate: several plausible one-line regressions in
// KrigePortableAnalysis.cpp (e.g. a factor-of-two error in the classical
// estimator, an off-by-half-bin indexing shift, dropping the pair-count
// factor from the WLS weights, a totalPossiblePairs formula typo, a wrong
// maximum-lag divisor, or reporting bin centres instead of true pair-lag
// means) do not change the *shape* of the output enough for a loose
// ballpark check to notice, but they do change specific numbers that a tight
// independent recomputation will catch immediately.
#include "KrigePortableAnalysis.h"
#include "KrigePortableCore.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace kriging::portable;

namespace
{
int failures = 0;
void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Close(double a, double b, double relative = 1.0e-9, double absolute = 1.0e-11)
{
    return std::abs(a - b) <= std::max(absolute, relative * std::max(std::abs(a), std::abs(b)));
}

// ---------------------------------------------------------------------------
// Independent (brute-force) reference recomputation of the empirical
// variogram bins. This mirrors the documented behaviour of
// ComputeEmpiricalVariogram literally: it is intentionally *not* a copy of
// the production implementation's internals (no linear-pair-index budgeting
// machinery, since every test below keeps pairBudget above C(n,2) so the
// production code visits every pair exactly once, in some order -- and the
// bin accumulations are order-independent sums, so a plain double loop over
// all i<j pairs is an exact, independently-derived reference).
// ---------------------------------------------------------------------------

constexpr double Pi = 3.141592653589793238462643383279502884;

double RefClamp(double value, double minimum, double maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

double RefWrap180(double degrees)
{
    double wrapped = std::fmod(degrees, 180.0);
    if (wrapped < 0.0) wrapped += 180.0;
    return wrapped;
}

double RefDirectionDifference180(double a, double b)
{
    const double difference = std::abs(RefWrap180(a) - RefWrap180(b));
    return std::min(difference, 180.0 - difference);
}

bool RefDirectionAccepted(const Vec3& delta, bool planar,
                          const DirectionalFilter& filter, double separation)
{
    if (!filter.enabled) return true;
    if (!(separation > 0.0)) return false;
    const double azimuth = std::atan2(delta.y, delta.x) * 180.0 / Pi;
    if (RefDirectionDifference180(azimuth, filter.azimuthDeg)
        > RefClamp(filter.azimuthToleranceDeg, 0.0, 90.0))
    {
        return false;
    }
    const double azimuthDifferenceRad = RefDirectionDifference180(azimuth, filter.azimuthDeg)
        * Pi / 180.0;
    if (filter.bandwidth > 0.0 && separation * std::sin(azimuthDifferenceRad) > filter.bandwidth)
    {
        return false;
    }
    if (!planar)
    {
        const double horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        double dip = std::atan2(delta.z, horizontal) * 180.0 / Pi;
        if (dip < -90.0) dip += 180.0;
        if (dip > 90.0) dip -= 180.0;
        const double oppositeDip = -dip;
        const double difference = std::min(std::abs(dip - filter.dipDeg),
                                            std::abs(oppositeDip - filter.dipDeg));
        if (difference > RefClamp(filter.dipToleranceDeg, 0.0, 90.0)) return false;
    }
    return true;
}

struct RefBin
{
    double lag = 0.0;
    double gamma = 0.0;
    std::uint64_t count = 0;
    bool valid = false;
};

std::vector<RefBin> BruteForceEmpiricalBins(const std::vector<Sample>& samples, bool planar,
                                            EmpiricalEstimator estimator,
                                            const DirectionalFilter& filter,
                                            double maximumLag, int binCount)
{
    std::vector<double> lagSum(static_cast<std::size_t>(binCount), 0.0);
    std::vector<double> classicalSum(static_cast<std::size_t>(binCount), 0.0);
    std::vector<double> robustRootSum(static_cast<std::size_t>(binCount), 0.0);
    std::vector<std::uint64_t> count(static_cast<std::size_t>(binCount), 0);
    const double binWidth = maximumLag / static_cast<double>(binCount);
    const int n = static_cast<int>(samples.size());
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            Vec3 delta = samples[static_cast<std::size_t>(j)].location
                - samples[static_cast<std::size_t>(i)].location;
            if (planar) delta.z = 0.0;
            const double separation = std::sqrt(delta.x * delta.x + delta.y * delta.y
                + delta.z * delta.z);
            if (!(separation > 0.0) || separation > maximumLag) continue;
            if (!RefDirectionAccepted(delta, planar, filter, separation)) continue;
            int binIndex = static_cast<int>(std::floor(separation / binWidth));
            if (binIndex == binCount) --binIndex;
            if (binIndex < 0 || binIndex >= binCount) continue;
            const double diff = samples[static_cast<std::size_t>(i)].value
                - samples[static_cast<std::size_t>(j)].value;
            const std::size_t b = static_cast<std::size_t>(binIndex);
            lagSum[b] += separation;
            classicalSum[b] += 0.5 * diff * diff;
            robustRootSum[b] += std::sqrt(std::abs(diff));
            ++count[b];
        }
    }
    std::vector<RefBin> bins(static_cast<std::size_t>(binCount));
    for (int b = 0; b < binCount; ++b)
    {
        const std::size_t idx = static_cast<std::size_t>(b);
        if (count[idx] == 0) continue;
        const double c = static_cast<double>(count[idx]);
        bins[idx].lag = lagSum[idx] / c;
        bins[idx].count = count[idx];
        if (estimator == EmpiricalEstimator::Classical)
        {
            bins[idx].gamma = classicalSum[idx] / c;
        }
        else
        {
            const double averageRoot = robustRootSum[idx] / c;
            bins[idx].gamma = 0.5 * std::pow(averageRoot, 4.0) / (0.457 + 0.494 / c);
        }
        bins[idx].valid = std::isfinite(bins[idx].gamma);
    }
    return bins;
}

void CompareBinsAgainstBruteForce(const EmpiricalReport& report,
                                  const std::vector<Sample>& samples, bool planar,
                                  EmpiricalEstimator estimator,
                                  const DirectionalFilter& filter,
                                  const std::string& label)
{
    const std::vector<RefBin> reference = BruteForceEmpiricalBins(
        samples, planar, estimator, filter, report.maximumLag,
        static_cast<int>(report.bins.size()));
    Check(reference.size() == report.bins.size(), label + ": bin count matches");
    for (std::size_t i = 0; i < std::min(reference.size(), report.bins.size()); ++i)
    {
        const RefBin& expected = reference[i];
        const EmpiricalBin& actual = report.bins[i];
        Check(actual.pairCount == expected.count,
              label + ": bin " + std::to_string(i) + " pair count matches brute force");
        Check(actual.valid == expected.valid,
              label + ": bin " + std::to_string(i) + " validity matches brute force");
        if (expected.count > 0)
        {
            Check(Close(actual.lag, expected.lag, 1.0e-9, 1.0e-9),
                  label + ": bin " + std::to_string(i) + " lag mean matches brute force");
            Check(Close(actual.semivariance, expected.gamma, 1.0e-9, 1.0e-9),
                  label + ": bin " + std::to_string(i) + " gamma matches brute force");
        }
    }
}

} // namespace

namespace
{

// ---------------------------------------------------------------------------
// Section 1: the core empirical/fit pipeline on a genuine spatial-covariance
// realization -- exercises totalPossiblePairs, the default maximum-lag
// derivation, bin-by-bin brute-force agreement (classical estimator), and a
// fitted-sill check against the realization's own sample variance.
// ---------------------------------------------------------------------------
void TestEmpiricalAndFitAgainstBruteForce()
{
    constexpr int N = 300;
    constexpr double TrueSill = 4.0;
    constexpr double TrueRange = 40.0;
    constexpr double DomainSize = 200.0;

    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> uniform(0.0, DomainSize);

    std::vector<Vec3> locations(N);
    for (int i = 0; i < N; ++i)
    {
        locations[static_cast<std::size_t>(i)] = Vec3(uniform(rng), uniform(rng), 0.0);
    }

    // Build the exact covariance matrix C(h) = TrueSill * exp(-h / TrueRange),
    // matching EvaluateNormalizedStructure's Exponential branch.
    DenseMatrix cov(N, N, 0.0);
    for (int i = 0; i < N; ++i)
    {
        for (int j = 0; j < N; ++j)
        {
            const Vec3& a = locations[static_cast<std::size_t>(i)];
            const Vec3& b = locations[static_cast<std::size_t>(j)];
            const double dx = a.x - b.x;
            const double dy = a.y - b.y;
            const double h = std::sqrt(dx * dx + dy * dy);
            cov(i, j) = TrueSill * std::exp(-h / TrueRange);
        }
        cov(i, i) += 1.0e-6; // numerical jitter for positive-definiteness.
    }

    std::vector<double> L(static_cast<std::size_t>(N) * static_cast<std::size_t>(N), 0.0);
    auto At = [&](std::vector<double>& m, int r, int c) -> double&
    {
        return m[static_cast<std::size_t>(r) * static_cast<std::size_t>(N) + static_cast<std::size_t>(c)];
    };
    bool spd = true;
    for (int i = 0; i < N && spd; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            double sum = cov(i, j);
            for (int k = 0; k < j; ++k) sum -= At(L, i, k) * At(L, j, k);
            if (i == j)
            {
                if (!(sum > 0.0)) { spd = false; break; }
                At(L, i, j) = std::sqrt(sum);
            }
            else
            {
                At(L, i, j) = sum / At(L, j, j);
            }
        }
    }
    Check(spd, "covariance matrix admits a Cholesky factor");

    std::normal_distribution<double> normal(0.0, 1.0);
    std::vector<double> z(static_cast<std::size_t>(N));
    for (double& v : z) v = normal(rng);

    std::vector<Sample> samples(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
    {
        double value = 0.0;
        for (int k = 0; k <= i; ++k) value += At(L, i, k) * z[static_cast<std::size_t>(k)];
        samples[static_cast<std::size_t>(i)].location = locations[static_cast<std::size_t>(i)];
        samples[static_cast<std::size_t>(i)].value = value;
        samples[static_cast<std::size_t>(i)].originalIndex = i;
    }

    // Sample variance of the actual realization -- this is what the fitted
    // total sill should be compared against, not the true population sill,
    // since a single finite-domain realization's own variance is what a
    // correct estimator is really recovering.
    double sampleMean = 0.0;
    for (const Sample& s : samples) sampleMean += s.value;
    sampleMean /= static_cast<double>(N);
    double sampleVariance = 0.0;
    for (const Sample& s : samples)
    {
        const double d = s.value - sampleMean;
        sampleVariance += d * d;
    }
    sampleVariance /= static_cast<double>(N - 1);

    // A4: totalPossiblePairs must be exactly n(n-1)/2, not n(n+1)/2 or any
    // other near-miss formula.
    EmpiricalSettings empiricalSettings;
    empiricalSettings.binCount = 15;
    empiricalSettings.pairBudget = 1000000ull; // > C(300,2)=44850, so no subsampling.
    EmpiricalReport empirical;
    Check(ComputeEmpiricalVariogram(samples, /*planar=*/true, empiricalSettings, empirical),
          "empirical variogram computation succeeds: " + empirical.message);
    Check(!empirical.pairsSubsampled, "pair budget was not exceeded");
    Check(empirical.meanBinPopulation >= 10.0, "mean bin population is at least 10");
    Check(empirical.totalPossiblePairs == 44850ull,
          "A4: totalPossiblePairs is exactly n(n-1)/2 = 44850 for n=300");

    // A6: default maximum lag is exactly diagonal / 3, where diagonal is the
    // planar Euclidean extent of the sample bounding box.
    Vec3 minimum{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), 0.0};
    Vec3 maximum{-minimum.x, -minimum.y, 0.0};
    for (const Sample& s : samples)
    {
        minimum.x = std::min(minimum.x, s.location.x);
        minimum.y = std::min(minimum.y, s.location.y);
        maximum.x = std::max(maximum.x, s.location.x);
        maximum.y = std::max(maximum.y, s.location.y);
    }
    const double dx = maximum.x - minimum.x;
    const double dy = maximum.y - minimum.y;
    const double diagonal = std::sqrt(dx * dx + dy * dy);
    Check(Close(empirical.maximumLag, diagonal / 3.0, 1.0e-9, 1.0e-9),
          "A6: default maximum lag equals sample-extent diagonal divided by three");

    // A1/A2/A7: brute-force per-bin lag mean, pair count, and classical
    // gamma must match a literal independent transcription of the formulas.
    CompareBinsAgainstBruteForce(empirical, samples, /*planar=*/true,
                                  EmpiricalEstimator::Classical, DirectionalFilter{},
                                  "classical estimator");

    FitSettings fitSettings;
    FitStructureSettings structure;
    structure.shape = Shape::Exponential;
    fitSettings.structures.push_back(structure);
    fitSettings.fitNugget = true;
    fitSettings.searchShapeParameters = false; // exponential has no free shape parameter.

    FitReport fit;
    Check(FitVariogramWeightedLeastSquares(empirical, fitSettings, fit),
          "weighted least-squares fit succeeds: " + fit.message);
    if (fit.succeeded)
    {
        Check(fit.variogram.structures.size() == 1, "fit returns exactly one structure");
        const double fittedRange = fit.variogram.structures.empty()
            ? 0.0 : fit.variogram.structures[0].range;
        const double fittedPartialSill = fit.variogram.structures.empty()
            ? 0.0 : fit.variogram.structures[0].partialSill;
        const double fittedTotalSill = fit.variogram.nugget + fittedPartialSill;

        std::cout << "true sill=" << TrueSill << " range=" << TrueRange
                  << " realization sample variance=" << sampleVariance << '\n';
        std::cout << "fitted nugget=" << fit.variogram.nugget
                  << " partialSill=" << fittedPartialSill
                  << " range=" << fittedRange
                  << " totalSill=" << fittedTotalSill
                  << " weightedR2=" << fit.weightedR2 << '\n';

        // Compare against the realization's OWN sample variance (tight,
        // +-25%), not the true population sill (loose, +-200%): a single
        // finite-domain draw's empirical variogram is estimating that
        // draw's sample statistics, and a large gap from TrueSill here is
        // expected finite-sample variability, not an estimator bug.
        Check(fittedTotalSill > sampleVariance * 0.75 && fittedTotalSill < sampleVariance * 1.25,
              "recovered total sill is within 25% of the realization's own sample variance");
        Check(fittedRange > TrueRange / 3.0 && fittedRange < TrueRange * 3.0,
              "recovered range is within a factor of three of the truth");
        Check(fit.weightedR2 > 0.7, "weighted R^2 exceeds 0.7");
        Check(!fit.nuggetDominates, "fit does not report nugget-dominated diagnostics");
    }

    // Cressie-Hawkins estimator, same samples: independent brute-force
    // cross-check of the robust-root formula.
    EmpiricalSettings chSettings = empiricalSettings;
    chSettings.estimator = EmpiricalEstimator::CressieHawkins;
    EmpiricalReport chReport;
    Check(ComputeEmpiricalVariogram(samples, /*planar=*/true, chSettings, chReport),
          "Cressie-Hawkins empirical variogram computation succeeds");
    CompareBinsAgainstBruteForce(chReport, samples, /*planar=*/true,
                                  EmpiricalEstimator::CressieHawkins, DirectionalFilter{},
                                  "Cressie-Hawkins estimator");
    // Sanity: the robust estimator's bin values should differ from the
    // classical estimator's (otherwise the CressieHawkins branch is dead
    // code masquerading as exercised).
    bool anyDiffers = false;
    for (std::size_t i = 0; i < chReport.bins.size() && i < empirical.bins.size(); ++i)
    {
        if (chReport.bins[i].valid && empirical.bins[i].valid
            && !Close(chReport.bins[i].semivariance, empirical.bins[i].semivariance, 1.0e-6, 1.0e-9))
        {
            anyDiffers = true;
            break;
        }
    }
    Check(anyDiffers, "Cressie-Hawkins bin values differ from the classical estimator's");

    // Directional filter: restrict to a cone around 40 degrees azimuth with
    // a finite bandwidth, and brute-force cross-check the accepted-pair
    // bins directly (this exercises DirectionAccepted's azimuth, tolerance,
    // and bandwidth branches, which are otherwise dead code).
    EmpiricalSettings directionalSettings = empiricalSettings;
    directionalSettings.direction.enabled = true;
    directionalSettings.direction.azimuthDeg = 40.0;
    directionalSettings.direction.azimuthToleranceDeg = 15.0;
    directionalSettings.direction.bandwidth = 8.0;
    EmpiricalReport directionalReport;
    Check(ComputeEmpiricalVariogram(samples, /*planar=*/true, directionalSettings, directionalReport),
          "directionally-filtered empirical variogram computation succeeds");
    std::uint64_t directionalAccepted = 0;
    for (const EmpiricalBin& bin : directionalReport.bins) directionalAccepted += bin.pairCount;
    Check(directionalAccepted > 0 && directionalAccepted < empirical.acceptedPairs,
          "directional filter accepts a strict, nonempty subset of pairs");
    CompareBinsAgainstBruteForce(directionalReport, samples, /*planar=*/true,
                                  EmpiricalEstimator::Classical, directionalSettings.direction,
                                  "directional filter");
}

// ---------------------------------------------------------------------------
// Section 2: cell declustering (F6 dead-code coverage + A5 mutant: inverted
// per-cell weight). Deliberately clustered fixture: a dense, tight cluster
// of low-value points (oversampling one location) plus a few widely
// separated high-value points. This is positively skewed (required for the
// declustering branch to activate at all), and because the isolated points
// stay in separate cells across the whole cell-size sweep while the cluster
// stays in one cell, the expected declustered mean is exactly the mean of
// per-cell means -- computable in closed form independent of which sweep
// step wins.
// ---------------------------------------------------------------------------
void TestCellDeclusteringAndInvertedWeightMutant()
{
    std::vector<Sample> samples;
    int index = 0;
    constexpr int ClusterCount = 30;
    constexpr double ClusterValue = 1.0;
    constexpr double IsolatedValue = 10.0;
    for (int i = 0; i < ClusterCount; ++i)
    {
        // Tight jitter (<< every swept cell size, whose minimum is 1.0) so
        // the whole cluster always falls in a single cell.
        samples.push_back({{0.001 * i, 0.0, 0.0}, ClusterValue, 0.0, index++});
    }
    samples.push_back({{100.0, 0.0, 0.0}, IsolatedValue, 0.0, index++});
    samples.push_back({{0.0, 100.0, 0.0}, IsolatedValue, 0.0, index++});
    samples.push_back({{100.0, 100.0, 0.0}, IsolatedValue, 0.0, index++});

    const double naiveMean = (ClusterCount * ClusterValue + 3.0 * IsolatedValue)
        / static_cast<double>(ClusterCount + 3);

    EmpiricalSettings settings;
    settings.binCount = 5;
    settings.computeDeclustering = true;
    settings.pairBudget = 1000000ull;
    EmpiricalReport report;
    Check(ComputeEmpiricalVariogram(samples, /*planar=*/true, settings, report),
          "clustered-fixture empirical variogram computation succeeds");
    Check(report.declustering.applied,
          "positively-skewed clustered fixture activates the declustering branch");
    Check(report.declustering.positivelySkewed,
          "clustered fixture is detected as positively skewed");

    // Every swept cell size keeps the cluster in one cell and the three
    // isolated points in three separate cells (they are 100 units apart,
    // and the swept cell size never exceeds extent/100*25 = 25), so the
    // declustered mean is exactly the mean of the four per-cell means:
    // (ClusterValue + 3*IsolatedValue) / 4, independent of which sweep step
    // the search picks.
    const double expectedDeclusteredMean = (ClusterValue + 3.0 * IsolatedValue) / 4.0;
    Check(Close(report.declustering.declusteredMean, expectedDeclusteredMean, 1.0e-6, 1.0e-6),
          "A5: declustered mean matches the closed-form per-cell-mean-of-means "
          "(inverted-weight mutant would instead reproduce something close to "
          "the naive arithmetic mean of " + std::to_string(naiveMean) + ")");
    Check(std::abs(report.declustering.declusteredMean - naiveMean)
              > std::abs(expectedDeclusteredMean - naiveMean) * 0.5,
          "declustered mean moved substantially toward the spatially-fair mean, away from the naive mean");
}

// ---------------------------------------------------------------------------
// Section 3: A3 mutant (WLS weights drop the pairCount factor). Bypasses
// ComputeEmpiricalVariogram entirely and hand-builds an EmpiricalReport whose
// bins are all placed at a lag so far beyond every candidate range in the
// fit's search grid that EvaluateNormalizedStructure saturates to 1.0 (bit
// for bit) for every bin, for every candidate range. That collapses the
// model to a single constant regressor (nugget + partial sill acting as one
// combined term), so the fitted total is exactly the definitional weighted
// mean of the bin semivariances -- weight_i = pairCount_i / gamma_i^2 for
// correct code. The three pair counts here are chosen (1, 1, 1000) so that
// dropping the pairCount factor from the weight changes that weighted mean
// by more than 4x, far outside the assertion's tolerance.
// ---------------------------------------------------------------------------
void TestWlsWeightsUsePairCountMutant()
{
    EmpiricalReport empirical;
    empirical.succeeded = true;
    empirical.message = "fabricated for WLS weight unit test";
    empirical.maximumLag = 1.0;
    empirical.totalPossiblePairs = 190ull; // n(n-1)/2 for n=20, clears the >=20 gate.
    empirical.acceptedPairs = 1002ull;
    empirical.meanBinPopulation = 300.0; // clears the >=10 gate.

    auto MakeBin = [](double lag, double semivariance, std::uint64_t pairCount)
    {
        EmpiricalBin bin;
        bin.lag = lag;
        bin.semivariance = semivariance;
        bin.pairCount = pairCount;
        bin.valid = true;
        return bin;
    };
    // lag=100 against a range search grid confined to [0.05, 1.5] (since
    // maximumLag=1.0): ratio >= 100/1.5 ~= 66.7, so exp(-ratio) underflows
    // to exactly 0.0 in double precision for every candidate range, and the
    // exponential structure's normalized value is bit-identical to 1.0.
    empirical.bins.push_back(MakeBin(100.0, 1.0, 1));
    empirical.bins.push_back(MakeBin(100.0, 1.0, 1));
    empirical.bins.push_back(MakeBin(100.0, 5.0, 1000));

    FitSettings settings;
    FitStructureSettings structure;
    structure.shape = Shape::Exponential;
    settings.structures.push_back(structure);
    settings.fitNugget = true;
    settings.searchShapeParameters = false;

    FitReport fit;
    Check(FitVariogramWeightedLeastSquares(empirical, settings, fit),
          "fabricated saturated-lag fit succeeds: " + fit.message);
    if (fit.succeeded)
    {
        Check(fit.variogram.structures.size() == 1, "fabricated fit returns one structure");
        const double total = fit.variogram.nugget
            + (fit.variogram.structures.empty() ? 0.0 : fit.variogram.structures[0].partialSill);
        // weighted mean with weight_i = pairCount_i / gamma_i^2:
        //   w = (1/1^2, 1/1^2, 1000/5^2) = (1, 1, 40)
        //   mean = (1*1 + 1*1 + 40*5) / (1+1+40) = 202/42
        constexpr double ExpectedWeightedMean = 202.0 / 42.0;
        Check(Close(total, ExpectedWeightedMean, 1.0e-6, 1.0e-6),
              "A3: fitted total (nugget+partialSill) matches the pairCount-weighted mean "
              "202/42 = " + std::to_string(ExpectedWeightedMean)
              + " (a mutant dropping the pairCount factor would instead give ~"
              + std::to_string(2.2 / 2.04) + ")");
    }
}

} // namespace

int main()
{
    TestEmpiricalAndFitAgainstBruteForce();
    TestCellDeclusteringAndInvertedWeightMutant();
    TestWlsWeightsUsePairCountMutant();

    if (failures == 0)
    {
        std::cout << "Portable analysis smoke test passed.\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
