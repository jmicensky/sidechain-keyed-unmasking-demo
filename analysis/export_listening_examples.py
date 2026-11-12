#!/usr/bin/env python3
"""Listening examples for the thesis (2026-11-12), Construction scene.

Part 1, the four processes, each at three strengths from its LHS sweep
(process_lhs_<process>.csv, Construction rows):
  3db, 6db  the point whose SBR gain over the unprocessed mix is closest to
            +3 / +6 dB
  max       the point with the largest SBR gain
plus the unprocessed mix.

Part 2, the WDRC chain (fast fitting of score_wdrc_chains.py: -50 dBFS, 2.5:1,
6 dB knee, 5 ms attack, 80 ms release): the WDRC alone, group compression and
Basic at their HI matched settings (sbr_matched_2scene_<process>_best.tsv) into
the WDRC, and for each a static background gain giving the same SBR into the
WDRC.

Every clip is the processed Dialogue plus the processed background sum from
`sbr_matched_search --eval --split-dir` (post-WDRC for part 2), matched to the
integrated loudness (BS.1770) of the unprocessed mix, then all clips are
lowered by one common offset so none peaks above -1 dBFS. Clips differ in
balance, not in loudness.

    /usr/bin/python3 analysis/export_listening_examples.py --tool <sbr_matched_search> --work <dir>
Writes paper-audio/listening/*.mp3, analysis/listening_examples.csv and
analysis/listening_examples_table.tex.
"""
import argparse
import csv
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
from process_metrics import integrated_lufs  # noqa: E402

SCENE = "construction"
OUT_DIR = REPO / "paper-audio" / "listening"
URL = "https://jmicensky.github.io/sidechain-keyed-unmasking-demo/paper-audio/listening/"
WDRC_FIT = "-50,2.5,6,5,80"
TARGET = "HI"  # matched setting used into the WDRC
STEPS = [("3db", 3.0, "+3 dB"), ("6db", 6.0, "+6 dB")]
PROCESSES = [("comp", "Group compression"), ("compmb", "Group multiband compression"),
             ("basic", "Sidechain, summed bus (Basic)"), ("perchannel", "Sidechain, per-channel")]
WDRC_PROCESSES = [("comp", "Group compression", "group compression"), ("basic", "Sidechain, summed bus (Basic)", "Basic")]


def lhs_points(proc):
    with open(HERE / f"process_lhs_{proc}.csv") as f:
        rows = [r for r in csv.DictReader(f) if r["scene"] == SCENE]
    cols = list(rows[0])
    params = cols[cols.index("scene") + 1:cols.index("sbr_db")]
    vec = lambda r: ",".join(r[p] for p in params)  # noqa: E731
    out = {}
    for key, goal, label in STEPS:
        r = min(rows, key=lambda r: abs(float(r["sbr_gain_db"]) - goal))
        out[key] = (label, r["point"], vec(r))
    best = max(rows, key=lambda r: float(r["sbr_gain_db"]))
    out["max"] = ("Maximum", best["point"], vec(best))
    return out


def matched(proc, target=TARGET):
    with open(HERE / f"sbr_matched_2scene_{proc}_best.tsv") as f:
        for line in f:
            label, mode, v = line.rstrip("\n").split("\t")[:3]
            if label == f"{proc}_{target}":
                return v
    raise KeyError(proc)


def run(tool, jobs, work, tag, wdrc=None):
    """jobs: [(id, mode, values)] -> {id: eval row}."""
    tsv, out = work / f"{tag}.in.tsv", work / f"{tag}.out.tsv"
    tsv.write_text("".join(f"{i}\t{m}\t{SCENE}\t{v}\n" for i, m, v in jobs))
    cmd = [str(tool), "--repo", str(REPO), "--scenes", SCENE, "--eval", str(tsv), "--out", str(out),
           "--split-dir", str(work)]
    if wdrc:
        cmd += ["--wdrc", wdrc]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return {r["id"]: r for r in csv.DictReader(f, delimiter="\t")}


