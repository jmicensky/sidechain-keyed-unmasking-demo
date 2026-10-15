#!/usr/bin/env python3
"""LaTeX report: the 6 best distinct settings per mode and target from the SBR
search, every parameter shown, plus the crossover with the earlier mean-margin-
gain sweeps. Reads the analysis/ CSV/TSV outputs; writes analysis/sbr_matched_settings_report.tex.

"Distinct": the local search clusters around its winner, so the literal top 6
are near-copies. Settings are taken in order of the search objective, skipping
any whose parameters are within an RMS distance of 0.10 (in units of each
parameter's search range) of one already taken.
"""
import csv
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCENES = ["construction", "publictransit", "restaurant"]
SCENE_SHORT = {"construction": "Constr.", "publictransit": "Transit", "restaurant": "Rest."}
TARGETS = {"HI": 5.1, "NH": 12.2}
MODES = [("basic", "Basic unmasking"), ("comp", "Group compression"), ("combined", "Combined")]
MIN_DISTANCE = 0.10
PROC_WEIGHT = 0.02

LABELS = {
    "dialogue_threshold_db": ("Dialogue comp.\\ threshold", "dB"), "dialogue_ratio": ("Dialogue comp.\\ ratio", ":1"),
    "dialogue_attack_ms": ("Dialogue comp.\\ attack", "ms"), "dialogue_release_ms": ("Dialogue comp.\\ release", "ms"),
    "music_threshold_db": ("Music comp.\\ threshold", "dB"), "music_ratio": ("Music comp.\\ ratio", ":1"),
    "background_threshold_db": ("Background comp.\\ threshold", "dB"), "background_ratio": ("Background comp.\\ ratio", ":1"),
    "safety_threshold_db": ("Safety comp.\\ threshold", "dB"), "safety_ratio": ("Safety comp.\\ ratio", ":1"),
    "other_threshold_db": ("Other comp.\\ threshold", "dB"), "other_ratio": ("Other comp.\\ ratio", ":1"),
    "comp_knee_db": ("Comp.\\ knee (all groups)", "dB"),
    "background_attack_ms": ("Comp.\\ attack (4 background groups)", "ms"),
    "background_release_ms": ("Comp.\\ release (4 background groups)", "ms"),
    "duck_threshold_db": ("Duck threshold", "dB"), "duck_ratio": ("Duck ratio", ":1"), "duck_knee_db": ("Duck knee", "dB"),
    "duck_attack_ms": ("Duck attack", "ms"), "duck_release_ms": ("Duck release", "ms"),
    "duck_max_reduction_db": ("Duck max reduction", "dB"),
}


def read_csv(path, delimiter=","):
    with open(path) as f:
        return list(csv.DictReader(f, delimiter=delimiter))


def fmt(name, v):
    if name.endswith("_ratio"):
        return f"{v:.2f}"
    if name.endswith("release_ms"):
        return f"{v:.0f}"
    return f"{v:.1f}"


def pick_distinct(rows, params, target):
    lo = {p: min(float(r[p]) for r in rows) for p in params}
    hi = {p: max(float(r[p]) for r in rows) for p in params}
    err_col = "worst_err_hi_db" if target == "HI" else "worst_err_nh_db"
    def J(r):
        return float(r[err_col]) + PROC_WEIGHT * float(r["processing_db"]) + float(r["penalty"])
    picked = []
    for r in sorted(rows, key=J):
        x = [(float(r[p]) - lo[p]) / max(hi[p] - lo[p], 1e-9) for p in params]
        if all(math.sqrt(sum((a - b) ** 2 for a, b in zip(x, y)) / len(x)) >= MIN_DISTANCE for _, y in picked):
            picked.append((r, x))
        if len(picked) == 6:
            break
    return [r for r, _ in picked], J


