#include "KrigePortableCore.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <vector>

namespace
{
std::atomic<bool> TrackAllocations{false};
std::atomic<std::size_t> AllocationCount{0};

void CountAllocation()
{
    if (TrackAllocations.load(std::memory_order_relaxed))
    {
        AllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
}
}

void* operator new(std::size_t size)
{
    CountAllocation();
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    CountAllocation();
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    CountAllocation();
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    CountAllocation();
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

int main()
{
    using namespace kriging::portable;

    std::mt19937 rng(314159);
    std::uniform_real_distribution<double> distribution(-100.0, 100.0);
    std::vector<Sample> samples;
    samples.reserve(300);
    for (int i = 0; i < 300; ++i)
    {
        const double x = distribution(rng);
        const double y = distribution(rng);
        samples.push_back({{x, y, 0.0},
            2.0 + std::sin(0.03 * x) - 0.4 * std::cos(0.05 * y), 0.0, i});
    }

    Variogram variogram;
    variogram.nugget = 0.03;
    variogram.nuggetMode = NuggetMode::Exact;
    variogram.structures.push_back({Shape::Spherical, 70.0, 0.7, 1.5, 1.0, {}});
    variogram.structures.push_back({Shape::Exponential, 160.0, 0.3, 1.5, 1.0,
                                    {20.0, 0.0, 0.0, 0.7, 1.0}});

    Settings settings;
    settings.method = Method::Ordinary;
    settings.solveMode = SolveMode::ForceGlobal;
    settings.mergeRadius = 0.0;

    Model model;
    BuildReport report;
    if (!model.Build(samples, variogram, settings, report))
    {
        std::cerr << "Build failed: " << report.message << '\n';
        return 1;
    }
    if (report.degraded || model.UsesLocalSolver())
    {
        std::cerr << "Allocation probe did not obtain a non-degraded global model.\n";
        return 1;
    }

    double checksum = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        const Vec3 point{-90.0 + 11.0 * i, 70.0 - 7.0 * i, 0.0};
        double value = 0.0, variance = 0.0;
        if (!model.EvaluateWithVariance(point, value, variance)) return 1;
        checksum += model.Evaluate(point) + value + variance;
    }

    AllocationCount.store(0, std::memory_order_relaxed);
    TrackAllocations.store(true, std::memory_order_release);
    for (int i = 0; i < 1000; ++i)
    {
        const Vec3 point{-120.0 + 0.23 * i, 80.0 - 0.17 * i, 0.0};
        double value = 0.0, variance = 0.0;
        if (!model.EvaluateWithVariance(point, value, variance))
        {
            TrackAllocations.store(false, std::memory_order_release);
            std::cerr << "Evaluation failed during allocation probe.\n";
            return 1;
        }
        checksum += model.Evaluate(point) + value + variance;
    }
    TrackAllocations.store(false, std::memory_order_release);

    const std::size_t allocations = AllocationCount.load(std::memory_order_relaxed);
    if (allocations != 0)
    {
        std::cerr << "Observed " << allocations
                  << " allocation(s) in global single-point query paths.\n";
        return 1;
    }
    if (!std::isfinite(checksum))
    {
        std::cerr << "Non-finite allocation-probe checksum.\n";
        return 1;
    }

    std::cout << "Global single-point query allocation probe: PASS\n";
    return 0;
}
