#!/usr/bin/env python3
"""
Generate deterministic sample datasets for kriging plugin demos.

All datasets use fixed seeds for reproducibility. Run this script to regenerate
all three CSV files in the same directory as this script.
"""

import numpy as np
import os
import csv


SEED_ORE = 42
SEED_DEATHS = 123
SEED_TEMPERATURE = 456


def generate_ore_body_3d():
    """
    Generate 3D ore body assay data (ore_body_3d.csv).

    Simulates drillhole data: clustered vertical strings of samples from an
    anisotropic ore body with two overlapping grade shells and background noise.

    Columns: X (meters), Y (meters), Z (meters, depth), Grade (g/t)

    Structure:
      - 8 drillholes arranged in a 2x4 grid, ~50m spacing
      - Each drillhole has ~50 samples spaced ~5m vertically
      - Shell 1: high-grade zone 150-250m depth, centered at (500, 500), ~80m radius
      - Shell 2: moderate-grade zone 250-350m depth, centered at (600, 450), ~70m radius
      - Background: low grade, lognormal-ish noise
    """
    rng = np.random.default_rng(SEED_ORE)

    samples = []

    # Drillhole collar locations (X, Y)
    collars = []
    for i in range(2):
        for j in range(4):
            collars.append((200 + i * 50, 300 + j * 50))

    for collar_x, collar_y in collars:
        # ~50 samples per hole, spaced ~5m vertically
        z_values = np.arange(0, 250, 5) + rng.normal(0, 0.5, 50)

        for z in z_values:
            if z < 0 or z > 500:
                continue

            # Add small horizontal jitter (typical drilling noise)
            x = collar_x + rng.normal(0, 2)
            y = collar_y + rng.normal(0, 2)

            # Background grade (lognormal-ish noise)
            grade = rng.lognormal(mean=np.log(0.5), sigma=0.8)

            # Shell 1: high-grade zone (150-250m, centered near 500,500)
            dist_to_shell1 = np.sqrt((x - 500)**2 + (y - 500)**2)
            if 150 <= z <= 250 and dist_to_shell1 < 100:
                shell1_strength = np.exp(-((z - 200)**2) / (50**2)) * np.exp(-(dist_to_shell1**2) / (80**2))
                grade += 8 * shell1_strength

            # Shell 2: moderate-grade zone (250-350m, centered near 600,450)
            dist_to_shell2 = np.sqrt((x - 600)**2 + (y - 450)**2)
            if 250 <= z <= 350 and dist_to_shell2 < 90:
                shell2_strength = np.exp(-((z - 300)**2) / (50**2)) * np.exp(-(dist_to_shell2**2) / (70**2))
                grade += 4 * shell2_strength

            # Add measurement noise
            grade += rng.normal(0, 0.3)
            grade = max(0.01, grade)  # Clip to positive

            samples.append((x, y, z, grade))

    return samples


def generate_playtest_deaths_2d():
    """
    Generate 2D playtest death-event data (playtest_deaths_2d.csv).

    Simulates player death locations on a 2000x2000 game map with a few
    mechanical hotspots (high-difficulty areas) and random noise.

    Columns: X (pixels), Y (pixels), Deaths (count in local 50-pixel window)

    Structure:
      - ~150 event points scattered across 2000x2000 map
      - 3-4 hotspots: high-difficulty zones with elevated death counts
        * Near (400, 400): tight platformer section
        * Near (1400, 600): lava/hazard area
        * Near (800, 1500): ambush encounter
        * Near (1600, 1600): boss arena
      - Background: low, sparse death events
    """
    rng = np.random.default_rng(SEED_DEATHS)

    samples = []

    # Hotspot centers and their intensity
    hotspots = [
        (400, 400, 25),      # platformer
        (1400, 600, 20),     # lava area
        (800, 1500, 18),     # ambush
        (1600, 1600, 22),    # boss
    ]

    # Background deaths: sparse, Poisson-like
    n_background = 80
    bg_x = rng.uniform(0, 2000, n_background)
    bg_y = rng.uniform(0, 2000, n_background)
    for x, y in zip(bg_x, bg_y):
        deaths = rng.poisson(1)
        samples.append((x, y, deaths))

    # Hotspot deaths: clustered around centers
    for hx, hy, intensity in hotspots:
        n_around = rng.poisson(20)
        for _ in range(n_around):
            # Cluster within ~150 pixels of center
            x = hx + rng.normal(0, 60)
            y = hy + rng.normal(0, 60)
            x = np.clip(x, 0, 2000)
            y = np.clip(y, 0, 2000)

            # Deaths inversely weighted by distance to hotspot center
            dist = np.sqrt((x - hx)**2 + (y - hy)**2)
            base_deaths = intensity * np.exp(-(dist**2) / (100**2))
            deaths = max(1, rng.poisson(base_deaths))

            samples.append((x, y, deaths))

    return samples


