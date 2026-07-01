#!/usr/bin/env python3
"""Chart generation for the VoxelBench GI benchmark.

Reads results/results.csv (columns: scene,method,mult,frame,psnr,ssim,gpsnr,
flicker,rays,ms,steps) and writes a set of comparison charts plus a
summary.csv into results/charts/.

Event (e.g. destructive edit) happens at frame 75; runs cover frames 0-149.

    converged_pre  = mean over frames 60..74
    converged_post = mean over frames 135..149

Usage:
    python3 tools/charts.py
    python3 tools/charts.py --csv path/to/results.csv --out path/to/charts
"""
import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RESULTS = os.path.join(ROOT, "results")
DEFAULT_CSV = os.path.join(RESULTS, "results.csv")
DEFAULT_OUT = os.path.join(RESULTS, "charts")

EVENT_FRAME = 75
NUM_FRAMES = 150
PRE_RANGE = (60, 74)    # inclusive
POST_RANGE = (135, 149)  # inclusive
LATE_FLICKER_RANGE = (100, 149)  # inclusive

METHOD_COLORS = {
    "PT": "tab:red",
    "PTO": "lightcoral",
    "PTP": "crimson",
    "PTC": "firebrick",
    "PTR": "orangered",
    "PTV": "darkorange",
    "PTS": "goldenrod",
    "PTL": "olive",
    "PTM": "sienna",
    "PTU": "peru",
    "PTA": "chocolate",
    "PTD": "saddlebrown",
    "PTE": "brown",
    "PTB": "darkgoldenrod",
    "PTF": "tan",
    "PTH": "darkkhaki",
    "PTK": "olive",
    "PTN": "yellowgreen",
    "PTW": "darkseagreen",
    "PTJ": "mediumseagreen",
    "PTI": "seagreen",
    "PTQ": "teal",
    "PTY": "cadetblue",
    "PTZ": "steelblue",
    "PTAA": "navy",
    "PTAB": "midnightblue",
    "PTAC": "royalblue",
    "PTAD": "dodgerblue",
    "PTAE": "deepskyblue",
    "PTAF": "cornflowerblue",
    "PTAG": "skyblue",
    "PTAH": "slateblue",
    "PTAI": "mediumslateblue",
    "PTAJ": "darkslateblue",
    "PTAK": "rebeccapurple",
    "PTAL": "purple",
    "PTAM": "darkorchid",
    "PTAN": "mediumorchid",
    "PTAO": "violet",
    "PTAP": "plum",
    "PTAQ": "orchid",
    "PTAR": "hotpink",
    "PTAS": "deeppink",
    "PTAT": "palevioletred",
    "PTAU": "lightpink",
    "PTAV": "mistyrose",
    "PTAW": "rosybrown",
    "PTAX": "indianred",
    "PTAY": "lightcoral",
    "PTAZ": "firebrick",
    "PTBA": "maroon",
    "PTBB": "darkred",
    "PTBC": "brown",
    "PTBD": "sienna",
    "PTBE": "saddlebrown",
    "PTBF": "peru",
    "PTBG": "chocolate",
    "PTG": "darkred",
    "PTX": "salmon",
    "DDGI": "tab:blue",
    "FCGI": "tab:green",
    "FCGIX": "limegreen",
    "FCLT": "darkgreen",
    "GFC": "tab:purple",
    "GFCAO": "tab:orange",
}
DEFAULT_COLOR = "gray"


def method_color(method):
    return METHOD_COLORS.get(method, DEFAULT_COLOR)


def load_results(csv_path):
    """Load results.csv. Returns an empty DataFrame with the expected
    columns if the file is missing or empty."""
    cols = ["scene", "method", "mult", "frame", "psnr", "ssim", "gpsnr",
            "flicker", "rays", "ms", "steps"]
    if not os.path.exists(csv_path):
        return pd.DataFrame(columns=cols)
    try:
        df = pd.read_csv(csv_path)
    except (pd.errors.EmptyDataError, FileNotFoundError):
        return pd.DataFrame(columns=cols)
    if df.empty:
        return pd.DataFrame(columns=cols)
    return df


def _mask_range(frames, lo, hi):
    return (frames >= lo) & (frames <= hi)


