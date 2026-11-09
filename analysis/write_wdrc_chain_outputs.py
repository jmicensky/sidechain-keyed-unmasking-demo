#!/usr/bin/env python3
"""Thesis figures and tables for the WDRC chain test (wdrc_chains.csv, from
score_wdrc_chains.py): class-based processing ahead of a fixed output WDRC,
against the WDRC alone and against a static background gain giving the same
SBR into the WDRC.

Writes:
  figures/wdrc_chain_sbr.pdf    SBR at the ear minus the matched static gain's, vs SBR into the WDRC
  figures/wdrc_chain_esii.pdf   Extended SII change (vs WDRC alone) vs SBR at the ear
  wdrc_chain_table_fast.tex     per-process results, fast-release fitting
  wdrc_chain_table_slow.tex     the same, slow-release fitting
(.png copies of the figures for quick viewing.)

    /usr/bin/python3 analysis/write_wdrc_chain_outputs.py
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

FITS = [("fast", "Fast fitting (release 80 ms)"), ("slow", "Slow fitting (release 1000 ms)")]
SCENES = [("construction", "Construction"), ("publictransit", "Public Transit")]
TARGETS = [("HI", 5.1), ("NH", 12.2)]
# Process identity: validated categorical slots 1-4 (dataviz palette, light
# surface) plus a marker shape each. Slots 3-4 are under 3:1 contrast on white,
# so the shape and the tables carry identity too.
PROCESSES = [("comp", "Group compression (single-band)", "#2a78d6", "o"),
             ("compmb", "Group compression (multiband)", "#eb6834", "s"),
             ("basic", "Sidechain, Basic (summed bus)", "#1baf7a", "^"),
             ("perchannel", "Sidechain, per-channel", "#eda100", "D")]
INK, MUTED, GRID, STATIC = "#1f1f1e", "#5c5b55", "#e4e3de", "#8a8983"


def load():
    with open(HERE / "wdrc_chains.csv") as f:
        return list(csv.DictReader(f))


def pick(rows, fit, scene, cond, target=""):
    return next(r for r in rows if r["fit"] == fit and r["scene"] == scene
                and r["condition"] == cond and r["target"] == target)


def style(ax):
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(MUTED)
    ax.tick_params(colors=MUTED, labelsize=7.5)


def static_points(rows, fit, scene, x, y):
    """Every static-gain condition through this fitting (target and matched), plus WDRC alone."""
    pts = [r for r in rows if r["fit"] == fit and r["scene"] == scene
           and (r["condition"] == "static" or r["condition"].startswith("static_match_")
                or r["condition"] == "wdrc_only")]
    pts.sort(key=lambda r: float(r[x]))
    return [float(r[x]) for r in pts], [float(r[y]) for r in pts]


def figure(rows, x, y, xlabel, ylabel, name, identity=False):
    fig, axes = plt.subplots(2, 2, figsize=(6.5, 5.6), squeeze=False)
    for i, (fit, fname) in enumerate(FITS):
        for j, (scene, sname) in enumerate(SCENES):
            ax = axes[i, j]
            style(ax)
            sx, sy = static_points(rows, fit, scene, x, y)
            if identity:
                lo, hi = min(sx + sy) - 1, max(sx + sy) + 1
                ax.plot([lo, hi], [lo, hi], color=GRID, linewidth=1.2, linestyle=(0, (4, 3)), zorder=1)
                ax.text(hi, hi, "no change\nthrough WDRC", fontsize=6.3, color=MUTED, ha="right", va="top")
            ax.plot(sx, sy, color=STATIC, linewidth=1.6, zorder=2, solid_capstyle="round")
            w = pick(rows, fit, scene, "wdrc_only")
            ax.scatter(float(w[x]), float(w[y]), marker="X", s=55, color=INK, zorder=4,
                       edgecolors="white", linewidths=0.8)
            for proc, _, color, marker in PROCESSES:
                for t, _ in TARGETS:
                    r = pick(rows, fit, scene, proc, t)
                    ax.scatter(float(r[x]), float(r[y]), marker=marker, s=34, color=color, zorder=3,
                               edgecolors="white", linewidths=0.8)
            if i == 0:
                ax.set_title(sname, fontsize=8.5, color=INK, pad=4)
            if j == 0:
                ax.set_ylabel(f"{fname}\n{ylabel}", fontsize=8, color=INK)
            if i == 1:
                ax.set_xlabel(xlabel, fontsize=8, color=INK)
    handles = [matplotlib.lines.Line2D([], [], linestyle="none", marker="X", markersize=7, color=INK,
                                       markeredgecolor="white", label="WDRC alone (unprocessed mix)"),
               matplotlib.lines.Line2D([], [], color=STATIC, linewidth=1.6, label="Static background gain")]
    handles += [matplotlib.lines.Line2D([], [], linestyle="none", marker=mk, markersize=6, color=col, label=lab)
                for _, lab, col, mk in PROCESSES]
    fig.legend(handles=handles, loc="upper center", ncol=3, frameon=False, fontsize=7.3,
               bbox_to_anchor=(0.5, 1.0), columnspacing=1.2, handletextpad=0.4)
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.55 / fig.get_figheight()), h_pad=1.2, w_pad=1.0)
    FIG_DIR.mkdir(exist_ok=True)
    out = FIG_DIR / f"{name}.pdf"
    fig.savefig(out)
    fig.savefig(out.with_suffix(".png"), dpi=130)
    plt.close(fig)
    print(f"wrote {out}")


def figure_vs_static(rows, name):
    """SBR at the ear of each process minus its static match (same SBR into the WDRC)."""
    fig, axes = plt.subplots(2, 2, figsize=(6.5, 5.2), squeeze=False, sharey=True)
    for i, (fit, fname) in enumerate(FITS):
        for j, (scene, sname) in enumerate(SCENES):
            ax = axes[i, j]
            style(ax)
            ax.axhline(0, color=STATIC, linewidth=1.6, zorder=1)
            for proc, _, color, marker in PROCESSES:
                for t, _ in TARGETS:
                    r = pick(rows, fit, scene, proc, t)
                    m = pick(rows, fit, scene, f"static_match_{proc}", t)
                    ax.scatter(float(r["sbr_in_db"]), float(r["sbr_out_db"]) - float(m["sbr_out_db"]),
                               marker=marker, s=38, color=color, zorder=3, edgecolors="white", linewidths=0.8)
            if i == 0:
                ax.set_title(sname, fontsize=8.5, color=INK, pad=4)
            if j == 0:
                ax.set_ylabel(f"{fname}\nSBR at the ear minus\nstatic gain (dB)", fontsize=8, color=INK)
            if i == 1:
                ax.set_xlabel("SBR into the WDRC (dB)", fontsize=8, color=INK)
    handles = [matplotlib.lines.Line2D([], [], color=STATIC, linewidth=1.6, label="Static background gain, same SBR into the WDRC")]
    handles += [matplotlib.lines.Line2D([], [], linestyle="none", marker=mk, markersize=6, color=col, label=lab)
                for _, lab, col, mk in PROCESSES]
    fig.legend(handles=handles, loc="upper center", ncol=3, frameon=False, fontsize=7.3,
               bbox_to_anchor=(0.5, 1.0), columnspacing=1.2, handletextpad=0.4)
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.55 / fig.get_figheight()), h_pad=1.2, w_pad=1.0)
    out = FIG_DIR / f"{name}.pdf"
    fig.savefig(out)
    fig.savefig(out.with_suffix(".png"), dpi=130)
    plt.close(fig)
    print(f"wrote {out}")


def signed(v, nd):
    s = f"{v:+.{nd}f}"
    if float(s) == 0:
        s = f"{0:.{nd}f}"
    return f"${s}$"


def table(rows, fit, fname):
    lines = []
    for s, sname in SCENES:
        w = pick(rows, fit, s, "wdrc_only")
        lines.append(r"\multicolumn{11}{@{}l}{\textbf{" + sname + r"}} \\")
        lines.append(" & WDRC alone & " + " & ".join([
            signed(float(w["sbr_in_db"]), 2), signed(float(w["sbr_out_db"]), 2), "--",
            signed(0.0, 3), "--", signed(float(w["wdrc_mean_gr_db"]), 1), "--",
            f"${float(w['dialogue_range_out_db']):.1f}$", "--"]) + r" \\")
        for t, tval in TARGETS:
            for k, (proc, label, *_) in enumerate(PROCESSES):
                r = pick(rows, fit, s, proc, t)
                m = pick(rows, fit, s, f"static_match_{proc}", t)
                d = lambda c: float(r[c]) - float(m[c])
                tcell = f"{t} ${tval:+.1f}$" if k == 0 else ""
                lines.append(f"{tcell} & {label} & " + " & ".join([
                    signed(float(r["sbr_in_db"]), 2), signed(float(r["sbr_out_db"]), 2), signed(d("sbr_out_db"), 2),
                    signed(float(r["esii_vs_wdrc"]), 3), signed(d("esii_vs_wdrc"), 3),
                    signed(float(r["wdrc_mean_gr_db"]), 1), signed(d("wdrc_mean_gr_db"), 1),
                    f"${float(r['dialogue_range_out_db']):.1f}$", signed(d("dialogue_range_out_db"), 1)]) + r" \\")
            lines.append(r"\addlinespace")
        lines.append(r"\midrule")
    lines.pop()
    return (r"""% Generated by analysis/write_wdrc_chain_outputs.py from wdrc_chains.csv. Do not edit by hand.
