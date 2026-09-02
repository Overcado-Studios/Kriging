using UnrealBuildTool;

public class KrigingBlueprint : ModuleRules
{
    public KrigingBlueprint(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp17;
        bEnableExceptions = false;
        bUseUnity = false;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "KrigingCore",
            // Engine-shipped module; depending on it is safe even though this
            // plugin's mesh output does not require it (see KrigingLibrary.h,
            // UKrigingLibrary::ExtractIsoSurfaceToProceduralMesh). It is the
            // most common consumer of ExtractIsoSurface's raw arrays, and
            // exposing that convenience directly avoids every Blueprint user
            // reimplementing CreateMeshSection glue.
            "ProceduralMeshComponent"
        });
    }
}