def compute_group_stats(group):
    """Given the rows for a single (scene, method, mult) group, return a
    dict of summary statistics."""
    g = group.sort_values("frame")
    frames = g["frame"].to_numpy()
    psnr = g["psnr"].to_numpy()
    ssim = g["ssim"].to_numpy() if "ssim" in g.columns else np.full_like(psnr, np.nan)
    gpsnr = g["gpsnr"].to_numpy() if "gpsnr" in g.columns else np.full_like(psnr, np.nan)
    flicker = g["flicker"].to_numpy() if "flicker" in g.columns else np.full_like(psnr, np.nan)
    ms = g["ms"].to_numpy() if "ms" in g.columns else np.array([])
    rays = g["rays"].to_numpy() if "rays" in g.columns else np.array([])

    pre_mask = _mask_range(frames, *PRE_RANGE)
    post_mask = _mask_range(frames, *POST_RANGE)
    late_mask = _mask_range(frames, *LATE_FLICKER_RANGE)

    def safe_mean(arr, mask=None):
        if mask is not None:
            arr = arr[mask]
        if arr.size == 0:
            return np.nan
        return float(np.nanmean(arr))

    converged_pre_psnr = safe_mean(psnr, pre_mask)
    converged_post_psnr = safe_mean(psnr, post_mask)
    converged_pre_ssim = safe_mean(ssim, pre_mask)
    converged_post_ssim = safe_mean(ssim, post_mask)
    converged_pre_gpsnr = safe_mean(gpsnr, pre_mask)
    converged_post_gpsnr = safe_mean(gpsnr, post_mask)
    mean_ms = safe_mean(ms)
    mean_rays = safe_mean(rays)
    flicker_late = safe_mean(flicker, late_mask)

    # recover_frames: number of frames after EVENT_FRAME until psnr first
    # reaches within 1 dB of converged_post (i.e. psnr >= converged_post - 1)
    recover_frames = np.nan
    if not np.isnan(converged_post_psnr):
        threshold = converged_post_psnr - 1.0
        post_event_mask = frames > EVENT_FRAME
        for f, p in zip(frames[post_event_mask], psnr[post_event_mask]):
            if p >= threshold:
                recover_frames = int(f - EVENT_FRAME)
                break

    return dict(
        converged_pre_psnr=converged_pre_psnr,
        converged_post_psnr=converged_post_psnr,
        converged_pre_ssim=converged_pre_ssim,
        converged_post_ssim=converged_post_ssim,
        converged_pre_gpsnr=converged_pre_gpsnr,
        converged_post_gpsnr=converged_post_gpsnr,
        mean_ms=mean_ms,
        mean_rays=mean_rays,
        recover_frames=recover_frames,
        flicker_late=flicker_late,
    )


def build_summary(df):
    """Return a DataFrame with one row per (scene, method, mult)."""
    cols = ["scene", "method", "mult", "converged_pre_psnr", "converged_post_psnr",
            "converged_pre_ssim", "converged_post_ssim", "converged_pre_gpsnr",
            "converged_post_gpsnr", "mean_ms", "mean_rays", "recover_frames",
            "flicker_late"]
    if df.empty:
        return pd.DataFrame(columns=cols)

    rows = []
    for (scene, method, mult), group in df.groupby(["scene", "method", "mult"], sort=True):
        stats = compute_group_stats(group)
        rows.append(dict(scene=scene, method=method, mult=mult, **stats))

    out = pd.DataFrame(rows, columns=cols)
    out = out.sort_values(["scene", "method", "mult"]).reset_index(drop=True)
    return out


def _annotate_mults(ax, xs, ys, mults):
    for x, y, m in zip(xs, ys, mults):
        ax.annotate(f"{m:g}x", (x, y), textcoords="offset points",
                     xytext=(4, 4), fontsize=7)


def plot_pareto(df, summary, scene, metric, out_path):
    """metric: 'converged_post_psnr'/'converged_pre_psnr' or ssim equivalents."""
    pre_key = f"converged_pre_{metric}"
    post_key = f"converged_post_{metric}"

    sdf = summary[summary["scene"] == scene]
    if sdf.empty:
        return False

    fig, ax = plt.subplots(figsize=(6.4, 4.6))
    plotted = False
    for method, mgroup in sdf.groupby("method"):
        mgroup = mgroup.sort_values("mult")
        xs = mgroup["mean_ms"].to_numpy()
        ys_post = mgroup[post_key].to_numpy()
        ys_pre = mgroup[pre_key].to_numpy()
        mults = mgroup["mult"].to_numpy()

        valid = ~(np.isnan(xs) | np.isnan(ys_post))
        if not valid.any():
            continue

        color = method_color(method)
        ax.plot(xs[valid], ys_post[valid], "o-", color=color, label=method)
        _annotate_mults(ax, xs[valid], ys_post[valid], mults[valid])

        valid_pre = ~(np.isnan(xs) | np.isnan(ys_pre))
        if valid_pre.any():
            ax.plot(xs[valid_pre], ys_pre[valid_pre], "o--", color=color,
                    alpha=0.35, lw=1.2)
        plotted = True

    if not plotted:
        plt.close(fig)
        return False

    ax.set_xscale("log")
    ax.set_xlabel("mean ms/frame (log)")
    metric_label = "PSNR (dB)" if metric == "psnr" else metric.upper()
    ax.set_ylabel(f"converged {metric_label}")
    ax.set_title(f"{scene}: quality vs cost (solid=post-event, faint dashed=pre-event)")
    ax.legend(fontsize=9)
    ax.grid(alpha=0.3, which="both")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    return True


