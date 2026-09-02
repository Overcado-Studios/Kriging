#include "KrigePortableCore.h"
#include "KrigeKdTree.h"
#include "KrigeLinearSolve.h"
#include "KrigeModel.h"
#include "KrigeTransform.h"
#include "KrigeTypes.h"
#include "KrigeVariogram.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kriging::portable;

namespace
{
int Failures = 0;

void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        ++Failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Close(double a, double b, double relative = 1.0e-9,
           double absolute = 1.0e-11)
{
    return std::abs(a - b) <= std::max(absolute,
        relative * std::max(std::abs(a), std::abs(b)));
}

Variogram DefaultVariogram(NuggetMode mode = NuggetMode::Exact)
{
    Variogram value;
    value.nugget = 0.02;
    value.nuggetMode = mode;
    value.structures.push_back({Shape::Spherical, 4.0, 0.7, 1.5, 1.0, {}});
    value.structures.push_back({Shape::Exponential, 9.0, 0.3, 1.5, 1.0,
        {25.0, 0.0, 0.0, 0.65, 1.0}});
    return value;
}

std::vector<Sample> RandomSamples(int count, unsigned seed = 42,
                                  bool volumetric = false)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(-5.0, 5.0);
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const double x = d(rng);
        const double y = d(rng);
        const double z = volumetric ? d(rng) : 0.0;
        samples.push_back({{x, y, z},
            std::sin(0.4 * x) + 0.3 * std::cos(0.7 * y)
                + (volumetric ? 0.1 * z : 0.0) + 0.05 * i,
            0.0, i});
    }
    return samples;
}

Model BuildOrThrow(const std::vector<Sample>& samples,
                   const Variogram& variogram,
                   const Settings& settings)
{
    Model model;
    BuildReport report;
    if (!model.Build(samples, variogram, settings, report))
    {
        throw std::runtime_error(report.message);
    }
    return model;
}

void TestStructureFunctionsAndAs241()
{
    for (Shape shape : {Shape::Spherical, Shape::Exponential,
                        Shape::Gaussian, Shape::Matern})
    {
        double previous = -1.0;
        for (int i = 0; i <= 10000; ++i)
        {
            const double ratio = 10.0 * i / 10000.0;
            const double value = EvaluateNormalizedStructure(shape, ratio, 1.5, 1.2);
            Check(std::isfinite(value) && value >= 0.0 && value <= 1.0,
                  "bounded structure must remain finite in [0,1]");
            Check(value + 1.0e-13 >= previous,
                  "bounded structure must be monotone");
            previous = value;
        }
    }
    Check(EvaluateNormalizedStructure(Shape::Matern, 1.0e8, 10.0, 1.0) == 1.0,
          "large-distance Matern underflow must resolve to the asymptotic sill");
    for (double p : {1.0e-15, 1.0e-9, 0.01, 0.2, 0.5, 0.8, 0.99,
                     1.0 - 1.0e-9, 1.0 - 1.0e-15})
    {
        const double x = InverseStandardNormalCdf(p);
        const double roundTrip = 0.5 * std::erfc(-x / std::sqrt(2.0));
        Check(Close(roundTrip, p, 2.0e-12, 2.0e-15),
              "AS241 inverse-normal round trip");
    }
}

void TestExactInterpolationAndNoShortcut()
{
    const auto samples = RandomSamples(30, 7);
    const Variogram variogram = DefaultVariogram();
    for (Method method : {Method::Simple, Method::Ordinary,
                          Method::UniversalLinear, Method::UniversalQuadratic})
    {
        Settings settings;
        settings.method = method;
        settings.knownMean = 0.35;
        settings.mergeRadius = 0.0;
        settings.solveMode = SolveMode::ForceGlobal;
        Model model = BuildOrThrow(samples, variogram, settings);
        Check(model.GetReport().finalRidge == 0.0,
              "well-posed exactness case should not require ridge");
        for (const Sample& sample : model.Samples())
        {
            Check(Close(model.Evaluate(sample.location), sample.value,
                        1.0e-10, 1.0e-11),
                  "solved exact-mode surface must reproduce samples");
        }
    }

    Model external;
    external.SetExternalDriftSampler([](const Vec3& point, double& out)
    {
        out = 0.2 * point.x - 0.1 * point.y;
        return true;
    }, 11);
    Settings externalSettings;
    externalSettings.method = Method::ExternalDrift;
    externalSettings.mergeRadius = 0.0;
    externalSettings.solveMode = SolveMode::ForceGlobal;
    BuildReport externalReport;
    Check(external.Build(samples, variogram, externalSettings, externalReport),
          "external-drift exactness build");
    for (const Sample& sample : external.Samples())
    {
        Check(Close(external.Evaluate(sample.location), sample.value,
                    1.0e-10, 1.0e-11),
              "external-drift exactness must come from solve");
    }

    Variogram gaussian;
    gaussian.nugget = 0.0;
    gaussian.nuggetMode = NuggetMode::Exact;
    gaussian.structures.push_back({Shape::Gaussian, 4.0, 1.0, 1.5, 1.0, {}});
    Settings ordinary;
    ordinary.method = Method::Ordinary;
    ordinary.mergeRadius = 0.0;
    ordinary.solveMode = SolveMode::ForceGlobal;
    Model gaussianModel = BuildOrThrow(samples, gaussian, ordinary);
    Check(!gaussianModel.GetReport().warnings.empty(),
          "Gaussian zero nugget should activate conditioning-floor warning");
    for (const Sample& sample : gaussianModel.Samples())
    {
        Check(Close(gaussianModel.Evaluate(sample.location), sample.value,
                    1.0e-9, 1.0e-10),
              "exact nugget floor must remain exact through solved system");
    }

    Variogram power;
    power.nugget = 0.02;
    power.nuggetMode = NuggetMode::Exact;
    power.structures.push_back({Shape::Power, 4.0, 0.7, 1.5, 1.2, {}});
    Model powerModel = BuildOrThrow(samples, power, ordinary);
    for (const Sample& sample : powerModel.Samples())
    {
        Check(Close(powerModel.Evaluate(sample.location), sample.value,
                    1.0e-9, 1.0e-10),
              "power semivariogram exactness must come from solve");
    }
}

