#pragma once

#include <cmath>
#include <limits>

// Modified Bessel K for 0 <= order <= 10 and positive double arguments.
//
// The reduction, Temme series, CF2 continued fraction, and forward recurrence
// are adapted from Boost.Math's Boost Software License 1.0 implementation in
// special_functions/detail/bessel_ik.hpp. Internal long-double arithmetic is
// deliberate: it suppresses cancellation around nearly integral orders while
// keeping a portable double API. See Docs/ATTRIBUTION.md.
namespace Kriging::Detail
{
    using BesselReal = long double;
    inline constexpr BesselReal Pi =
        3.141592653589793238462643383279502884L;

    inline BesselReal Gamma1pm1(BesselReal X)
    {
        return std::expm1(std::lgamma(1.0L + X));
    }

    inline BesselReal SinPiOverPiX(BesselReal X)
    {
        if (std::abs(X) < 1.0e-10L)
        {
            const BesselReal Y = Pi * X;
            const BesselReal Y2 = Y * Y;
            return 1.0L - Y2 / 6.0L + Y2 * Y2 / 120.0L;
        }
        return std::sin(Pi * X) / (Pi * X);
    }

    inline BesselReal Sinhc(BesselReal X)
    {
        if (std::abs(X) < 1.0e-10L)
        {
            const BesselReal X2 = X * X;
            return 1.0L + X2 / 6.0L + X2 * X2 / 120.0L;
        }
        return std::sinh(X) / X;
    }

    inline bool TemmeKPair(BesselReal U, BesselReal X,
                           BesselReal& OutKu, BesselReal& OutKu1)
    {
        if (!(X > 0.0L) || X > 2.0L || std::abs(U) > 0.5L)
        {
            return false;
        }

        constexpr BesselReal EulerGamma =
            0.577215664901532860606512090082402431L;
        const BesselReal Epsilon = std::numeric_limits<BesselReal>::epsilon();
        const BesselReal Gp = Gamma1pm1(U);
        const BesselReal Gm = Gamma1pm1(-U);
        const BesselReal A = std::log(0.5L * X);
        const BesselReal B = std::exp(U * A);
        const BesselReal Sigma = -A * U;
        const BesselReal C = std::abs(U) < Epsilon ? 1.0L : SinPiOverPiX(U);
        const BesselReal D = std::abs(Sigma) < Epsilon ? 1.0L : Sinhc(Sigma);
        const BesselReal Gamma1 = std::abs(U) < Epsilon
            ? -EulerGamma
            : (0.5L / U) * (Gp - Gm) * C;
        const BesselReal Gamma2 = 0.5L * (2.0L + Gp + Gm) * C;

        BesselReal P = (Gp + 1.0L) / (2.0L * B);
        BesselReal Q = (1.0L + Gm) * B / 2.0L;
        BesselReal F = (std::cosh(Sigma) * Gamma1 + D * (-A) * Gamma2) / C;
        BesselReal H = P;
        BesselReal Coefficient = 1.0L;
        BesselReal Sum = F;
        BesselReal Sum1 = H;

        bool bConverged = false;
        for (int K = 1; K < 1024; ++K)
        {
            const BesselReal KD = static_cast<BesselReal>(K);
            F = (KD * F + P + Q) / (KD * KD - U * U);
            P /= KD - U;
            Q /= KD + U;
            H = P - KD * F;
            Coefficient *= X * X / (4.0L * KD);
            Sum += Coefficient * F;
            Sum1 += Coefficient * H;
            if (std::abs(Coefficient * F) <= std::abs(Sum) * Epsilon)
            {
                bConverged = true;
                break;
            }
        }

        OutKu = Sum;
        OutKu1 = 2.0L * Sum1 / X;
        return bConverged && std::isfinite(OutKu) && std::isfinite(OutKu1)
            && OutKu > 0.0L && OutKu1 > 0.0L;
    }

    inline bool ContinuedFractionKPair(BesselReal U, BesselReal X,
                                       BesselReal& OutKu, BesselReal& OutKu1)
    {
        if (!(X > 1.0L) || std::abs(U) > 0.5L)
        {
            return false;
        }

        const BesselReal Epsilon = std::numeric_limits<BesselReal>::epsilon();
        BesselReal A = U * U - 0.25L;
        BesselReal B = 2.0L * (X + 1.0L);
        BesselReal D = 1.0L / B;
        BesselReal F = D;
        BesselReal Delta = D;
        BesselReal PreviousQ = 0.0L;
        BesselReal CurrentQ = 1.0L;
        BesselReal Q = -A;
        BesselReal C = -A;
        BesselReal S = 1.0L + Q * Delta;

        bool bConverged = false;
        for (int K = 2; K < 20000; ++K)
        {
            const BesselReal KD = static_cast<BesselReal>(K);
            A -= 2.0L * (KD - 1.0L);
            B += 2.0L;
            const BesselReal Denominator = B + A * D;
            if (Denominator == 0.0L || A == 0.0L)
            {
                return false;
            }
            D = 1.0L / Denominator;
            Delta *= B * D - 1.0L;
            F += Delta;

            const BesselReal NextQ = (PreviousQ - (B - 2.0L) * CurrentQ) / A;
            PreviousQ = CurrentQ;
            CurrentQ = NextQ;
            C *= -A / KD;
            Q += C * NextQ;
            S += Q * Delta;

            if (NextQ != 0.0L && std::abs(NextQ) < Epsilon)
            {
                C *= NextQ;
                PreviousQ /= NextQ;
                CurrentQ /= NextQ;
            }
            if (std::abs(Q * Delta) <= std::abs(S) * Epsilon)
            {
                bConverged = true;
                break;
            }
        }

        if (!bConverged || S == 0.0L)
        {
            return false;
        }
        OutKu = std::sqrt(Pi / (2.0L * X)) * std::exp(-X) / S;
        OutKu1 = OutKu * (0.5L + U + X + (U * U - 0.25L) * F) / X;
        return std::isfinite(OutKu) && std::isfinite(OutKu1)
            && OutKu > 0.0L && OutKu1 > 0.0L;
    }

    inline double ModifiedBesselK(double NuInput, double XInput)
    {
        BesselReal Nu = std::abs(static_cast<BesselReal>(NuInput));
        const BesselReal X = static_cast<BesselReal>(XInput);
        if (!(X > 0.0L))
        {
            return std::numeric_limits<double>::infinity();
        }

        const int IntegerOrder = static_cast<int>(std::floor(Nu + 0.5L));
        const BesselReal ReducedOrder = Nu - static_cast<BesselReal>(IntegerOrder);
        BesselReal Ku = 0.0L;
        BesselReal Ku1 = 0.0L;
        const bool bBaseOk = X <= 2.0L
            ? TemmeKPair(ReducedOrder, X, Ku, Ku1)
            : ContinuedFractionKPair(ReducedOrder, X, Ku, Ku1);
        if (!bBaseOk)
        {
            return std::numeric_limits<double>::quiet_NaN();
        }

        BesselReal Previous = Ku;
        BesselReal Current = Ku1;
        for (int K = 1; K <= IntegerOrder; ++K)
        {
            const BesselReal Order = ReducedOrder + static_cast<BesselReal>(K);
            const BesselReal Next = Previous + (2.0L * Order / X) * Current;
            Previous = Current;
            Current = Next;
        }
        return static_cast<double>(Previous);
    }
}