def plot_response(df, scene, out_path):
    """psnr vs frame for mult=1.0, all methods, with vertical line at EVENT_FRAME."""
    sdf = df[(df["scene"] == scene) & (np.isclose(df["mult"], 1.0))]
    if sdf.empty:
        return False

    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    plotted = False
    for method, mgroup in sdf.groupby("method"):
        mgroup = mgroup.sort_values("frame")
        color = method_color(method)
        ax.plot(mgroup["frame"], mgroup["psnr"], color=color, label=method, lw=1.6)
        plotted = True

    if not plotted:
        plt.close(fig)
        return False

    ax.axvline(EVENT_FRAME, color="k", ls="--", lw=1, alpha=0.6)
    ax.set_xlabel("frame")
    ax.set_ylabel("PSNR (dB)")
    ax.set_title(f"{scene}: PSNR over time at 1.0x budget")
    ax.legend(fontsize=9)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    return True


def plot_flicker(df, scene, out_path):
    """Bar chart: mean flicker over frames 100..149 per method at mult=1.0."""
    sdf = df[(df["scene"] == scene) & (np.isclose(df["mult"], 1.0))]
    if sdf.empty:
        return False

    sdf = sdf[_mask_range(sdf["frame"].to_numpy(), *LATE_FLICKER_RANGE)]
    if sdf.empty:
        return False

    means = sdf.groupby("method")["flicker"].mean().dropna()
    if means.empty:
        return False

    methods = list(means.index)
    values = means.to_numpy()
    colors = [method_color(m) for m in methods]

    fig, ax = plt.subplots(figsize=(6.0, 4.2))
    ax.bar(methods, values, color=colors)
    ax.set_ylabel(f"mean flicker (frames {LATE_FLICKER_RANGE[0]}-{LATE_FLICKER_RANGE[1]})")
    ax.set_title(f"{scene}: temporal flicker at 1.0x budget")
    ax.grid(alpha=0.3, axis="y")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    return True


def write_summary_csv(summary, out_path):
    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)
    summary.to_csv(out_path, index=False)


def run(csv_path=DEFAULT_CSV, out_dir=DEFAULT_OUT):
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    df = load_results(csv_path)
    summary = build_summary(df)

    write_summary_csv(summary, os.path.join(out_dir, "summary.csv"))

    written = ["summary.csv"]

    scenes = sorted(df["scene"].dropna().unique()) if not df.empty else []
    for scene in scenes:
        if plot_pareto(df, summary, scene, "psnr", os.path.join(out_dir, f"pareto_psnr_{scene}.png")):
            written.append(f"pareto_psnr_{scene}.png")
        if plot_pareto(df, summary, scene, "ssim", os.path.join(out_dir, f"pareto_ssim_{scene}.png")):
            written.append(f"pareto_ssim_{scene}.png")
        if plot_pareto(df, summary, scene, "gpsnr", os.path.join(out_dir, f"pareto_gpsnr_{scene}.png")):
            written.append(f"pareto_gpsnr_{scene}.png")
        if plot_response(df, scene, os.path.join(out_dir, f"response_{scene}.png")):
            written.append(f"response_{scene}.png")
        if plot_flicker(df, scene, os.path.join(out_dir, f"flicker_{scene}.png")):
            written.append(f"flicker_{scene}.png")

    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", default=DEFAULT_CSV, help="path to results.csv")
    parser.add_argument("--out", default=DEFAULT_OUT, help="output directory for charts")
    args = parser.parse_args(argv)

    written = run(args.csv, args.out)
    print(f"wrote {len(written)} file(s) to {args.out}")
    for w in written:
        print(f"  {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
