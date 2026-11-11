#!/usr/bin/env python3
"""Thesis figure and tables for the WDRC chain test in the VR coffee shop.

Data: analysis/vr_wdrc_2026-11-09/, copied from the Unity project
(Modified_Johnson_Healy/LL_CoffeeShop_Results/WdrcTest/2026-11-09, run by
NativePlugins/LLDynamics/tools/wdrc_chain_test.py and wdrc_chain_metrics.py on
calibration recording 20261022_164227; its manifest is copied alongside).

Same analysis as write_wdrc_grid_outputs.py for the web scenes, on the mean
over the 12 conversations of each fitting, condition and target (the 'all'
rows written by the Unity tools are not used; the means are recomputed here).

Writes:
  figures/wdrc_grid_vr.pdf/.png   SBR at the ear vs output range, VR grid
  wdrc_grid_vr_table.tex          equal-output-range advantage, VR grid
  wdrc_chains_vr_table.tex        the two fittings, with intelligibility measures
  vr_conversations_table.tex      the 12 conversations and their SBR into the WDRC

    /usr/bin/python3 analysis/write_wdrc_vr_outputs.py
"""
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.lines  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

from write_wdrc_grid_outputs import (INK, MUTED, PROC_COLOR, SPEEDS, STATIC_COLOR,  # noqa: E402
                                     line, style)

HERE = Path(__file__).resolve().parent
DATA = HERE / "vr_wdrc_2026-11-09"
FIG_DIR = HERE / "figures"
TARGETS = [("HI", "HI setting"), ("NH", "NH setting")]
PROCESSES = [("comp", "Group compression"), ("basic", "Sidechain, Basic")]
ZONES = [("Zone1_Downstairs", "Downstairs"), ("Zone2_Upstairs", "Upstairs"), ("Zone3_Downstairs", "Patio")]
NUM = ("sbr_in_db", "sbr_out_db", "sbr_loss_db", "wdrc_mean_gr_db", "in_range_db", "out_range_db",
       "range_reduction_db", "dialogue_range_out_db")


def read(name):
    with open(DATA / name) as f:
        return [r for r in csv.DictReader(f) if r["zone"] != "all"]


def conversation_means(rows, cols):
    """Mean over conversations per (fit, speed, condition, target)."""
    groups = defaultdict(list)
    for r in rows:
        groups[(r["fit"], r["speed"], r["condition"], r["target"])].append(r)
    out = {}
    for k, v in groups.items():
        assert len(v) == 12, (k, len(v))
        out[k] = {c: float(np.mean([float(x[c]) for x in v])) for c in cols}
    return out


def grid_advantage(means):
    fits = sorted({(f, s) for f, s, _, _ in means})
    adv, pts = [], {}
    for proc, _ in PROCESSES:
        for t, _ in TARGETS:
            for sp, *_ in SPEEDS:
                st = [means[(f, s, f"static_match_{proc}", t)] for f, s in fits if s == sp]
                pr = [means[(f, s, proc, t)] for f, s in fits if s == sp]
                pts[proc, t, sp] = (st, pr)
                xs, _, b, r2 = line(st)
                for d in pr:
                    x = d["out_range_db"]
                    adv.append({"process": proc, "target": t, "speed": sp, "out_range_db": x,
                                "advantage_db": d["sbr_out_db"] - float(np.polyval(b, x)),
                                "inside": xs.min() <= x <= xs.max(), "r2": r2})
    return adv, pts