void TestPartitionUnityAndConstantField()
{
    const auto samples = RandomSamples(24, 17);
    for (Method method : {Method::Ordinary, Method::UniversalLinear,
                          Method::UniversalQuadratic})
    {
        Settings settings;
        settings.method = method;
        settings.mergeRadius = 0.0;
        settings.solveMode = SolveMode::ForceGlobal;
        Model model = BuildOrThrow(samples, DefaultVariogram(), settings);
        std::vector<double> weights;
        Check(model.ComputeWeights({0.35, -1.2, 0.0}, weights),
              "weight solve should succeed");
        double sum = 0.0;
        for (double weight : weights) sum += weight;
        Check(Close(sum, 1.0, 1.0e-11, 1.0e-12),
              "ordinary/universal weights sum to one");
    }

    auto constant = samples;
    for (Sample& sample : constant) sample.value = 7.25;
    for (Method method : {Method::Simple, Method::Ordinary,
                          Method::UniversalLinear, Method::UniversalQuadratic,
                          Method::InverseDistance})
    {
        Settings settings;
        settings.method = method;
        settings.knownMean = 7.25;
        settings.mergeRadius = 0.0;
        settings.solveMode = SolveMode::ForceGlobal;
        Model model = BuildOrThrow(constant, DefaultVariogram(), settings);
        Check(Close(model.Evaluate({1.3, -2.1, 0.0}), 7.25,
                    1.0e-11, 1.0e-11),
              "constant field must remain constant");
    }
}

void TestDriftReproductionAndLargeCoordinates()
{
    std::vector<Sample> samples;
    int index = 0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            const Vec3 point{5.0e7 + 1000.0 * x,
                             -3.0e7 + 1000.0 * y, 0.0};
            const double localX = point.x - 5.0e7;
            const double localY = point.y + 3.0e7;
            samples.push_back({point,
                3.0 + 0.0017 * localX - 0.0006 * localY,
                0.0, index++});
        }
    }
    Variogram variogram = DefaultVariogram();
    variogram.nugget = 0.0;
    for (Structure& structure : variogram.structures) structure.range *= 1000.0;

    Settings linear;
    linear.method = Method::UniversalLinear;
    linear.mergeRadius = 0.0;
    linear.solveMode = SolveMode::ForceGlobal;
    Model linearModel = BuildOrThrow(samples, variogram, linear);
    for (Vec3 point : std::vector<Vec3>{{5.0e7 + 350.0, -3.0e7 - 1400.0, 0.0},
                                        {5.0e7 + 1200.0, -3.0e7 + 700.0, 0.0}})
    {
        const double expected = 3.0 + 0.0017 * (point.x - 5.0e7)
            - 0.0006 * (point.y + 3.0e7);
        Check(Close(linearModel.Evaluate(point), expected, 1.0e-8, 1.0e-9),
              "centered/scaled linear drift reproduction at large coordinates");
    }

    for (Sample& sample : samples)
    {
        const double x = (sample.location.x - 5.0e7) / 1000.0;
        const double y = (sample.location.y + 3.0e7) / 1000.0;
        sample.value = 2.0 + 0.8 * x - 0.4 * y
            + 0.3 * x * x - 0.2 * x * y + 0.5 * y * y;
    }
    Settings quadratic = linear;
    quadratic.method = Method::UniversalQuadratic;
    Model quadraticModel = BuildOrThrow(samples, variogram, quadratic);
    for (Vec3 point : std::vector<Vec3>{{5.0e7 + 350.0, -3.0e7 - 1400.0, 0.0},
                                        {5.0e7 + 1200.0, -3.0e7 + 700.0, 0.0}})
    {
        const double x = (point.x - 5.0e7) / 1000.0;
        const double y = (point.y + 3.0e7) / 1000.0;
        const double expected = 2.0 + 0.8 * x - 0.4 * y
            + 0.3 * x * x - 0.2 * x * y + 0.5 * y * y;
        Check(Close(quadraticModel.Evaluate(point), expected, 1.0e-8, 1.0e-9),
              "centered/scaled quadratic drift reproduction");
    }
}