def settings_table(mode, mode_label, target):
    rows = read_csv(HERE / f"sbr_matched_{mode}.csv")
    params = [k for k in rows[0] if k in LABELS]
    best, J = pick_distinct(rows, params, target)
    t = TARGETS[target]
    cols = len(best)
    o = []
    o.append(f"\\subsection*{{{mode_label}: {target} target ({t:+.1f}\\,dB)}}")
    o.append("\\begin{table}[H]\\centering\\scriptsize")
    o.append(f"\\begin{{tabular}}{{@{{}}l l {'r' * cols}@{{}}}}")
    o.append("\\toprule")
    from export_sbr_presets import SELECTION, PROVISIONAL
    chosen = SELECTION[(mode, target)]
    def head(i):
        if i + 1 != chosen:
            return f"\\textbf{{\\#{i + 1}}}"
        tag = "Unity, prov." if (mode, target) in PROVISIONAL else "Unity"
        return f"\\textbf{{\\#{i + 1}}} \\textbf{{({tag})}}"
    o.append("\\textbf{Parameter} & & " + " & ".join(head(i) for i in range(cols)) + " \\\\")
    o.append("\\midrule")
    for p in params:
        label, unit = LABELS[p]
        o.append(f"{label} & {unit} & " + " & ".join(fmt(p, float(r[p])) for r in best) + " \\\\")
    if mode in ("basic", "combined"):
        o.append("Key (Dialogue) gain & dB & " + " & ".join("0" for _ in best) + " \\\\")
    if mode in ("comp", "combined"):
        o.append("Makeup gain (all groups) & dB & " + " & ".join("0" for _ in best) + " \\\\")
    o.append("\\midrule")
    for s in SCENES:
        o.append(f"SBR, {SCENE_SHORT[s]} & dB & " + " & ".join(f"{float(r[s + '_sbr_db']):+.2f}" for r in best) + " \\\\")
    err_col = "worst_err_hi_db" if target == "HI" else "worst_err_nh_db"
    o.append("Worst-scene $|$SBR$-$target$|$ & dB & " + " & ".join(f"{float(r[err_col]):.2f}" for r in best) + " \\\\")
    if mode in ("basic", "combined"):
        o.append("Mean duck, avg.\\ of scenes & dB & " + " & ".join(
            f"{sum(float(r[s + '_mean_duck_db']) for s in SCENES) / 3:.1f}" for r in best) + " \\\\")
        o.append("Duck engaged, avg.\\ of scenes & \\% & " + " & ".join(
            f"{100 * sum(float(r[s + '_duck_engaged_frac']) for s in SCENES) / 3:.0f}" for r in best) + " \\\\")
    if mode in ("comp", "combined"):
        o.append("Dialogue comp.\\ GR, avg.\\ & dB & " + " & ".join(
            f"{sum(float(r[s + '_dialogue_gr_db']) for s in SCENES) / 3:.1f}" for r in best) + " \\\\")
        o.append("Background comp.\\ GR, avg.\\ & dB & " + " & ".join(
            f"{sum(float(r[s + '_background_gr_db']) for s in SCENES) / 3:.1f}" for r in best) + " \\\\")
        o.append("Dialogue dyn.\\ range, avg.\\ & dB & " + " & ".join(
            f"{sum(float(r[s + '_dialogue_range_db']) for s in SCENES) / 3:.1f}" for r in best) + " \\\\")
    o.append("Search objective $J$ & & " + " & ".join(f"{J(r):.2f}" for r in best) + " \\\\")
    o.append("\\bottomrule")
    o.append("\\end{tabular}")
    o.append("\\end{table}")
    return "\n".join(o)


def pearson(x, y):
    n = len(x); mx = sum(x) / n; my = sum(y) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x); syy = sum((b - my) ** 2 for b in y)
    return sxy / math.sqrt(sxx * syy)


def ranks(x):
    order = sorted(range(len(x)), key=lambda i: x[i])
    r = [0] * len(x)
    for k, i in enumerate(order):
        r[i] = k
    return r