def figure(adv, pts):
    fig, axes = plt.subplots(2, 2, figsize=(6.5, 5.6), squeeze=False)
    for ti, (t, tlabel) in enumerate(TARGETS):
        for pi, (proc, plabel) in enumerate(PROCESSES):
            ax = axes[ti, pi]
            style(ax)
            notes = []
            for sp, _, mk in SPEEDS:
                st, pr = pts[proc, t, sp]
                for data, color, filled, ls in ((st, STATIC_COLOR, False, "--"), (pr, PROC_COLOR, True, "-")):
                    x, y, b, _ = line(data)
                    ax.scatter(x, y, s=22, marker=mk, linewidths=1.0, zorder=3,
                               facecolors=color if filled else "white", edgecolors=color)
                    xs = np.linspace(x.min(), x.max(), 20)
                    ax.plot(xs, np.polyval(b, xs), color=color, linestyle=ls, linewidth=1.4, zorder=2)
                a = [r["advantage_db"] for r in adv if r["process"] == proc and r["target"] == t
                     and r["speed"] == sp and r["inside"]]
                notes.append(f"{sp}: {np.mean(a):+.2f} dB (n={len(a)})" if a else f"{sp}: no overlap")
            ax.text(0.98, 0.03, "advantage at equal range\n" + "\n".join(notes), transform=ax.transAxes,
                    va="bottom", ha="right", fontsize=6.8, color=INK, linespacing=1.3,
                    bbox=dict(boxstyle="round,pad=0.25", facecolor="white", edgecolor="none", alpha=0.85))
            if ti == 0:
                ax.set_title(plabel, fontsize=8.5, color=INK, pad=4)
            if ti == 1:
                ax.set_xlabel("Output dynamic range of the mix (dB)", fontsize=8, color=INK)
            if pi == 0:
                ax.set_ylabel(f"{tlabel}\nSBR at the ear (dB)", fontsize=8, color=INK)
    handles = [matplotlib.lines.Line2D([], [], color=PROC_COLOR, linestyle="-", marker="o",
                                       markerfacecolor=PROC_COLOR, markersize=5, label="Processed"),
               matplotlib.lines.Line2D([], [], color=STATIC_COLOR, linestyle="--", marker="o",
                                       markerfacecolor="white", markersize=5, label="Static match")]
    handles += [matplotlib.lines.Line2D([], [], color=MUTED, linestyle="none", marker=mk,
                                        markerfacecolor="white", markersize=5, label=lab)
                for _, lab, mk in SPEEDS]
    h = fig.get_figheight()
    fig.text(0.01, 1 - 0.18 / h, "VR coffee shop, mean of 12 conversations", fontsize=9, color=INK,
             ha="left", va="center")
    fig.legend(handles=handles, loc="upper center", ncol=4, frameon=False, fontsize=7.5,
               bbox_to_anchor=(0.5, 1 - 0.3 / h))
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.55 / h), h_pad=1.2, w_pad=1.0)
    FIG_DIR.mkdir(exist_ok=True)
    out = FIG_DIR / "wdrc_grid_vr.pdf"
    fig.savefig(out)
    fig.savefig(out.with_suffix(".png"), dpi=130)
    plt.close(fig)
    print(f"wrote {out}")


def grid_table(adv):
    def cell(proc, t, sp):
        a = [r["advantage_db"] for r in adv if r["process"] == proc and r["target"] == t
             and r["speed"] == sp and r["inside"]]
        return f"${np.mean(a):+.2f}$ (${min(a):+.2f}$ to ${max(a):+.2f}$; {len(a)})" if a else "--"
    body = [f"{plabel} & {t} & " + " & ".join(cell(proc, t, sp) for sp, *_ in SPEEDS) + r" \\"
            for proc, plabel in PROCESSES for t, _ in TARGETS]
    body.insert(2, r"\addlinespace")
    tex = r"""% Equal-output-range SBR advantage over the static match, VR WDRC fitting grid.
% Generated by analysis/write_wdrc_vr_outputs.py from analysis/vr_wdrc_2026-11-09/. Do not edit by hand.
\begin{table}[H]
\centering
\footnotesize
\setstretch{1}  % single-spaced rows in the double-spaced thesis
\caption{\protect\aicaption{AI draft --- rewrite: VR coffee shop: SBR at the ear of each process minus that of a static background gain with the same SBR into the WDRC, compared at the same output dynamic range of the mix, across the 24 WDRC fittings. Values are means over the 12 conversations, computed as in Table~\ref{tab:wdrc-grid}. Cells give the mean, the range, and the number of fittings inside the static condition's span of output ranges.}}
\label{tab:wdrc-grid-vr}
\begin{tabular}{@{}ll cc@{}}
\toprule
\textbf{Process} & \textbf{Target} & \textbf{Fast (dB)} & \textbf{Slow (dB)} \\
\midrule
""" + "\n".join(body) + r"""
\bottomrule
\end{tabular}
\end{table}
"""
    (HERE / "wdrc_grid_vr_table.tex").write_text(tex)
    print(f"wrote {HERE / 'wdrc_grid_vr_table.tex'}")


