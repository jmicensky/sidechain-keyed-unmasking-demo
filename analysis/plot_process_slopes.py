#!/usr/bin/env python3
"""How much each key parameter changes the SBR and the Extended SII, per
process, from the process LHS sweeps (process_lhs_<process>.csv).

For each process and scene, one multiple linear regression of the metric
change on ALL swept parameters at once, so each slope is the change per unit
of that parameter with the other parameters held fixed. Attack, release and
ratio enter as log2 (slope = change per doubling); thresholds, knee and max
reduction enter in dB (slope = change per dB).

Figures show the raw sweep points (which also carry the other parameters'
effects) with a line of the adjusted slope through the scene's mean point.

Writes:
  figures/process_slopes_<process>.pdf   one figure per process
  process_slopes.csv                     every slope, with its 95% CI and the
                                         regression R^2

    /usr/bin/python3 analysis/plot_process_slopes.py
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.lines  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy import stats  # noqa: E402

HERE = Path(__file__).resolve().parent
FIG_DIR = HERE / "figures"

# Scene identity: validated categorical slots 1-2 (dataviz palette, light
# surface), plus a marker shape each so identity never rests on color alone.
SCENES = [("construction", "Construction", "#2a78d6", "o"),
          ("publictransit", "Public Transit", "#eb6834", "^")]
METRICS = [("sbr_gain_db", "SBR change (dB)", "dB"),
           ("esii_gain", "Extended SII change", "")]
LOG2 = ("_ms", "ratio")  # parameters that enter (and are plotted) on a log2 scale

# Per process: panel rows as (row label or None, [(parameter, x label), ...]).
PANELS = {
    "basic": ("Sidechain, Basic (summed bus)", [
        (None, [("threshold_db", "Threshold (dB)"), ("attack_ms", "Attack (ms)"), ("ratio", "Ratio")])]),
    "perchannel": ("Sidechain, per-channel", [
        (None, [("background_threshold_db", "Background threshold (dB)"),
                ("attack_ms", "Attack (ms)"), ("background_ratio", "Background ratio")])]),
    "comp": ("Group compression (single-band)", [
        ("Dialogue compressor", [("dialogue_threshold_db", "Dialogue threshold (dB)"),
                                 ("dialogue_attack_ms", "Dialogue attack (ms)"),
                                 ("dialogue_ratio", "Dialogue ratio")]),
        ("Background compressor", [("background_threshold_db", "Background threshold (dB)"),
                                   ("background_attack_ms", "Attack, non-Dialogue (ms)"),
                                   ("background_ratio", "Background ratio")])]),
    "compmb": ("Group compression (multiband)", [
        ("Dialogue compressor", [("dialogue_threshold_db", "Dialogue threshold (dB)"),
                                 ("dialogue_attack_ms", "Dialogue attack (ms)"),
                                 ("dialogue_ratio", "Dialogue ratio")]),
        ("Background compressor", [("background_threshold_db", "Background threshold (dB)"),
                                   ("background_attack_ms", "Attack, non-Dialogue (ms)"),
                                   ("background_ratio", "Background ratio")])]),
}

INK, MUTED, GRID = "#1f1f1e", "#5c5b55", "#e4e3de"


def is_log(param):
    return param.endswith(LOG2[0]) or LOG2[1] in param


def load(proc):
    with open(HERE / f"process_lhs_{proc}.csv") as f:
        rows = list(csv.DictReader(f))
    cols = list(rows[0])
    params = cols[cols.index("scene") + 1:cols.index("sbr_db")]
    return rows, params


def fit(rows, params, metric):
    """OLS of metric on every parameter (log2 where is_log). Returns {param: (slope, ci95)}, R^2."""
    X = np.array([[np.log2(float(r[p])) if is_log(p) else float(r[p]) for p in params] for r in rows])
    y = np.array([float(r[metric]) for r in rows])
    A = np.column_stack([np.ones(len(y)), X])
    beta, *_ = np.linalg.lstsq(A, y, rcond=None)
    resid = y - A @ beta
    dof = len(y) - A.shape[1]
    sigma2 = resid @ resid / dof
    # Coefficient covariance sigma2 * (A^T A)^-1, via QR (A = QR) for numerical stability.
    r_inv = np.linalg.inv(np.linalg.qr(A, mode="r"))
    se = np.sqrt(sigma2 * np.sum(r_inv ** 2, axis=1))
    t = stats.t.ppf(0.975, dof)
    r2 = 1 - (resid @ resid) / np.sum((y - y.mean()) ** 2)
    return {p: (beta[i + 1], t * se[i + 1]) for i, p in enumerate(params)}, r2


def unit(param, metric_unit):
    per = "doubling" if is_log(param) else "dB"
    return f"{metric_unit + ' ' if metric_unit else ''}per {per}"


def style(ax):
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(MUTED)
    ax.tick_params(colors=MUTED, labelsize=7.5)


def plot_process(proc, slope_rows):
    title, groups = PANELS[proc]
    rows, params = load(proc)
    fits = {}
    for s, *_ in SCENES:
        sr = [r for r in rows if r["scene"] == s]
        for m, *_ in METRICS:
            fits[s, m] = fit(sr, params, m)
            for p in params:
                b, ci = fits[s, m][0][p]
                slope_rows.append({"process": proc, "scene": s, "metric": m, "parameter": p,
                                   "scale": "log2" if is_log(p) else "linear", "slope": b,
                                   "ci95": ci, "r2": fits[s, m][1], "n": len(sr)})

    n_rows = len(groups) * len(METRICS)
    # Capped at 6.4 in tall so a full-width figure plus caption fits a thesis page.
    fig, axes = plt.subplots(n_rows, 3, figsize=(6.5, min(2.05 * n_rows + 0.5, 6.4)), squeeze=False)
    for gi, (glabel, plist) in enumerate(groups):
        for mi, (m, mlabel, munit) in enumerate(METRICS):
            r_i = gi * len(METRICS) + mi
            for c, (p, xlabel) in enumerate(plist):
                ax = axes[r_i, c]
                style(ax)
                notes = []
                for s, sname, color, marker in SCENES:
                    sr = [r for r in rows if r["scene"] == s]
                    x = np.array([float(r[p]) for r in sr])
                    y = np.array([float(r[m]) for r in sr])
                    ax.scatter(x, y, s=10, color=color, marker=marker, alpha=0.55, linewidths=0,
                               label=sname, zorder=2)
                    b, ci = fits[s, m][0][p]
                    xs = np.geomspace(x.min(), x.max(), 50) if is_log(p) else np.linspace(x.min(), x.max(), 50)
                    tx = np.log2 if is_log(p) else (lambda v: v)
                    ys = y.mean() + b * (tx(xs) - tx(x).mean())
                    ax.plot(xs, ys, color=color, linewidth=2, zorder=3, solid_capstyle="round")
                    nd = 4 if m == "esii_gain" else 2
                    notes.append(f"{'C' if s == 'construction' else 'PT'}: {b:+.{nd}f} ± {ci:.{nd}f}")
                if is_log(p):
                    ax.set_xscale("log", base=2)
                    ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
                ax.text(0.02, 0.98, "\n".join(notes), transform=ax.transAxes, va="top", ha="left",
                        fontsize=6.8, color=INK, linespacing=1.3,
                        bbox=dict(boxstyle="round,pad=0.25", facecolor="white", edgecolor="none", alpha=0.85))
                ax.set_title(unit(p, munit), fontsize=7, color=MUTED, loc="right", pad=2)
                if r_i == n_rows - 1 or (len(groups) > 1 and mi == len(METRICS) - 1):
                    ax.set_xlabel(xlabel, fontsize=8, color=INK)
                if c == 0:
                    ylab = mlabel if glabel is None else f"{glabel}\n{mlabel}"
                    ax.set_ylabel(ylab, fontsize=8, color=INK)
    # Legend from proxy markers, so the plotted points keep their own size and alpha.
    handles = [matplotlib.lines.Line2D([], [], linestyle="none", marker=mk, markersize=6, color=col,
                                       label=f"{name} ({'C' if s == 'construction' else 'PT'})")
               for s, name, col, mk in SCENES]
    top = 1 - 0.38 / fig.get_figheight()
    fig.text(0.01, 1 - 0.2 / fig.get_figheight(), title, fontsize=9, color=INK, ha="left", va="center")
    fig.legend(handles=handles, loc="center right", ncol=2, frameon=False, fontsize=8,
               bbox_to_anchor=(0.99, 1 - 0.2 / fig.get_figheight()))
    fig.tight_layout(rect=(0, 0, 1, top), h_pad=1.0, w_pad=0.8)
    FIG_DIR.mkdir(exist_ok=True)
    out = FIG_DIR / f"process_slopes_{proc}.pdf"
    fig.savefig(out)
    fig.savefig(out.with_suffix(".png"), dpi=130)
    plt.close(fig)
    print(f"wrote {out}")


def main():
    plt.rcParams.update({"font.family": "serif", "font.serif": ["Times New Roman", "STIXGeneral", "DejaVu Serif"],
                         "mathtext.fontset": "stix", "pdf.fonttype": 42})
    slope_rows = []
    for proc in ("comp", "compmb", "basic", "perchannel"):
        plot_process(proc, slope_rows)
    with open(HERE / "process_slopes.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(slope_rows[0]))
        w.writeheader()
        for r in slope_rows:
            w.writerow({k: (f"{v:.5f}" if isinstance(v, float) else v) for k, v in r.items()})
    print(f"wrote {HERE / 'process_slopes.csv'}")


if __name__ == "__main__":
    main()
