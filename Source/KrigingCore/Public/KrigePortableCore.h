#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Under UnrealBuildTool, KRIGINGCORE_API is pre-defined to DLLEXPORT/DLLIMPORT on the
// command line, but those tokens are only defined by Unreal's platform headers, which
// this portable header deliberately never includes. Supplying the identical definitions
// here keeps the macro functional in translation units that include no Unreal headers;
// Unreal's own later definitions are token-identical, so no redefinition conflict arises.
#if defined(_MSC_VER)
	#ifndef DLLEXPORT
		#define DLLEXPORT __declspec(dllexport)
	#endif
	#ifndef DLLIMPORT
		#define DLLIMPORT __declspec(dllimport)
	#endif
#else
	#ifndef DLLEXPORT
		#define DLLEXPORT __attribute__((visibility("default")))
	#endif
	#ifndef DLLIMPORT
		#define DLLIMPORT __attribute__((visibility("default")))
	#endif
#endif
#ifndef KRIGINGCORE_API
#define KRIGINGCORE_API
#endif

namespace kriging::portable
{
constexpr int MaxGlobalSamples = 512;
constexpr int MaxDriftTerms = 10;
constexpr int MaxSystemOrder = MaxGlobalSamples + MaxDriftTerms;

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3() = default;
    Vec3(double inX, double inY, double inZ = 0.0) : x(inX), y(inY), z(inZ) {}

    Vec3 operator+(const Vec3& rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    Vec3 operator-(const Vec3& rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    Vec3 operator*(double scale) const { return {x * scale, y * scale, z * scale}; }
    Vec3 operator/(double scale) const { return {x / scale, y / scale, z / scale}; }
    Vec3& operator+=(const Vec3& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
};

enum class Shape : std::uint8_t { Spherical, Exponential, Gaussian, Matern, Power };
enum class Method : std::uint8_t { Simple, Ordinary, UniversalLinear, UniversalQuadratic, ExternalDrift, InverseDistance };
enum class NuggetMode : std::uint8_t { Exact, Filtered };
enum class Transform : std::uint8_t { None, Logarithmic, NormalScore };
enum class SolveMode : std::uint8_t { Automatic, ForceGlobal, ForceLocal };

struct Anisotropy
{
    double azimuthDeg = 0.0;
    double dipDeg = 0.0;
    double plungeDeg = 0.0;
    double ratioY = 1.0;
    double ratioZ = 1.0;
};

struct Structure
{
    Shape shape = Shape::Spherical;
    double range = 1000.0;
    double partialSill = 1.0;
    double maternNu = 1.5;
    double powerAlpha = 1.0;
    Anisotropy anisotropy;
};

struct Variogram
{
    double nugget = 0.0;
    std::vector<Structure> structures;
    NuggetMode nuggetMode = NuggetMode::Exact;
};

struct Sample
{
    Vec3 location;
    double value = 0.0;
    double measurementVariance = 0.0;
    int originalIndex = -1;
};

struct Settings
{
    Method method = Method::Ordinary;
    Transform transform = Transform::None;
    SolveMode solveMode = SolveMode::Automatic;
    bool planar = true;
    double knownMean = 0.0;
    int globalSolveThreshold = MaxGlobalSamples;
    int maxNeighbours = 32;
    double searchRadiusScale = 1.5;
    bool sectorBalanced = true;
    double mergeRadius = 1.0;
    bool lognormalBiasCorrection = false;
};

struct BuildReport
{
    bool succeeded = false;
    bool degraded = false;
    bool usedLocalSolver = false;
    int mergedPointCount = 0;
    int effectiveCount = 0;
    double maxMergedClusterDiameter = 0.0;
    double finalRidge = 0.0;
    double conditionProxy = 0.0;
    int negativeVarianceClamps = 0;
    int localIdwFallbacks = 0;
    std::vector<std::string> warnings;
    std::string message;
};

struct CrossValidationReport
{
    bool succeeded = false;
    bool usedFastPath = false;
    bool verifiedAgainstBruteForce = false;
    int count = 0;
    double meanError = 0.0;
    double meanAbsoluteError = 0.0;
    double rootMeanSquareError = 0.0;
    double meanStandardizedError = 0.0;
    double rootMeanSquareStandardizedError = 0.0;
    double correlation = 0.0;
    std::vector<double> estimated;
    std::vector<double> residuals;
    std::vector<double> standardizedResiduals;
    std::vector<double> standardErrors;
    std::string message;
};

class KRIGINGCORE_API DenseMatrix
{
public:
    DenseMatrix() = default;
    DenseMatrix(int rows, int columns, double value = 0.0) { Resize(rows, columns, value); }

    void Resize(int rows, int columns, double value = 0.0);
    int Rows() const { return rows_; }
    int Columns() const { return columns_; }
    bool Empty() const { return data_.empty(); }
    double* Data() { return data_.data(); }
    const double* Data() const { return data_.data(); }
    double& operator()(int row, int column)
    {
        return data_[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_)
            + static_cast<std::size_t>(column)];
    }
    double operator()(int row, int column) const
    {
        return data_[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_)
            + static_cast<std::size_t>(column)];
    }

private:
    int rows_ = 0;
    int columns_ = 0;
    std::vector<double> data_;
};

class KRIGINGCORE_API PartialPivLU
{
public:
    bool Factorize(const DenseMatrix& matrix);
    bool Solve(const double* rhs, int count, double* outSolution) const;
    bool Solve(const std::vector<double>& rhs, std::vector<double>& outSolution) const;
    bool Inverse(DenseMatrix& outInverse) const;
    bool InverseDiagonal(std::vector<double>& outDiagonal) const;
    int Order() const { return order_; }
    double ConditionProxy() const { return conditionProxy_; }

private:
    int order_ = 0;
    DenseMatrix lu_;
    std::vector<int> pivotRows_;
    double conditionProxy_ = 0.0;
};

KRIGINGCORE_API double InverseStandardNormalCdf(double probability);
KRIGINGCORE_API double EvaluateNormalizedStructure(Shape shape, double ratio, double maternNu, double powerAlpha);

struct Neighbour
{
    int sampleIndex = -1;
    int originalIndex = -1;
    double distanceSquared = 0.0;
};

class KRIGINGCORE_API KdTree
{
public:
    KdTree();
    ~KdTree();
    KdTree(KdTree&&) noexcept;
    KdTree& operator=(KdTree&&) noexcept;
    KdTree(const KdTree&) = delete;
    KdTree& operator=(const KdTree&) = delete;

