#!/usr/bin/env python3
"""Thesis figures and table for the WDRC fitting grid (analysis/wdrc_grid.csv,
from score_wdrc_grid.py).

Question: for the same dynamic range at the ear, does class-based processing
ahead of the WDRC deliver a higher SBR than a static background gain with the
same SBR into the WDRC?

Across the 24 fittings (threshold x ratio x speed), the SBR at the ear of each
chain falls along a near-straight line in the output dynamic range of the mix
(99th - 30th percentile of 125 ms levels), one line per WDRC speed. The
"equal-range advantage" of a processed point is its SBR at the ear minus the
static-match line of the same speed, scene and target, evaluated at the
processed point's output range. Only points inside the static match's range of
output ranges are used (no extrapolation).

Writes:
  figures/wdrc_grid_<process>.pdf/.png    SBR at the ear vs output range
  wdrc_grid_table.tex                      equal-range advantage
  wdrc_grid_advantage.csv                  per-point advantage

    /usr/bin/python3 analysis/write_wdrc_grid_outputs.py
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.lines  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = Path(__file__).resolve().parent
FIG_DIR = HERE / "figures"
SCENES = [("construction", "Construction"), ("publictransit", "Public Transit")]
TARGETS = [("HI", "HI setting"), ("NH", "NH setting")]
PROCESSES = [("comp", "Group compression (single-band)"), ("basic", "Sidechain, Basic (summed bus)")]
SPEEDS = [("fast", "fast (80 ms release)", "o"), ("slow", "slow (1000 ms release)", "^")]
# Validated categorical slots 1-2 (dataviz palette, light surface); fill vs
# hollow and solid vs dashed carry the chain too, so it never rests on color.
PROC_COLOR, STATIC_COLOR = "#2a78d6", "#eb6834"
INK, MUTED, GRID = "#1f1f1e", "#5c5b55", "#e4e3de"
NUM = ("threshold_db", "ratio", "sbr_in_db", "sbr_out_db", "sbr_loss_db", "wdrc_mean_gr_db",
       "in_range_db", "out_range_db", "range_reduction_db")


def load():
    with open(HERE / "wdrc_grid.csv") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k in NUM:
            r[k] = float(r[k])
    return rows


def sel(rows, cond, target, scene, speed):
    return [r for r in rows if r["condition"] == cond and r["target"] == target
            and r["scene"] == scene and r["speed"] == speed]


def line(pts):
    x = np.array([r["out_range_db"] for r in pts])
    y = np.array([r["sbr_out_db"] for r in pts])
    b = np.polyfit(x, y, 1)
    r2 = 1 - np.sum((y - np.polyval(b, x)) ** 2) / np.sum((y - y.mean()) ** 2)
    return x, y, b, r2


def advantage(rows):
    out = []
    for proc, _ in PROCESSES:
        for t, _ in TARGETS:
            for s, _ in SCENES:
                for sp, *_ in SPEEDS:
                    xs, _, b, r2 = line(sel(rows, f"static_match_{proc}", t, s, sp))
                    for r in sel(rows, proc, t, s, sp):
                        x = r["out_range_db"]
                        inside = xs.min() <= x <= xs.max()
                        out.append({"process": proc, "target": t, "scene": s, "speed": sp,
                                    "threshold_db": r["threshold_db"], "ratio": r["ratio"],
                                    "out_range_db": x, "sbr_out_db": r["sbr_out_db"],
                                    "static_line_sbr_db": float(np.polyval(b, x)),
                                    "advantage_db": r["sbr_out_db"] - float(np.polyval(b, x)),
                                    "inside_static_range": int(inside), "static_line_r2": r2})
    return out


def style(ax):
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(MUTED)
    ax.tick_params(colors=MUTED, labelsize=7.5)


def figure(rows, adv, proc, title):
    fig, axes = plt.subplots(2, 2, figsize=(6.5, 5.6), squeeze=False)
    for ti, (t, tlabel) in enumerate(TARGETS):
        for si, (s, sname) in enumerate(SCENES):
            ax = axes[ti, si]
            style(ax)
            notes = []
            for sp, _, mk in SPEEDS:
                for cond, color, filled, ls in ((f"static_match_{proc}", STATIC_COLOR, False, "--"),
                                                (proc, PROC_COLOR, True, "-")):
                    x, y, b, _ = line(sel(rows, cond, t, s, sp))
                    ax.scatter(x, y, s=22, marker=mk, linewidths=1.0, zorder=3,
                               facecolors=color if filled else "white", edgecolors=color)
                    xs = np.linspace(x.min(), x.max(), 20)
                    ax.plot(xs, np.polyval(b, xs), color=color, linestyle=ls, linewidth=1.4, zorder=2)
                a = [r["advantage_db"] for r in adv if r["process"] == proc and r["target"] == t
                     and r["scene"] == s and r["speed"] == sp and r["inside_static_range"]]
                notes.append(f"{sp}: {np.mean(a):+.2f} dB (n={len(a)})")
            ax.text(0.98, 0.03, "advantage at equal range\n" + "\n".join(notes), transform=ax.transAxes,
                    va="bottom", ha="right", fontsize=6.8, color=INK, linespacing=1.3,
                    bbox=dict(boxstyle="round,pad=0.25", facecolor="white", edgecolor="none", alpha=0.85))
            if ti == 0:
                ax.set_title(sname, fontsize=8.5, color=INK, pad=4)
            if ti == 1:
                ax.set_xlabel("Output dynamic range of the mix (dB)", fontsize=8, color=INK)
            if si == 0:
                ax.set_ylabel(f"{tlabel}\nSBR at the ear (dB)", fontsize=8, color=INK)
    handles = [matplotlib.lines.Line2D([], [], color=PROC_COLOR, linestyle="-", marker="o",
                                       markerfacecolor=PROC_COLOR, markersize=5, label="Processed"),
               matplotlib.lines.Line2D([], [], color=STATIC_COLOR, linestyle="--", marker="o",
                                       markerfacecolor="white", markersize=5, label="Static match")]
    handles += [matplotlib.lines.Line2D([], [], color=MUTED, linestyle="none", marker=mk,
                                        markerfacecolor="white", markersize=5, label=lab)
                for _, lab, mk in SPEEDS]
    h = fig.get_figheight()
    fig.text(0.01, 1 - 0.18 / h, title, fontsize=9, color=INK, ha="left", va="center")
    fig.legend(handles=handles, loc="upper center", ncol=4, frameon=False, fontsize=7.5,
               bbox_to_anchor=(0.5, 1 - 0.3 / h))
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.55 / h), h_pad=1.2, w_pad=1.0)
    FIG_DIR.mkdir(exist_ok=True)
    out = FIG_DIR / f"wdrc_grid_{proc}.pdf"
    fig.savefig(out)
    fig.savefig(out.with_suffix(".png"), dpi=130)
    plt.close(fig)
    print(f"wrote {out}")


def table(adv):
    def cell(proc, t, s, sp):
        a = [r["advantage_db"] for r in adv if r["process"] == proc and r["target"] == t
             and r["scene"] == s and r["speed"] == sp and r["inside_static_range"]]
        return f"${np.mean(a):+.2f}$ (${min(a):+.2f}$ to ${max(a):+.2f}$; {len(a)})"
    body = []
    for proc, plabel in PROCESSES:
        for t, _ in TARGETS:
            body.append(f"{plabel} & {t} & " + " & ".join(cell(proc, t, s, sp) for s, _ in SCENES
                                                         for sp, *_ in SPEEDS) + r" \\")
        body.append(r"\addlinespace")
    body.pop()
    tex = r"""% Equal-output-range SBR advantage over the static match, WDRC fitting grid.
