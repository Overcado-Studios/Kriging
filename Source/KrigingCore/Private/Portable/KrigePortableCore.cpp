#include "KrigePortableCore.h"
#include "KrigeBessel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace kriging::portable
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double Tiny = 1.0e-14;

bool Finite(double value) { return std::isfinite(value); }
double Clamp(double value, double low, double high) { return std::max(low, std::min(high, value)); }

double SquaredDistance(const Vec3& a, const Vec3& b, bool planar)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = planar ? 0.0 : a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool Coincident(const Vec3& a, const Vec3& b, bool planar)
{
    return a.x == b.x && a.y == b.y && (planar || a.z == b.z);
}

struct Mat3
{
    double a[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
};

Mat3 Multiply(const Mat3& left, const Mat3& right)
{
    Mat3 result{};
    std::memset(result.a, 0, sizeof(result.a));
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            for (int k = 0; k < 3; ++k)
            {
                result.a[row][column] += left.a[row][k] * right.a[k][column];
            }
        }
    }
    return result;
}

Mat3 RotationX(double radians)
{
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Mat3 result;
    result.a[1][1] = c; result.a[1][2] = -s;
    result.a[2][1] = s; result.a[2][2] = c;
    return result;
}

Mat3 RotationY(double radians)
{
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Mat3 result;
    result.a[0][0] = c; result.a[0][2] = s;
    result.a[2][0] = -s; result.a[2][2] = c;
    return result;
}

Mat3 RotationZ(double radians)
{
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Mat3 result;
    result.a[0][0] = c; result.a[0][1] = -s;
    result.a[1][0] = s; result.a[1][1] = c;
    return result;
}

struct PreparedAnisotropy
{
    Mat3 rotation;
    double ratioY = 1.0;
    double ratioZ = 1.0;
    bool identityRatios = true;
    bool planar = true;

    explicit PreparedAnisotropy(const Anisotropy& source, bool inPlanar)
        : ratioY(Clamp(source.ratioY, 0.01, 1.0)),
          ratioZ(Clamp(source.ratioZ, 0.01, 1.0)),
          planar(inPlanar)
    {
        identityRatios = ratioY == 1.0 && (planar || ratioZ == 1.0);
        if (identityRatios)
        {
            rotation = Mat3{};
            return;
        }
        const double azimuth = -source.azimuthDeg * Pi / 180.0;
        const double dip = -source.dipDeg * Pi / 180.0;
        const double plunge = -source.plungeDeg * Pi / 180.0;
        rotation = planar
            ? RotationZ(azimuth)
            : Multiply(Multiply(RotationZ(azimuth), RotationY(dip)), RotationX(plunge));
    }

    double Distance(const Vec3& delta) const
    {
        if (identityRatios)
        {
            return std::sqrt(delta.x * delta.x + delta.y * delta.y + (planar ? 0.0 : delta.z * delta.z));
        }
        const double x = rotation.a[0][0] * delta.x + rotation.a[0][1] * delta.y + rotation.a[0][2] * delta.z;
        const double y = rotation.a[1][0] * delta.x + rotation.a[1][1] * delta.y + rotation.a[1][2] * delta.z;
        const double sy = y / ratioY;
        if (planar)
        {
            return std::sqrt(x * x + sy * sy);
        }
        const double z = rotation.a[2][0] * delta.x + rotation.a[2][1] * delta.y + rotation.a[2][2] * delta.z;
        const double sz = z / ratioZ;
        return std::sqrt(x * x + sy * sy + sz * sz);
    }
};

struct PreparedStructure
{
    Shape shape = Shape::Spherical;
    double range = 1.0;
    double partialSill = 1.0;
    double maternNu = 1.5;
    double powerAlpha = 1.0;
    PreparedAnisotropy anisotropy{Anisotropy{}, true};

    PreparedStructure(const Structure& source, bool planar)
        : shape(source.shape), range(source.range), partialSill(source.partialSill),
          maternNu(source.maternNu), powerAlpha(source.powerAlpha),
          anisotropy(source.anisotropy, planar)
    {
    }

    double G(const Vec3& delta) const
    {
        return EvaluateNormalizedStructure(shape, anisotropy.Distance(delta) / range, maternNu, powerAlpha);
    }
};

struct PreparedVariogram
{
    std::vector<PreparedStructure> structures;
    double nugget = 0.0;
    double totalSill = 0.0;
    NuggetMode nuggetMode = NuggetMode::Exact;
    bool planar = true;
    bool hasPower = false;

    bool Build(const Variogram& source, bool inPlanar, std::vector<std::string>& warnings, std::string& error)
    {
        structures.clear();
        nugget = std::max(0.0, source.nugget);
        nuggetMode = source.nuggetMode;
        planar = inPlanar;
        totalSill = nugget;
        hasPower = false;
        if (source.structures.empty() || source.structures.size() > 3)
        {
            error = source.structures.empty()
                ? "At least one variogram structure is required."
                : "At most three nested variogram structures are supported.";
            return false;
        }
        bool conditionSensitive = false;
        for (const Structure& structure : source.structures)
        {
            if (!(structure.range > 0.0) || structure.partialSill < 0.0
                || !Finite(structure.range) || !Finite(structure.partialSill))
            {
                error = "Every variogram structure requires a finite positive range and finite non-negative partial sill.";
                return false;
            }
            if (!Finite(structure.anisotropy.ratioY) || !Finite(structure.anisotropy.ratioZ)
                || structure.anisotropy.ratioY < 0.01 || structure.anisotropy.ratioY > 1.0
                || structure.anisotropy.ratioZ < 0.01 || structure.anisotropy.ratioZ > 1.0)
            {
                error = "Anisotropy ratios must be finite and in [0.01, 1].";
                return false;
            }
            if (structure.shape == Shape::Matern && (!Finite(structure.maternNu)
                || structure.maternNu < 0.1 || structure.maternNu > 10.0))
            {
                error = "Matern nu must be in [0.1, 10].";
                return false;
            }
            if (structure.shape == Shape::Power && (!Finite(structure.powerAlpha)
                || !(structure.powerAlpha > 0.0 && structure.powerAlpha < 2.0)))
            {
                error = "Power alpha must be in (0, 2).";
                return false;
            }
            structures.emplace_back(structure, planar);
            hasPower = hasPower || structure.shape == Shape::Power;
            if (structure.shape != Shape::Power)
            {
                totalSill += structure.partialSill;
            }
            conditionSensitive = conditionSensitive || structure.shape == Shape::Gaussian
                || (structure.shape == Shape::Matern && structure.maternNu > 2.5);
        }
        if (!Finite(nugget) || !Finite(totalSill))
        {
            error = "Variogram nugget and total sill must be finite.";
            return false;
        }
        if (conditionSensitive && totalSill > 0.0 && nugget < 1.0e-4 * totalSill)
        {
            const double raised = 1.0e-4 * totalSill;
            warnings.push_back("Raised the effective nugget to the Gaussian/high-nu Matern conditioning floor. Exact nugget mode still includes it in the query RHS at coincident points.");
            totalSill += raised - nugget;
            nugget = raised;
        }
        return true;
    }

    double Covariance(const Vec3& a, const Vec3& b, bool includeNuggetAtZero) const
    {
        const Vec3 delta = a - b;
        double value = 0.0;
        for (const PreparedStructure& structure : structures)
        {
            if (structure.shape != Shape::Power)
            {
                const double g = structure.G(delta);
                if (!Finite(g)) return std::numeric_limits<double>::quiet_NaN();
                value += structure.partialSill * (1.0 - g);
            }
        }
        if (includeNuggetAtZero && Coincident(a, b, planar))
        {
            value += nugget;
        }
        return value;
    }

    double Semivariogram(const Vec3& a, const Vec3& b, bool includeNugget) const
    {
        if (Coincident(a, b, planar))
        {
            return 0.0;
        }
        double value = includeNugget ? nugget : 0.0;
        const Vec3 delta = a - b;
        for (const PreparedStructure& structure : structures)
        {
            const double g = structure.G(delta);
            if (!Finite(g)) return std::numeric_limits<double>::quiet_NaN();
            value += structure.partialSill * g;
        }
        return value;
    }

    double MaxRange() const
    {
        double result = 0.0;
        for (const PreparedStructure& structure : structures)
        {
            result = std::max(result, structure.range);
        }
        return result;
    }
};

struct TransformState
{
    Transform transform = Transform::None;
    double logDelta = 0.0;
    std::vector<std::pair<double, double>> valueToScore;
    std::vector<std::pair<double, double>> scoreToValue;

    static double Interpolate(const std::vector<std::pair<double, double>>& table, double x)
    {
        if (table.empty()) return x;
        if (table.size() == 1) return table.front().second;
        auto upper = std::lower_bound(table.begin(), table.end(), x,
            [](const auto& pair, double value) { return pair.first < value; });
        if (upper == table.begin())
        {
            const auto& a = table[0]; const auto& b = table[1];
            const double denominator = b.first - a.first;
            if (std::abs(denominator) <= Tiny) return a.second;
            const double t = (x - a.first) / denominator;
            return a.second + t * (b.second - a.second);
        }
        if (upper == table.end())
        {
            const auto& a = table[table.size() - 2]; const auto& b = table.back();
            const double denominator = b.first - a.first;
            if (std::abs(denominator) <= Tiny) return b.second;
            const double t = (x - a.first) / denominator;
            return a.second + t * (b.second - a.second);
        }
        const auto& a = *(upper - 1); const auto& b = *upper;
        const double denominator = b.first - a.first;
        if (std::abs(denominator) <= Tiny) return 0.5 * (a.second + b.second);
        const double t = (x - a.first) / denominator;
        return a.second + t * (b.second - a.second);
    }

