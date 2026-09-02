#pragma once

#include "KrigePortableCore.h"

// Provisional Unreal-module C++ aliases. The verified implementation lives in
// kriging::portable so the same source can be compiled by the standalone gate
// and by UnrealBuildTool without divergent numerical paths.
namespace Kriging
{
using FKrigeVector = kriging::portable::Vec3;
using EKrigeShape = kriging::portable::Shape;
using EKrigeMethod = kriging::portable::Method;
using EKrigeNuggetMode = kriging::portable::NuggetMode;
using EKrigeTransform = kriging::portable::Transform;
using EKrigeSolveMode = kriging::portable::SolveMode;
using FKrigeAnisotropy = kriging::portable::Anisotropy;
using FKrigeStructure = kriging::portable::Structure;
using FKrigeVariogram = kriging::portable::Variogram;
using FKrigeSample = kriging::portable::Sample;
using FKrigeSolveSettings = kriging::portable::Settings;
using FKrigeBuildReport = kriging::portable::BuildReport;
using FKrigeCrossValidationReport = kriging::portable::CrossValidationReport;
}