    bool Build(const std::vector<Vec3>& positions,
               const std::vector<int>& originalIndices,
               bool planar);
    void Reset();
    bool Empty() const;
    int Size() const;
    int FindExact(const Vec3& at) const;
    void FindKNearest(const Vec3& at, int maximumCount, double radius,
                      std::vector<Neighbour>& out) const;
    void FindRadius(const Vec3& at, double radius,
                    std::vector<Neighbour>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class KRIGINGCORE_API Model
{
public:
    using ExternalDriftSampler = std::function<bool(const Vec3&, double&)>;

    Model();
    ~Model();
    Model(Model&&) noexcept;
    Model& operator=(Model&&) noexcept;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    void SetExternalDriftSampler(ExternalDriftSampler sampler, std::uint64_t contentHash = 0);
    bool Build(const std::vector<Sample>& samples,
               const Variogram& variogram,
               const Settings& settings,
               BuildReport& outReport);

    bool IsValid() const;
    bool UsesLocalSolver() const;
    double Evaluate(const Vec3& at) const;
    bool EvaluateWithVariance(const Vec3& at, double& outValue, double& outVariance) const;
    void EvaluateBatch(const std::vector<Vec3>& points, std::vector<double>& outValues) const;

    bool CrossValidate(CrossValidationReport& out) const;
    bool CrossValidateBruteForce(CrossValidationReport& out, int maximumSamples = 60) const;

    // Diagnostic path used by the property suite. It always solves the actual
    // augmented system; kriging evaluation has no exact-sample shortcut.
    bool ComputeWeights(const Vec3& at, std::vector<double>& outSampleWeights,
                        std::vector<double>* outConstraintMultipliers = nullptr) const;
    bool CopyGlobalDualWeights(std::vector<double>& outDualWeights) const;

    BuildReport GetReport() const;
    const std::vector<Sample>& Samples() const;
    const Variogram& SourceVariogram() const;
    const Settings& SourceSettings() const;
    int DriftCount() const;
    double MaximumRange() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kriging::portable