    bool Build(const std::vector<Sample>& samples, Transform requested,
               std::vector<double>& values, std::vector<double>& measurementVariances,
               std::string& error)
    {
        transform = requested;
        values.resize(samples.size());
        measurementVariances.resize(samples.size());
        valueToScore.clear();
        scoreToValue.clear();
        if (transform == Transform::None)
        {
            for (std::size_t i = 0; i < samples.size(); ++i)
            {
                values[i] = samples[i].value;
                measurementVariances[i] = samples[i].measurementVariance;
            }
            return true;
        }
        if (transform == Transform::Logarithmic)
        {
            double minimum = std::numeric_limits<double>::infinity();
            for (const Sample& sample : samples) minimum = std::min(minimum, sample.value);
            logDelta = 1.0 - minimum;
            for (std::size_t i = 0; i < samples.size(); ++i)
            {
                const double shifted = samples[i].value + logDelta;
                if (!(shifted > 0.0))
                {
                    error = "Logarithmic transform produced a non-positive shifted value.";
                    return false;
                }
                values[i] = std::log(shifted);
                measurementVariances[i] = samples[i].measurementVariance / (shifted * shifted);
            }
            return true;
        }

        std::vector<int> order(samples.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&samples](int left, int right)
        {
            const auto l = static_cast<std::size_t>(left);
            const auto r = static_cast<std::size_t>(right);
            if (samples[l].value != samples[r].value) return samples[l].value < samples[r].value;
            return samples[l].originalIndex < samples[r].originalIndex;
        });
        valueToScore.reserve(samples.size());
        scoreToValue.reserve(samples.size());
        for (std::size_t rank = 0; rank < order.size(); ++rank)
        {
            const std::size_t index = static_cast<std::size_t>(order[rank]);
            const double p = (static_cast<double>(rank) + 0.5) / static_cast<double>(order.size());
            const double score = InverseStandardNormalCdf(p);
            values[index] = score;
            // A rank transform has no stable analytic variance transform at ties.
            // Preserve the supplied variance as transformed-space variance and say
            // so in the conventions instead of inventing a derivative.
            measurementVariances[index] = samples[index].measurementVariance;
            valueToScore.emplace_back(samples[index].value, score);
            scoreToValue.emplace_back(score, samples[index].value);
        }
        std::stable_sort(valueToScore.begin(), valueToScore.end());
        return true;
    }

    double Forward(double value) const
    {
        if (transform == Transform::None) return value;
        if (transform == Transform::Logarithmic) return std::log(value + logDelta);
        return Interpolate(valueToScore, value);
    }

    double Inverse(double value) const
    {
        if (transform == Transform::None) return value;
        if (transform == Transform::Logarithmic) return std::exp(value) - logDelta;
        return Interpolate(scoreToValue, value);
    }
};

std::uint64_t HashIndices(const std::vector<int>& indices)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (int index : indices)
    {
        hash ^= static_cast<std::uint32_t>(index);
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(indices.size());
    hash *= 1099511628211ull;
    return hash;
}

bool RelativeClose(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

} // namespace

void DenseMatrix::Resize(int rows, int columns, double value)
{
    rows_ = std::max(0, rows);
    columns_ = std::max(0, columns);
    data_.assign(static_cast<std::size_t>(rows_) * static_cast<std::size_t>(columns_), value);
}

bool PartialPivLU::Factorize(const DenseMatrix& matrix)
{
    order_ = 0;
    conditionProxy_ = 0.0;
    pivotRows_.clear();
    lu_ = DenseMatrix();
    if (matrix.Rows() <= 0 || matrix.Rows() != matrix.Columns()) return false;
    order_ = matrix.Rows();
    lu_ = matrix;
    pivotRows_.resize(static_cast<std::size_t>(order_));

    double matrixMaximum = 0.0;
    for (int row = 0; row < order_; ++row)
    {
        for (int column = 0; column < order_; ++column)
        {
            matrixMaximum = std::max(matrixMaximum, std::abs(lu_(row, column)));
        }
    }
    const double pivotFloor = std::max(1.0e-15, matrixMaximum * 1.0e-14);
    double minimumPivot = std::numeric_limits<double>::infinity();
    double maximumPivot = 0.0;

    for (int k = 0; k < order_; ++k)
    {
        int pivot = k;
        double pivotMagnitude = std::abs(lu_(k, k));
        for (int row = k + 1; row < order_; ++row)
        {
            const double candidate = std::abs(lu_(row, k));
            if (candidate > pivotMagnitude)
            {
                pivotMagnitude = candidate;
                pivot = row;
            }
        }
        if (!(pivotMagnitude > pivotFloor) || !Finite(pivotMagnitude))
        {
            order_ = 0;
            return false;
        }
        pivotRows_[static_cast<std::size_t>(k)] = pivot;
        if (pivot != k)
        {
            for (int column = 0; column < order_; ++column)
            {
                std::swap(lu_(k, column), lu_(pivot, column));
            }
        }
        const double diagonal = lu_(k, k);
        const double diagonalMagnitude = std::abs(diagonal);
        minimumPivot = std::min(minimumPivot, diagonalMagnitude);
        maximumPivot = std::max(maximumPivot, diagonalMagnitude);
        for (int row = k + 1; row < order_; ++row)
        {
            lu_(row, k) /= diagonal;
            const double factor = lu_(row, k);
            for (int column = k + 1; column < order_; ++column)
            {
                lu_(row, column) -= factor * lu_(k, column);
            }
        }
    }
    conditionProxy_ = maximumPivot / std::max(minimumPivot, std::numeric_limits<double>::min());
    return true;
}

bool PartialPivLU::Solve(const double* rhs, int count, double* outSolution) const
{
    if (order_ <= 0 || count != order_ || !rhs || !outSolution) return false;
    std::copy(rhs, rhs + count, outSolution);
    for (int k = 0; k < order_; ++k)
    {
        const int pivot = pivotRows_[static_cast<std::size_t>(k)];
        if (pivot != k) std::swap(outSolution[k], outSolution[pivot]);
    }
    for (int row = 0; row < order_; ++row)
    {
        for (int column = 0; column < row; ++column)
        {
            outSolution[row] -= lu_(row, column) * outSolution[column];
        }
    }
    for (int row = order_ - 1; row >= 0; --row)
    {
        for (int column = row + 1; column < order_; ++column)
        {
            outSolution[row] -= lu_(row, column) * outSolution[column];
        }
        const double diagonal = lu_(row, row);
        if (!(std::abs(diagonal) > std::numeric_limits<double>::min()) || !Finite(diagonal)) return false;
        outSolution[row] /= diagonal;
    }
    for (int index = 0; index < order_; ++index)
    {
        if (!Finite(outSolution[index])) return false;
    }
    return true;
}

bool PartialPivLU::Solve(const std::vector<double>& rhs, std::vector<double>& outSolution) const
{
    if (static_cast<int>(rhs.size()) != order_) return false;
    outSolution.resize(rhs.size());
    return Solve(rhs.data(), static_cast<int>(rhs.size()), outSolution.data());
}

bool PartialPivLU::Inverse(DenseMatrix& outInverse) const
{
    if (order_ <= 0) return false;
    outInverse.Resize(order_, order_, 0.0);
    std::vector<double> rhs(static_cast<std::size_t>(order_), 0.0);
    std::vector<double> solution(static_cast<std::size_t>(order_), 0.0);
    for (int column = 0; column < order_; ++column)
    {
        std::fill(rhs.begin(), rhs.end(), 0.0);
        rhs[static_cast<std::size_t>(column)] = 1.0;
        if (!Solve(rhs.data(), order_, solution.data())) return false;
        for (int row = 0; row < order_; ++row)
        {
            outInverse(row, column) = solution[static_cast<std::size_t>(row)];
        }
    }
    return true;
}

bool PartialPivLU::InverseDiagonal(std::vector<double>& outDiagonal) const
{
    if (order_ <= 0) return false;
    outDiagonal.resize(static_cast<std::size_t>(order_));
    std::vector<double> rhs(static_cast<std::size_t>(order_), 0.0);
    std::vector<double> solution(static_cast<std::size_t>(order_), 0.0);
    for (int column = 0; column < order_; ++column)
    {
        std::fill(rhs.begin(), rhs.end(), 0.0);
        rhs[static_cast<std::size_t>(column)] = 1.0;
        if (!Solve(rhs.data(), order_, solution.data())) return false;
        outDiagonal[static_cast<std::size_t>(column)] = solution[static_cast<std::size_t>(column)];
    }
    return true;
}

double InverseStandardNormalCdf(double probability)
{
    const double p = Clamp(probability, 1.0e-15, 1.0 - 1.0e-15);
    constexpr double A[] = {
        3.3871328727963666080, 1.3314166789178437745e2,
        1.9715909503065514427e3, 1.3731693765509461125e4,
        4.5921953931549871457e4, 6.7265770927008700853e4,
        3.3430575583588128105e4, 2.5090809287301226727e3};
    constexpr double B[] = {
        1.0, 4.2313330701600911252e1,
        6.8718700749205790830e2, 5.3941960214247511077e3,
        2.1213794301586595867e4, 3.9307895800092710614e4,
        2.8729085735721942676e4, 5.2264952788528545610e3};
    constexpr double C[] = {
        1.42343711074968357734, 4.63033784615654529590,
        5.76949722146069140550, 3.64784832476320460504,
        1.27045825245236838258, 2.41780725177450611770e-1,
        2.27238449892691845833e-2, 7.74545014278341407640e-4};
    constexpr double D[] = {
        1.0, 2.05319162663775882187,
        1.67638483018380384940, 6.89767334985100004550e-1,
        1.48103976427480074590e-1, 1.51986665636164571966e-2,
        5.47593808499534494600e-4, 1.05075007164441684324e-9};
    constexpr double E[] = {
        6.65790464350110377720, 5.46378491116411436990,
        1.78482653991729133580, 2.96560571828504891230e-1,
        2.65321895265761230930e-2, 1.24266094738807843860e-3,
        2.71155556874348757815e-5, 2.01033439929228813265e-7};
    constexpr double F[] = {
        1.0, 5.99832206555887937690e-1,
        1.36929880922735805310e-1, 1.48753612908506148525e-2,
        7.86869131145613259100e-4, 1.84631831751005468180e-5,
        1.42151175831644588870e-7, 2.04426310338993978564e-15};

    auto Polynomial = [](const double (&coefficients)[8], double x)
    {
        double value = coefficients[7];
        for (int index = 6; index >= 0; --index)
        {
            value = value * x + coefficients[index];
        }
        return value;
    };

    const double q = p - 0.5;
    if (std::abs(q) <= 0.425)
    {
        const double r = 0.180625 - q * q;
        return q * Polynomial(A, r) / Polynomial(B, r);
    }

    const double tail = q < 0.0 ? p : 1.0 - p;
    double r = std::sqrt(-std::log(tail));
    double value = 0.0;
    if (r <= 5.0)
    {
        r -= 1.6;
        value = Polynomial(C, r) / Polynomial(D, r);
    }
    else
    {
        r -= 5.0;
        value = Polynomial(E, r) / Polynomial(F, r);
    }
    return q < 0.0 ? -value : value;
}