def mix(work, rid):
    d, sr = sf.read(str(work / f"{rid}_dialogue.wav"), dtype="float64", always_2d=True)
    b, _ = sf.read(str(work / f"{rid}_background.wav"), dtype="float64", always_2d=True)
    return d + b, sr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    # Part 1 + the pre-WDRC SBR of the matched settings (for the static matches).
    jobs = [("unprocessed", "none", "0")]
    meta = {"unprocessed": ("Unprocessed", "", "", "")}
    for proc, label in PROCESSES:
        for level, (strength, point, v) in lhs_points(proc).items():
            rid = f"{proc}_{level}"
            jobs.append((rid, proc, v))
            meta[rid] = (label, strength, f"LHS {point}", "")
    for proc, *_ in WDRC_PROCESSES:
        jobs.append((f"pre_{proc}", proc, matched(proc)))
    res = run(args.tool, jobs, work, "listening")
    sbr0 = float(res["unprocessed"]["sbr_db"])

    # Part 2, through the WDRC.
    wjobs = [("wdrc_only", "none", "0")]
    meta["wdrc_only"] = ("WDRC alone", "", "", "")
    for proc, label, short in WDRC_PROCESSES:
        sin = float(res[f"pre_{proc}"]["sbr_db"])
        wjobs.append((f"wdrc_{proc}_{TARGET}", proc, matched(proc)))
        wjobs.append((f"wdrc_static_{proc}_{TARGET}", "static", repr(sbr0 - sin)))
        meta[f"wdrc_{proc}_{TARGET}"] = (label, f"{TARGET} setting", "", "")
        meta[f"wdrc_static_{proc}_{TARGET}"] = ("Static gain", f"matched to {short}", f"{sbr0 - sin:+.1f} dB", "")
    wres = run(args.tool, wjobs, work, "listening_wdrc", wdrc=WDRC_FIT)

    ids = [j[0] for j in jobs if not j[0].startswith("pre_")] + [j[0] for j in wjobs]
    clips = {rid: mix(work, rid) for rid in ids}
    sr = clips["unprocessed"][1]
    ref = integrated_lufs(clips["unprocessed"][0], sr)
    gains = {rid: 10 ** ((ref - integrated_lufs(x, sr)) / 20) for rid, (x, _) in clips.items()}
    worst = max(20 * np.log10(np.abs(x).max() * gains[rid]) for rid, (x, _) in clips.items())
    offset = 10 ** (min(0.0, -1.0 - worst) / 20)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rows = []
    for rid in ids:
        x = clips[rid][0] * gains[rid] * offset
        wav = work / f"{rid}_mix.wav"
        sf.write(str(wav), x.astype(np.float32), sr, subtype="FLOAT")
        mp3 = OUT_DIR / f"{SCENE}_{rid}.mp3"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-codec:a", "libmp3lame",
                        "-b:a", "192k", str(mp3)], check=True)
        through = rid in wres
        r = wres[rid] if through else res[rid]
        sbr_in = float(r["sbr_db"])
        sbr_ear = float(r["sbr_out_db"]) if through else sbr_in
        proc, level, note, _ = meta[rid]
        rows.append({"id": rid, "part": "wdrc" if through else "process", "process": proc, "level": level,
                     "setting": note, "sbr_in_db": round(sbr_in, 3), "sbr_ear_db": round(sbr_ear, 3),
                     "sbr_gain_db": round(sbr_ear - sbr0, 3),
                     "level_db": round(20 * np.log10(gains[rid] * offset), 2),
                     "peak_dbfs": round(20 * np.log10(np.abs(x).max()), 2), "file": mp3.name})
        print(f"{rid:24s} SBR {sbr_ear:+6.2f} dB  peak {rows[-1]['peak_dbfs']:6.2f} dBFS")
    with open(HERE / "listening_examples.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    table(rows)


def table(rows):
    link = lambda r: rf"\href{{{URL}{r['file']}}}{{Listen}}"  # noqa: E731
    p1 = [r for r in rows if r["part"] == "process"]
    p2 = [r for r in rows if r["part"] == "wdrc"]
    body1 = []
    for r in p1:
        name = r["process"] if r["level"] in ("", STEPS[0][2]) else ""
        body1.append(f"{name} & {r['level']} & {r['setting']} & ${r['sbr_ear_db']:+.1f}$ & "
                     f"${r['sbr_gain_db']:+.1f}$ & {link(r)} \\\\")
        if r["level"] in ("", "Maximum"):
            body1.append(r"\addlinespace")
    body1.pop()
    body2 = []
    for r in p2:
        body2.append(f"{r['process']} & {r['level']} & ${r['sbr_in_db']:+.1f}$ & ${r['sbr_ear_db']:+.1f}$ & "
                     f"{link(r)} \\\\")
        if r["id"] == "wdrc_only" or r["id"].startswith("wdrc_static"):
            body2.append(r"\addlinespace")
    body2.pop()
    tex = r"""% Listening examples, Construction scene. Generated by
% analysis/export_listening_examples.py (MP3s in paper-audio/listening/). Do not edit by hand.
\begin{table}[H]
\centering
\footnotesize
\setstretch{1}
\caption{\protect\aicaption{AI draft --- rewrite: Listening examples for the four processes on the same Construction clip. For each process, the $+3$ and $+6$~dB rows are the swept settings whose SBR gain over the unprocessed mix is closest to those values, and ``Maximum'' is the setting with the largest SBR gain in its parameter sweep. Every clip is matched in integrated loudness to the unprocessed mix, so the clips differ in balance, not level.}}
\label{tab:listening-processes}
\begin{tabular}{@{}lllrrl@{}}
\toprule
\textbf{Process} & \textbf{Strength} & \textbf{Setting} & \textbf{SBR (dB)} & \textbf{Gain (dB)} & \textbf{Audio} \\
\midrule
""" + "\n".join(body1) + r"""
\bottomrule
\end{tabular}
\end{table}

\begin{table}[H]
\centering
\footnotesize
\setstretch{1}
\caption{\protect\aicaption{AI draft --- rewrite: Listening examples through the hearing-aid WDRC (threshold $-50$~dBFS, 2.5:1, 80~ms release) on the same Construction clip. Group compression and Basic sidechain use their HI settings (Johnson and Healy's $+5.1$~dB target); each is paired with a static background gain giving the same SBR into the WDRC. Every clip is matched in integrated loudness to the unprocessed mix.}}
\label{tab:listening-wdrc}
\begin{tabular}{@{}llrrl@{}}
\toprule
\textbf{Chain into the WDRC} & & \textbf{SBR in (dB)} & \textbf{SBR at ear (dB)} & \textbf{Audio} \\
\midrule
""" + "\n".join(body2) + r"""
\bottomrule
\end{tabular}
\end{table}
"""
    (HERE / "listening_examples_table.tex").write_text(tex)
    print("wrote listening_examples_table.tex")


if __name__ == "__main__":
    main()
