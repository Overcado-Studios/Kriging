#pragma once

#include "KrigePortableCore.h"

namespace Kriging
{
inline double KrigeEvaluateNormalizedStructure(
    kriging::portable::Shape Shape,
    double DistanceOverRange,
    double MaternNu = 1.5,
    double PowerAlpha = 1.0)
{
    return kriging::portable::EvaluateNormalizedStructure(
        Shape, DistanceOverRange, MaternNu, PowerAlpha);
}
}
