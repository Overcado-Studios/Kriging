#include "KrigePortableCore.h"
#include "KrigeBessel.h"

#include <iomanip>
#include <iostream>
#include <string>

int main()
{
    std::cout << std::setprecision(17);
    std::string Kind;
    while (std::cin >> Kind)
    {
        if (Kind == "B")
        {
            double Nu = 0.0;
            double X = 0.0;
            std::cin >> Nu >> X;
            std::cout << Kriging::Detail::ModifiedBesselK(Nu, X) << '\n';
        }
        else if (Kind == "S")
        {
            int ShapeValue = 0;
            double Ratio = 0.0;
            double Nu = 1.5;
            double Alpha = 1.0;
            std::cin >> ShapeValue >> Ratio >> Nu >> Alpha;
            const auto Shape = static_cast<kriging::portable::Shape>(ShapeValue);
            std::cout << kriging::portable::EvaluateNormalizedStructure(
                Shape, Ratio, Nu, Alpha) << '\n';
        }
        else if (Kind == "N")
        {
            double Probability = 0.5;
            std::cin >> Probability;
            std::cout << kriging::portable::InverseStandardNormalCdf(Probability) << '\n';
        }
        else
        {
            std::cerr << "Unknown probe command: " << Kind << '\n';
            return 2;
        }
    }
    return 0;
}
