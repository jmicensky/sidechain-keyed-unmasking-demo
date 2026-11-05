#!/usr/bin/env python3
"""Crossover between the speech-band sweeps and the Johnson & Healy SBR search
(2026-11-04 revision of sbr_margin_crossover.py; see HANDOFF_speech_band_sii.md).

1. SBR of every Basic point in the new LHS sweeps (lhs_speech_<scene>_dialogue.csv),
   each with its own sampled max reduction. Reports the correlation of SBR gain
   with speech-band margin gain and with Extended SII gain, and where the best
   margin and SII settings land against the J&H targets.
2. Speech-band margin, Extended/static SII and ESTOI of the SBR-matched settings
   (all modes), from the separated Dialogue/background renders that
   `sbr_matched_search --eval --split-dir` writes. Margin is
   compute_separated_margin and SII is sii.compute_sii, the same functions the
   sweeps use, so the numbers are comparable.

Restaurant is excluded. sbr_margin_crossover.py and its outputs are kept for the
old padded-band margin.

    python3 analysis/sbr_speech_crossover.py --tool <sbr_matched_search binary> --work <scratch dir>
        [--settings analysis/sbr_matched_settings.tsv] [--tag _x] [--step2-only]
2-scene settings:
    --settings analysis/sbr_matched_2scene_{basic,comp,combined}_best.tsv --tag _2scene --step2-only
Writes analysis/sbr_speech_crossover.tsv, _winners.tsv and _summary.txt.
"""
import argparse
import csv
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
from compute_metrics import HAVE_PYSTOI, compute_separated_margin, compute_stoi, load_mono  # noqa: E402
from sii import compute_sii  # noqa: E402

SCENES = ["construction", "publictransit"]
TARGETS = {"HI": 5.1, "NH": 12.2}
BASIC_DIMS = ["threshold_db", "ratio", "knee_db", "attack_ms", "release_ms", "max_reduction_db"]


def run_tool(tool, lines, out, split_dir=None):
    tsv = Path(out).with_suffix(".in.tsv")
    tsv.write_text("\n".join(lines) + "\n")
    cmd = [tool, "--repo", str(REPO), "--eval", str(tsv), "--out", str(out)]
    if split_dir:
        cmd += ["--split-dir", str(split_dir)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return {r["id"]: r for r in csv.DictReader(f, delimiter="\t")}


def pearson(x, y):
    return float(np.corrcoef(x, y)[0, 1])


def spearman(x, y):
    return pearson(np.argsort(np.argsort(x)), np.argsort(np.argsort(y)))


def score(dlg, bg, dry, sr):
    m = compute_separated_margin(dlg, bg, sr)
    s = compute_sii(dlg, bg, sr)
    estoi = compute_stoi(dry, dlg + bg, sr, extended=True)[0] if HAVE_PYSTOI else float("nan")
    return {"speech_margin_db": m["mean_margin_db"], "esii": s["esii"],
            "static_sii": s["static_sii"], "estoi": estoi}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--settings", nargs="+", default=[str(HERE / "sbr_matched_settings.tsv")],
                    help="one or more settings TSVs (label, mode, values)")
    ap.add_argument("--tag", default="", help="suffix for output files, e.g. _2scene")
    ap.add_argument("--step2-only", action="store_true",
                    help="skip step 1 (it does not depend on --settings)")
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    report = []
    p = report.append

    # ------------------------------------------------ 1. SBR of the sweep points
    if not args.step2_only:
        step1(args, work, p)
    step2(args, work, p)
    text = "\n".join(report)
    (HERE / f"sbr_speech_crossover{args.tag}_summary.txt").write_text(text + "\n")
    print(text)