double EvaluateNormalizedStructure(Shape shape, double ratio, double maternNu, double powerAlpha)
{
    ratio = std::max(0.0, ratio);
    switch (shape)
    {
    case Shape::Spherical:
        if (ratio >= 1.0) return 1.0;
        return 1.5 * ratio - 0.5 * ratio * ratio * ratio;
    case Shape::Exponential:
        return -std::expm1(-ratio);
    case Shape::Gaussian:
        return -std::expm1(-ratio * ratio);
    case Shape::Power:
        return std::pow(ratio, Clamp(powerAlpha, 0.01, 1.99));
    case Shape::Matern:
    {
        if (std::abs(maternNu - 0.5) <= 1.0e-14) return -std::expm1(-ratio);
        if (!(ratio > 0.0)) return 0.0;
        const double x = std::sqrt(2.0 * maternNu) * ratio;
        if (x >= 60.0) return 1.0;
        if (x < 1.0e-8)
        {
            if (maternNu < 0.99)
            {
                const double coefficient = -std::tgamma(-maternNu) / std::tgamma(maternNu);
                return Clamp(coefficient * std::pow(0.5 * x, 2.0 * maternNu), 0.0, 1.0);
            }
            if (maternNu <= 1.01)
            {
                constexpr double EulerGamma = 0.577215664901532860606512090082402431;
                return Clamp(-0.5 * x * x * (std::log(0.5 * x) + EulerGamma - 0.5), 0.0, 1.0);
            }
            return Clamp(x * x / (4.0 * (maternNu - 1.0)), 0.0, 1.0);
        }
        const double k = ::Kriging::Detail::ModifiedBesselK(maternNu, x);
        if (!(k > 0.0) || !Finite(k)) return std::numeric_limits<double>::quiet_NaN();
        const double logCorrelation = (1.0 - maternNu) * std::log(2.0)
            - std::lgamma(maternNu) + maternNu * std::log(x) + std::log(k);
        return Clamp(-std::expm1(std::min(0.0, logCorrelation)), 0.0, 1.0);
    }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

struct KdTree::Impl
{
    struct Node
    {
        int sampleIndex = -1;
        int left = -1;
        int right = -1;
        int axis = 0;
    };

    std::vector<Vec3> positions;
    std::vector<int> originalIndices;
    std::vector<Node> nodes;
    int root = -1;
    bool planar = true;

    static double Coordinate(const Vec3& point, int axis)
    {
        return axis == 0 ? point.x : (axis == 1 ? point.y : point.z);
    }

    static bool Better(const Neighbour& a, const Neighbour& b)
    {
        if (a.distanceSquared != b.distanceSquared)
        {
            return a.distanceSquared < b.distanceSquared;
        }
        if (a.originalIndex != b.originalIndex)
        {
            return a.originalIndex < b.originalIndex;
        }
        return a.sampleIndex < b.sampleIndex;
    }

    bool IndexLess(int left, int right, int axis) const
    {
        const Vec3& a = positions[static_cast<std::size_t>(left)];
        const Vec3& b = positions[static_cast<std::size_t>(right)];
        const int axes[3] = {axis, (axis + 1) % 3, (axis + 2) % 3};
        for (int i = 0; i < 3; ++i)
        {
            const int current = axes[i];
            if (planar && current == 2) continue;
            const double av = Coordinate(a, current);
            const double bv = Coordinate(b, current);
            if (av != bv) return av < bv;
        }
        if (originalIndices[static_cast<std::size_t>(left)]
            != originalIndices[static_cast<std::size_t>(right)])
        {
            return originalIndices[static_cast<std::size_t>(left)]
                < originalIndices[static_cast<std::size_t>(right)];
        }
        return left < right;
    }

    int ChooseAxis(const std::vector<int>& permutation, int begin, int end) const
    {
        Vec3 minimum{std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::infinity()};
        Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
        for (int i = begin; i < end; ++i)
        {
            const Vec3& point = positions[static_cast<std::size_t>(
                permutation[static_cast<std::size_t>(i)])];
            minimum.x = std::min(minimum.x, point.x);
            minimum.y = std::min(minimum.y, point.y);
            minimum.z = std::min(minimum.z, point.z);
            maximum.x = std::max(maximum.x, point.x);
            maximum.y = std::max(maximum.y, point.y);
            maximum.z = std::max(maximum.z, point.z);
        }
        const double spreadX = maximum.x - minimum.x;
        const double spreadY = maximum.y - minimum.y;
        const double spreadZ = planar ? -1.0 : maximum.z - minimum.z;
        if (spreadY > spreadX && spreadY >= spreadZ) return 1;
        if (spreadZ > spreadX && spreadZ > spreadY) return 2;
        return 0;
    }

    int BuildRecursive(std::vector<int>& permutation, int begin, int end)
    {
        if (begin >= end) return -1;
        const int axis = ChooseAxis(permutation, begin, end);
        const int middle = begin + (end - begin) / 2;
        std::nth_element(permutation.begin() + begin,
                         permutation.begin() + middle,
                         permutation.begin() + end,
                         [this, axis](int left, int right)
                         {
                             return IndexLess(left, right, axis);
                         });
        const int nodeIndex = static_cast<int>(nodes.size());
        nodes.push_back({permutation[static_cast<std::size_t>(middle)], -1, -1, axis});
        const int left = BuildRecursive(permutation, begin, middle);
        const int right = BuildRecursive(permutation, middle + 1, end);
        nodes[static_cast<std::size_t>(nodeIndex)].left = left;
        nodes[static_cast<std::size_t>(nodeIndex)].right = right;
        return nodeIndex;
    }

    void QueryKNearest(int nodeIndex, const Vec3& at, int maximumCount,
                       double radiusSquared, std::vector<Neighbour>& best) const
    {
        if (nodeIndex < 0) return;
        const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
        const Vec3& point = positions[static_cast<std::size_t>(node.sampleIndex)];
        const double distanceSquared = SquaredDistance(at, point, planar);
        if (distanceSquared <= radiusSquared)
        {
            const Neighbour candidate{node.sampleIndex,
                                      originalIndices[static_cast<std::size_t>(node.sampleIndex)],
                                      distanceSquared};
            const auto position = std::lower_bound(best.begin(), best.end(), candidate, Better);
            if (static_cast<int>(best.size()) < maximumCount)
            {
                best.insert(position, candidate);
            }
            else if (position != best.end())
            {
                best.insert(position, candidate);
                best.pop_back();
            }
        }

        const double delta = Coordinate(at, node.axis) - Coordinate(point, node.axis);
        const int nearChild = delta <= 0.0 ? node.left : node.right;
        const int farChild = delta <= 0.0 ? node.right : node.left;
        QueryKNearest(nearChild, at, maximumCount, radiusSquared, best);

        double threshold = radiusSquared;
        if (static_cast<int>(best.size()) >= maximumCount)
        {
            threshold = std::min(threshold, best.back().distanceSquared);
        }
        if (delta * delta <= threshold)
        {
            QueryKNearest(farChild, at, maximumCount, radiusSquared, best);
        }
    }

    void QueryRadius(int nodeIndex, const Vec3& at, double radiusSquared,
                     std::vector<Neighbour>& out) const
    {
        if (nodeIndex < 0) return;
        const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
        const Vec3& point = positions[static_cast<std::size_t>(node.sampleIndex)];
        const double distanceSquared = SquaredDistance(at, point, planar);
        if (distanceSquared <= radiusSquared)
        {
            out.push_back({node.sampleIndex,
                           originalIndices[static_cast<std::size_t>(node.sampleIndex)],
                           distanceSquared});
        }
        const double delta = Coordinate(at, node.axis) - Coordinate(point, node.axis);
        const int nearChild = delta <= 0.0 ? node.left : node.right;
        const int farChild = delta <= 0.0 ? node.right : node.left;
        QueryRadius(nearChild, at, radiusSquared, out);
        if (delta * delta <= radiusSquared)
        {
            QueryRadius(farChild, at, radiusSquared, out);
        }
    }
};

KdTree::KdTree() : impl_(std::make_unique<Impl>()) {}
KdTree::~KdTree() = default;
KdTree::KdTree(KdTree&&) noexcept = default;
KdTree& KdTree::operator=(KdTree&&) noexcept = default;

bool KdTree::Build(const std::vector<Vec3>& positions,
                   const std::vector<int>& originalIndices,
                   bool planar)
{
    Reset();
    if (positions.size() != originalIndices.size()) return false;
    impl_->positions = positions;
    impl_->originalIndices = originalIndices;
    impl_->planar = planar;
    impl_->nodes.reserve(positions.size());
    std::vector<int> permutation(positions.size());
    std::iota(permutation.begin(), permutation.end(), 0);
    impl_->root = impl_->BuildRecursive(permutation, 0,
                                       static_cast<int>(permutation.size()));
    return true;
}

void KdTree::Reset()
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->positions.clear();
    impl_->originalIndices.clear();
    impl_->nodes.clear();
    impl_->root = -1;
    impl_->planar = true;
}

bool KdTree::Empty() const { return !impl_ || impl_->positions.empty(); }
int KdTree::Size() const { return impl_ ? static_cast<int>(impl_->positions.size()) : 0; }

int KdTree::FindExact(const Vec3& at) const
{
    if (!impl_) return -1;
    int best = -1;
    int bestOriginal = std::numeric_limits<int>::max();
    for (int index = 0; index < static_cast<int>(impl_->positions.size()); ++index)
    {
        if (Coincident(at, impl_->positions[static_cast<std::size_t>(index)], impl_->planar)
            && (impl_->originalIndices[static_cast<std::size_t>(index)] < bestOriginal
                || (impl_->originalIndices[static_cast<std::size_t>(index)] == bestOriginal && index < best)))
        {
            best = index;
            bestOriginal = impl_->originalIndices[static_cast<std::size_t>(index)];
        }
    }
    return best;
}

void KdTree::FindKNearest(const Vec3& at, int maximumCount, double radius,
                          std::vector<Neighbour>& out) const
{
    out.clear();
    if (!impl_ || maximumCount <= 0 || impl_->root < 0) return;
    maximumCount = std::min(maximumCount,
                            static_cast<int>(impl_->positions.size()));
    const double radiusSquared = radius > 0.0
        ? radius * radius : std::numeric_limits<double>::infinity();
    out.reserve(static_cast<std::size_t>(maximumCount));
    impl_->QueryKNearest(impl_->root, at, maximumCount, radiusSquared, out);
}

void KdTree::FindRadius(const Vec3& at, double radius,
                        std::vector<Neighbour>& out) const
{
    out.clear();
    if (!impl_ || impl_->root < 0) return;
    const double nonNegativeRadius = std::max(0.0, radius);
    impl_->QueryRadius(impl_->root, at,
                       nonNegativeRadius * nonNegativeRadius, out);
    std::sort(out.begin(), out.end(), Impl::Better);
}

struct Model::Impl
{
    struct System
    {
        std::vector<int> indices;
        DenseMatrix matrix;
        PartialPivLU solver;
        std::vector<double> dual;
        int driftCount = 0;
        double ridge = 0.0;
        double conditionProxy = 0.0;
        bool factorized = false;
    };

    struct CacheBucketEntry
    {
        std::shared_ptr<System> system;
    };

    bool valid = false;
    bool useLocal = false;
    Variogram sourceVariogram;
    Settings settings;
    PreparedVariogram variogram;
    TransformState transform;
    ExternalDriftSampler externalDrift;
    std::uint64_t externalDriftContentHash = 0;
    std::vector<Sample> samples;
    std::vector<Vec3> positions;
    KdTree spatialIndex;
    std::vector<double> workingValues;
    std::vector<double> workingMeasurementVariances;
    Vec3 driftCenter;
    Vec3 driftScale{1.0, 1.0, 1.0};
    double externalDriftCenter = 0.0;
    double externalDriftScale = 1.0;
    double transformedKnownMean = 0.0;
    System global;
    BuildReport report;
    mutable std::atomic<int> negativeVarianceClamps{0};
    mutable std::atomic<int> localIdwFallbacks{0};
    mutable std::mutex cacheMutex;
    mutable std::unordered_map<std::uint64_t, std::vector<CacheBucketEntry>> localCache;
    mutable std::size_t cacheEntries = 0;

    int DriftCount() const
    {
        switch (settings.method)
        {
        case Method::Simple:
        case Method::InverseDistance: return 0;
        case Method::Ordinary: return 1;
        case Method::UniversalLinear: return settings.planar ? 3 : 4;
        case Method::UniversalQuadratic: return settings.planar ? 6 : 10;
        case Method::ExternalDrift: return 2;
        }
        return 0;
    }

    bool DriftBasis(const Vec3& at, std::array<double, MaxDriftTerms>& out, int& outCount) const
    {
        outCount = 0;
        const Vec3 normalized{
            (at.x - driftCenter.x) / driftScale.x,
            (at.y - driftCenter.y) / driftScale.y,
            settings.planar ? 0.0 : (at.z - driftCenter.z) / driftScale.z};
        switch (settings.method)
        {
        case Method::Simple:
        case Method::InverseDistance: return true;
        case Method::Ordinary:
            out[0] = 1.0; outCount = 1; return true;
        case Method::UniversalLinear:
            out[0] = 1.0; out[1] = normalized.x; out[2] = normalized.y; outCount = 3;
            if (!settings.planar) out[static_cast<std::size_t>(outCount++)] = normalized.z;
            return true;
        case Method::UniversalQuadratic:
            if (settings.planar)
            {
                const double x = normalized.x, y = normalized.y;
                const double values[] = {1.0, x, y, x * x, x * y, y * y};
                std::copy(std::begin(values), std::end(values), out.begin()); outCount = 6;
            }
            else
            {
                const double x = normalized.x, y = normalized.y, z = normalized.z;
                const double values[] = {1.0, x, y, z, x * x, x * y, x * z, y * y, y * z, z * z};
                std::copy(std::begin(values), std::end(values), out.begin()); outCount = 10;
            }
            return true;
        case Method::ExternalDrift:
        {
            double value = 0.0;
            if (!externalDrift || !externalDrift(at, value) || !Finite(value)) return false;
            out[0] = 1.0;
            out[1] = (value - externalDriftCenter) / externalDriftScale;
            outCount = 2;
            return true;
        }
        }
        return false;
    }

    bool MergeDuplicates(const std::vector<Sample>& sorted, double radius,
                         std::vector<Sample>& merged, int& mergedCount, double& maxDiameter) const
    {
        merged.clear(); mergedCount = 0; maxDiameter = 0.0;
        if (sorted.empty()) return true;
        if (radius <= 0.0)
        {
            merged = sorted;
            return true;
        }
        const double radiusSquared = radius * radius;
        std::vector<bool> assigned(sorted.size(), false);
        for (std::size_t anchor = 0; anchor < sorted.size(); ++anchor)
        {
            if (assigned[anchor]) continue;
            std::vector<std::size_t> cluster{anchor};
            assigned[anchor] = true;
            for (std::size_t candidate = anchor + 1; candidate < sorted.size(); ++candidate)
            {
                if (assigned[candidate]) continue;
                if (sorted[candidate].location.x - sorted[anchor].location.x > radius) break;
                bool completeLink = true;
                for (std::size_t member : cluster)
                {
                    if (SquaredDistance(sorted[candidate].location, sorted[member].location, settings.planar) > radiusSquared)
                    {
                        completeLink = false;
                        break;
                    }
                }
                if (completeLink)
                {
                    assigned[candidate] = true;
                    cluster.push_back(candidate);
                }
            }
            Sample aggregate;
            aggregate.originalIndex = std::numeric_limits<int>::max();
            for (std::size_t member : cluster)
            {
                aggregate.location += sorted[member].location;
                aggregate.value += sorted[member].value;
                aggregate.measurementVariance += sorted[member].measurementVariance;
                aggregate.originalIndex = std::min(aggregate.originalIndex, sorted[member].originalIndex);
            }
            const double count = static_cast<double>(cluster.size());
            aggregate.location = aggregate.location / count;
            aggregate.value /= count;
            aggregate.measurementVariance /= count * count;
            for (std::size_t i = 0; i < cluster.size(); ++i)
            {
                for (std::size_t j = i + 1; j < cluster.size(); ++j)
                {
                    maxDiameter = std::max(maxDiameter,
                        std::sqrt(SquaredDistance(sorted[cluster[i]].location, sorted[cluster[j]].location, settings.planar)));
                }
            }
            merged.push_back(aggregate);
        }
        mergedCount = static_cast<int>(sorted.size() - merged.size());
        return true;
    }

    bool AssembleMatrix(const std::vector<int>& indices, double ridge, DenseMatrix& matrix, std::string& error) const
    {
        const int k = indices.empty() ? static_cast<int>(samples.size()) : static_cast<int>(indices.size());
        const int p = DriftCount();
        matrix.Resize(k + p, k + p, 0.0);
        auto modelIndex = [&indices](int local)
        {
            return indices.empty() ? local : indices[static_cast<std::size_t>(local)];
        };
        for (int i = 0; i < k; ++i)
        {
            const int modelI = modelIndex(i);
            for (int j = i; j < k; ++j)
            {
                const int modelJ = modelIndex(j);
                double value = 0.0;
                if (!variogram.hasPower)
                {
                    value = variogram.Covariance(positions[static_cast<std::size_t>(modelI)],
                        positions[static_cast<std::size_t>(modelJ)], true);
                }
                else
                {
                    const bool filtered = sourceVariogram.nuggetMode == NuggetMode::Filtered;
                    value = -variogram.Semivariogram(positions[static_cast<std::size_t>(modelI)],
                        positions[static_cast<std::size_t>(modelJ)], !filtered);
                }
                if (!Finite(value))
                {
                    error = "Variogram evaluation produced a non-finite matrix entry.";
                    return false;
                }
                matrix(i, j) = value;
                matrix(j, i) = value;
            }
            if (variogram.hasPower && sourceVariogram.nuggetMode == NuggetMode::Filtered)
            {
                matrix(i, i) += variogram.nugget;
            }
            matrix(i, i) += workingMeasurementVariances[static_cast<std::size_t>(modelI)] + ridge;
            if (p > 0)
            {
                std::array<double, MaxDriftTerms> basis{};
                int count = 0;
                if (!DriftBasis(positions[static_cast<std::size_t>(modelI)], basis, count) || count != p)
                {
                    error = "Could not evaluate the drift basis at effective sample "
                        + std::to_string(modelI) + " (original index "
                        + std::to_string(samples[static_cast<std::size_t>(modelI)].originalIndex) + ").";
                    return false;
                }
                for (int basisIndex = 0; basisIndex < p; ++basisIndex)
                {
                    matrix(i, k + basisIndex) = basis[static_cast<std::size_t>(basisIndex)];
                    matrix(k + basisIndex, i) = basis[static_cast<std::size_t>(basisIndex)];
                }
            }
        }
        return true;
    }

    bool Rhs(const Vec3& at, const std::vector<int>& indices,
             std::array<double, MaxSystemOrder>& rhs, int& order) const
    {
        const int k = indices.empty() ? static_cast<int>(samples.size()) : static_cast<int>(indices.size());
        const int p = DriftCount();
        order = k + p;
        if (order > MaxSystemOrder) return false;
        std::fill(rhs.begin(), rhs.end(), 0.0);
        for (int i = 0; i < k; ++i)
        {
            const int modelIndex = indices.empty() ? i : indices[static_cast<std::size_t>(i)];
            if (!variogram.hasPower)
            {
                rhs[static_cast<std::size_t>(i)] = variogram.Covariance(at,
                    positions[static_cast<std::size_t>(modelIndex)],
                    sourceVariogram.nuggetMode == NuggetMode::Exact);
            }
            else
            {
                const bool filtered = sourceVariogram.nuggetMode == NuggetMode::Filtered;
                rhs[static_cast<std::size_t>(i)] = -variogram.Semivariogram(at,
                    positions[static_cast<std::size_t>(modelIndex)], !filtered);
            }
            if (!Finite(rhs[static_cast<std::size_t>(i)])) return false;
        }
        if (p > 0)
        {
            std::array<double, MaxDriftTerms> basis{};
            int count = 0;
            if (!DriftBasis(at, basis, count) || count != p) return false;
            for (int i = 0; i < p; ++i)
            {
                rhs[static_cast<std::size_t>(k + i)] = basis[static_cast<std::size_t>(i)];
            }
        }
        return true;
    }

    bool BuildSystem(const std::vector<int>& indices, double ridge, System& out, std::string& error) const
    {
        const int k = indices.empty() ? static_cast<int>(samples.size()) : static_cast<int>(indices.size());
        const int p = DriftCount();
        if (k < 3 || (p > 0 && k < 3 * p) || k + p > MaxSystemOrder)
        {
            error = "The system does not satisfy sample-count or fixed-workspace limits.";
            return false;
        }
        System candidate;
        candidate.indices = indices;
        candidate.driftCount = p;
        candidate.ridge = ridge;
        if (!AssembleMatrix(indices, ridge, candidate.matrix, error)) return false;
        if (!candidate.solver.Factorize(candidate.matrix))
        {
            error = "singular or near-singular augmented system";
            return false;
        }
        std::vector<double> extended(static_cast<std::size_t>(k + p), 0.0);
        const double knownMean = settings.method == Method::Simple ? transformedKnownMean : 0.0;
        for (int i = 0; i < k; ++i)
        {
            const int modelIndex = indices.empty() ? i : indices[static_cast<std::size_t>(i)];
            extended[static_cast<std::size_t>(i)] = settings.method == Method::Simple
                ? workingValues[static_cast<std::size_t>(modelIndex)] - knownMean
                : workingValues[static_cast<std::size_t>(modelIndex)];
        }
        if (!candidate.solver.Solve(extended, candidate.dual))
        {
            error = "factorized system could not solve for dual weights";
            return false;
        }
        candidate.conditionProxy = candidate.solver.ConditionProxy();
        candidate.factorized = true;
        out = std::move(candidate);
        return true;
    }

    bool FactorizeWithEscalation(const std::vector<int>& indices, System& out,
                                 double& finalRidge, std::string& error) const
    {
        const double scale = std::max(1.0e-12, variogram.totalSill > 0.0 ? variogram.totalSill : 1.0);
        const double attempts[] = {0.0, 1.0e-12 * scale, 1.0e-10 * scale, 1.0e-8 * scale, 1.0e-6 * scale};
        for (double ridge : attempts)
        {
            std::string attemptError;
            if (BuildSystem(indices, ridge, out, attemptError))
            {
                finalRidge = ridge;
                return true;
            }
            error = std::move(attemptError);
            if (error.find("drift basis") != std::string::npos
                || error.find("non-finite") != std::string::npos) break;
        }
        return false;
    }

    std::vector<int> Nearest(const Vec3& at, int maximum, double radius, bool balanced) const
    {
        maximum = std::max(0, std::min(maximum, static_cast<int>(samples.size())));
        std::vector<Neighbour> nearest;
        spatialIndex.FindKNearest(at, maximum, 0.0, nearest);
        if (!balanced || maximum == 0)
        {
            std::vector<int> result;
            result.reserve(nearest.size());
            for (const Neighbour& neighbour : nearest)
            {
                if (radius <= 0.0 || neighbour.distanceSquared <= radius * radius)
                {
                    result.push_back(neighbour.sampleIndex);
                }
            }
            for (const Neighbour& neighbour : nearest)
            {
                if (static_cast<int>(result.size()) == maximum) break;
                if (std::find(result.begin(), result.end(), neighbour.sampleIndex) == result.end())
                {
                    result.push_back(neighbour.sampleIndex);
                }
            }
            return result;
        }

        std::vector<Neighbour> inRadius;
        spatialIndex.FindRadius(at, radius, inRadius);
        const int sectors = settings.planar ? 4 : 8;
        const int cap = (maximum + sectors - 1) / sectors;
        std::array<int, 8> counts{};
        std::vector<int> result;
        result.reserve(static_cast<std::size_t>(maximum));
        std::vector<bool> selected(samples.size(), false);
        for (const Neighbour& neighbour : inRadius)
        {
            const Vec3 delta = positions[static_cast<std::size_t>(neighbour.sampleIndex)] - at;
            int sector = (delta.x >= 0.0 ? 1 : 0) | (delta.y >= 0.0 ? 2 : 0);
            if (!settings.planar) sector |= (delta.z >= 0.0 ? 4 : 0);
            if (counts[static_cast<std::size_t>(sector)] >= cap) continue;
            result.push_back(neighbour.sampleIndex);
            selected[static_cast<std::size_t>(neighbour.sampleIndex)] = true;
            ++counts[static_cast<std::size_t>(sector)];
            if (static_cast<int>(result.size()) == maximum) return result;
        }
        for (const Neighbour& neighbour : nearest)
        {
            if (!selected[static_cast<std::size_t>(neighbour.sampleIndex)])
            {
                result.push_back(neighbour.sampleIndex);
                selected[static_cast<std::size_t>(neighbour.sampleIndex)] = true;
                if (static_cast<int>(result.size()) == maximum) break;
            }
        }
        return result;
    }

    std::shared_ptr<const System> LocalSystemFor(std::vector<int> indices) const
    {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        const int p = DriftCount();
        if (indices.size() < 3 || (p > 0 && static_cast<int>(indices.size()) < 3 * p)) return {};
        const std::uint64_t key = HashIndices(indices);
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const auto found = localCache.find(key);
            if (found != localCache.end())
            {
                for (const CacheBucketEntry& entry : found->second)
                {
                    if (entry.system && entry.system->indices == indices) return entry.system;
                }
            }
        }
        auto system = std::make_shared<System>();
        double ridge = 0.0;
        std::string error;
        if (!FactorizeWithEscalation(indices, *system, ridge, error)) return {};
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const auto found = localCache.find(key);
            if (found != localCache.end())
            {
                for (const CacheBucketEntry& entry : found->second)
                {
                    if (entry.system && entry.system->indices == indices) return entry.system;
                }
            }
            if (cacheEntries < 4096)
            {
                localCache[key].push_back({system});
                ++cacheEntries;
            }
        }
        return system;
    }

    std::shared_ptr<const System> LocalSystemAt(const Vec3& at) const
    {
        const double radius = settings.searchRadiusScale * std::max(1.0, variogram.MaxRange());
        return LocalSystemFor(Nearest(at, settings.maxNeighbours, radius, settings.sectorBalanced));
    }

    bool EvaluateTransformed(const Vec3& at, const System& system,
                             double& transformedValue, double* variance,
                             double* lagMultiplier = nullptr) const
    {
        std::array<double, MaxSystemOrder> rhs{};
        int order = 0;
        if (!Rhs(at, system.indices, rhs, order)
            || order != static_cast<int>(system.dual.size())) return false;
        double estimate = 0.0;
        for (int i = 0; i < order; ++i)
        {
            estimate += rhs[static_cast<std::size_t>(i)]
                * system.dual[static_cast<std::size_t>(i)];
        }
        if (settings.method == Method::Simple) estimate += transformedKnownMean;
        transformedValue = estimate;
        if (variance)
        {
            if (!system.factorized) return false;
            std::array<double, MaxSystemOrder> solution{};
            if (!system.solver.Solve(rhs.data(), order, solution.data())) return false;
            double product = 0.0;
            for (int i = 0; i < order; ++i)
            {
                product += rhs[static_cast<std::size_t>(i)]
                    * solution[static_cast<std::size_t>(i)];
            }
            double value = variogram.hasPower ? -product : variogram.totalSill - product;
            if (value < 0.0)
            {
                negativeVarianceClamps.fetch_add(1, std::memory_order_relaxed);
                value = 0.0;
            }
            *variance = value;
            if (lagMultiplier)
            {
                const int k = system.indices.empty()
                    ? static_cast<int>(samples.size())
                    : static_cast<int>(system.indices.size());
                *lagMultiplier = system.driftCount > 0
                    ? solution[static_cast<std::size_t>(k)] : 0.0;
            }
        }
        return Finite(transformedValue) && (!variance || Finite(*variance));
    }

    bool BiasNeedsVariance() const
    {
        return settings.transform == Transform::Logarithmic
            && settings.lognormalBiasCorrection;
    }

    double BackTransform(double transformed, const double* variance, const double* lag) const
    {
        if (settings.transform == Transform::Logarithmic
            && settings.lognormalBiasCorrection && variance)
        {
            return std::exp(transformed + 0.5 * *variance - (lag ? *lag : 0.0))
                - transform.logDelta;
        }
        return transform.Inverse(transformed);
    }

    bool IdwWorking(const Vec3& at, const std::vector<int>& candidateInput,
                    double& outMean, double& outVariance) const
    {
        std::vector<int> candidates = candidateInput;
        if (candidates.empty())
        {
            candidates = Nearest(at, settings.maxNeighbours, 0.0, false);
        }
        double weightSum = 0.0;
        double weighted = 0.0;
        int exact = -1;
        for (int index : candidates)
        {
            const std::size_t i = static_cast<std::size_t>(index);
            const double d2 = SquaredDistance(at, positions[i], settings.planar);
            if (d2 <= std::numeric_limits<double>::epsilon())
            {
                exact = index;
                break;
            }
            const double weight = 1.0 / d2;
            weightSum += weight;
            weighted += weight * workingValues[i];
        }
        if (exact >= 0)
        {
            const std::size_t i = static_cast<std::size_t>(exact);
            outMean = workingValues[i];
            outVariance = workingMeasurementVariances[i];
            return Finite(outMean) && Finite(outVariance);
        }
        if (!(weightSum > 0.0))
        {
            outMean = std::numeric_limits<double>::quiet_NaN();
            outVariance = 0.0;
            return false;
        }
        outMean = weighted / weightSum;
        double residual = 0.0;
        for (int index : candidates)
        {
            const std::size_t i = static_cast<std::size_t>(index);
            const double d2 = SquaredDistance(at, positions[i], settings.planar);
            if (d2 > std::numeric_limits<double>::epsilon())
            {
                const double delta = workingValues[i] - outMean;
                residual += delta * delta / d2;
            }
        }
        outVariance = std::max(0.0, residual / weightSum);
        return Finite(outMean) && Finite(outVariance);
    }

    double Idw(const Vec3& at, const std::vector<int>& candidateInput, double* variance) const
    {
        double mean = 0.0;
        double workingVariance = 0.0;
        if (!IdwWorking(at, candidateInput, mean, workingVariance))
        {
            if (variance) *variance = workingVariance;
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (variance) *variance = workingVariance;
        return BackTransform(mean, variance ? &workingVariance : nullptr, nullptr);
    }

    double Evaluate(const Vec3& at) const
    {
        if (!valid || !Finite(at.x) || !Finite(at.y) || !Finite(at.z))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (BiasNeedsVariance())
        {
            double value = 0.0, variance = 0.0;
            return EvaluateWithVariance(at, value, variance)
                ? value : std::numeric_limits<double>::quiet_NaN();
        }
        if (settings.method == Method::InverseDistance || report.degraded)
        {
            return Idw(at, {}, nullptr);
        }
        const System* system = &global;
        std::shared_ptr<const System> local;
        if (useLocal)
        {
            local = LocalSystemAt(at);
            if (!local)
            {
                localIdwFallbacks.fetch_add(1, std::memory_order_relaxed);
                return Idw(at, {}, nullptr);
            }
            system = local.get();
        }
        double transformed = 0.0;
        if (!EvaluateTransformed(at, *system, transformed, nullptr))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return BackTransform(transformed, nullptr, nullptr);
    }

    bool EvaluateWithVariance(const Vec3& at, double& value, double& variance) const
    {
        value = variance = std::numeric_limits<double>::quiet_NaN();
        if (!valid || !Finite(at.x) || !Finite(at.y) || !Finite(at.z)) return false;
        if (settings.method == Method::InverseDistance || report.degraded)
        {
            value = Idw(at, {}, &variance);
            return Finite(value) && Finite(variance);
        }
        const System* system = &global;
        std::shared_ptr<const System> local;
        if (useLocal)
        {
            local = LocalSystemAt(at);
            if (!local)
            {
                localIdwFallbacks.fetch_add(1, std::memory_order_relaxed);
                value = Idw(at, {}, &variance);
                return Finite(value) && Finite(variance);
            }
            system = local.get();
        }
        double transformed = 0.0;
        double lag = 0.0;
        if (!EvaluateTransformed(at, *system, transformed, &variance, &lag)) return false;
        value = BackTransform(transformed, &variance, &lag);
        return Finite(value) && Finite(variance);
    }

    static void FinalizeCv(const std::vector<double>& truth,
                           const std::vector<double>& estimates,
                           const std::vector<double>& standardErrors,
                           bool fast, CrossValidationReport& out,
                           const std::vector<double>* standardizedResidualsOverride = nullptr)
    {
        out = CrossValidationReport{};
        const std::size_t n = truth.size();
        if (n == 0 || estimates.size() != n || standardErrors.size() != n
            || (standardizedResidualsOverride
                && standardizedResidualsOverride->size() != n))
        {
            out.message = "Cross-validation arrays are inconsistent.";
            return;
        }
        out.count = static_cast<int>(n);
        out.usedFastPath = fast;
        out.estimated = estimates;
        out.residuals.resize(n);
        out.standardizedResiduals.resize(n);
        out.standardErrors = standardErrors;
        const double denominator = static_cast<double>(n);
        const double meanTruth = std::accumulate(truth.begin(), truth.end(), 0.0) / denominator;
        const double meanEstimate = std::accumulate(estimates.begin(), estimates.end(), 0.0) / denominator;
        double sumError = 0.0, sumAbs = 0.0, sumSquared = 0.0;
        double sumStandardized = 0.0, sumStandardizedSquared = 0.0;
        double covariance = 0.0, truthVariance = 0.0, estimateVariance = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double error = truth[i] - estimates[i];
            const double standardError = std::max(standardErrors[i],
                std::numeric_limits<double>::min());
            const double standardized = standardizedResidualsOverride
                ? (*standardizedResidualsOverride)[i]
                : error / standardError;
            out.residuals[i] = error;
            out.standardizedResiduals[i] = standardized;
            sumError += error;
            sumAbs += std::abs(error);
            sumSquared += error * error;
            sumStandardized += standardized;
            sumStandardizedSquared += standardized * standardized;
            const double truthDelta = truth[i] - meanTruth;
            const double estimateDelta = estimates[i] - meanEstimate;
            covariance += truthDelta * estimateDelta;
            truthVariance += truthDelta * truthDelta;
            estimateVariance += estimateDelta * estimateDelta;
        }
        out.meanError = sumError / denominator;
        out.meanAbsoluteError = sumAbs / denominator;
        out.rootMeanSquareError = std::sqrt(sumSquared / denominator);
        out.meanStandardizedError = sumStandardized / denominator;
        out.rootMeanSquareStandardizedError = std::sqrt(sumStandardizedSquared / denominator);
        out.correlation = truthVariance > 0.0 && estimateVariance > 0.0
            ? covariance / std::sqrt(truthVariance * estimateVariance) : 0.0;
        out.succeeded = true;
        out.message = fast
            ? "Cross-validation completed with the inverse-diagonal fast path."
            : "Cross-validation completed by brute-force rebuilding.";
    }
};

