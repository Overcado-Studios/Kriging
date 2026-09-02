#include "KrigePortableCore.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

#include <cmath>
#include <vector>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FKrigePortableCoreSmokeTest,
    "Kriging.Core.PortableSmoke",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKrigePortableCoreSmokeTest::RunTest(const FString& Parameters)
{
    using namespace kriging::portable;
    (void)Parameters;

    std::vector<Sample> Samples;
    int OriginalIndex = 0;
    for (int Y = -2; Y <= 2; ++Y)
    {
        for (int X = -2; X <= 2; ++X)
        {
            Samples.push_back({{static_cast<double>(X), static_cast<double>(Y), 0.0},
                2.0 + 0.4 * X - 0.2 * Y, 0.0, OriginalIndex++});
        }
    }

    Variogram VariogramValue;
    VariogramValue.nugget = 0.0;
    VariogramValue.nuggetMode = NuggetMode::Exact;
    VariogramValue.structures.push_back(
        {Shape::Spherical, 4.0, 1.0, 1.5, 1.0, {}});

    Settings SettingsValue;
    SettingsValue.method = Method::UniversalLinear;
    SettingsValue.solveMode = SolveMode::ForceGlobal;
    SettingsValue.mergeRadius = 0.0;

    Model ModelValue;
    BuildReport Build;
    TestTrue(TEXT("Universal-linear model builds"),
        ModelValue.Build(Samples, VariogramValue, SettingsValue, Build));
    if (!ModelValue.IsValid())
    {
        AddError(FString::Printf(TEXT("Build failed: %s"),
            UTF8_TO_TCHAR(Build.message.c_str())));
        return false;
    }

    const Vec3 Query{0.35, -1.1, 0.0};
    const double Expected = 2.0 + 0.4 * Query.x - 0.2 * Query.y;
    TestTrue(TEXT("Linear drift is reproduced"),
        std::abs(ModelValue.Evaluate(Query) - Expected) <= 1.0e-9);

    CrossValidationReport CrossValidation;
    TestTrue(TEXT("Cross-validation succeeds"),
        ModelValue.CrossValidate(CrossValidation));
    TestTrue(TEXT("Small-model fast LOO is checked against brute force"),
        CrossValidation.verifiedAgainstBruteForce);
    return true;
}
#endif