def step1(args, work, p):
    points, lines = {}, [f"unproc_{s}\tnone\t{s}\t0" for s in SCENES]
    for s in SCENES:
        with open(HERE / f"lhs_speech_{s}_dialogue.csv") as f:
            points[s] = list(csv.DictReader(f))
        for i, r in enumerate(points[s]):
            lines.append(f"{s}_{i}\tbasic\t{s}\t" + ",".join(repr(float(r[k])) for k in BASIC_DIMS))
    sbr = run_tool(args.tool, lines, work / "eval_points.tsv")
    unproc_sbr = {s: float(sbr[f"unproc_{s}"]["sbr_db"]) for s in SCENES}

    p("STEP 1 - Basic points of the speech-band LHS sweeps, re-measured as SBR")
    out_rows = []
    for s in SCENES:
        rows = points[s]
        mg = np.array([float(r["basic_speech_margin_db_gain"]) for r in rows])
        eg = np.array([float(r["basic_esii_gain"]) for r in rows])
        sg = np.array([float(sbr[f"{s}_{i}"]["sbr_db"]) for i in range(len(rows))])
        gain = sg - unproc_sbr[s]
        for i, r in enumerate(rows):
            out_rows.append({"scene": s, "point": i, **{k: r[k] for k in BASIC_DIMS},
                             "speech_margin_gain_db": mg[i], "esii_gain": eg[i],
                             "sbr_db": sg[i], "sbr_gain_db": gain[i]})
        p(f"\n  {s}: {len(rows)} points; unprocessed SBR {unproc_sbr[s]:+.2f} dB")
        p(f"    SBR gain vs margin gain: Pearson {pearson(mg, gain):+.3f}, Spearman {spearman(mg, gain):+.3f}")
        p(f"    SBR gain vs ESII gain:   Pearson {pearson(eg, gain):+.3f}, Spearman {spearman(eg, gain):+.3f}")
        p(f"    SBR range {sg.min():+.2f} to {sg.max():+.2f} dB (max reduction sampled 3-12 dB)")
        for label, metric in (("margin", mg), ("ESII", eg)):
            k = int(np.argmax(metric))
            p(f"    best {label}: point {k}, margin {mg[k]:+.2f} dB, ESII {eg[k]:+.3f}, SBR {sg[k]:+.2f} dB")
        for t, val in TARGETS.items():
            near = np.argsort(np.abs(sg - val))[:3]
            p(f"    closest to {t} {val:+.1f}: " + "; ".join(
                f"#{k} SBR {sg[k]:+.2f}, margin {mg[k]:+.2f}, ESII {eg[k]:+.3f}" for k in near))
    with open(HERE / f"sbr_speech_crossover{args.tag}.tsv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out_rows[0]), delimiter="\t")
        w.writeheader()
        w.writerows(out_rows)


def step2(args, work, p):
    # ------------------------------------------------ 2. metrics of the SBR winners
    p(f"\nSTEP 2 - speech-band metrics of the SBR-matched settings ({', '.join(Path(x).name for x in args.settings)})")
    split = work / f"split{args.tag}"
    split.mkdir(exist_ok=True)
    specs, lines = [], [f"unproc_{s}\tnone\t{s}\t0" for s in SCENES]
    for line in (l for x in args.settings for l in Path(x).read_text().splitlines()):
        if not line.strip():
            continue
        label, mode, values = line.split("\t")[:3]
        for s in SCENES:
            specs.append((label, mode, s))
            lines.append(f"{label}_{s}\t{mode}\t{s}\t{values}")
    res = run_tool(args.tool, lines, work / f"eval_winners{args.tag}.tsv", split)
    base, dry = {}, {}
    for s in SCENES:
        d, sr = load_mono(split / f"unproc_{s}_dialogue.wav")
        b, _ = load_mono(split / f"unproc_{s}_background.wav")
        dry[s] = (d, sr)
        base[s] = score(d, b, d, sr)
        p(f"  unprocessed {s:<14} SBR {float(res[f'unproc_{s}']['sbr_db']):+6.2f} dB  " +
          "  ".join(f"{k} {v:.3f}" for k, v in base[s].items()))
    cols = ["speech_margin_db", "esii", "static_sii", "estoi"]
    with open(HERE / f"sbr_speech_winners{args.tag}.tsv", "w") as f:
        f.write("setting\tmode\tscene\tsbr_db\t" + "\t".join(f"{c}\t{c}_gain" for c in cols) + "\n")
        for label, mode, s in specs:
            d, sr = load_mono(split / f"{label}_{s}_dialogue.wav")
            b, _ = load_mono(split / f"{label}_{s}_background.wav")
            sc = score(d, b, dry[s][0], sr)
            sbr_db = float(res[f"{label}_{s}"]["sbr_db"])
            f.write(f"{label}\t{mode}\t{s}\t{sbr_db:.3f}\t" +
                    "\t".join(f"{sc[c]:.4f}\t{sc[c] - base[s][c]:.4f}" for c in cols) + "\n")
            p(f"  {label:<12} {s:<14} SBR {sbr_db:+6.2f} dB  margin gain {sc['speech_margin_db'] - base[s]['speech_margin_db']:+6.2f} dB  "
              f"ESII gain {sc['esii'] - base[s]['esii']:+.3f}  static SII gain {sc['static_sii'] - base[s]['static_sii']:+.3f}  "
              f"ESTOI gain {sc['estoi'] - base[s]['estoi']:+.3f}")



if __name__ == "__main__":
    main()
