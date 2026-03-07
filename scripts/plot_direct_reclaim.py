#!/usr/bin/env python3
"""
Lethe (Rebalancer) vs Lethe (Direct Reclaim) vs Kernel vs VoliMem.
Single log-scale bar plot for A-zipfian work throughput.
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os
from datetime import datetime

# ── Data ──────────────────────────────────────────────────────────────────────

kernel = pd.read_csv("kernel.csv")
volimem = pd.read_csv("volimem.csv")

lethe_rebal = pd.read_csv("lethe.csv")
# Normalize column names to match the others
lethe_rebal = lethe_rebal.rename(columns={"Cache_MB": "Limit_MB"})

lethe_direct = pd.read_csv("lethe-direct.csv")

RATIOS = [100, 75, 50, 25]
WORKLOAD = "A-zipfian"

SYSTEMS = ["kernel", "volimem", "lethe_rebal", "lethe_direct"]
DFS = {
    "kernel": kernel,
    "volimem": volimem,
    "lethe_rebal": lethe_rebal,
    "lethe_direct": lethe_direct,
}

COLORS = {
    "kernel": "#4C72B0",
    "volimem": "#55A868",
    "lethe_rebal": "#DD8452",
    "lethe_direct": "#C44E52",
}

LABELS = {
    "kernel": "Kernel (NVMe-oF/RDMA)",
    "volimem": "VoliMem",
    "lethe_rebal": "My Swapper (With Rebalancer)",
    "lethe_direct": "My Swapper (Direct Reclaim)",
}


def agg(df, workload, ratio, col):
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

# ── Figure ────────────────────────────────────────────────────────────────────

fig, ax = plt.subplots(figsize=(10, 6))
fig.suptitle(f"Work Throughput — {WORKLOAD}", fontsize=14, fontweight="bold", y=1.01)

n_systems = len(SYSTEMS)
width = 0.18
x = np.arange(len(RATIOS))

series = {}
for sys in SYSTEMS:
    series[sys] = build_series(DFS[sys], WORKLOAD, "Work_Throughput")

offsets = {s: (i - (n_systems - 1) / 2) * width for i, s in enumerate(SYSTEMS)}

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

# Speedup annotations: all systems vs Kernel
km, ks = series["kernel"]
vm, vs_ = series["volimem"]
rm, rs = series["lethe_rebal"]
dm, ds = series["lethe_direct"]

for i in range(len(RATIOS)):
    # VoliMem vs Kernel
    v_ratio = vm[i] / km[i] if km[i] > 0 else 0
    v_color = "#2a7f2a" if v_ratio >= 1 else "#c0392b"
    ax.text(
        x[i] + offsets["volimem"],
        (vm[i] + vs_[i]) / 1e3 * 1.15,
        f"{v_ratio:.1f}x",
        ha="center",
        va="bottom",
        fontsize=7.5,
        color=v_color,
        fontweight="bold",
    )

    # Rebalancer vs Kernel
    r_ratio = rm[i] / km[i] if km[i] > 0 else 0
    r_color = "#2a7f2a" if r_ratio >= 1 else "#c0392b"
    ax.text(
        x[i] + offsets["lethe_rebal"],
        (rm[i] + rs[i]) / 1e3 * 1.15,
        f"{r_ratio:.1f}x",
        ha="center",
        va="bottom",
        fontsize=7.5,
        color=r_color,
        fontweight="bold",
    )

    # Direct Reclaim vs Kernel
    d_ratio = dm[i] / km[i] if km[i] > 0 else 0
    d_color = "#2a7f2a" if d_ratio >= 1 else "#c0392b"
    ax.text(
        x[i] + offsets["lethe_direct"],
        (dm[i] + ds[i]) / 1e3 * 1.15,
        f"{d_ratio:.1f}x",
        ha="center",
        va="bottom",
        fontsize=7.5,
        color=d_color,
        fontweight="bold",
    )

ax.set_xticks(x)
ax.set_xticklabels([f"{r}%" for r in RATIOS])
ax.set_xlabel("Local Memory Ratio (% of WSS)")
ax.set_ylabel("Throughput (log scale, K ops/s)")
ax.legend(loc="upper right", fontsize=9)

OUT_DIR = f"plots_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
os.makedirs(OUT_DIR, exist_ok=True)

plt.tight_layout()
plt.savefig(f"{OUT_DIR}/plot_direct_reclaim.pdf", bbox_inches="tight")
plt.savefig(f"{OUT_DIR}/plot_direct_reclaim.png", dpi=150, bbox_inches="tight")
print(f"Saved to {OUT_DIR}/plot_direct_reclaim.pdf / .png")

# ── Summary table ─────────────────────────────────────────────────────────────

print(f"\n── {WORKLOAD} Work Throughput (K ops/s, mean +/- std) ──")
header = (
    f"{'Ratio':>6}  {'Kernel':>16}  {'VoliMem':>16}  "
    f"{'Rebalancer':>16}  {'Direct':>16}  {'R/K':>6}  {'D/K':>6}"
)
print(header)
print("-" * len(header))
for r in RATIOS:
    k_m, k_s = agg(kernel, WORKLOAD, r, "Work_Throughput")
    v_m, v_s = agg(volimem, WORKLOAD, r, "Work_Throughput")
    r_m, r_s = agg(lethe_rebal, WORKLOAD, r, "Work_Throughput")
    d_m, d_s = agg(lethe_direct, WORKLOAD, r, "Work_Throughput")
    rk = r_m / k_m if k_m > 0 else float("nan")
    dk = d_m / k_m if k_m > 0 else float("nan")
    print(
        f"{r:>5}%  {k_m / 1e3:>7.1f}K +/-{k_s / 1e3:>5.1f}K  "
        f"{v_m / 1e3:>7.1f}K +/-{v_s / 1e3:>5.1f}K  "
        f"{r_m / 1e3:>7.1f}K +/-{r_s / 1e3:>5.1f}K  "
        f"{d_m / 1e3:>7.1f}K +/-{d_s / 1e3:>5.1f}K  "
        f"{rk:>5.2f}x  {dk:>5.2f}x"
    )
