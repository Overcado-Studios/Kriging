#include "KrigePortableCore.h"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    using namespace kriging::portable;
    std::cout << std::setprecision(17);

    std::string Tag;
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
        SettingsValue.solveMode = SolveMode::ForceGlobal;
        SettingsValue.mergeRadius = 0.0;

        Model ModelValue;
        if (SettingsValue.method == Method::ExternalDrift)
        {
            ModelValue.SetExternalDriftSampler([](const Vec3& Point, double& Out)
            {
                Out = 0.35 * Point.x - 0.21 * Point.y + 0.13 * Point.z;
                return true;
            }, 0x1234u);
        }
        BuildReport Build;
        if (!ModelValue.Build(Samples, VariogramValue, SettingsValue, Build))
        {
            std::cout << "E " << Build.message << '\n';
            continue;
        }
        std::cout << "B " << Build.finalRidge << ' '
                  << (Build.degraded ? 1 : 0) << '\n';
        for (const Vec3& Query : Queries)
        {
            double Value = 0.0;
            double Variance = 0.0;
            if (!ModelValue.EvaluateWithVariance(Query, Value, Variance))
            {
                std::cout << "E query_failed\n";
                break;
            }
            std::cout << "R " << Value << ' ' << Variance << '\n';
        }
    }
    return 0;
}
