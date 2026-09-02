#include "KrigePortableAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace kriging::portable
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;

bool Finite(double value)
{
    return std::isfinite(value) != 0;
}

double Clamp(double value, double minimum, double maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

double DegreesToRadians(double degrees)
{
    return degrees * Pi / 180.0;
}

double Wrap180(double degrees)
{
    double wrapped = std::fmod(degrees, 180.0);
    if (wrapped < 0.0) wrapped += 180.0;
    return wrapped;
}

double DirectionDifference180(double a, double b)
{
    const double difference = std::abs(Wrap180(a) - Wrap180(b));
    return std::min(difference, 180.0 - difference);
}

double Distance(const Vec3& a, const Vec3& b, bool planar)
{
    const double x = a.x - b.x;
    const double y = a.y - b.y;
    const double z = planar ? 0.0 : a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}

std::uint64_t PairCount(std::uint64_t count)
{
    return count < 2 ? 0 : count * (count - 1) / 2;
}

std::uint64_t PrefixPairs(std::uint64_t i, std::uint64_t count)
{
    return i * (2 * count - i - 1) / 2;
}

std::pair<std::uint64_t, std::uint64_t> PairFromLinear(
    std::uint64_t linear,
    std::uint64_t count)
{
    const long double b = static_cast<long double>(2 * count - 1);
    const long double discriminant = std::max(
        0.0L,
        b * b - 8.0L * static_cast<long double>(linear));
    std::uint64_t i = static_cast<std::uint64_t>(
        std::floor((b - std::sqrt(discriminant)) * 0.5L));
    if (i >= count - 1) i = count - 2;
    while (i > 0 && PrefixPairs(i, count) > linear) --i;
    while (i + 1 < count - 1 && PrefixPairs(i + 1, count) <= linear) ++i;
    const std::uint64_t offset = linear - PrefixPairs(i, count);
    return {i, i + 1 + offset};
}

std::uint64_t GreatestCommonDivisor(std::uint64_t a, std::uint64_t b)
{
    while (b != 0)
    {
        const std::uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

std::uint64_t Mix64(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

bool DirectionAccepted(const Vec3& delta,
                       bool planar,
                       const DirectionalFilter& filter,
                       double separation)
{
    if (!filter.enabled) return true;
    if (!(separation > 0.0)) return false;

    const double azimuth = std::atan2(delta.y, delta.x) * 180.0 / Pi;
    if (DirectionDifference180(azimuth, filter.azimuthDeg)
        > Clamp(filter.azimuthToleranceDeg, 0.0, 90.0))
    {
        return false;
    }

    const double azimuthDifference = DegreesToRadians(
        DirectionDifference180(azimuth, filter.azimuthDeg));
    if (filter.bandwidth > 0.0
        && separation * std::sin(azimuthDifference) > filter.bandwidth)
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
        const double difference = std::min(
            std::abs(dip - filter.dipDeg),
            std::abs(oppositeDip - filter.dipDeg));
        if (difference > Clamp(filter.dipToleranceDeg, 0.0, 90.0))
        {
            return false;
        }
    }
    return true;
}

struct BinAccumulator
{
    double lagSum = 0.0;
    double classicalSum = 0.0;
    double robustRootSum = 0.0;
    std::uint64_t count = 0;
};

struct CellKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const CellKey& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct CellKeyHash
{
    std::size_t operator()(const CellKey& key) const
    {
        std::uint64_t value = Mix64(static_cast<std::uint64_t>(key.x));
        value ^= Mix64(static_cast<std::uint64_t>(key.y) + 0x1234ull);
        value ^= Mix64(static_cast<std::uint64_t>(key.z) + 0x5678ull);
        return static_cast<std::size_t>(value);
    }
};

DeclusteringReport ComputeDeclustering(const std::vector<Sample>& samples, bool planar)
{
    DeclusteringReport report;
    report.sampleWeights.assign(samples.size(), 1.0);
    if (samples.size() < 3) return report;

    double mean = 0.0;
    for (const Sample& sample : samples) mean += sample.value;
    mean /= static_cast<double>(samples.size());
    double second = 0.0;
    double third = 0.0;
    for (const Sample& sample : samples)
    {
        const double delta = sample.value - mean;
        second += delta * delta;
        third += delta * delta * delta;
    }
    second /= static_cast<double>(samples.size());
    third /= static_cast<double>(samples.size());
    report.positivelySkewed = second > 0.0
        && third / std::pow(second, 1.5) > 0.25;
    if (!report.positivelySkewed)
    {
        report.declusteredMean = mean;
        return report;
    }

    Vec3 minimum{std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity()};
    Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    for (const Sample& sample : samples)
    {
        minimum.x = std::min(minimum.x, sample.location.x);
        minimum.y = std::min(minimum.y, sample.location.y);
        minimum.z = std::min(minimum.z, sample.location.z);
        maximum.x = std::max(maximum.x, sample.location.x);
        maximum.y = std::max(maximum.y, sample.location.y);
        maximum.z = std::max(maximum.z, sample.location.z);
    }
    const double extent = std::max({maximum.x - minimum.x,
                                    maximum.y - minimum.y,
                                    planar ? 0.0 : maximum.z - minimum.z});
    if (!(extent > 0.0))
    {
        report.declusteredMean = mean;
        return report;
    }

    double bestMean = std::numeric_limits<double>::infinity();
    double bestSize = 0.0;
    std::vector<double> bestWeights(samples.size(), 1.0);
    constexpr int SweepCount = 24;
    for (int step = 0; step < SweepCount; ++step)
    {
        const double fraction = static_cast<double>(step)
            / static_cast<double>(SweepCount - 1);
        const double cellSize = (extent / 100.0)
            * std::pow(25.0, fraction);
        std::unordered_map<CellKey, int, CellKeyHash> occupancy;
        occupancy.reserve(samples.size() * 2);
        std::vector<CellKey> keys(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const Vec3& point = samples[i].location;
            CellKey key{
                static_cast<std::int64_t>(std::floor((point.x - minimum.x) / cellSize)),
                static_cast<std::int64_t>(std::floor((point.y - minimum.y) / cellSize)),
                planar ? 0 : static_cast<std::int64_t>(
                    std::floor((point.z - minimum.z) / cellSize))};
            keys[i] = key;
            ++occupancy[key];
        }
        std::vector<double> weights(samples.size(), 0.0);
        double weightSum = 0.0;
        double weightedMean = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const double weight = 1.0
                / static_cast<double>(occupancy[keys[i]]);
            weights[i] = weight;
            weightSum += weight;
            weightedMean += weight * samples[i].value;
        }
        if (!(weightSum > 0.0)) continue;
        weightedMean /= weightSum;
        const double normalization = static_cast<double>(samples.size()) / weightSum;
        for (double& weight : weights) weight *= normalization;
        if (weightedMean < bestMean)
        {
            bestMean = weightedMean;
            bestSize = cellSize;
            bestWeights = std::move(weights);
        }
    }

    if (Finite(bestMean))
    {
        report.applied = true;
        report.chosenCellSize = bestSize;
        report.declusteredMean = bestMean;
        report.sampleWeights = std::move(bestWeights);
    }
    else
    {
        report.declusteredMean = mean;
    }
    return report;
}

struct FitCandidate
{
    std::vector<double> ranges;
    std::vector<double> shapeParameters;
    std::vector<double> coefficients;
    double sse = std::numeric_limits<double>::infinity();
};

double ShapeValue(Shape shape, double lag, double range, double shapeParameter)
{
    if (!(range > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return EvaluateNormalizedStructure(
        shape,
        lag / range,
        shape == Shape::Matern ? shapeParameter : 1.5,
        shape == Shape::Power ? shapeParameter : 1.0);
}

bool SolveActiveSet(const std::vector<EmpiricalBin>& bins,
                    const std::vector<FitStructureSettings>& structures,
                    const std::vector<double>& ranges,
                    const std::vector<double>& shapeParameters,
                    bool fitNugget,
                    FitCandidate& out)
{
    const int modelColumns = static_cast<int>(structures.size()) + 1;
    std::vector<std::array<double, 4>> rows;
    std::vector<double> targets;
    std::vector<double> weights;
    rows.reserve(bins.size());
    targets.reserve(bins.size());
    weights.reserve(bins.size());
    for (const EmpiricalBin& bin : bins)
    {
        if (!bin.valid || bin.pairCount == 0 || !Finite(bin.semivariance)) continue;
        std::array<double, 4> row{};
        row[0] = 1.0;
        bool valid = true;
        for (std::size_t structureIndex = 0;
             structureIndex < structures.size(); ++structureIndex)
        {
            row[structureIndex + 1] = ShapeValue(
                structures[structureIndex].shape,
                bin.lag,
                ranges[structureIndex],
                shapeParameters[structureIndex]);
            valid = valid && Finite(row[structureIndex + 1]);
        }
        if (!valid) return false;
        const double gamma = std::max(std::abs(bin.semivariance), 1.0e-12);
        rows.push_back(row);
        targets.push_back(bin.semivariance);
        weights.push_back(static_cast<double>(bin.pairCount) / (gamma * gamma));
    }
    if (rows.size() < static_cast<std::size_t>(modelColumns)) return false;

    const int maskBegin = fitNugget ? 1 : 2;
    const int maskEnd = 1 << modelColumns;
    FitCandidate best;
    for (int mask = maskBegin; mask < maskEnd; ++mask)
    {
        if (!fitNugget && (mask & 1) != 0) continue;
        std::array<int, 4> active{};
        int activeCount = 0;
        for (int column = 0; column < modelColumns; ++column)
        {
            if ((mask & (1 << column)) != 0)
            {
                active[static_cast<std::size_t>(activeCount++)] = column;
            }
        }
        if (activeCount == 0) continue;
        DenseMatrix normal(activeCount, activeCount, 0.0);
        std::vector<double> rhs(static_cast<std::size_t>(activeCount), 0.0);
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            const double weight = weights[rowIndex];
            for (int a = 0; a < activeCount; ++a)
            {
                const double xa = rows[rowIndex][static_cast<std::size_t>(
                    active[static_cast<std::size_t>(a)])];
                rhs[static_cast<std::size_t>(a)] += weight * xa * targets[rowIndex];
                for (int b = 0; b < activeCount; ++b)
                {
                    const double xb = rows[rowIndex][static_cast<std::size_t>(
                        active[static_cast<std::size_t>(b)])];
                    normal(a, b) += weight * xa * xb;
                }
            }
        }
        PartialPivLU solver;
        if (!solver.Factorize(normal)) continue;
        std::vector<double> activeSolution;
        if (!solver.Solve(rhs, activeSolution)) continue;
        std::vector<double> coefficients(
            static_cast<std::size_t>(modelColumns), 0.0);
        bool feasible = true;
        for (int a = 0; a < activeCount; ++a)
        {
            const double coefficient = activeSolution[static_cast<std::size_t>(a)];
            if (coefficient < -1.0e-10 || !Finite(coefficient))
            {
                feasible = false;
                break;
            }
            coefficients[static_cast<std::size_t>(
                active[static_cast<std::size_t>(a)])] = std::max(0.0, coefficient);
        }
        if (!feasible) continue;
        double sse = 0.0;
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            double predicted = 0.0;
            for (int column = 0; column < modelColumns; ++column)
            {
                predicted += rows[rowIndex][static_cast<std::size_t>(column)]
                    * coefficients[static_cast<std::size_t>(column)];
            }
            const double residual = targets[rowIndex] - predicted;
            sse += weights[rowIndex] * residual * residual;
        }
        if (sse < best.sse)
        {
            best.sse = sse;
            best.coefficients = std::move(coefficients);
        }
    }
    if (!Finite(best.sse)) return false;
    best.ranges = ranges;
    best.shapeParameters = shapeParameters;
    out = std::move(best);
    return true;
}

std::vector<double> LogGrid(int count, double minimum, double maximum)
{
    count = std::max(2, count);
    std::vector<double> values(static_cast<std::size_t>(count));
    const double logMinimum = std::log(minimum);
    const double logMaximum = std::log(maximum);
    for (int i = 0; i < count; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(count - 1);
        values[static_cast<std::size_t>(i)] = std::exp(
            logMinimum + t * (logMaximum - logMinimum));
    }
    return values;
}

void SearchRangesRecursive(const std::vector<double>& grid,
                           int structureCount,
                           int depth,
                           int minimumIndex,
                           std::vector<double>& ranges,
                           const std::vector<EmpiricalBin>& bins,
                           const FitSettings& settings,
                           const std::vector<double>& shapeParameters,
                           FitCandidate& best)
{
    if (depth == structureCount)
    {
        FitCandidate candidate;
        if (SolveActiveSet(bins, settings.structures, ranges,
                           shapeParameters, settings.fitNugget, candidate)
            && candidate.sse < best.sse)
        {
            best = std::move(candidate);
        }
        return;
    }
    for (int index = minimumIndex; index < static_cast<int>(grid.size()); ++index)
    {
        ranges[static_cast<std::size_t>(depth)] = grid[static_cast<std::size_t>(index)];
        SearchRangesRecursive(grid, structureCount, depth + 1, index,
                              ranges, bins, settings, shapeParameters, best);
    }
}

FitCandidate SearchRanges(const std::vector<EmpiricalBin>& bins,
                          const FitSettings& settings,
                          const std::vector<double>& shapeParameters,
                          double maximumLag)
{
    const int count = static_cast<int>(settings.structures.size());
    const int gridCount = count >= 3
        ? std::max(4, settings.tripleRangeGrid)
        : std::max(4, settings.singleRangeGrid);
    const std::vector<double> grid = LogGrid(
        gridCount,
        0.05 * maximumLag,
        1.5 * maximumLag);
    std::vector<double> ranges(static_cast<std::size_t>(count), grid.front());
    FitCandidate best;
    SearchRangesRecursive(grid, count, 0, 0, ranges, bins, settings,
                          shapeParameters, best);
    return best;
}

double GoldenRefineAxis(const std::vector<EmpiricalBin>& bins,
                        const FitSettings& settings,
                        std::vector<double>& ranges,
                        const std::vector<double>& shapeParameters,
                        int axis,
                        double minimum,
                        double maximum,
                        FitCandidate& inOutBest)
{
    constexpr double Golden = 0.6180339887498948482;
    double left = minimum;
    double right = maximum;
    double c = right - Golden * (right - left);
    double d = left + Golden * (right - left);
    auto Evaluate = [&](double value)
    {
        std::vector<double> candidateRanges = ranges;
        candidateRanges[static_cast<std::size_t>(axis)] = value;
        FitCandidate candidate;
        if (!SolveActiveSet(bins, settings.structures, candidateRanges,
                            shapeParameters, settings.fitNugget, candidate))
        {
            return std::numeric_limits<double>::infinity();
        }
        if (candidate.sse < inOutBest.sse) inOutBest = candidate;
        return candidate.sse;
    };
    double fc = Evaluate(c);
    double fd = Evaluate(d);
    for (int iteration = 0; iteration < 28; ++iteration)
    {
        if (fc < fd)
        {
            right = d;
            d = c;
            fd = fc;
            c = right - Golden * (right - left);
            fc = Evaluate(c);
        }
        else
        {
            left = c;
            c = d;
            fc = fd;
            d = left + Golden * (right - left);
            fd = Evaluate(d);
        }
    }
    return fc < fd ? c : d;
}

std::vector<double> InitialShapeParameters(const FitSettings& settings)
{
    std::vector<double> parameters(settings.structures.size(), 1.0);
    for (std::size_t i = 0; i < settings.structures.size(); ++i)
    {
        parameters[i] = settings.structures[i].shape == Shape::Matern
            ? Clamp(settings.structures[i].initialMaternNu, 0.1, 10.0)
            : settings.structures[i].shape == Shape::Power
                ? Clamp(settings.structures[i].initialPowerAlpha, 0.01, 1.99)
                : 1.0;
    }
    return parameters;
}

void RefineShapeParameters(const std::vector<EmpiricalBin>& bins,
                           const FitSettings& settings,
                           double maximumLag,
                           std::vector<double>& parameters,
                           FitCandidate& best)
{
    if (!settings.searchShapeParameters) return;
    const std::array<double, 6> maternValues{{0.5, 1.0, 1.5, 2.5, 3.5, 5.0}};
    std::array<double, 20> powerValues{};
    for (int i = 0; i < 20; ++i)
    {
        powerValues[static_cast<std::size_t>(i)] = 0.05
            + static_cast<double>(i) * (1.90 / 19.0);
    }
    for (int pass = 0; pass < 3; ++pass)
    {
        bool changed = false;
        for (std::size_t axis = 0; axis < settings.structures.size(); ++axis)
        {
            const Shape shape = settings.structures[axis].shape;
            if (shape != Shape::Matern && shape != Shape::Power) continue;
            const double previous = parameters[axis];
            const double* begin = shape == Shape::Matern
                ? maternValues.data() : powerValues.data();
            const int count = shape == Shape::Matern
                ? static_cast<int>(maternValues.size())
                : static_cast<int>(powerValues.size());
            for (int candidateIndex = 0; candidateIndex < count; ++candidateIndex)
            {
                std::vector<double> candidateParameters = parameters;
                candidateParameters[axis] = begin[candidateIndex];
                FitCandidate candidate = SearchRanges(
                    bins, settings, candidateParameters, maximumLag);
                if (candidate.sse < best.sse)
                {
                    best = candidate;
                    parameters = candidateParameters;
                    changed = true;
                }
            }
            if (!changed) parameters[axis] = previous;
        }
        if (!changed) break;
    }
}

} // namespace

bool ComputeEmpiricalVariogram(const std::vector<Sample>& samples,
                               bool planar,
                               const EmpiricalSettings& inputSettings,
                               EmpiricalReport& outReport)
{
    outReport = EmpiricalReport{};
    if (samples.size() < 2)
    {
        outReport.message = "At least two samples are required for an empirical variogram.";
        return false;
    }
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const Sample& sample = samples[i];
        if (!Finite(sample.location.x) || !Finite(sample.location.y)
            || !Finite(sample.location.z) || !Finite(sample.value))
        {
            outReport.message = "Sample " + std::to_string(i)
                + " contains a non-finite coordinate or value.";
            return false;
        }
    }

    EmpiricalSettings settings = inputSettings;
    settings.binCount = std::max(1, settings.binCount);
    settings.variogramMapSize = std::max(3, settings.variogramMapSize);
    if ((settings.variogramMapSize & 1) == 0) ++settings.variogramMapSize;
    settings.pairBudget = std::max<std::uint64_t>(1, settings.pairBudget);

    Vec3 minimum{std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity()};
    Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    for (const Sample& sample : samples)
    {
        minimum.x = std::min(minimum.x, sample.location.x);
        minimum.y = std::min(minimum.y, sample.location.y);
        minimum.z = std::min(minimum.z, sample.location.z);
        maximum.x = std::max(maximum.x, sample.location.x);
        maximum.y = std::max(maximum.y, sample.location.y);
        maximum.z = std::max(maximum.z, sample.location.z);
    }
    const double diagonal = Distance(minimum, maximum, planar);
    const double maximumLag = settings.maximumLag > 0.0
        ? settings.maximumLag : diagonal / 3.0;
    if (!(maximumLag > 0.0) || !Finite(maximumLag))
    {
        outReport.message = "The sample extent does not define a positive maximum lag.";
        return false;
    }
    outReport.maximumLag = maximumLag;
    outReport.bins.resize(static_cast<std::size_t>(settings.binCount));
    std::vector<BinAccumulator> accumulators(
        static_cast<std::size_t>(settings.binCount));
    const double binWidth = maximumLag / static_cast<double>(settings.binCount);

    if (settings.computeVariogramMap)
    {
        outReport.map.size = settings.variogramMapSize;
        outReport.map.maximumLag = maximumLag;
        const std::size_t cells = static_cast<std::size_t>(settings.variogramMapSize)
            * static_cast<std::size_t>(settings.variogramMapSize);
        outReport.map.semivariance.assign(cells, 0.0);
        outReport.map.pairCounts.assign(cells, 0);
    }

    const std::uint64_t totalPairs = PairCount(
        static_cast<std::uint64_t>(samples.size()));
    outReport.totalPossiblePairs = totalPairs;
    const std::uint64_t visitCount = std::min(totalPairs, settings.pairBudget);
    outReport.pairsSubsampled = visitCount < totalPairs;
    if (outReport.pairsSubsampled)
    {
        outReport.warnings.push_back(
            "Pair population exceeded the deterministic pair budget and was subsampled.");
    }
    std::uint64_t start = totalPairs > 0
        ? Mix64(settings.randomSeed) % totalPairs : 0;
    std::uint64_t stride = totalPairs > 1
        ? (Mix64(settings.randomSeed ^ 0xa5a5a5a5a5a5a5a5ull)
            % (totalPairs - 1)) + 1 : 1;
    while (totalPairs > 1 && GreatestCommonDivisor(stride, totalPairs) != 1)
    {
        ++stride;
        if (stride >= totalPairs) stride = 1;
    }

    auto AccumulateMap = [&](const Vec3& delta, double pairSemivariance)
    {
        if (!settings.computeVariogramMap) return;
        const int mapSize = outReport.map.size;
        auto Insert = [&](double x, double y)
        {
            if (std::abs(x) > maximumLag || std::abs(y) > maximumLag) return;
            const double normalizedX = (x + maximumLag) / (2.0 * maximumLag);
            const double normalizedY = (y + maximumLag) / (2.0 * maximumLag);
            const int ix = std::max(0, std::min(mapSize - 1,
                static_cast<int>(std::floor(normalizedX * mapSize))));
            const int iy = std::max(0, std::min(mapSize - 1,
                static_cast<int>(std::floor(normalizedY * mapSize))));
            const std::size_t index = static_cast<std::size_t>(iy)
                * static_cast<std::size_t>(mapSize)
                + static_cast<std::size_t>(ix);
            outReport.map.semivariance[index] += pairSemivariance;
            ++outReport.map.pairCounts[index];
        };
        Insert(delta.x, delta.y);
        Insert(-delta.x, -delta.y);
    };

    for (std::uint64_t visit = 0; visit < visitCount; ++visit)
    {
        const std::uint64_t linear = totalPairs > 0
            ? (start + visit * stride) % totalPairs : 0;
        const auto pair = PairFromLinear(
            linear, static_cast<std::uint64_t>(samples.size()));
        const Sample& a = samples[static_cast<std::size_t>(pair.first)];
        const Sample& b = samples[static_cast<std::size_t>(pair.second)];
        Vec3 delta = b.location - a.location;
        if (planar) delta.z = 0.0;
        const double separation = std::sqrt(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        ++outReport.visitedPairs;
        if (!(separation > 0.0) || separation > maximumLag
            || !DirectionAccepted(delta, planar, settings.direction, separation))
        {
            continue;
        }
        int binIndex = static_cast<int>(std::floor(separation / binWidth));
        if (binIndex == settings.binCount) --binIndex;
        if (binIndex < 0 || binIndex >= settings.binCount) continue;
        const double difference = a.value - b.value;
        const double pairSemivariance = 0.5 * difference * difference;
        BinAccumulator& accumulator = accumulators[static_cast<std::size_t>(binIndex)];
        accumulator.lagSum += separation;
        accumulator.classicalSum += pairSemivariance;
        accumulator.robustRootSum += std::sqrt(std::abs(difference));
        ++accumulator.count;
        ++outReport.acceptedPairs;
        AccumulateMap(delta, pairSemivariance);
    }

    std::uint64_t populatedPairs = 0;
    int populatedBins = 0;
    for (int binIndex = 0; binIndex < settings.binCount; ++binIndex)
    {
        const BinAccumulator& accumulator = accumulators[static_cast<std::size_t>(binIndex)];
        EmpiricalBin& bin = outReport.bins[static_cast<std::size_t>(binIndex)];
        bin.pairCount = accumulator.count;
        if (accumulator.count == 0)
        {
            bin.lag = (static_cast<double>(binIndex) + 0.5) * binWidth;
            continue;
        }
        const double count = static_cast<double>(accumulator.count);
        bin.lag = accumulator.lagSum / count;
        if (settings.estimator == EmpiricalEstimator::Classical)
        {
            bin.semivariance = accumulator.classicalSum / count;
        }
        else
        {
            const double averageRoot = accumulator.robustRootSum / count;
            bin.semivariance = 0.5 * std::pow(averageRoot, 4.0)
                / (0.457 + 0.494 / count);
        }
        bin.valid = Finite(bin.semivariance);
        if (bin.valid)
        {
            populatedPairs += accumulator.count;
            ++populatedBins;
        }
    }
    outReport.meanBinPopulation = populatedBins > 0
        ? static_cast<double>(populatedPairs) / static_cast<double>(populatedBins)
        : 0.0;

    if (settings.computeVariogramMap)
    {
        for (std::size_t cell = 0; cell < outReport.map.semivariance.size(); ++cell)
        {
            const std::uint64_t count = outReport.map.pairCounts[cell];
            outReport.map.semivariance[cell] = count > 0
                ? outReport.map.semivariance[cell] / static_cast<double>(count)
                : std::numeric_limits<double>::quiet_NaN();
        }
    }
    if (settings.computeDeclustering)
    {
        outReport.declustering = ComputeDeclustering(samples, planar);
    }
    if (populatedBins == 0)
    {
        outReport.message = "No sample pairs passed the lag and directional filters.";
        return false;
    }
    outReport.succeeded = true;
    outReport.message = outReport.pairsSubsampled
        ? "Empirical variogram computed from a deterministic pair sample."
        : "Empirical variogram computed from all sample pairs.";
    return true;
}

double EvaluateSemivariogramAtLag(const Variogram& variogram, double lag)
{
    if (!(lag > 0.0)) return 0.0;
    double value = variogram.nugget;
    for (const Structure& structure : variogram.structures)
    {
        value += structure.partialSill * EvaluateNormalizedStructure(
            structure.shape,
            lag / std::max(structure.range, 1.0e-12),
            structure.maternNu,
            structure.powerAlpha);
    }
    return value;
}

bool FitVariogramWeightedLeastSquares(const EmpiricalReport& empirical,
                                      const FitSettings& inputSettings,
                                      FitReport& outReport)
{
    outReport = FitReport{};
    if (!empirical.succeeded)
    {
        outReport.message = "A successful empirical variogram is required.";
        return false;
    }
    if (empirical.acceptedPairs == 0 || empirical.bins.empty())
    {
        outReport.message = "The empirical variogram contains no usable pairs.";
        return false;
    }
    std::uint64_t inferredSampleCount = 0;
    while (PairCount(inferredSampleCount) < empirical.totalPossiblePairs)
    {
        ++inferredSampleCount;
    }
    if (inferredSampleCount < 20)
    {
        outReport.message = "At least 20 samples are required for automatic fitting.";
        return false;
    }
    if (empirical.meanBinPopulation < 10.0)
    {
        outReport.message = "The mean empirical-bin population is below 10; fitting would be under-supported.";
        return false;
    }

    FitSettings settings = inputSettings;
    if (settings.structures.empty() || settings.structures.size() > 3)
    {
        outReport.message = "Automatic fitting requires one to three structures.";
        return false;
    }
    settings.singleRangeGrid = std::max(4, settings.singleRangeGrid);
    settings.tripleRangeGrid = std::max(4, settings.tripleRangeGrid);
    settings.goldenPasses = std::max(0, settings.goldenPasses);
    const double maximumLag = empirical.maximumLag;
    if (!(maximumLag > 0.0))
    {
        outReport.message = "The empirical maximum lag must be positive.";
        return false;
    }

    std::vector<double> shapeParameters = InitialShapeParameters(settings);
    FitCandidate best = SearchRanges(
        empirical.bins, settings, shapeParameters, maximumLag);
    if (!Finite(best.sse))
    {
        outReport.message = "No feasible non-negative sill solution was found.";
        return false;
    }
    RefineShapeParameters(empirical.bins, settings, maximumLag,
                          shapeParameters, best);
    if (!best.shapeParameters.empty()) shapeParameters = best.shapeParameters;

    std::vector<double> ranges = best.ranges;
    for (int pass = 0; pass < settings.goldenPasses; ++pass)
    {
        for (int axis = 0; axis < static_cast<int>(ranges.size()); ++axis)
        {
            const double minimum = axis == 0
                ? 0.05 * maximumLag
                : ranges[static_cast<std::size_t>(axis - 1)];
            const double maximum = axis + 1 == static_cast<int>(ranges.size())
                ? 1.5 * maximumLag
                : ranges[static_cast<std::size_t>(axis + 1)];
            ranges[static_cast<std::size_t>(axis)] = GoldenRefineAxis(
                empirical.bins, settings, ranges, shapeParameters,
                axis, minimum, maximum, best);
            ranges = best.ranges;
        }
    }

    Variogram fitted;
    fitted.nuggetMode = settings.nuggetMode;
    fitted.nugget = best.coefficients.empty() ? 0.0 : best.coefficients[0];
    fitted.structures.resize(settings.structures.size());
    for (std::size_t i = 0; i < settings.structures.size(); ++i)
    {
        Structure structure;
        structure.shape = settings.structures[i].shape;
        structure.anisotropy = settings.structures[i].anisotropy;
        structure.range = best.ranges[i];
        structure.partialSill = best.coefficients[i + 1];
        structure.maternNu = structure.shape == Shape::Matern
            ? best.shapeParameters[i] : settings.structures[i].initialMaternNu;
        structure.powerAlpha = structure.shape == Shape::Power
            ? best.shapeParameters[i] : settings.structures[i].initialPowerAlpha;
        fitted.structures[i] = structure;
    }

    double weightedMeanNumerator = 0.0;
    double weightSum = 0.0;
    for (const EmpiricalBin& bin : empirical.bins)
    {
        if (!bin.valid || bin.pairCount == 0) continue;
        const double gamma = std::max(std::abs(bin.semivariance), 1.0e-12);
        const double weight = static_cast<double>(bin.pairCount) / (gamma * gamma);
        weightedMeanNumerator += weight * bin.semivariance;
        weightSum += weight;
    }
    const double weightedMean = weightSum > 0.0
        ? weightedMeanNumerator / weightSum : 0.0;
    double total = 0.0;
    double residual = 0.0;
    for (const EmpiricalBin& bin : empirical.bins)
    {
        if (!bin.valid || bin.pairCount == 0) continue;
        const double gamma = std::max(std::abs(bin.semivariance), 1.0e-12);
        const double weight = static_cast<double>(bin.pairCount) / (gamma * gamma);
        const double predicted = EvaluateSemivariogramAtLag(fitted, bin.lag);
        const double delta = bin.semivariance - predicted;
        const double centered = bin.semivariance - weightedMean;
        residual += weight * delta * delta;
        total += weight * centered * centered;
    }

    outReport.variogram = std::move(fitted);
    outReport.weightedSse = residual;
    outReport.weightedR2 = total > 0.0 ? 1.0 - residual / total : 1.0;
    double totalSill = outReport.variogram.nugget;
    double maximumRange = 0.0;
    for (const Structure& structure : outReport.variogram.structures)
    {
        totalSill += structure.partialSill;
        maximumRange = std::max(maximumRange, structure.range);
    }
    outReport.rangeExceedsMaximumLag = maximumRange > maximumLag;
    outReport.nuggetDominates = totalSill > 0.0
        && outReport.variogram.nugget / totalSill > 0.60;
    if (outReport.rangeExceedsMaximumLag)
    {
        outReport.warnings.push_back(
            "The fitted range exceeds the maximum empirical lag; the fit extrapolates beyond supported separations.");
    }
    if (outReport.nuggetDominates)
    {
        outReport.warnings.push_back(
            "The fitted nugget exceeds 60 percent of the total sill; the data show weak spatial structure.");
    }
    outReport.succeeded = true;
    outReport.message = "Weighted least-squares variogram fit completed. Review diagnostics before applying it.";
    return true;
}

} // namespace kriging::portable