def chains_table():
    cols = NUM + ("esii_vs_wdrc", "estoi_vs_wdrc", "bg_level_change_db_vs_wdrc")
    m = conversation_means(read("wdrc_chains_vr_metrics.csv"), cols)
    order = [("wdrc_only", "", "WDRC alone")]
    for t, _ in TARGETS:
        order += [("comp", t, "Group compression"), ("static_match_comp", t, "\\quad static match"),
                  ("basic", t, "Sidechain, Basic"), ("static_match_basic", t, "\\quad static match"),
                  ("static", t, "Static gain at target")]

    def f(v, nd=2, sign=True):
        return f"${v:+.{nd}f}$" if sign else f"${v:.{nd}f}$"
    body = []
    for speed, slabel in (("fast", "Fast release (80 ms)"), ("slow", "Slow release (1000 ms)")):
        body.append(r"\multicolumn{10}{@{}l}{\textit{" + slabel + r"}} \\")
        for cond, t, label in order:
            d = m[(speed, speed, cond, t)]
            body.append(f"{label} & {t or '--'} & {f(d['sbr_in_db'])} & {f(d['sbr_out_db'])} & "
                        f"{f(d['wdrc_mean_gr_db'], 1, False)} & {f(d['out_range_db'], 1, False)} & "
                        f"{f(d['dialogue_range_out_db'], 1, False)} & {f(d['esii_vs_wdrc'], 3)} & "
                        f"{f(d['estoi_vs_wdrc'], 3)} & {f(d['bg_level_change_db_vs_wdrc'])} \\\\")
            if cond in ("wdrc_only", "static") and t != "NH":
                body.append(r"\addlinespace")
        body.append(r"\midrule")
    body.pop()
    tex = r"""% VR coffee shop: chains through the two WDRC fittings, means over the 12 conversations.
% Generated by analysis/write_wdrc_vr_outputs.py from analysis/vr_wdrc_2026-11-09/. Do not edit by hand.
\begin{table}[H]
\centering
\scriptsize
\setstretch{1}  % single-spaced rows in the double-spaced thesis
\caption{\protect\aicaption{AI draft --- rewrite: VR coffee shop: each chain through the fast and slow WDRC fittings (threshold $-55.5$~dBFS, ratio 2.5:1), means over the 12 conversations. SBR in is the SBR into the WDRC and SBR ear the SBR at its output; WDRC GR is the WDRC's mean gain reduction; Out range is the output dynamic range of the mix and Dlg range the Dialogue's range at the ear. $\Delta$ESII, $\Delta$ESTOI and $\Delta$Bkg are the Extended SII, ESTOI and background level relative to the WDRC alone on the same fitting, after loudness matching.}}
\label{tab:wdrc-chains-vr}
\resizebox{\linewidth}{!}{%
\begin{tabular}{@{}ll rrrrr rrr@{}}
\toprule
\textbf{Chain} & \textbf{Target} & \textbf{SBR in (dB)} & \textbf{SBR ear (dB)} & \textbf{WDRC GR (dB)} & \textbf{Out range (dB)} & \textbf{Dlg range (dB)} & $\boldsymbol{\Delta}$\textbf{ESII} & $\boldsymbol{\Delta}$\textbf{ESTOI} & $\boldsymbol{\Delta}$\textbf{Bkg (dB)} \\
\midrule
""" + "\n".join(body) + r"""
\bottomrule
\end{tabular}%
}
\end{table}
"""
    (HERE / "wdrc_chains_vr_table.tex").write_text(tex)
    print(f"wrote {HERE / 'wdrc_chains_vr_table.tex'}")