void TestTranslationRotationAndAnisotropyIdentity()
{
    const auto samples = RandomSamples(22, 19);
    Variogram isotropic = DefaultVariogram();
    for (Structure& structure : isotropic.structures)
    {
        structure.anisotropy.ratioY = 1.0;
        structure.anisotropy.ratioZ = 1.0;
    }
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model original = BuildOrThrow(samples, isotropic, settings);
    const Vec3 query{0.8, -1.3, 0.0};
    const double baseline = original.Evaluate(query);

    std::vector<Sample> translated = samples;
    const Vec3 offset{1.0e7, -2.0e7, 0.0};
    for (Sample& sample : translated) sample.location = sample.location + offset;
    Model translatedModel = BuildOrThrow(translated, isotropic, settings);
    Check(Close(translatedModel.Evaluate(query + offset), baseline,
                1.0e-9, 1.0e-10), "translation invariance");

    const double angle = 0.71;
    const double c = std::cos(angle), s = std::sin(angle);
    auto rotate = [c, s](Vec3 p)
    {
        return Vec3{c * p.x - s * p.y, s * p.x + c * p.y, p.z};
    };
    std::vector<Sample> rotated = samples;
    for (Sample& sample : rotated) sample.location = rotate(sample.location);
    Model rotatedModel = BuildOrThrow(rotated, isotropic, settings);
    Check(Close(rotatedModel.Evaluate(rotate(query)), baseline,
                1.0e-9, 1.0e-10), "isotropic rotation invariance");

    Variogram angled = isotropic;
    angled.structures[0].anisotropy.azimuthDeg = 123.0;
    angled.structures[0].anisotropy.dipDeg = 47.0;
    angled.structures[0].anisotropy.plungeDeg = -31.0;
    angled.structures[1].anisotropy.azimuthDeg = -87.0;
    Model identityAngles = BuildOrThrow(samples, angled, settings);
    Check(identityAngles.Evaluate(query) == baseline,
          "all-one anisotropy ratios must be bit-identical at any angle");

    auto samples3d = RandomSamples(40, 991, true);
    Settings settings3d = settings;
    settings3d.planar = false;
    Variogram iso3d = isotropic;
    for (Structure& structure : iso3d.structures)
    {
        structure.anisotropy.ratioY = 1.0;
        structure.anisotropy.ratioZ = 1.0;
    }
    Model base3d = BuildOrThrow(samples3d, iso3d, settings3d);
    Variogram angled3d = iso3d;
    angled3d.structures[0].anisotropy.azimuthDeg = 23.0;
    angled3d.structures[0].anisotropy.dipDeg = -17.0;
    angled3d.structures[0].anisotropy.plungeDeg = 41.0;
    Model identity3d = BuildOrThrow(samples3d, angled3d, settings3d);
    const Vec3 query3d{0.4, -0.7, 1.1};
    Check(identity3d.Evaluate(query3d) == base3d.Evaluate(query3d),
          "3D all-one anisotropy ratios are bit-identical");
}

void TestLocalGlobalFullNeighbourParity()
{
    const auto samples = RandomSamples(40, 29);
    Settings globalSettings;
    globalSettings.method = Method::Ordinary;
    globalSettings.mergeRadius = 0.0;
    globalSettings.solveMode = SolveMode::ForceGlobal;
    Model global = BuildOrThrow(samples, DefaultVariogram(), globalSettings);

    Settings localSettings = globalSettings;
    localSettings.solveMode = SolveMode::ForceLocal;
    localSettings.maxNeighbours = static_cast<int>(samples.size());
    localSettings.searchRadiusScale = 1000.0;
    Model local = BuildOrThrow(samples, DefaultVariogram(), localSettings);
    for (int i = 0; i < 25; ++i)
    {
        const Vec3 query{-4.0 + 0.31 * i, 3.0 - 0.19 * i, 0.0};
        Check(Close(local.Evaluate(query), global.Evaluate(query),
                    1.0e-10, 1.0e-11),
              "local full-neighbour value must match global");
        double gv = 0.0, gvar = 0.0, lv = 0.0, lvar = 0.0;
        Check(global.EvaluateWithVariance(query, gv, gvar),
              "global variance query");
        Check(local.EvaluateWithVariance(query, lv, lvar),
              "local variance query");
        Check(Close(lv, gv, 1.0e-10, 1.0e-11)
              && Close(lvar, gvar, 1.0e-9, 1.0e-11),
              "local full-neighbour value/variance parity");
    }
}

void TestFastBruteForceLoo()
{
    for (NuggetMode nuggetMode : {NuggetMode::Exact, NuggetMode::Filtered})
    {
        for (Method method : {Method::Simple, Method::Ordinary,
                              Method::UniversalLinear, Method::UniversalQuadratic,
                              Method::ExternalDrift})
        {
            for (int count : {20, 60})
            {
                const auto samples = RandomSamples(count,
                    static_cast<unsigned>(100 + count + static_cast<int>(method) * 1000
                        + static_cast<int>(nuggetMode) * 10000));
                Settings settings;
                settings.method = method;
                settings.knownMean = 0.25;
                settings.mergeRadius = 0.0;
                settings.solveMode = SolveMode::ForceGlobal;
                Model model;
                if (method == Method::ExternalDrift)
                {
                    model.SetExternalDriftSampler([](const Vec3& point, double& out)
                    {
                        out = point.x + 0.25 * point.y;
                        return true;
                    }, 17);
                }
                BuildReport build;
                Check(model.Build(samples, DefaultVariogram(nuggetMode), settings, build),
                      "LOO method build");
                CrossValidationReport fast;
                Check(model.CrossValidate(fast), "cross-validation should succeed");
                Check(fast.usedFastPath, "eligible global model should use fast LOO");
                Check(fast.verifiedAgainstBruteForce,
                      "n=20/60 fast LOO must be verified against brute force");
                CrossValidationReport brute;
                Check(model.CrossValidateBruteForce(brute, 60),
                      "explicit brute-force LOO should succeed");
                Check(fast.estimated.size() == brute.estimated.size(),
                      "LOO output sizes");
                for (std::size_t i = 0; i < fast.estimated.size(); ++i)
                {
                    Check(Close(fast.estimated[i], brute.estimated[i],
                                1.0e-8, 1.0e-10),
                          "fast/brute LOO estimate parity");
                    Check(Close(fast.standardErrors[i], brute.standardErrors[i],
                                1.0e-8, 1.0e-10),
                          "fast/brute LOO standard-error parity");
                }
            }
        }
    }

    auto noisy = RandomSamples(20, 2026);
    for (std::size_t i = 0; i < noisy.size(); ++i)
    {
        noisy[i].measurementVariance = 0.01 + 0.001 * static_cast<double>(i);
    }
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model noisyModel = BuildOrThrow(noisy, DefaultVariogram(), settings);
    CrossValidationReport noisyReport;
    Check(noisyModel.CrossValidate(noisyReport),
          "measurement-variance LOO should use bounded brute reference");
    Check(!noisyReport.usedFastPath,
          "fast LOO must not claim measurement-variance standard errors");

    auto large = RandomSamples(80, 8080);
    Model largeModel = BuildOrThrow(large, DefaultVariogram(), settings);
    CrossValidationReport largeReport;
    Check(largeModel.CrossValidate(largeReport), "large fast LOO should succeed");
    Check(largeReport.usedFastPath && !largeReport.verifiedAgainstBruteForce,
          "large fast LOO must expose lack of per-model brute verification");
}