def crossover():
    rows = read_csv(HERE / "sbr_margin_crossover.tsv", "\t")
    winners = read_csv(HERE / "sbr_margin_winners.tsv", "\t")
    o = []
    o.append("\\section{Crossover with the mean-margin-gain sweeps}")
    o.append("The earlier mean-margin-gain sweeps (Basic ducking, Dialogue key, max reduction fixed at 6\\,dB; 150 / 50 / 100 "
             "points on Construction / Public Transit / Restaurant) were re-measured as SBR. Margins use the band those "
             "sweeps used (Dialogue 400--7000\\,Hz, narrowed 0.85\\,octave each side, 20\\,ms frames, speech-active gate); "
             "recomputing them from the renders reproduces the sweep CSV values exactly.")
    o.append("")
    o.append("\\begin{table}[H]\\centering\\small")
    o.append("\\caption{Margin gain versus SBR within each margin sweep.}")
    o.append("\\begin{tabular}{@{}lrrrrrr@{}}\\toprule")
    o.append("Scene & Points & Pearson $r$ & Spearman $\\rho$ & Best margin gain & SBR of that point & Unprocessed SBR \\\\\\midrule")
    for s in SCENES:
        sr = [r for r in rows if r["scene"] == s]
        mg = [float(r["basic_mean_margin_gain_db"]) for r in sr]
        sg = [float(r["sbr_gain_db"]) for r in sr]
        sb = [float(r["sbr_db"]) for r in sr]
        k = max(range(len(mg)), key=lambda i: mg[i])
        o.append(f"{SCENE_SHORT[s]} & {len(sr)} & {pearson(mg, sg):+.3f} & {pearson(ranks(mg), ranks(sg)):+.3f} & "
                 f"{mg[k]:+.2f} & {sb[k]:+.2f} & {sb[k] - sg[k]:+.2f} \\\\")
    o.append("\\bottomrule\\end{tabular}\\end{table}")
    o.append("")
    o.append("\\begin{table}[H]\\centering\\scriptsize")
    o.append("\\caption{Top-5 margin-gain settings from each sweep, and the SBR they produce.}")
    o.append("\\begin{tabular}{@{}llrrrrrrr@{}}\\toprule")
    o.append("Scene & Point & Thr.\\ dB & Ratio & Knee dB & Atk.\\ ms & Rel.\\ ms & Margin gain dB & SBR dB \\\\\\midrule")
    for s in SCENES:
        sr = sorted([r for r in rows if r["scene"] == s], key=lambda r: -float(r["basic_mean_margin_gain_db"]))[:5]
        for r in sr:
            o.append(f"{SCENE_SHORT[s]} & \\#{r['point']} & {float(r['threshold_db']):.1f} & {float(r['ratio']):.2f} & "
                     f"{float(r['knee_db']):.1f} & {float(r['attack_ms']):.1f} & {float(r['release_ms']):.0f} & "
                     f"{float(r['basic_mean_margin_gain_db']):+.2f} & {float(r['sbr_db']):+.2f} \\\\")
        o.append("\\midrule" if s != SCENES[-1] else "\\bottomrule")
    o.append("\\end{tabular}\\end{table}")
    o.append("")
    o.append("\\begin{table}[H]\\centering\\scriptsize")
    o.append("\\caption{Margin gain of the SBR-matched settings. \\emph{True} margin is measured from the separately "
             "rendered processed Dialogue and processed background; the \\emph{estimator} is the sweep's "
             "\\texttt{compute\\_masking\\_margin} (mix minus dry Dialogue), valid only when Dialogue is unprocessed (Basic). "
             "Last column: best estimator margin gain found in that scene's sweep.}")
    o.append("\\begin{tabular}{@{}llrrrr@{}}\\toprule")
    o.append("Setting & Scene & SBR dB & True margin gain dB & Estimator margin gain dB & Best in margin sweep dB \\\\\\midrule")
    prev = None
    for r in winners:
        if prev and prev != r["setting"]:
            o.append("\\midrule")
        prev = r["setting"]
        est = float(r["estimator_margin_gain_db"])
        est_s = "--" if math.isnan(est) else f"{est:+.2f}"
        o.append(f"\\texttt{{{r['setting'].replace('_', chr(92) + '_')}}} & {SCENE_SHORT[r['scene']]} & {float(r['sbr_db']):+.2f} & "
                 f"{float(r['true_margin_gain_db']):+.2f} & {est_s} & {float(r['best_margin_gain_in_sweep_db']):+.2f} \\\\")
    o.append("\\bottomrule\\end{tabular}\\end{table}")
    return "\n".join(o)


