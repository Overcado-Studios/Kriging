// Standalone C++17 probe for the PyKrige cross-validation golden gate.
//
// Follows the wire-protocol style of Tests/Review/reference_probe.cpp: it
// reads one model description per iteration from stdin (tag "M" plus
// structures "T", samples "P", queries "Q") and calls the *shipped public
// API* (kriging::portable::Model::Build / EvaluateWithVariance) exactly the
// way a real integrator would. Unlike reference_probe.cpp, output is JSON
// (one object per model) so validate_goldens.py can compare it directly
// against goldens.json without a bespoke line-oriented parser.
//
// This probe deliberately supports only the subset of the public API that
// generate_goldens.py's scenarios exercise (Ordinary / UniversalLinear,
// single-structure, isotropic variograms, 2D and 3D). It is not a general
// replacement for reference_probe.cpp.

#include "KrigePortableCore.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string JsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}
} // namespace

int main()
{
    using namespace kriging::portable;
    std::cout << std::setprecision(17);

    std::string Tag;
    bool FirstModel = true;
    std::cout << "[\n";
    while (std::cin >> Tag)
    {
        if (Tag != "M")
        {
            std::cerr << "Expected M, received " << Tag << '\n';
            return 2;
        }

        int MethodValue = 0;
        int PlanarValue = 1;
        double KnownMean = 0.0;
        double Nugget = 0.0;
        int NuggetModeValue = 0;
        int StructureCount = 0;
        int SampleCount = 0;
        int QueryCount = 0;
        std::cin >> MethodValue >> PlanarValue >> KnownMean >> Nugget
                 >> NuggetModeValue >> StructureCount >> SampleCount >> QueryCount;

        Variogram VariogramValue;
        VariogramValue.nugget = Nugget;
        VariogramValue.nuggetMode = static_cast<NuggetMode>(NuggetModeValue);
        for (int Index = 0; Index < StructureCount; ++Index)
        {
            std::cin >> Tag;
            if (Tag != "T") return 3;
            int ShapeValue = 0;
            Structure StructureValue;
            std::cin >> ShapeValue
                     >> StructureValue.range
                     >> StructureValue.partialSill
                     >> StructureValue.maternNu
                     >> StructureValue.powerAlpha
                     >> StructureValue.anisotropy.azimuthDeg
                     >> StructureValue.anisotropy.dipDeg
                     >> StructureValue.anisotropy.plungeDeg
                     >> StructureValue.anisotropy.ratioY
                     >> StructureValue.anisotropy.ratioZ;
            StructureValue.shape = static_cast<Shape>(ShapeValue);
            VariogramValue.structures.push_back(StructureValue);
        }

        std::vector<Sample> Samples;
        Samples.reserve(static_cast<std::size_t>(SampleCount));
        for (int Index = 0; Index < SampleCount; ++Index)
        {
            std::cin >> Tag;
            if (Tag != "P") return 4;
            Sample SampleValue;
            std::cin >> SampleValue.location.x
                     >> SampleValue.location.y
                     >> SampleValue.location.z
                     >> SampleValue.value
                     >> SampleValue.measurementVariance
                     >> SampleValue.originalIndex;
            Samples.push_back(SampleValue);
        }

        std::vector<Vec3> Queries;
        Queries.reserve(static_cast<std::size_t>(QueryCount));
        for (int Index = 0; Index < QueryCount; ++Index)
        {
            std::cin >> Tag;
            if (Tag != "Q") return 5;
            Vec3 Query;
            std::cin >> Query.x >> Query.y >> Query.z;
            Queries.push_back(Query);
        }

        Settings SettingsValue;
        SettingsValue.method = static_cast<Method>(MethodValue);
        SettingsValue.planar = PlanarValue != 0;
        SettingsValue.knownMean = KnownMean;
        // Force the exact global dense solve: these scenarios are small and
        // we want a byte-for-byte comparable linear-algebra path, not the
        // neighbourhood/local-solver heuristics used for huge point clouds.
        SettingsValue.solveMode = SolveMode::ForceGlobal;
        // A nonzero mergeRadius (default 1.0) would silently fuse samples
        // that are closer than 1 world unit apart; several scenarios here
        // use coordinates in [-7, 7], so disable merging entirely.
        SettingsValue.mergeRadius = 0.0;

        Model ModelValue;
        BuildReport Build;
        if (!FirstModel) std::cout << ",\n";
        FirstModel = false;
        if (!ModelValue.Build(Samples, VariogramValue, SettingsValue, Build))
        {
            std::cout << "  {\"build_failed\": true, \"message\": \""
                       << JsonEscape(Build.message) << "\"}";
            continue;
        }
        std::cout << "  {\"build_failed\": false, \"ridge\": " << Build.finalRidge
                   << ", \"degraded\": " << (Build.degraded ? "true" : "false")
                   << ", \"results\": [";
        bool FirstResult = true;
        bool QueryFailed = false;
        for (const Vec3& Query : Queries)
        {
            double Value = 0.0;
            double Variance = 0.0;
            if (!FirstResult) std::cout << ", ";
            FirstResult = false;
            if (!ModelValue.EvaluateWithVariance(Query, Value, Variance))
            {
                std::cout << "{\"failed\": true}";
                QueryFailed = true;
                continue;
            }
            std::cout << "{\"value\": " << Value << ", \"variance\": " << Variance << "}";
        }
        std::cout << "]";
        if (QueryFailed) std::cout << ", \"had_query_failure\": true";
        std::cout << "}";
    }
    std::cout << "\n]\n";
    return 0;
}