Model::Model() : impl_(std::make_unique<Impl>()) {}
Model::~Model() = default;
Model::Model(Model&&) noexcept = default;
Model& Model::operator=(Model&&) noexcept = default;

void Model::SetExternalDriftSampler(ExternalDriftSampler sampler, std::uint64_t contentHash)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (impl_->valid)
    {
        impl_->valid = false;
        impl_->report.succeeded = false;
        impl_->report.message = "External drift source changed; rebuild the model before evaluating.";
    }
    impl_->externalDrift = std::move(sampler);
    impl_->externalDriftContentHash = contentHash;
}

bool Model::Build(const std::vector<Sample>& inputSamples,
                  const Variogram& inputVariogram,
                  const Settings& inputSettings,
                  BuildReport& outReport)
{
    outReport = BuildReport{};

    Impl fresh;
    if (impl_)
    {
        fresh.externalDrift = impl_->externalDrift;
        fresh.externalDriftContentHash = impl_->externalDriftContentHash;
    }
    fresh.sourceVariogram = inputVariogram;
    fresh.settings = inputSettings;
    fresh.settings.globalSolveThreshold = std::max(16,
        std::min(MaxGlobalSamples, fresh.settings.globalSolveThreshold));
    fresh.settings.maxNeighbours = std::max(8,
        std::min(256, fresh.settings.maxNeighbours));
    fresh.settings.searchRadiusScale = std::max(0.01, fresh.settings.searchRadiusScale);
    fresh.settings.mergeRadius = std::max(0.0, fresh.settings.mergeRadius);

    auto Commit = [&](bool succeeded)
    {
        auto replacement = std::make_unique<Impl>();
        replacement->valid = succeeded && fresh.valid;
        replacement->useLocal = fresh.useLocal;
        replacement->sourceVariogram = std::move(fresh.sourceVariogram);
        replacement->settings = fresh.settings;
        replacement->variogram = std::move(fresh.variogram);
        replacement->transform = std::move(fresh.transform);
        replacement->externalDrift = std::move(fresh.externalDrift);
        replacement->externalDriftContentHash = fresh.externalDriftContentHash;
        replacement->samples = std::move(fresh.samples);
        replacement->positions = std::move(fresh.positions);
        replacement->spatialIndex = std::move(fresh.spatialIndex);
        replacement->workingValues = std::move(fresh.workingValues);
        replacement->workingMeasurementVariances = std::move(fresh.workingMeasurementVariances);
        replacement->driftCenter = fresh.driftCenter;
        replacement->driftScale = fresh.driftScale;
        replacement->externalDriftCenter = fresh.externalDriftCenter;
        replacement->externalDriftScale = fresh.externalDriftScale;
        replacement->transformedKnownMean = fresh.transformedKnownMean;
        replacement->global = std::move(fresh.global);
        replacement->report = std::move(fresh.report);
        impl_ = std::move(replacement);
        outReport = GetReport();
        return impl_->valid;
    };
    auto Fail = [&](std::string message)
    {
        fresh.valid = false;
        fresh.report.succeeded = false;
        fresh.report.message = std::move(message);
        return Commit(false);
    };

    if (inputSamples.size() < 3)
    {
        return Fail("At least three samples are required.");
    }
    if (!Finite(inputVariogram.nugget) || inputVariogram.nugget < 0.0)
    {
        return Fail("The variogram nugget must be finite and non-negative.");
    }
    bool hasPower = false;
    for (const Structure& structure : inputVariogram.structures)
    {
        hasPower = hasPower || structure.shape == Shape::Power;
    }
    if (hasPower && fresh.settings.method == Method::Simple)
    {
        return Fail("The unbounded power variogram is not admissible for simple kriging.");
    }
    if (fresh.settings.method == Method::ExternalDrift && !fresh.externalDrift)
    {
        return Fail("External-drift kriging requires a sampling delegate.");
    }
    if (!Finite(fresh.settings.knownMean)
        || !Finite(fresh.settings.searchRadiusScale)
        || !Finite(fresh.settings.mergeRadius))
    {
        return Fail("Solve settings contain a non-finite numeric value.");
    }

    std::vector<Sample> sorted = inputSamples;
    for (std::size_t i = 0; i < sorted.size(); ++i)
    {
        if (sorted[i].originalIndex < 0) sorted[i].originalIndex = static_cast<int>(i);
        if (fresh.settings.planar) sorted[i].location.z = 0.0;
        if (!Finite(sorted[i].location.x) || !Finite(sorted[i].location.y)
            || !Finite(sorted[i].location.z) || !Finite(sorted[i].value)
            || !Finite(sorted[i].measurementVariance)
            || sorted[i].measurementVariance < 0.0)
        {
            return Fail("Sample " + std::to_string(i)
                + " contains a non-finite coordinate, value, or measurement variance.");
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const Sample& a, const Sample& b)
    {
        if (a.location.x != b.location.x) return a.location.x < b.location.x;
        if (a.location.y != b.location.y) return a.location.y < b.location.y;
        if (a.location.z != b.location.z) return a.location.z < b.location.z;
        return a.originalIndex < b.originalIndex;
    });
    if (!fresh.MergeDuplicates(sorted, fresh.settings.mergeRadius, fresh.samples,
                               fresh.report.mergedPointCount,
                               fresh.report.maxMergedClusterDiameter))
    {
        return Fail("Duplicate merging failed.");
    }
    fresh.report.effectiveCount = static_cast<int>(fresh.samples.size());
    if (fresh.samples.size() < 3)
    {
        return Fail("Fewer than three effective samples remain after bounded-diameter duplicate merging.");
    }
    if (fresh.report.maxMergedClusterDiameter > fresh.settings.mergeRadius + 1.0e-12)
    {
        return Fail("Duplicate merge cluster diameter exceeded MergeRadius; this is an internal error.");
    }
    if (fresh.report.mergedPointCount > 0)
    {
        fresh.report.warnings.push_back(
            "Merged duplicate samples with complete-link clusters whose diameter cannot exceed MergeRadius.");
    }

    Vec3 minimum{std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity()};
    Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    fresh.positions.reserve(fresh.samples.size());
    for (const Sample& sample : fresh.samples)
    {
        fresh.positions.push_back(sample.location);
        minimum.x = std::min(minimum.x, sample.location.x);
        minimum.y = std::min(minimum.y, sample.location.y);
        minimum.z = std::min(minimum.z, sample.location.z);
        maximum.x = std::max(maximum.x, sample.location.x);
        maximum.y = std::max(maximum.y, sample.location.y);
        maximum.z = std::max(maximum.z, sample.location.z);
    }
    std::vector<int> originalIndices;
    originalIndices.reserve(fresh.samples.size());
    for (const Sample& sample : fresh.samples)
    {
        originalIndices.push_back(sample.originalIndex);
    }
    if (!fresh.spatialIndex.Build(fresh.positions, originalIndices, fresh.settings.planar))
    {
        return Fail("Could not build the spatial index.");
    }

    fresh.driftCenter = (minimum + maximum) * 0.5;
    fresh.driftScale = {
        std::max(1.0, 0.5 * (maximum.x - minimum.x)),
        std::max(1.0, 0.5 * (maximum.y - minimum.y)),
        fresh.settings.planar ? 1.0 : std::max(1.0, 0.5 * (maximum.z - minimum.z))};

    if (fresh.settings.method == Method::ExternalDrift)
    {
        std::vector<double> values(fresh.samples.size());
        double sum = 0.0;
        for (std::size_t i = 0; i < fresh.samples.size(); ++i)
        {
            if (!fresh.externalDrift(fresh.samples[i].location, values[i])
                || !Finite(values[i]))
            {
                return Fail("External drift could not be sampled at effective sample "
                    + std::to_string(i) + " (original index "
                    + std::to_string(fresh.samples[i].originalIndex) + ").");
            }
            sum += values[i];
        }
        fresh.externalDriftCenter = sum / static_cast<double>(values.size());
        double maximumDeviation = 0.0;
        for (double value : values)
        {
            maximumDeviation = std::max(maximumDeviation,
                                        std::abs(value - fresh.externalDriftCenter));
        }
        if (!(maximumDeviation > 1.0e-12))
        {
            return Fail("External drift is constant across the samples and is linearly dependent on the ordinary-kriging intercept.");
        }
        fresh.externalDriftScale = maximumDeviation;
    }

    std::string error;
    if (!fresh.variogram.Build(inputVariogram, fresh.settings.planar,
                               fresh.report.warnings, error))
    {
        return Fail(error);
    }
    if (!fresh.transform.Build(fresh.samples, fresh.settings.transform,
                               fresh.workingValues,
                               fresh.workingMeasurementVariances, error))
    {
        return Fail(error);
    }
    if (fresh.settings.method == Method::Simple)
    {
        fresh.transformedKnownMean = fresh.transform.Forward(fresh.settings.knownMean);
        if (!Finite(fresh.transformedKnownMean))
        {
            return Fail("The simple-kriging known mean is outside the domain of the selected value transform.");
        }
    }

    const int drift = fresh.DriftCount();
    if (drift > 0 && static_cast<int>(fresh.samples.size()) < 3 * drift)
    {
        return Fail("The selected drift basis does not satisfy the n >= 3p requirement.");
    }

    fresh.useLocal = fresh.settings.solveMode == SolveMode::ForceLocal
        || (fresh.settings.solveMode == SolveMode::Automatic
            && static_cast<int>(fresh.samples.size()) > fresh.settings.globalSolveThreshold);
    if (fresh.settings.solveMode == SolveMode::ForceGlobal)
    {
        if (static_cast<int>(fresh.samples.size()) > MaxGlobalSamples)
        {
            return Fail("ForceGlobal is limited to 512 effective samples by the fixed-workspace query contract. Use Automatic or ForceLocal.");
        }
        fresh.useLocal = false;
    }
    if (fresh.useLocal && drift > 0 && fresh.settings.maxNeighbours < 3 * drift)
    {
        fresh.settings.maxNeighbours = std::min(256, 3 * drift);
        fresh.report.warnings.push_back(
            "Raised MaxNeighbours to satisfy the n >= 3p local drift requirement.");
    }

    if (fresh.settings.method == Method::InverseDistance)
    {
        fresh.valid = true;
        fresh.report.succeeded = true;
        fresh.report.message = "Inverse-distance model built successfully.";
    }
    else if (fresh.useLocal)
    {
        fresh.valid = true;
        fresh.report.succeeded = true;
        fresh.report.usedLocalSolver = true;
        fresh.report.message =
            "Local kriging model built successfully. Grid evaluation uses per-query neighbourhoods in this correctness gate; unproved tile blending is disabled.";
    }
    else
    {
        double ridge = 0.0;
        if (!fresh.FactorizeWithEscalation({}, fresh.global, ridge, error))
        {
            fresh.report.degraded = true;
            fresh.report.warnings.push_back(
                "Global kriging factorization failed after bounded ridge escalation; value queries use k-nearest inverse-distance weighting.");
            fresh.report.message = "Model degraded to inverse-distance weighting.";
            fresh.valid = true;
            fresh.report.succeeded = true;
        }
        else
        {
            fresh.report.finalRidge = ridge;
            fresh.report.conditionProxy = fresh.global.conditionProxy;
            if (ridge > 0.0)
            {
                fresh.report.warnings.push_back(
                    "A nonzero ridge was required. Exact-mode sample reproduction is measured from the solved surface and may be affected; no exact-sample shortcut is used.");
            }
            if (fresh.global.conditionProxy > 1.0e10)
            {
                fresh.report.warnings.push_back(
                    "Condition proxy exceeds 1e10; inspect coincident samples, range-to-extent ratio, and nugget choice.");
            }
            fresh.valid = true;
            fresh.report.succeeded = true;
            fresh.report.message = "Global kriging model built successfully.";
        }
    }

    return Commit(true);
}