def generate_temperature_timesteps():
    """
    Generate 4D temperature data with time dimension (temperature_timesteps.csv).

    Simulates 6 hourly sensor readings from ~80 fixed sensor locations,
    with a warm front moving across the domain over time.

    Columns: X (meters), Y (meters), Z (meters, height), Hour (0-5), TempC (temperature)

    Structure:
      - 80 sensor locations on a grid (fixed X,Y,Z across all timesteps)
      - Sensors span a 400x400x100m volume
      - Warm front initially centered at x=-100 moves rightward at ~80m/hour
      - Each hour: temperature at location = base + front influence + diurnal variation + noise
    """
    rng = np.random.default_rng(SEED_TEMPERATURE)

    samples = []

    # Create a grid of sensor locations (X, Y, Z)
    # ~80 sensors total: 10 x 8 x 1 grid (or spread across 2-3 z levels)
    n_x, n_y, n_z = 10, 8, 1
    sensor_xs = np.linspace(0, 400, n_x)
    sensor_ys = np.linspace(0, 400, n_y)
    sensor_zs = np.linspace(0, 100, n_z)

    sensors = []
    for x in sensor_xs:
        for y in sensor_ys:
            for z in sensor_zs:
                sensors.append((x, y, z))

    # 6 hourly timesteps
    for hour in range(6):
        # Warm front position: starts at x=-100, moves +80 m/hour
        front_x = -100 + 80 * hour
        front_width = 150  # smooth width of the front

        for sensor_x, sensor_y, sensor_z in sensors:
            # Base temperature (varies with height)
            base_temp = 15 + (sensor_z / 100) * 5

            # Warm front influence: Gaussian centered at front_x
            dist_to_front = sensor_x - front_x
            front_strength = np.exp(-(dist_to_front**2) / (front_width**2))
            temp_from_front = 12 * front_strength  # warm front adds up to 12°C at center

            # Diurnal cycle: warmer in "day" hours (2-4), cooler at "night"
            diurnal = 3 * np.cos(2 * np.pi * (hour - 2) / 6)

            # Spatial variation with Y (slight temperature gradient)
            y_gradient = (sensor_y / 400) * 2

            # Add noise
            noise = rng.normal(0, 0.5)

            temp = base_temp + temp_from_front + diurnal + y_gradient + noise

            samples.append((sensor_x, sensor_y, sensor_z, hour, temp))

    return samples


def write_csv(filename, data, header):
    """Write list of tuples to CSV file."""
    filepath = os.path.join(os.path.dirname(__file__), filename)
    with open(filepath, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(header)
        for row in data:
            writer.writerow(row)
    print(f"Wrote {len(data)} rows to {filename}")


def analyze_and_print_stats():
    """Analyze the datasets and print statistics."""

    print("\n" + "="*70)
    print("SAMPLE DATASET GENERATION STATISTICS")
    print("="*70)

    # Ore body stats
    ore_data = generate_ore_body_3d()
    ore_grades = np.array([row[3] for row in ore_data])
    print("\nORE_BODY_3D.CSV")
    print(f"  Total samples: {len(ore_data)}")
    print(f"  Grade (g/t) statistics:")
    print(f"    Min:    {ore_grades.min():.3f}")
    print(f"    Mean:   {ore_grades.mean():.3f}")
    print(f"    Median: {np.median(ore_grades):.3f}")
    print(f"    Std:    {ore_grades.std():.3f}")
    print(f"    75th %: {np.percentile(ore_grades, 75):.3f}")
    print(f"    90th %: {np.percentile(ore_grades, 90):.3f}")
    print(f"    Max:    {ore_grades.max():.3f}")
    print(f"  X range: [{min(r[0] for r in ore_data):.1f}, {max(r[0] for r in ore_data):.1f}]")
    print(f"  Y range: [{min(r[1] for r in ore_data):.1f}, {max(r[1] for r in ore_data):.1f}]")
    print(f"  Z range: [{min(r[2] for r in ore_data):.1f}, {max(r[2] for r in ore_data):.1f}]")

    # Deaths stats
    deaths_data = generate_playtest_deaths_2d()
    deaths_counts = np.array([row[2] for row in deaths_data])
    print("\nPLAYTEST_DEATHS_2D.CSV")
    print(f"  Total events: {len(deaths_data)}")
    print(f"  Deaths per event statistics:")
    print(f"    Min:    {deaths_counts.min():.0f}")
    print(f"    Mean:   {deaths_counts.mean():.2f}")
    print(f"    Median: {np.median(deaths_counts):.1f}")
    print(f"    Std:    {deaths_counts.std():.2f}")
    print(f"    75th %: {np.percentile(deaths_counts, 75):.1f}")
    print(f"    90th %: {np.percentile(deaths_counts, 90):.1f}")
    print(f"    Max:    {deaths_counts.max():.0f}")
    print(f"  X range: [0, 2000]")
    print(f"  Y range: [0, 2000]")

    # Temperature stats
    temp_data = generate_temperature_timesteps()
    temps = np.array([row[4] for row in temp_data])
    print("\nTEMPERATURE_TIMESTEPS.CSV")
    print(f"  Total readings: {len(temp_data)}")
    print(f"  Sensor count: {len(set((r[0], r[1], r[2]) for r in temp_data))}")
    print(f"  Timesteps: 6 (hours 0-5)")
    print(f"  Temperature (°C) statistics:")
    print(f"    Min:    {temps.min():.2f}")
    print(f"    Mean:   {temps.mean():.2f}")
    print(f"    Median: {np.median(temps):.2f}")
    print(f"    Std:    {temps.std():.2f}")
    print(f"    75th %: {np.percentile(temps, 75):.2f}")
    print(f"    90th %: {np.percentile(temps, 90):.2f}")
    print(f"    Max:    {temps.max():.2f}")
    print(f"  X range: [0, 400] (meters)")
    print(f"  Y range: [0, 400] (meters)")
    print(f"  Z range: [0, 100] (meters)")

    print("\n" + "="*70)

    return ore_data, deaths_data, temp_data


if __name__ == "__main__":
    ore_data, deaths_data, temp_data = analyze_and_print_stats()

    write_csv("ore_body_3d.csv", ore_data, ["X", "Y", "Z", "Grade"])
    write_csv("playtest_deaths_2d.csv", deaths_data, ["X", "Y", "Deaths"])
    write_csv("temperature_timesteps.csv", temp_data, ["X", "Y", "Z", "Hour", "TempC"])

    print("\nAll datasets generated successfully in: " + os.path.dirname(__file__))