def sbr5_section():
    """The three settings chosen for Unity: +5.0 dB SBR on Construction, one per snapshot."""
    tsv = HERE / "sbr5_settings.tsv"
    if not tsv.exists():
        return ""
    settings = {l.split("\t")[1]: l.split("\t")[2] for l in tsv.read_text().splitlines() if l}
    evals = read_csv(HERE / "sbr5_eval.tsv", "\t")
    lld = read_csv(HERE / "sbr5_lld_settings.tsv", "\t")
    lld_sbr = read_csv(HERE / "sbr5_lld_sbr.tsv", "\t")
    lag = next(k for k in lld_sbr[0] if k.startswith("lld_trimmed_sbr_db_lag")).split("lag")[-1]
    modes = [("basic", "Unmasked"), ("comp", "GroupCompress"), ("combined", "GroupCompressed$>$Unmasked")]

    def dims(mode):
        with open(HERE / f"sbr5_{mode}.csv") as f:
            header = next(csv.reader(f))
        return [h for h in header[2:] if h in LABELS]

    vals = {m: dict(zip(dims(m), (float(v) for v in settings[m].split(",")))) for m, _ in modes}
    allp = [p for p in LABELS if any(p in vals[m] for m, _ in modes)]
    o = ["\\clearpage", "\\section{Unity settings: SBR $+5.0$\\,dB on Construction}",
         "One setting per Unity mixer snapshot, each searched so the Construction scene lands at $+5.0 \\pm 0.10$\\,dB "
         "(objective: error beyond $\\pm0.05$\\,dB penalized, then least gain reduction). Key and makeup gains 0\\,dB; "
         "Dialogue compressed 1.5--6\\,dB in the two compression modes. Values are the rounded ones shipped to Unity; "
         "the SBR rows were re-measured with those rounded values. Public Transit and Restaurant are for reference only.",
         "", "\\begin{table}[H]\\centering\\scriptsize",
         "\\caption{Web-engine parameters of the three Unity settings (--- = not part of that mode).}",
         "\\resizebox{\\linewidth}{!}{\\begin{tabular}{@{}l l rrr@{}}\\toprule",
         "\\textbf{Parameter} & & " + " & ".join(f"\\textbf{{{snap}}}" for _, snap in modes) + " \\\\\\midrule"]
    for p in allp:
        label, unit = LABELS[p]
        o.append(f"{label} & {unit} & " + " & ".join(fmt(p, vals[m][p]) if p in vals[m] else "---" for m, _ in modes) + " \\\\")
    o.append("Key / makeup gain & dB & 0 & 0 & 0 \\\\\\midrule")
    for sc in SCENES:
        cells = []
        for m, _ in modes:
            e = next(x for x in evals if x["id"] == f"{m}_T" and x["scene"] == sc)
            v = float(e["sbr_db"])
            cells.append(f"\\textbf{{{v:+.2f}}}" if sc == "construction" else f"{v:+.2f}")
        o.append(f"SBR, {SCENE_SHORT[sc]}{' (reference)' if sc == 'construction' else ''} & dB & " + " & ".join(cells) + " \\\\")
    o.append(f"SBR, Constr., LL Dynamics values (emulated, {lag}-sample lag) & dB & " + " & ".join(
        f"{float(next(x for x in lld_sbr if x['setting'] == m + '_T')[f'lld_trimmed_sbr_db_lag{lag}']):+.2f}" for m, _ in modes) + " \\\\")
    o += ["\\bottomrule\\end{tabular}}\\end{table}", "",
          "\\begin{table}[H]\\centering\\scriptsize",
          "\\caption{LL Dynamics values to enter in Unity (Construction web-test levels; add each group's level offset "
          "$\\Delta$ in Unity). Mean GR = speech-active mean gain reduction on Construction.}",
          "\\resizebox{\\linewidth}{!}{\\begin{tabular}{@{}lllrrrrrrr@{}}\\toprule",
          "Snapshot & Effect & Group & Thr.\\ dB & Ratio & Knee dB & Atk.\\ ms & Rel.\\ ms & Max GR dB & Mean GR dB \\\\\\midrule"]
    for i, (m, snap) in enumerate(modes):
        for r in [x for x in lld if x["setting"] == f"{m}_T"]:
            grp = "Music, Bkg., FX" if r["channel"] == "Music/Background/Safety/Other" else r["channel"]
            o.append(f"{snap} & {r['stage']} & {grp} & {float(r['threshold_db']):.1f} & {float(r['ratio']):.2f} & "
                     f"{float(r['knee_db']):.1f} & {float(r['attack_ms']):.1f} & {float(r['release_ms']):.0f} & "
                     f"{float(r['max_gr_db']):.1f} & {float(r['construction_mean_gr_active_db']):.1f} \\\\")
        o.append("\\midrule" if i < len(modes) - 1 else "\\bottomrule")
    o.append("\\end{tabular}}\\end{table}")
    return "\n".join(o)