bool Model::IsValid() const { return impl_ && impl_->valid; }
bool Model::UsesLocalSolver() const { return impl_ && impl_->useLocal; }
double Model::Evaluate(const Vec3& at) const
{
    return impl_ ? impl_->Evaluate(at) : std::numeric_limits<double>::quiet_NaN();
}
bool Model::EvaluateWithVariance(const Vec3& at, double& value, double& variance) const
{
    return impl_ && impl_->EvaluateWithVariance(at, value, variance);
}
void Model::EvaluateBatch(const std::vector<Vec3>& points, std::vector<double>& outValues) const
{
    outValues.resize(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) outValues[i] = Evaluate(points[i]);
}

bool Model::ComputeWeights(const Vec3& at, std::vector<double>& outSampleWeights,
                           std::vector<double>* outConstraintMultipliers) const
{
    outSampleWeights.clear();
    if (outConstraintMultipliers) outConstraintMultipliers->clear();
    if (!impl_ || !impl_->valid || impl_->settings.method == Method::InverseDistance
        || impl_->report.degraded) return false;
    const Impl::System* system = &impl_->global;
    std::shared_ptr<const Impl::System> local;
    if (impl_->useLocal)
    {
        local = impl_->LocalSystemAt(at);
        if (!local) return false;
        system = local.get();
    }
    std::array<double, MaxSystemOrder> rhs{};
    std::array<double, MaxSystemOrder> solution{};
    int order = 0;
    if (!impl_->Rhs(at, system->indices, rhs, order)
        || !system->solver.Solve(rhs.data(), order, solution.data())) return false;
    const int k = system->indices.empty()
        ? static_cast<int>(impl_->samples.size())
        : static_cast<int>(system->indices.size());
    if (system->indices.empty())
    {
        outSampleWeights.assign(solution.begin(), solution.begin() + k);
    }
    else
    {
        outSampleWeights.assign(impl_->samples.size(), 0.0);
        for (int i = 0; i < k; ++i)
        {
            const int sampleIndex = system->indices[static_cast<std::size_t>(i)];
            outSampleWeights[static_cast<std::size_t>(sampleIndex)]
                = solution[static_cast<std::size_t>(i)];
        }
    }
    if (outConstraintMultipliers)
    {
        outConstraintMultipliers->assign(solution.begin() + k,
                                         solution.begin() + order);
    }
    return true;
}

