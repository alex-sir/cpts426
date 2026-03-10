#!/usr/bin/env python3
import subprocess
import re
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
import sys
from collections import defaultdict


def run_cache_timing():
    """Run cache_timing once and parse the output."""
    try:
        result = subprocess.run(
            ["sudo", "taskset", "-c", "0", "./cache_timing"],
            capture_output=True,
            text=True,
            timeout=10,
        )

        if result.returncode != 0:
            print(f"Error running cache_timing: {result.stderr}", file=sys.stderr)
            return None

        # Parse output to extract latencies
        latencies = {}
        for line in result.stdout.splitlines():
            # Match lines like "L1D            5"
            match = re.match(r"^(L1D|L2|L3|DRAM)\s+(\d+)", line)
            if match:
                level = match.group(1)
                cycles = int(match.group(2))
                latencies[level] = cycles

        # Ensure we got all four levels
        if len(latencies) == 4:
            return latencies
        else:
            print(f"Warning: incomplete parse, got {latencies}", file=sys.stderr)
            return None

    except subprocess.TimeoutExpired:
        print("Error: cache_timing timed out", file=sys.stderr)
        return None
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return None


def collect_measurements(n_runs):
    """Run cache_timing N times and collect all measurements."""
    measurements = defaultdict(list)

    print(f"Running cache_timing {n_runs} times...")
    for i in range(n_runs):
        if (i + 1) % 5 == 0:
            print(f"  Progress: {i + 1}/{n_runs}")

        result = run_cache_timing()
        if result:
            for level, cycles in result.items():
                measurements[level].append(cycles)
        else:
            print(f"  Run {i + 1} failed, skipping...")

    print(f"Collected {len(measurements['L1D'])} successful measurements.\n")
    return measurements