void TestTransformsAndBiasConsistency()
{
    auto samples = RandomSamples(24, 5150);
    for (Sample& sample : samples) sample.value += 6.0;
    for (Transform transform : {Transform::Logarithmic, Transform::NormalScore})
    {
        Settings settings;
        settings.method = Method::Ordinary;
        settings.transform = transform;
        settings.mergeRadius = 0.0;
        settings.solveMode = SolveMode::ForceGlobal;
        Model model = BuildOrThrow(samples, DefaultVariogram(), settings);
        for (const Sample& sample : model.Samples())
        {
            Check(Close(model.Evaluate(sample.location), sample.value,
                        transform == Transform::NormalScore ? 1.0e-7 : 1.0e-9,
                        transform == Transform::NormalScore ? 1.0e-7 : 1.0e-10),
                  "transformed exact-mode sample round trip");
        }
        CrossValidationReport transformedCv;
        Check(model.CrossValidate(transformedCv),
              "transformed-model brute-force cross-validation");
        Check(!transformedCv.usedFastPath
              && transformedCv.message.find("transformed space") != std::string::npos,
              "transformed cross-validation labels standardized-statistic space");
        for (double standardized : transformedCv.standardizedResiduals)
        {
            Check(std::isfinite(standardized),
                  "transformed cross-validation standardized residual is finite");
        }
    }

    Settings bias;
    bias.method = Method::Ordinary;
    bias.transform = Transform::Logarithmic;
    bias.lognormalBiasCorrection = true;
    bias.mergeRadius = 0.0;
    bias.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, DefaultVariogram(), bias);
    for (int i = 0; i < 20; ++i)
    {
        const Vec3 query{-4.0 + 0.3 * i, 2.0 - 0.11 * i, 0.0};
        double value = 0.0, variance = 0.0;
        Check(model.EvaluateWithVariance(query, value, variance),
              "bias-corrected value/variance query");
        Check(Close(model.Evaluate(query), value, 1.0e-12, 1.0e-12),
              "bias correction must not depend on entry point");
    }
}

void TestFilteredNuggetAndMeasurementVariance()
{
    const auto samples = RandomSamples(20, 404);
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model exact = BuildOrThrow(samples, DefaultVariogram(NuggetMode::Exact), settings);
    Model filtered = BuildOrThrow(samples, DefaultVariogram(NuggetMode::Filtered), settings);
    double filteredDifference = 0.0;
    for (const Sample& sample : samples)
    {
        filteredDifference = std::max(filteredDifference,
            std::abs(filtered.Evaluate(sample.location) - sample.value));
        Check(Close(exact.Evaluate(sample.location), sample.value,
                    1.0e-10, 1.0e-11),
              "exact nugget mode honours samples");
    }
    Check(filteredDifference > 1.0e-6,
          "filtered nugget mode must smooth rather than exact-shortcut samples");

    auto lowNoise = samples;
    auto highNoise = samples;
    lowNoise[0].measurementVariance = 0.01;
    highNoise[0].measurementVariance = 100.0;
    Model low = BuildOrThrow(lowNoise, DefaultVariogram(), settings);
    Model high = BuildOrThrow(highNoise, DefaultVariogram(), settings);
    std::vector<double> lowWeights, highWeights;
    Check(low.ComputeWeights(samples[0].location, lowWeights),
          "low-noise weight solve");
    Check(high.ComputeWeights(samples[0].location, highWeights),
          "high-noise weight solve");
    std::size_t targetIndex = low.Samples().size();
    for (std::size_t i = 0; i < low.Samples().size(); ++i)
    {
        if (low.Samples()[i].originalIndex == samples[0].originalIndex)
        {
            targetIndex = i;
            break;
        }
    }
    Check(targetIndex < lowWeights.size(),
          "measurement-variance target survives deterministic sorting");
    if (targetIndex < lowWeights.size() && targetIndex < highWeights.size())
    {
        Check(std::abs(highWeights[targetIndex]) < std::abs(lowWeights[targetIndex]),
              "increasing measurement variance decreases sample influence");
    }
}

void TestSimpleFarLimitAndVariance()
{
    const auto samples = RandomSamples(16, 1212);
    Variogram variogram;
    variogram.nugget = 0.1;
    variogram.nuggetMode = NuggetMode::Exact;
    variogram.structures.push_back({Shape::Spherical, 2.0, 0.9, 1.5, 1.0, {}});
    Settings settings;
    settings.method = Method::Simple;
    settings.knownMean = 2.75;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, variogram, settings);
    double value = 0.0, variance = 0.0;
    Check(model.EvaluateWithVariance({1.0e6, -1.0e6, 0.0}, value, variance),
          "simple far-field variance query");
    Check(Close(value, settings.knownMean, 1.0e-12, 1.0e-12),
          "simple kriging tends to known mean far from samples");
    Check(Close(variance, 1.0, 1.0e-12, 1.0e-12),
          "simple kriging variance tends to total sill");
}