bool Model::CopyGlobalDualWeights(std::vector<double>& outDualWeights) const
{
    outDualWeights.clear();
    if (!impl_ || !impl_->valid || impl_->useLocal || !impl_->global.factorized)
    {
        return false;
    }
    outDualWeights = impl_->global.dual;
    return true;
}

BuildReport Model::GetReport() const
{
    if (!impl_) return {};
    BuildReport result = impl_->report;
    result.negativeVarianceClamps = impl_->negativeVarianceClamps.load(std::memory_order_relaxed);
    result.localIdwFallbacks = impl_->localIdwFallbacks.load(std::memory_order_relaxed);
    if (result.localIdwFallbacks > 0)
    {
        result.degraded = true;
        result.warnings.push_back(std::to_string(result.localIdwFallbacks)
            + " local evaluations degraded to k-nearest inverse-distance weighting.");
    }
    return result;
}

const std::vector<Sample>& Model::Samples() const
{
    static const std::vector<Sample> empty;
    return impl_ ? impl_->samples : empty;
}
const Variogram& Model::SourceVariogram() const
{
    static const Variogram empty;
    return impl_ ? impl_->sourceVariogram : empty;
}
const Settings& Model::SourceSettings() const
{
    static const Settings empty;
    return impl_ ? impl_->settings : empty;
}
int Model::DriftCount() const { return impl_ ? impl_->DriftCount() : 0; }
double Model::MaximumRange() const { return impl_ ? impl_->variogram.MaxRange() : 0.0; }