% Generated by analysis/write_wdrc_grid_outputs.py from analysis/wdrc_grid.csv. Do not edit by hand.
\begin{table}[H]
\centering
\scriptsize
\setstretch{1}  % single-spaced rows in the double-spaced thesis
\caption{\protect\todo{AI draft --- rewrite: SBR at the ear of each process minus that of a static background gain with the same SBR into the WDRC, compared at the same output dynamic range of the mix, across the grid of WDRC fittings (threshold $-60$ to $-30$~dBFS, ratio 1.5:1 to 3.5:1). Within each release speed the static-gain condition's SBR at the ear is a straight-line function of the output range; each processed point is compared with that line at its own output range. Cells give the mean, the range, and the number of fittings inside the static condition's span of output ranges. Positive values mean more SBR at the ear for the same output dynamic range.}}
\label{tab:wdrc-grid}
\resizebox{\linewidth}{!}{%
\begin{tabular}{@{}ll cc cc@{}}
\toprule
 & & \multicolumn{2}{c}{\textbf{Construction}} & \multicolumn{2}{c}{\textbf{Public Transit}} \\
\cmidrule(lr){3-4} \cmidrule(lr){5-6}
\textbf{Process} & \textbf{Target} & \textbf{Fast (dB)} & \textbf{Slow (dB)} & \textbf{Fast (dB)} & \textbf{Slow (dB)} \\
\midrule
""" + "\n".join(body) + r"""
\bottomrule
\end{tabular}%
}
\end{table}
"""
    out = HERE / "wdrc_grid_table.tex"
    out.write_text(tex)
    print(f"wrote {out}")


def main():
    plt.rcParams.update({"font.family": "serif", "font.serif": ["Times New Roman", "STIXGeneral", "DejaVu Serif"],
                         "mathtext.fontset": "stix", "pdf.fonttype": 42})
    rows = load()
    adv = advantage(rows)
    for proc, title in PROCESSES:
        figure(rows, adv, proc, title)
    table(adv)
    with open(HERE / "wdrc_grid_advantage.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(adv[0]))
        w.writeheader()
        for r in adv:
            w.writerow({k: (f"{v:.4f}" if isinstance(v, float) else v) for k, v in r.items()})
    print(f"wrote {HERE / 'wdrc_grid_advantage.csv'}")


if __name__ == "__main__":
    main()
