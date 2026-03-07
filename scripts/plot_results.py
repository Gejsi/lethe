#!/usr/bin/env python3
"""
Lethe vs VoliMem-only vs Kernel (NVMe-oF) benchmark visualization.
Run locally: python3 plot_results.py
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os
from datetime import datetime

# ── Data ──────────────────────────────────────────────────────────────────────

KERNEL_CSV = "kernel.csv"
VOLIMEM_CSV = "volimem.csv"
LETHE_CSV = "lethe.csv"

OUT_DIR = f"plots_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
os.makedirs(OUT_DIR, exist_ok=True)
print(f"Saving plots to: {OUT_DIR}/")

kernel = pd.read_csv(KERNEL_CSV)
volimem = pd.read_csv(VOLIMEM_CSV)
lethe = pd.read_csv(LETHE_CSV)

RATIOS = [100, 75, 50, 25]
WORKLOADS = ["A-uniform", "A-zipfian"]

SYSTEMS = ["kernel", "volimem", "lethe"]
DFS = {"kernel": kernel, "volimem": volimem, "lethe": lethe}


def agg(df, workload, ratio, col):
    """Return mean ± std for a given workload/ratio."""
    mask = (df["Workload"] == workload) & (df["Ratio"] == ratio)
    vals = df.loc[mask, col]
    return vals.mean(), vals.std()


def build_series(df, workload, col):
    means, stds = [], []
    for r in RATIOS:
        m, s = agg(df, workload, r, col)
        means.append(m)
        stds.append(s)
    return np.array(means), np.array(stds)


# ── Style ─────────────────────────────────────────────────────────────────────

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.size": 11,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "grid.linestyle": "--",
})

COLORS = {
    "kernel": "#4C72B0",
    "volimem": "#55A868",
    "lethe": "#DD8452",
}

LABELS = {
    "kernel": "Kernel (NVMe-oF/RDMA)",
    "volimem": "VoliMem",
    "lethe": "My Swapper",
}

# ── Figure: Work throughput (main comparison) ─────────────────────────────────

fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), sharey=False)
fig.suptitle("Work Throughput", fontsize=14, fontweight="bold", y=1.01)

n_systems = 3
width = 0.25
x = np.arange(len(RATIOS))

for ax, wl in zip(axes, WORKLOADS):
    series = {}
    for sys in SYSTEMS:
        series[sys] = build_series(DFS[sys], wl, "Work_Throughput")

    offsets = {s: (i - 1) * width for i, s in enumerate(SYSTEMS)}

    for sys in SYSTEMS:
        m, s = series[sys]
        ax.bar(
            x + offsets[sys],
            m / 1e3,
            width,
            yerr=s / 1e3,
            label=LABELS[sys],
            color=COLORS[sys],
            capsize=3,
            error_kw={"elinewidth": 1.0},
        )

    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(
        ticker.FuncFormatter(lambda v, _: f"{v:.0f}K" if v >= 1 else f"{v:.1f}K")
    )

    # Speedup annotations: Lethe vs Kernel
    km, ks = series["kernel"]
    lm, ls = series["lethe"]
    vm, vs_ = series["volimem"]
    for i in range(len(RATIOS)):
        all_tops = [km[i] + ks[i], vm[i] + vs_[i], lm[i] + ls[i]]
        top = max(all_tops) / 1e3

        # Lethe vs Kernel
        ratio_val = lm[i] / km[i] if km[i] > 0 else 0
        color = "#2a7f2a" if ratio_val >= 1 else "#c0392b"
        ax.text(
            x[i],
            top * 1.25,
            f"{ratio_val:.2f}×",
            ha="center",
            va="bottom",
            fontsize=8.5,
            color=color,
            fontweight="bold",
        )

    ax.set_title(wl, fontsize=12)
    ax.set_xticks(x)
    ax.set_xticklabels([f"{r}%" for r in RATIOS])
    ax.set_xlabel("Local Memory Ratio (% of WSS)")
    ax.set_ylabel("Throughput (log scale, K ops/s)")
    ax.legend(loc="upper right", fontsize=8)

plt.tight_layout()
plt.savefig(f"{OUT_DIR}/plot_work_throughput.pdf", bbox_inches="tight")
plt.savefig(f"{OUT_DIR}/plot_work_throughput.png", dpi=150, bbox_inches="tight")
print(f"  plot_work_throughput.pdf / .png")

# ── Helper: zoomed bar chart for a subset of ratios ──────────────────────────


def build_series_ratios(df, workload, col, ratios):
    means, stds = [], []
    for r in ratios:
        m, s = agg(df, workload, r, col)
        means.append(m)
        stds.append(s)
    return np.array(means), np.array(stds)


def plot_bar_zoom(ratios, title, filename):
    fig_z, axes_z = plt.subplots(1, 2, figsize=(14, 5.5), sharey=False)
    fig_z.suptitle(title, fontsize=14, fontweight="bold", y=1.01)

    xz = np.arange(len(ratios))

    for ax, wl in zip(axes_z, WORKLOADS):
        series = {}
        for sys in SYSTEMS:
            series[sys] = build_series_ratios(DFS[sys], wl, "Work_Throughput", ratios)

        offsets = {s: (i - 1) * width for i, s in enumerate(SYSTEMS)}

        for sys in SYSTEMS:
            m, s = series[sys]
            ax.bar(
                xz + offsets[sys],
                m / 1e3,
                width,
                yerr=s / 1e3,
                label=LABELS[sys],
                color=COLORS[sys],
                capsize=3,
                error_kw={"elinewidth": 1.0},
            )

        # Speedup annotations: Lethe vs Kernel
        km, ks = series["kernel"]
        lm, ls = series["lethe"]
        vm, vs_ = series["volimem"]
        for i in range(len(ratios)):
            all_tops = [km[i] + ks[i], vm[i] + vs_[i], lm[i] + ls[i]]
            top = max(all_tops) / 1e3

            ratio_val = lm[i] / km[i] if km[i] > 0 else 0
            color = "#2a7f2a" if ratio_val >= 1 else "#c0392b"
            ax.text(
                xz[i],
                top * 1.08,
                f"{ratio_val:.2f}×",
                ha="center",
                va="bottom",
                fontsize=8.5,
                color=color,
                fontweight="bold",
            )

        ax.set_title(wl, fontsize=12)
        ax.set_xticks(xz)
        ax.set_xticklabels([f"{r}%" for r in ratios])
        ax.set_xlabel("Local Memory Ratio (% of WSS)")
        ax.set_ylabel("Throughput (K ops/s)")
        ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:.0f}K"))
        ax.legend(loc="upper right", fontsize=8)

    plt.tight_layout()
    plt.savefig(f"{OUT_DIR}/{filename}.pdf", bbox_inches="tight")
    plt.savefig(f"{OUT_DIR}/{filename}.png", dpi=150, bbox_inches="tight")
    print(f"  {filename}.pdf / .png")


# ── Figure: Bar chart — low pressure (100% and 75%) ─────────────────────────

plot_bar_zoom([100, 75], "Work Throughput — Low Pressure (100%–75%)", "plot_work_high")

# ── Figure: Bar chart — high pressure (50% and 25%) ─────────────────────────

plot_bar_zoom([50, 25], "Work Throughput — High Pressure (50%–25%)", "plot_work_low")

# ── Print summary table ───────────────────────────────────────────────────────

print("\n── Work Throughput Summary (K ops/s, mean ± std) ──")
header = (
    f"{'Workload':<12} {'Ratio':>6}  {'Kernel':>16}  "
    f"{'VoliMem':>16}  {'Lethe':>16}  {'L/K':>7}  {'L/V':>7}"
)
print(header)
print("-" * len(header))
for wl in WORKLOADS:
    for r in RATIOS:
        km, ks = agg(kernel, wl, r, "Work_Throughput")
        vm, vs_ = agg(volimem, wl, r, "Work_Throughput")
        lm, ls = agg(lethe, wl, r, "Work_Throughput")
        lk = lm / km if km > 0 else float("nan")
        lv = lm / vm if vm > 0 else float("nan")
        print(
            f"{wl:<12} {r:>5}%  {km / 1e3:>7.1f}K ±{ks / 1e3:>5.1f}K  "
            f"{vm / 1e3:>7.1f}K ±{vs_ / 1e3:>5.1f}K  "
            f"{lm / 1e3:>7.1f}K ±{ls / 1e3:>5.1f}K  {lk:>6.2f}×  {lv:>6.2f}×"
        )
    print()