bool Model::CrossValidateBruteForce(CrossValidationReport& out, int maximumSamples) const
{
    out = CrossValidationReport{};
    if (!impl_ || !impl_->valid || impl_->samples.size() < 4)
    {
        out.message = "Brute-force leave-one-out requires a valid model with at least four effective samples.";
        return false;
    }
    maximumSamples = std::max(1, maximumSamples);
    if (static_cast<int>(impl_->samples.size()) > maximumSamples)
    {
        out.message = "Brute-force leave-one-out is capped at "
            + std::to_string(maximumSamples)
            + " samples to prevent an unbounded synchronous O(n^4) operation.";
        return false;
    }
    std::vector<double> truth(impl_->samples.size());
    std::vector<double> estimates(impl_->samples.size());
    std::vector<double> standardErrors(impl_->samples.size());
    std::vector<double> standardizedResiduals(impl_->samples.size());
    for (std::size_t held = 0; held < impl_->samples.size(); ++held)
    {
        std::vector<Sample> training;
        training.reserve(impl_->samples.size() - 1);
        for (std::size_t i = 0; i < impl_->samples.size(); ++i)
        {
            if (i != held) training.push_back(impl_->samples[i]);
        }
        Model reference;
        reference.SetExternalDriftSampler(impl_->externalDrift,
                                          impl_->externalDriftContentHash);
        BuildReport build;
        if (!reference.Build(training, impl_->sourceVariogram, impl_->settings, build))
        {
            out.message = "Leave-one-out rebuild failed at sample "
                + std::to_string(held) + ": " + build.message;
            return false;
        }

        const Vec3 query = impl_->samples[held].location;
        double workingEstimate = 0.0;
        double workingVariance = 0.0;
        double lagMultiplier = 0.0;
        bool evaluated = false;
        if (reference.impl_->settings.method == Method::InverseDistance
            || reference.impl_->report.degraded)
        {
            evaluated = reference.impl_->IdwWorking(
                query, {}, workingEstimate, workingVariance);
        }
        else
        {
            const Impl::System* system = &reference.impl_->global;
            std::shared_ptr<const Impl::System> local;
            if (reference.impl_->useLocal)
            {
                local = reference.impl_->LocalSystemAt(query);
                if (local)
                {
                    system = local.get();
                }
                else
                {
                    evaluated = reference.impl_->IdwWorking(
                        query, {}, workingEstimate, workingVariance);
                }
            }
            if (!reference.impl_->useLocal || local)
            {
                evaluated = reference.impl_->EvaluateTransformed(
                    query, *system, workingEstimate, &workingVariance,
                    &lagMultiplier);
            }
        }
        if (!evaluated)
        {
            out.message = "Leave-one-out evaluation failed at sample "
                + std::to_string(held) + ".";
            return false;
        }

        const double standardError = std::sqrt(std::max(
            workingVariance, std::numeric_limits<double>::min()));
        const double workingTruth = reference.impl_->transform.Forward(
            impl_->samples[held].value);
        if (!Finite(workingTruth))
        {
            out.message = "Leave-one-out transform failed at sample "
                + std::to_string(held) + ".";
            return false;
        }
        truth[held] = impl_->samples[held].value;
        estimates[held] = reference.impl_->BackTransform(
            workingEstimate, &workingVariance, &lagMultiplier);
        standardErrors[held] = standardError;
        standardizedResiduals[held] =
            (workingTruth - workingEstimate) / standardError;
    }
    Impl::FinalizeCv(truth, estimates, standardErrors, false, out,
                     &standardizedResiduals);
    if (out.succeeded && impl_->settings.transform != Transform::None)
    {
        out.message += " Standardized residuals and standard errors are in transformed space; unstandardized errors are in original value space.";
    }
    return out.succeeded;
}