def main():
    parts = [r"""\documentclass[11pt]{article}
\usepackage[margin=0.9in]{geometry}
\usepackage{amsmath,booktabs,siunitx,float,enumitem,graphicx}
\usepackage[colorlinks=true,linkcolor=blue,urlcolor=blue]{hyperref}
\setlength{\emergencystretch}{2em}
\title{SBR-Matched Dynamics Settings:\\ the Six Best Settings per Mode and Target}
\author{Generated by \texttt{analysis/write\_sbr\_settings\_tex.py}}
\date{}
\begin{document}
\maketitle

\section{How these were found}
\begin{itemize}[nosep]
\item \textbf{Metric:} Johnson \& Healy (2024) speech-to-background ratio: speech-active RMS of processed Dialogue over the
RMS of the processed sum of Music, Background, Safety and Other (broadband, unweighted, both channels).
\item \textbf{Targets:} HI $+5.1$\,dB (J\&H 95\,\% window 4.7--5.8\,dB); NH $+12.2$\,dB (window 2.8--22.4\,dB).
\item \textbf{One setting for all three scenes:} the objective is
$J = \max_{\text{scene}} |\text{SBR} - \text{target}| + 0.02\,\bar{P} + \text{penalty}$, where $\bar{P}$ is the mean gain
reduction in dB (prefers gentler processing) and the penalty enforces Dialogue compression of 1.5--6\,dB mean in every
scene (group compression and Combined).
\item \textbf{Dynamics only:} key gain and every makeup gain fixed at 0\,dB.
\item \textbf{Search:} Latin Hypercube (800 / 1500 / 2000 points), then an elitist local search per target.
\item \textbf{Columns \#1--\#6:} the best \emph{distinct} settings in order of $J$. Each differs from every setting to its
left by an RMS distance of at least 0.10 (parameters scaled to their search ranges), so the columns show different ways
of reaching the target, not near-copies of \#1. \#1 is the setting used for the presets, renders and Unity handoff
(those use it rounded to slider precision).
\item \textbf{Chosen for the Unity test sequence} (column marked \textbf{Unity}): Basic unmasking HI \#1 and NH \#3;
Group compression HI \#1 and NH \#1. Combined (group compression, then Basic unmasking keyed on uncompressed Dialogue)
is not chosen yet; its \#1 is marked provisional.
\item Scene SBRs unprocessed: Construction $+1.31$, Public Transit $-4.50$, Restaurant $-2.71$\,dB; raw Dialogue dynamic
range (99th$-$30th percentile of 125\,ms levels) 14.3, 14.7, 14.5\,dB.
\end{itemize}
"""]
    parts.append(sbr5_section().replace("\\clearpage\n", "", 1))
    parts.append("\\clearpage")
    for mode, label in MODES:
        parts.append(f"\\section{{{label}}}")
        for target in TARGETS:
            parts.append(settings_table(mode, label, target))
        parts.append("\\clearpage")
    parts.append(crossover())
    parts.append(r"""
\subsection*{What the crossover shows}
\begin{itemize}[nosep]
\item \textbf{Same direction, different question.} Within each margin sweep, margin gain and SBR gain rank the settings
almost identically (Spearman $\rho \geq 0.997$): anything that ducks more improves both. The best margin-gain settings are
also the highest-SBR settings in those sweeps.
\item \textbf{But margin gain is ``more is better'' and SBR has an optimum.} With max reduction capped at 6\,dB, the
best margin settings reach SBR $+7.1$ (Construction), $+1.4$ (Public Transit) and $+3.3$\,dB (Restaurant): near the HI
target in Construction only, and well short of the NH target everywhere. Reaching $+12.2$\,dB needs max reduction
above 6\,dB, which the margin sweeps never varied.
\item \textbf{The margin estimator reads high.} Because it estimates the masker as mix power minus dry-Dialogue power,
it overstates the true in-band margin gain: by about 1\,dB at the sweeps' best points (e.g.\ $+6.97$ vs.\ a true
$+5.96$\,dB; the true value cannot exceed the 6\,dB cap) and by 5--7\,dB for the deep NH ducks. It also cannot be used
for group compression, where Dialogue itself is processed.
\item \textbf{At equal SBR, margin differs by mode.} The SBR-matched settings hit the same broadband SBR, yet their
in-band margin gains differ widely (e.g.\ HI target, Construction: group compression $+5.7$, Combined $+11.2$\,dB),
because margin averages per-frame dB in the speech band while SBR is a broadband energy ratio.
\end{itemize}
\end{document}
""")
    out = HERE / "sbr_matched_settings_report.tex"
    out.write_text("\n\n".join(parts))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