void TestBoundedDuplicateMerging()
{
    // The chain is laid out along Y with X held constant. MergeDuplicates
    // sorts samples by (x, y, z) and uses an x-window `break` as a pruning
    // optimization while scanning candidates for a given anchor. If the
    // chain were laid out along X instead (0, 0.9, 1.8 on X), that window
    // would itself stop the scan before the complete-link distance check
    // between the anchor and the far point is ever exercised, so a test
    // built that way cannot distinguish correct complete-link merging from
    // a single-link (transitive) regression: both would prune candidate 2
    // via the x-window before evaluating full-cluster distance. Holding X
    // constant removes that pruning path and forces the merge decision for
    // candidate 2 through the actual complete-link distance test against
    // every existing cluster member.
    std::vector<Sample> samples = {
        {{0.0, 0.0, 0.0}, 1.0, 0.0, 0},
        {{0.0, 0.9, 0.0}, 2.0, 0.0, 1},
        {{0.0, 1.8, 0.0}, 3.0, 0.0, 2},
        {{5.0, 2.0, 0.0}, 4.0, 0.0, 3},
        {{-3.0, 1.0, 0.0}, 5.0, 0.0, 4}
    };
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 1.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, DefaultVariogram(), settings);
    const BuildReport report = model.GetReport();
    // Complete-link: {0.0, 0.9} merge (diameter 0.9 <= radius); 1.8 is
    // outside the radius from 0.0, so it must stay a separate point rather
    // than joining the cluster transitively through 0.9. A single-link
    // (transitive) regression would instead merge all three into one
    // cluster of diameter 1.8, producing mergedPointCount == 2 and
    // maxMergedClusterDiameter == 1.8, which the checks below catch.
    Check(report.mergedPointCount == 1,
          "0,0.9,1.8 Y-axis chain must not collapse transitively");
    Check(report.effectiveCount == 4,
          "bounded duplicate merge effective count");
    Check(report.maxMergedClusterDiameter <= settings.mergeRadius + 1.0e-12,
          "merge cluster diameter cap");
    // Exact-value pin on the reported diameter: the only merged cluster is
    // {0.0, 0.9}, whose true diameter is 0.9. This is the field the runtime
    // backstop at the "Duplicate merge cluster diameter exceeded
    // MergeRadius" internal-error check (KrigePortableCore.cpp) compares
    // against MergeRadius. That backstop branch is a defense against a
    // hypothetical future bug in MergeDuplicates rather than a path any
    // correct-complete-link input can reach at realistic coordinate
    // magnitudes: with correct complete-link logic, every reported cluster
    // diameter is a pairwise distance already checked <= MergeRadius before
    // merging, so it cannot exceed MergeRadius by more than sqrt()'s
    // rounding error, which the guard's 1e-12 slack absorbs outside of
    // pathological coordinate magnitudes. The branch is otherwise only
    // reachable by mutating MergeDuplicates itself (verified separately: a
    // single-link mutant trips it). We therefore
    // test the invariant it protects -- that the reported diameter is
    // exactly the true diameter of the merged cluster -- instead of trying
    // to reach the branch through the public API.
    Check(Close(report.maxMergedClusterDiameter, 0.9, 1.0e-9, 1.0e-12),
          "reported max merged cluster diameter matches the true Y-chain cluster diameter");
}

void TestAnchorOnlyMergeMutantCoverage()
{
    // L-shaped 3-point duplicate fixture, radius 1.0: the anchor (0,0) is
    // within radius of both (0.9,0) and (0,0.9) individually, but those two
    // non-anchor points are 0.9*sqrt(2) ~= 1.2728 apart, which exceeds the
    // radius. A correct complete-link merge must therefore NOT collapse all
    // three into one cluster: whichever of {(0.9,0), (0,0.9)} is processed
    // second (per the (x,y,z) sort order MergeDuplicates consumes) will be
    // rejected because its distance to the *other* already-merged member
    // (not just the anchor) exceeds the radius. An anchor-only merging
    // regression (checking each candidate only against the anchor, never
    // against other cluster members already accepted) would instead merge
    // all three, producing a cluster whose true diameter (1.2728) exceeds
    // MergeRadius (1.0) -- which trips the runtime "Duplicate merge cluster
    // diameter exceeded MergeRadius" internal-error guard and makes the
    // build fail outright, which we assert must NOT happen for correct code.
    //
    // Two extra widely separated points keep effectiveCount >= 3 after the
    // merge and, sorted by (x,y,z), sandwich the L on either side without
    // being merge candidates for it themselves.
    std::vector<Sample> samples = {
        {{-3.0, 1.0, 0.0}, 5.0, 0.0, 4},
        {{0.0, 0.0, 0.0}, 1.0, 0.0, 0},
        {{0.0, 0.9, 0.0}, 2.0, 0.0, 1},
        {{0.9, 0.0, 0.0}, 3.0, 0.0, 2},
        {{5.0, 2.0, 0.0}, 4.0, 0.0, 3}
    };
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 1.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, DefaultVariogram(), settings);
    const BuildReport report = model.GetReport();
    // Sorted by (x,y,z): (-3,1), (0,0), (0,0.9), (0.9,0), (5,2). Anchor
    // (0,0) merges with (0,0.9) (distance 0.9 <= radius); the next
    // candidate (0.9,0) is 0.9 from the anchor but 1.2728 from (0,0.9), so
    // complete-link correctly rejects it and it stays its own point.
    Check(report.mergedPointCount == 1,
          "L-shaped fixture: exactly one pair merges, the far corner stays separate");
    Check(report.effectiveCount == 4,
          "L-shaped fixture: five points collapse to four after bounded merging");
    Check(Close(report.maxMergedClusterDiameter, 0.9, 1.0e-9, 1.0e-12),
          "L-shaped fixture: reported merged-cluster diameter is exactly the true "
          "diameter of the {(0,0),(0,0.9)} pair, not the 1.2728 far-pair distance");
    // The runtime diameter guard (KrigePortableCore.cpp's "internal error"
    // check comparing maxMergedClusterDiameter against MergeRadius) must
    // never fire for correct complete-link code: the build above already
    // had to succeed (BuildOrThrow would have thrown otherwise), and the
    // reported diameter must not exceed the radius by more than the guard's
    // slack.
    Check(report.maxMergedClusterDiameter <= settings.mergeRadius + 1.0e-12,
          "L-shaped fixture: merge cluster diameter cap respected, diameter guard does not fire");
}