bool Model::CrossValidate(CrossValidationReport& out) const
{
    out = CrossValidationReport{};
    if (!impl_ || !impl_->valid)
    {
        out.message = "Model is not valid.";
        return false;
    }
    const bool hasMeasurementVariance = std::any_of(
        impl_->workingMeasurementVariances.begin(),
        impl_->workingMeasurementVariances.end(),
        [](double value) { return value > 0.0; });
    if (impl_->useLocal || impl_->settings.method == Method::InverseDistance
        || impl_->report.degraded || impl_->settings.transform != Transform::None
        || impl_->variogram.hasPower || !impl_->global.factorized
        || impl_->global.ridge != 0.0 || hasMeasurementVariance)
    {
        return CrossValidateBruteForce(out, 60);
    }
    std::vector<double> inverseDiagonal;
    if (!impl_->global.solver.InverseDiagonal(inverseDiagonal)
        || inverseDiagonal.size() < impl_->samples.size())
    {
        return CrossValidateBruteForce(out, 60);
    }
    std::vector<double> truth(impl_->samples.size());
    std::vector<double> estimates(impl_->samples.size());
    std::vector<double> standardErrors(impl_->samples.size());
    for (std::size_t i = 0; i < impl_->samples.size(); ++i)
    {
        const double diagonal = inverseDiagonal[i];
        if (!(diagonal > 0.0) || !Finite(diagonal))
        {
            return CrossValidateBruteForce(out, 60);
        }
        const double residual = impl_->global.dual[i] / diagonal;
        truth[i] = impl_->samples[i].value;
        estimates[i] = truth[i] - residual;
        standardErrors[i] = std::sqrt(1.0 / diagonal);
    }
    CrossValidationReport fast;
    Impl::FinalizeCv(truth, estimates, standardErrors, true, fast);

    if (impl_->samples.size() <= 60)
    {
        CrossValidationReport brute;
        if (!CrossValidateBruteForce(brute, 60))
        {
            out = brute;
            return false;
        }
        bool parity = fast.estimated.size() == brute.estimated.size();
        for (std::size_t i = 0; parity && i < fast.estimated.size(); ++i)
        {
            parity = RelativeClose(fast.estimated[i], brute.estimated[i], 1.0e-8)
                && RelativeClose(fast.standardErrors[i], brute.standardErrors[i], 1.0e-8);
        }
        if (!parity)
        {
            out = brute;
            out.message = "Inverse-diagonal leave-one-out failed runtime parity; returned the brute-force reference result.";
            return true;
        }
        fast.verifiedAgainstBruteForce = true;
        fast.message = "Cross-validation completed with the inverse-diagonal fast path and matched brute force at runtime.";
    }
    else
    {
        fast.message =
            "Cross-validation completed with the inverse-diagonal fast path. "
            "This model was not rebuilt point-by-point because the synchronous reference is capped at 60 samples; "
            "verifiedAgainstBruteForce remains false.";
    }
    out = std::move(fast);
    return out.succeeded;
}

} // namespace kriging::portable