def conversations_table():
    rows = [r for r in read("wdrc_chains_vr.csv") if r["fit"] == "fast"]
    with open(DATA / "calibration_manifest_20261022_164227.tsv") as f:
        man = [r for r in csv.DictReader((l for l in f if not l.startswith("#")), delimiter="\t")]
    body = []
    for z, zlabel in ZONES:
        for c in "1234":
            takes = [r for r in man if r["zone"] == z and r["conversation"] == c]
            voice = "Woman" if takes[0]["sentence"].startswith("Woman") else "Man"
            sents = len({r["sentence"] for r in takes})
            get = {(r["condition"], r["target"]): float(r["sbr_in_db"]) for r in rows
                   if r["zone"] == z and r["conversation"] == c}
            cells = [f"${get[('wdrc_only', '')]:+.1f}$"] + [f"${get[(p, t)]:+.1f}$"
                                                             for p in ("basic", "comp") for t in ("HI", "NH")]
            body.append(f"{zlabel if c == '1' else ''} & {c} & {voice} & {sents} & {len(takes)} & "
                        + " & ".join(cells) + r" \\")
        body.append(r"\addlinespace")
    body.pop()
    tex = r"""% VR coffee shop: the 12 conversations of calibration recording 20261022_164227.
% Generated by analysis/write_wdrc_vr_outputs.py from analysis/vr_wdrc_2026-11-09/. Do not edit by hand.
\begin{table}[H]
\centering
\footnotesize
\setstretch{1}  % single-spaced rows in the double-spaced thesis
\caption{\protect\aicaption{AI draft --- rewrite: The 12 conversations of the VR recording used for the WDRC test: talker voice, number of distinct sentences and of recorded takes, and the SBR into the WDRC (J\&H speech-active measure, pooled over the takes) unprocessed and after calibrated Basic ducking and group compression at the HI ($+5.1$~dB) and NH ($+12.2$~dB) targets. Values below a target are conversations where the calibration trim reached its 6~dB limit; the one value above (Basic HI, Patio conversation 3) is a conversation whose unprocessed mix was already above the HI target.}}
\label{tab:vr-conversations}
\begin{tabular}{@{}llccc r rr rr@{}}
\toprule
 & & & & & & \multicolumn{2}{c}{\textbf{Basic}} & \multicolumn{2}{c}{\textbf{Group comp.}} \\
\cmidrule(lr){7-8} \cmidrule(lr){9-10}
\textbf{Zone} & \textbf{Conv.} & \textbf{Voice} & \textbf{Sent.} & \textbf{Takes} & \textbf{Unproc.} & \textbf{HI} & \textbf{NH} & \textbf{HI} & \textbf{NH} \\
\midrule
""" + "\n".join(body) + r"""
\bottomrule
\end{tabular}
\end{table}
"""
    (HERE / "vr_conversations_table.tex").write_text(tex)
    print(f"wrote {HERE / 'vr_conversations_table.tex'}")


def main():
    plt.rcParams.update({"font.family": "serif", "font.serif": ["Times New Roman", "STIXGeneral", "DejaVu Serif"],
                         "mathtext.fontset": "stix", "pdf.fonttype": 42})
    means = conversation_means(read("wdrc_grid_vr.csv"), NUM)
    adv, pts = grid_advantage(means)
    figure(adv, pts)
    grid_table(adv)
    chains_table()
    conversations_table()
    for proc, _ in PROCESSES:
        for t, _ in TARGETS:
            for sp, *_ in SPEEDS:
                a = [r for r in adv if r["process"] == proc and r["target"] == t and r["speed"] == sp]
                ins = [r["advantage_db"] for r in a if r["inside"]]
                print(f"  {proc:5} {t} {sp}: R2 {a[0]['r2']:.3f}  in span {len(ins)}/{len(a)}  "
                      f"positive {sum(v > 0 for v in ins)}  mean {np.mean(ins) if ins else float('nan'):+.2f}")


if __name__ == "__main__":
    main()
