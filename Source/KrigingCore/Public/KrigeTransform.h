#pragma once

#include "KrigePortableCore.h"

namespace Kriging
{
inline double KrigeInverseStandardNormalCdf(double Probability)
{
    return kriging::portable::InverseStandardNormalCdf(Probability);
}
}