void TestMaternContinuityAndLimitConvention()
{
    const double ratio = 0.8;
    const double below = EvaluateNormalizedStructure(Shape::Matern, ratio, 9.99, 1.0);
    const double at = EvaluateNormalizedStructure(Shape::Matern, ratio, 10.0, 1.0);
    Check(Close(below, at, 2.0e-3, 1.0e-5),
          "Matern must not have a hard nu=10 discontinuity");
    const double sameRangeGaussian = EvaluateNormalizedStructure(
        Shape::Gaussian, ratio, 1.5, 1.0);
    const double rescaledGaussian = EvaluateNormalizedStructure(
        Shape::Gaussian, ratio / std::sqrt(2.0), 1.5, 1.0);
    Check(std::abs(at - rescaledGaussian) < std::abs(at - sameRangeGaussian),
          "large-nu Matern is closer after sqrt(2) range conversion");
    Check(Close(EvaluateNormalizedStructure(Shape::Matern, ratio, 0.5, 1.0),
                EvaluateNormalizedStructure(Shape::Exponential, ratio, 1.5, 1.0),
                1.0e-13, 1.0e-14),
          "Matern nu=0.5 exact exponential special case");
}

void TestExternalDriftFailureAndRepointing()
{
    const auto samples = RandomSamples(12, 37);
    Settings settings;
    settings.method = Method::ExternalDrift;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model failing;
    failing.SetExternalDriftSampler([](const Vec3& point, double& out)
    {
        if (point.x > 2.0) return false;
        out = point.x + point.y;
        return true;
    });
    BuildReport failed;
    Check(!failing.Build(samples, DefaultVariogram(), settings, failed),
          "external drift domain failure returns build error");
    Check(failed.message.find("original index") != std::string::npos,
          "external drift failure names original sample index");

    Model constant;
    constant.SetExternalDriftSampler([](const Vec3&, double& out)
    {
        out = 4.0;
        return true;
    });
    Check(!constant.Build(samples, DefaultVariogram(), settings, failed),
          "constant external drift is rejected as rank deficient");

    Model model;
    model.SetExternalDriftSampler([](const Vec3& point, double& out)
    {
        out = point.x + 0.2 * point.y;
        return true;
    }, 1);
    BuildReport report;
    Check(model.Build(samples, DefaultVariogram(), settings, report),
          "external drift build");
    model.SetExternalDriftSampler([](const Vec3& point, double& out)
    {
        out = point.y;
        return true;
    }, 2);
    Check(!model.IsValid(),
          "repointing external drift invalidates model instead of asserting");
}