\begin{table}[H]
\centering
\footnotesize
\setstretch{1}  % single-spaced rows in the double-spaced thesis
\caption{\protect\aicaption{AI draft --- rewrite: Each process at its SBR-matched setting followed by the same output WDRC, """
            + fname.lower().replace("fitting", "fitting:") + r""" threshold $-50$~dBFS, 2.5:1, 6~dB knee, attack 5~ms. ``vs static'' is the process minus a static background gain that gives the same SBR into the WDRC. $\Delta$Ext.\ SII is relative to the WDRC alone. WDRC gain reduction is the mean gain change over the clip (negative is reduction, so a positive ``vs static'' means the WDRC reduced less); Dialogue range is the 99th minus 30th percentile of 125~ms Dialogue levels at the output.}}
\label{tab:wdrc-chain-""" + fit + r"""}
\resizebox{\linewidth}{!}{%
\begin{tabular}{@{}ll rrr rr rr rr@{}}
\toprule
 & & \multicolumn{3}{c}{\textbf{SBR (dB)}} & \multicolumn{2}{c}{\textbf{$\Delta$Ext.\ SII}} & \multicolumn{2}{c}{\textbf{WDRC gain red.\ (dB)}} & \multicolumn{2}{c}{\textbf{Dialogue range (dB)}} \\
\cmidrule(lr){3-5} \cmidrule(lr){6-7} \cmidrule(lr){8-9} \cmidrule(lr){10-11}
\textbf{Target} & \textbf{Condition} & \textbf{Into WDRC} & \textbf{At ear} & \textbf{vs static} & \textbf{Process} & \textbf{vs static} & \textbf{Process} & \textbf{vs static} & \textbf{At ear} & \textbf{vs static} \\
\midrule
""" + "\n".join(lines) + r"""
\bottomrule
\end{tabular}%
}
\end{table}
""")


def main():
    plt.rcParams.update({"font.family": "serif", "font.serif": ["Times New Roman", "STIXGeneral", "DejaVu Serif"],
                         "mathtext.fontset": "stix", "pdf.fonttype": 42})
    rows = load()
    FIG_DIR.mkdir(exist_ok=True)
    figure_vs_static(rows, "wdrc_chain_sbr")
    figure(rows, "sbr_out_db", "esii_vs_wdrc", "SBR at the ear (dB)", "Ext. SII change vs WDRC alone",
           "wdrc_chain_esii")
    for fit, fname in FITS:
        out = HERE / f"wdrc_chain_table_{fit}.tex"
        out.write_text(table(rows, fit, fname))
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