def generate_histogram_pdf(measurements, output_file="cache_latency_histogram.pdf"):
    """Generate a PDF with histograms for each cache level."""

    levels = ["L1D", "L2", "L3", "DRAM"]
    colors = ["#3498db", "#e74c3c", "#2ecc71", "#f39c12"]

    pdf = matplotlib.backends.backend_pdf.PdfPages(output_file)

    # Page 1: All four histograms in one figure
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle(
        "Cache and DRAM Access Latency Distributions", fontsize=16, fontweight="bold"
    )

    for idx, level in enumerate(levels):
        ax = axes[idx // 2, idx % 2]
        data = measurements[level]

        if not data:
            ax.text(0.5, 0.5, "No data", ha="center", va="center")
            ax.set_title(f"{level} Cache")
            continue

        # Compute statistics
        mean_val = sum(data) / len(data)
        median_val = sorted(data)[len(data) // 2]
        min_val = min(data)
        max_val = max(data)

        # Plot histogram
        n, bins, patches = ax.hist(
            data, bins=30, color=colors[idx], alpha=0.7, edgecolor="black"
        )

        # Add vertical lines for mean and median
        ax.axvline(
            mean_val,
            color="red",
            linestyle="--",
            linewidth=2,
            label=f"Mean: {mean_val:.1f}",
        )
        ax.axvline(
            median_val,
            color="blue",
            linestyle="--",
            linewidth=2,
            label=f"Median: {median_val:.1f}",
        )

        ax.set_xlabel("Latency (cycles)", fontsize=10)
        ax.set_ylabel("Frequency", fontsize=10)
        ax.set_title(
            f"{level} - Range: [{min_val}, {max_val}] cycles",
            fontsize=11,
            fontweight="bold",
        )
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    pdf.savefig(fig)
    plt.close()

    # Page 2: Individual detailed histogram for each level
    for idx, level in enumerate(levels):
        fig, ax = plt.subplots(figsize=(10, 6))
        data = measurements[level]

        if not data:
            ax.text(0.5, 0.5, "No data", ha="center", va="center")
            ax.set_title(f"{level} Cache - No Data")
            pdf.savefig(fig)
            plt.close()
            continue

        mean_val = sum(data) / len(data)
        median_val = sorted(data)[len(data) // 2]
        std_val = (sum((x - mean_val) ** 2 for x in data) / len(data)) ** 0.5

        n, bins, patches = ax.hist(
            data, bins=50, color=colors[idx], alpha=0.7, edgecolor="black"
        )

        ax.axvline(
            mean_val,
            color="red",
            linestyle="--",
            linewidth=2.5,
            label=f"Mean: {mean_val:.2f} cycles",
        )
        ax.axvline(
            median_val,
            color="blue",
            linestyle="--",
            linewidth=2.5,
            label=f"Median: {median_val:.2f} cycles",
        )

        # Add statistics box
        textstr = "\n".join(
            [
                f"Samples: {len(data)}",
                f"Mean: {mean_val:.2f}",
                f"Median: {median_val:.2f}",
                f"Std Dev: {std_val:.2f}",
                f"Min: {min(data)}",
                f"Max: {max(data)}",
            ]
        )
        props = dict(boxstyle="round", facecolor="wheat", alpha=0.5)
        ax.text(
            0.72,
            0.97,
            textstr,
            transform=ax.transAxes,
            fontsize=11,
            verticalalignment="top",
            bbox=props,
        )

        ax.set_xlabel("Latency (cycles)", fontsize=12)
        ax.set_ylabel("Frequency", fontsize=12)
        ax.set_title(
            f"{level} Access Latency Distribution", fontsize=14, fontweight="bold"
        )
        ax.legend(fontsize=11)
        ax.grid(True, alpha=0.3)

        plt.tight_layout()
        pdf.savefig(fig)
        plt.close()

    # Page 3: Box plot comparison
    fig, ax = plt.subplots(figsize=(10, 6))
    data_list = [measurements[level] for level in levels]

    bp = ax.boxplot(
        data_list, labels=levels, patch_artist=True, notch=True, showmeans=True
    )

    for patch, color in zip(bp["boxes"], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.7)

    ax.set_ylabel("Latency (cycles)", fontsize=12)
    ax.set_title("Cache Latency Comparison (Box Plot)", fontsize=14, fontweight="bold")
    ax.grid(True, alpha=0.3, axis="y")

    plt.tight_layout()
    pdf.savefig(fig)
    plt.close()

    pdf.close()
    print(f"Histogram PDF saved to: {output_file}")


def print_summary_statistics(measurements):
    """Print summary statistics to console."""
    print("=" * 60)
    print("Summary Statistics (cycles)")
    print("=" * 60)
    print(f"{'Level':<10} {'Mean':<10} {'Median':<10} {'Min':<10} {'Max':<10}")
    print("-" * 60)

    for level in ["L1D", "L2", "L3", "DRAM"]:
        data = measurements[level]
        if data:
            mean_val = sum(data) / len(data)
            median_val = sorted(data)[len(data) // 2]
            min_val = min(data)
            max_val = max(data)
            print(
                f"{level:<10} {mean_val:<10.2f} {median_val:<10} "
                f"{min_val:<10} {max_val:<10}"
            )
        else:
            print(f"{level:<10} {'N/A':<10} {'N/A':<10} {'N/A':<10} {'N/A':<10}")

    print("=" * 60)


def main():
    if len(sys.argv) != 2:
        print("Usage: python3 histogram_generator.py <N>")
        print("  N = number of times to run cache_timing")
        sys.exit(1)

    try:
        n_runs = int(sys.argv[1])
        if n_runs < 1:
            raise ValueError
    except ValueError:
        print("Error: N must be a positive integer")
        sys.exit(1)

    print(f"CPT_S 426 Lab 7 Task 3: Cache Latency Histogram Generator")
    print(f"Running cache_timing {n_runs} times...\n")

    # Collect measurements
    measurements = collect_measurements(n_runs)

    # Check we got some data
    if not measurements["L1D"]:
        print("Error: No successful measurements collected!")
        sys.exit(1)

    # Print statistics
    print_summary_statistics(measurements)

    # Generate PDF
    print("\nGenerating histogram PDF...")
    generate_histogram_pdf(measurements)

    print("\nDone! Check cache_latency_histogram.pdf for results.")


if __name__ == "__main__":
    main()