void TestKdTreeAgainstBruteForce()
{
    std::mt19937 rng(123456);
    std::uniform_real_distribution<double> distribution(-100.0, 100.0);
    for (bool planar : {true, false})
    {
        std::vector<Vec3> positions;
        std::vector<int> originalIndices;
        positions.reserve(1004);
        originalIndices.reserve(1004);
        for (int i = 0; i < 1000; ++i)
        {
            positions.push_back({distribution(rng), distribution(rng),
                                 planar ? 0.0 : distribution(rng)});
            originalIndices.push_back(i + 10);
        }
        const Vec3 duplicatePoint{3.25, -4.75, planar ? 0.0 : 8.5};
        positions.push_back(duplicatePoint);
        originalIndices.push_back(7);
        positions.push_back(duplicatePoint);
        originalIndices.push_back(3);
        positions.push_back(duplicatePoint);
        originalIndices.push_back(9);
        positions.push_back(duplicatePoint);
        originalIndices.push_back(3);

        KdTree tree;
        Check(tree.Build(positions, originalIndices, planar),
              "k-d tree build");
        const int exact = tree.FindExact(duplicatePoint);
        Check(exact >= 0 && originalIndices[static_cast<std::size_t>(exact)] == 3,
              "exact k-d lookup prefers the lowest original index");
        Check(exact == 1001,
              "exact k-d lookup deterministically breaks duplicate original-index ties");

        for (int queryIndex = 0; queryIndex < 80; ++queryIndex)
        {
            const Vec3 query{distribution(rng), distribution(rng),
                             planar ? 0.0 : distribution(rng)};
            for (int maximum : {1, 2, 7, 31, 2048})
            {
                for (double radius : {0.0, 18.0, 250.0})
                {
                    std::vector<Neighbour> actual;
                    tree.FindKNearest(query, maximum, radius, actual);
                    std::vector<Neighbour> expected;
                    const double radiusSquared = radius > 0.0
                        ? radius * radius
                        : std::numeric_limits<double>::infinity();
                    for (int i = 0; i < static_cast<int>(positions.size()); ++i)
                    {
                        const Vec3 difference = positions[static_cast<std::size_t>(i)] - query;
                        const double distanceSquared = difference.x * difference.x
                            + difference.y * difference.y
                            + (planar ? 0.0 : difference.z * difference.z);
                        if (distanceSquared <= radiusSquared)
                        {
                            expected.push_back({i, originalIndices[static_cast<std::size_t>(i)],
                                                distanceSquared});
                        }
                    }
                    std::sort(expected.begin(), expected.end(),
                        [](const Neighbour& left, const Neighbour& right)
                        {
                            if (left.distanceSquared != right.distanceSquared)
                            {
                                return left.distanceSquared < right.distanceSquared;
                            }
                            if (left.originalIndex != right.originalIndex)
                            {
                                return left.originalIndex < right.originalIndex;
                            }
                            return left.sampleIndex < right.sampleIndex;
                        });
                    if (static_cast<int>(expected.size()) > maximum)
                    {
                        expected.resize(static_cast<std::size_t>(maximum));
                    }
                    Check(actual.size() == expected.size(),
                          "k-d nearest count matches brute force");
                    for (std::size_t i = 0;
                         i < std::min(actual.size(), expected.size()); ++i)
                    {
                        Check(actual[i].sampleIndex == expected[i].sampleIndex
                              && actual[i].originalIndex == expected[i].originalIndex
                              && actual[i].distanceSquared == expected[i].distanceSquared,
                              "k-d nearest ordering matches brute force");
                    }
                }
            }

            const double radius = 22.0;
            std::vector<Neighbour> actualRadius;
            tree.FindRadius(query, radius, actualRadius);
            std::vector<Neighbour> expectedRadius;
            for (int i = 0; i < static_cast<int>(positions.size()); ++i)
            {
                const Vec3 difference = positions[static_cast<std::size_t>(i)] - query;
                const double distanceSquared = difference.x * difference.x
                    + difference.y * difference.y
                    + (planar ? 0.0 : difference.z * difference.z);
                if (distanceSquared <= radius * radius)
                {
                    expectedRadius.push_back({i, originalIndices[static_cast<std::size_t>(i)],
                                              distanceSquared});
                }
            }
            auto order = [](const Neighbour& left, const Neighbour& right)
            {
                if (left.distanceSquared != right.distanceSquared)
                {
                    return left.distanceSquared < right.distanceSquared;
                }
                if (left.originalIndex != right.originalIndex)
                {
                    return left.originalIndex < right.originalIndex;
                }
                return left.sampleIndex < right.sampleIndex;
            };
            std::sort(expectedRadius.begin(), expectedRadius.end(), order);
            Check(actualRadius.size() == expectedRadius.size(),
                  "k-d radius count matches brute force");
            for (std::size_t i = 0;
                 i < std::min(actualRadius.size(), expectedRadius.size()); ++i)
            {
                Check(actualRadius[i].sampleIndex == expectedRadius[i].sampleIndex
                      && actualRadius[i].distanceSquared == expectedRadius[i].distanceSquared,
                      "k-d radius ordering matches brute force");
            }
        }
    }
}

void TestDeterminismAndPermutationInvariance()
{
    const auto samples = RandomSamples(45, 9182);
    Settings settings;
    settings.method = Method::UniversalLinear;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model first = BuildOrThrow(samples, DefaultVariogram(), settings);
    Model second = BuildOrThrow(samples, DefaultVariogram(), settings);
    std::vector<double> firstDual, secondDual;
    Check(first.CopyGlobalDualWeights(firstDual)
          && second.CopyGlobalDualWeights(secondDual),
          "global dual weights are available");
    Check(firstDual.size() == secondDual.size()
          && std::memcmp(firstDual.data(), secondDual.data(),
                         firstDual.size() * sizeof(double)) == 0,
          "identical builds produce byte-identical dual weights");

    auto shuffled = samples;
    std::mt19937 rng(77);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    Model permuted = BuildOrThrow(shuffled, DefaultVariogram(), settings);
    std::vector<double> permutedDual;
    Check(permuted.CopyGlobalDualWeights(permutedDual),
          "permuted model dual weights are available");
    Check(firstDual.size() == permutedDual.size()
          && std::memcmp(firstDual.data(), permutedDual.data(),
                         firstDual.size() * sizeof(double)) == 0,
          "sample permutation does not change sorted dual weights");
    for (int i = 0; i < 100; ++i)
    {
        const Vec3 point{-7.0 + 0.13 * i, 4.0 - 0.07 * i, 0.0};
        Check(first.Evaluate(point) == permuted.Evaluate(point),
              "sample permutation leaves evaluations bit-identical");
    }
}

void TestVarianceAndNestedAdditivity()
{
    const auto samples = RandomSamples(28, 777);
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, DefaultVariogram(), settings);
    std::mt19937 rng(8181);
    std::uniform_real_distribution<double> distribution(-12.0, 12.0);
    for (int i = 0; i < 10000; ++i)
    {
        double value = 0.0, variance = 0.0;
        Check(model.EvaluateWithVariance({distribution(rng), distribution(rng), 0.0},
                                         value, variance),
              "variance query succeeds");
        Check(std::isfinite(value) && std::isfinite(variance) && variance >= 0.0,
              "kriging variance remains finite and nonnegative");
    }

    Variogram single;
    single.nugget = 0.04;
    single.nuggetMode = NuggetMode::Exact;
    single.structures.push_back({Shape::Exponential, 6.5, 1.0, 1.5, 1.0,
                                 {17.0, 0.0, 0.0, 0.7, 1.0}});
    Variogram nested = single;
    nested.structures.clear();
    nested.structures.push_back({Shape::Exponential, 6.5, 0.5, 1.5, 1.0,
                                 {17.0, 0.0, 0.0, 0.7, 1.0}});
    nested.structures.push_back(nested.structures[0]);
    Model one = BuildOrThrow(samples, single, settings);
    Model two = BuildOrThrow(samples, nested, settings);
    for (int i = 0; i < 100; ++i)
    {
        const Vec3 point{-8.0 + 0.17 * i, 5.0 - 0.11 * i, 0.0};
        double oneValue = 0.0, oneVariance = 0.0;
        double twoValue = 0.0, twoVariance = 0.0;
        Check(one.EvaluateWithVariance(point, oneValue, oneVariance)
              && two.EvaluateWithVariance(point, twoValue, twoVariance),
              "nested additivity variance queries");
        Check(Close(oneValue, twoValue, 1.0e-12, 1.0e-12)
              && Close(oneVariance, twoVariance, 1.0e-12, 1.0e-12),
              "identical nested structures add exactly");
    }
}

void TestBuildGuardsAndTransactionalFailure()
{
    const auto samples = RandomSamples(20, 88);
    Settings ordinary;
    ordinary.method = Method::Ordinary;
    ordinary.mergeRadius = 0.0;
    ordinary.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, DefaultVariogram(), ordinary);
    BuildReport report;
    Check(!model.Build(std::vector<Sample>{samples[0], samples[1]},
                       DefaultVariogram(), ordinary, report),
          "fewer-than-three build is rejected");
    Check(!model.IsValid() && std::isnan(model.Evaluate({0.0, 0.0, 0.0})),
          "failed rebuild transaction invalidates stale model state");

    Settings quadratic = ordinary;
    quadratic.method = Method::UniversalQuadratic;
    auto tooFewForQuadratic = RandomSamples(17, 909);
    Check(!model.Build(tooFewForQuadratic, DefaultVariogram(), quadratic, report),
          "fewer than 3p samples rejects quadratic drift");

    Variogram power;
    power.nugget = 0.0;
    power.structures.push_back({Shape::Power, 2.0, 1.0, 1.5, 1.2, {}});
    Settings simple = ordinary;
    simple.method = Method::Simple;
    Check(!model.Build(samples, power, simple, report),
          "simple kriging rejects unbounded power variogram");

    Variogram tooMany = DefaultVariogram();
    tooMany.structures.push_back(tooMany.structures.front());
    tooMany.structures.push_back(tooMany.structures.front());
    Check(!model.Build(samples, tooMany, ordinary, report),
          "more than three nested structures are rejected");

    Variogram invalidRatio = DefaultVariogram();
    invalidRatio.structures.front().anisotropy.ratioY = 0.0;
    Check(!model.Build(samples, invalidRatio, ordinary, report),
          "invalid anisotropy ratio is rejected");

    auto nonFinite = samples;
    nonFinite.front().value = std::numeric_limits<double>::quiet_NaN();
    Check(!model.Build(nonFinite, DefaultVariogram(), ordinary, report),
          "non-finite sample is rejected");

    auto positive = samples;
    for (Sample& sample : positive) sample.value += 10.0;
    Settings logarithmic = ordinary;
    logarithmic.method = Method::Simple;
    logarithmic.transform = Transform::Logarithmic;
    logarithmic.knownMean = -100.0;
    Check(!model.Build(positive, DefaultVariogram(), logarithmic, report),
          "simple known mean outside logarithmic domain is rejected");
}

void TestBruteForceLooCap()
{
    const auto samples = RandomSamples(61, 6001);
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceGlobal;
    Model model = BuildOrThrow(samples, DefaultVariogram(), settings);
    CrossValidationReport report;
    Check(!model.CrossValidateBruteForce(report, 60),
          "brute-force LOO refuses models above its synchronous cap");
    Check(report.message.find("capped at 60") != std::string::npos,
          "brute-force LOO cap is explained");
}

void TestPowerAndLocalFallbackReporting()
{
    const auto samples = RandomSamples(35, 2027);
    Variogram power;
    power.nugget = 0.02;
    power.nuggetMode = NuggetMode::Filtered;
    power.structures.push_back({Shape::Power, 5.0, 0.8, 1.5, 1.3, {}});
    Settings settings;
    settings.method = Method::Ordinary;
    settings.mergeRadius = 0.0;
    settings.solveMode = SolveMode::ForceLocal;
    settings.maxNeighbours = 16;
    settings.searchRadiusScale = 3.0;
    Model model = BuildOrThrow(samples, power, settings);
    for (int i = 0; i < 50; ++i)
    {
        const double value = model.Evaluate({-5.0 + 0.2 * i, 3.0 - 0.09 * i, 0.0});
        Check(std::isfinite(value), "local power-model evaluation is finite");
    }
    const BuildReport report = model.GetReport();
    Check(report.localIdwFallbacks >= 0 && report.negativeVarianceClamps >= 0,
          "evaluation counters are available through report snapshots");
}

} // namespace

int main()
{
    try
    {
        TestStructureFunctionsAndAs241();
        TestExactInterpolationAndNoShortcut();
        TestPartitionUnityAndConstantField();
        TestDriftReproductionAndLargeCoordinates();
        TestTranslationRotationAndAnisotropyIdentity();
        TestLocalGlobalFullNeighbourParity();
        TestFastBruteForceLoo();
        TestTransformsAndBiasConsistency();
        TestFilteredNuggetAndMeasurementVariance();
        TestSimpleFarLimitAndVariance();
        TestBoundedDuplicateMerging();
        TestAnchorOnlyMergeMutantCoverage();
        TestMaternContinuityAndLimitConvention();
        TestExternalDriftFailureAndRepointing();
        TestKdTreeAgainstBruteForce();
        TestDeterminismAndPermutationInvariance();
        TestVarianceAndNestedAdditivity();
        TestBuildGuardsAndTransactionalFailure();
        TestBruteForceLooCap();
        TestPowerAndLocalFallbackReporting();
    }
    catch (const std::exception& exception)
    {
        ++Failures;
        std::cerr << "UNCAUGHT: " << exception.what() << '\n';
    }

    if (Failures != 0)
    {
        std::cerr << Failures << " test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "Portable kriging core property suite: PASS\n";
    return 0;
}
