#!/usr/bin/env python3
"""Crossover between the earlier mean-margin-gain sweeps and the J&H SBR search.

1. SBR of every point in the earlier margin sweeps (Basic ducking, Dialogue key,
   max reduction 6 dB - what run_lhs_sweep.py used), on the scene it was swept on.
   -> correlation of margin gain with SBR gain, and where the best-margin settings
      land relative to the J&H targets.
2. Margin gain of the SBR-matched settings (all modes). compute_masking_margin()
   estimates the masker as mix minus the DRY Dialogue, which is only valid when
   Dialogue passes unprocessed (true for ducking, not for group compression). So
   margin here is computed from the separately rendered processed Dialogue and
   processed background - same band (class range narrowed 0.85 oct each side),
   same 20 ms / 50 % frames, same activity gate (dry Dialogue within 40 dB of peak).
   Step 0 checks this against compute_masking_margin() on Basic points.

Needs numpy, scipy, soundfile (any venv) plus the built tool:
    python analysis/sbr_margin_crossover.py --tool <sbr_matched_search binary> --work <scratch dir>
Writes analysis/sbr_margin_crossover.tsv / _summary.txt / _winners.tsv.
"""
import argparse
import csv
import math
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
from compute_metrics import (  # noqa: E402
    MARGIN_MEASUREMENT_PAD_OCTAVES, ACTIVE_FLOOR_DB_BELOW_PEAK, bandpass, frame_rms_db, load_mono,
    compute_masking_margin,
)

SCENES = {
    "construction": HERE / "lhs_sweep_results.csv",
    "publictransit": HERE / "lhs_sweep_results_publictransit_dialogue.csv",
    "restaurant": HERE / "lhs_sweep_results_restaurant_dialogue.csv",
}
DIALOGUE_RANGE = (400.0, 7000.0)  # the band the margin sweeps used (Dialogue range was 400-7000 Hz until 2026-10-08)
TARGETS = {"HI": 5.1, "NH": 12.2}
UNPROC_SBR = {"construction": 1.310, "publictransit": -4.504, "restaurant": -2.715}


def true_margin(dry, dlg, bg, sr):
    pad = 2 ** MARGIN_MEASUREMENT_PAD_OCTAVES
    lo, hi = DIALOGUE_RANGE[0] * pad, DIALOGUE_RANGE[1] / pad
    dry_db = frame_rms_db(bandpass(dry, sr, lo, hi), sr)
    d_db = frame_rms_db(bandpass(dlg, sr, lo, hi), sr)
    b_db = frame_rms_db(bandpass(bg, sr, lo, hi), sr)
    n = min(len(dry_db), len(d_db), len(b_db))
    active = dry_db[:n] > (np.max(dry_db[:n]) - ACTIVE_FLOOR_DB_BELOW_PEAK)
    return float(np.mean(d_db[:n][active] - b_db[:n][active]))


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
    rx = np.argsort(np.argsort(x)); ry = np.argsort(np.argsort(y))
    return pearson(rx, ry)


def margin_for(tool, work, specs):
    """specs: list of (id, mode, scene, values). Returns {id: (margin, sbr)} with
    margin from split renders, relative to that scene's unprocessed margin."""
    split = Path(work) / "split"
    split.mkdir(parents=True, exist_ok=True)
    lines = [f"{i}\t{m}\t{s}\t{','.join(repr(v) for v in vals)}" for i, m, s, vals in specs]
    lines += [f"unproc_{s}\tnone\t{s}\t0" for s in SCENES]
    res = run_tool(tool, lines, Path(work) / "eval_split.tsv", split)
    out = {}
    base = {}
    dry = {}
    for s in SCENES:
        d, sr = load_mono(split / f"unproc_{s}_dialogue.wav")
        b, _ = load_mono(split / f"unproc_{s}_background.wav")
        dry[s] = (d, sr)
        base[s] = true_margin(d, d, b, sr)
    for i, m, s, _ in specs:
        d, sr = load_mono(split / f"{i}_dialogue.wav")
        b, _ = load_mono(split / f"{i}_background.wav")
        out[i] = (true_margin(dry[s][0], d, b, sr) - base[s], float(res[i]["sbr_db"]), d, b)
    return out, dry, base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    report = []
    p = report.append

    # ---------------------------------------------------------------- 1. SBR of margin-sweep points
    points = {}
    lines = []
    for scene, path in SCENES.items():
        with open(path) as f:
            rows = list(csv.DictReader(f))
        points[scene] = rows
        for i, r in enumerate(rows):
            vals = [float(r["threshold_db"]), float(r["ratio"]), float(r["knee_db"]),
                    float(r["attack_ms"]), float(r["release_ms"]), 6.0]
            lines.append(f"{scene}_{i}\tbasic\t{scene}\t{','.join(repr(v) for v in vals)}")
    sbr = run_tool(args.tool, lines, Path(args.work) / "eval_margin_points.tsv")

    # ---------------------------------------------------------------- 0. check the margin port
    p("STEP 0 - margin computed from separated renders vs compute_masking_margin() vs the sweep CSV")
    p("(Basic points: Dialogue passes unprocessed, so all three should agree closely)")
    check_specs = []
    for scene, rows in points.items():
        order = np.argsort([-float(r["basic_mean_margin_gain_db"]) for r in rows])
        for k in (order[0], order[len(order) // 2], order[-1]):
            r = rows[k]
            vals = [float(r["threshold_db"]), float(r["ratio"]), float(r["knee_db"]),
                    float(r["attack_ms"]), float(r["release_ms"]), 6.0]
            check_specs.append((f"chk_{scene}_{k}", "basic", scene, vals))
    chk, dry, base = margin_for(args.tool, args.work, check_specs)
    for i, m, s, _ in check_specs:
        k = int(i.split("_")[-1])
        csv_gain = float(points[s][k]["basic_mean_margin_gain_db"])
        margin_split, _, d, b = chk[i]
        sr = dry[s][1]
        est = compute_masking_margin(dry[s][0], d + b, sr, *DIALOGUE_RANGE, 0.0)["mean_margin_db"]
        est0 = compute_masking_margin(dry[s][0], dry[s][0] + load_mono(Path(args.work) / "split" / f"unproc_{s}_background.wav")[0],
                                      sr, *DIALOGUE_RANGE, 0.0)["mean_margin_db"]
        p(f"  {s:<14} point {k:>3}: sweep CSV {csv_gain:+6.2f} dB | compute_masking_margin {est - est0:+6.2f} dB | "
          f"separated renders {margin_split:+6.2f} dB")
    p("")

    # ---------------------------------------------------------------- 1b. crossover statistics
    p("STEP 1 - the earlier margin-gain sweeps (Basic, Dialogue key, max reduction 6 dB), re-measured as SBR")
    out_rows = []
    for scene, rows in points.items():
        mg = np.array([float(r["basic_mean_margin_gain_db"]) for r in rows])
        sg = np.array([float(sbr[f"{scene}_{i}"]["sbr_db"]) for i in range(len(rows))])
        gain = sg - UNPROC_SBR[scene]
        for i, r in enumerate(rows):
            out_rows.append({"scene": scene, "point": i, **{k: r[k] for k in ("threshold_db", "ratio", "knee_db", "attack_ms", "release_ms")},
                             "basic_mean_margin_gain_db": mg[i], "sbr_db": sg[i], "sbr_gain_db": gain[i]})
        p(f"\n  {scene}: {len(rows)} points; unprocessed SBR {UNPROC_SBR[scene]:+.2f} dB")
        p(f"    margin gain vs SBR gain: Pearson r = {pearson(mg, gain):+.3f}, Spearman rho = {spearman(mg, gain):+.3f}")
        p(f"    margin gain range {mg.min():+.2f} to {mg.max():+.2f} dB; SBR range {sg.min():+.2f} to {sg.max():+.2f} dB "
          f"(6 dB max-reduction cap limits SBR gain to about +6 dB)")
        top = np.argsort(-mg)[:5]
        p("    top-5 margin-gain settings -> their SBR:")
        for k in top:
            r = rows[k]
            p(f"      #{k:<3} margin {mg[k]:+5.2f} dB  SBR {sg[k]:+6.2f} dB  (thr {float(r['threshold_db']):6.1f}, ratio "
              f"{float(r['ratio']):4.1f}, knee {float(r['knee_db']):4.1f}, atk {float(r['attack_ms']):4.1f}, rel {float(r['release_ms']):5.0f})")
        for t, val in TARGETS.items():
            near = np.argsort(np.abs(sg - val))[:3]
            ranks = {k: int(np.where(np.argsort(-mg) == k)[0][0]) + 1 for k in near}
            p(f"    closest to {t} {val:+.1f}: " + "; ".join(
                f"#{k} SBR {sg[k]:+.2f}, margin {mg[k]:+.2f} (margin rank {ranks[k]}/{len(rows)})" for k in near))
    with open(HERE / "sbr_margin_crossover.tsv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()), delimiter="\t")
        w.writeheader()
        w.writerows(out_rows)

    # ---------------------------------------------------------------- 2. margin of the SBR winners
    p("\nSTEP 2 - margin gain of the SBR-matched settings (separated-render margin, all modes)")
    specs = []
    tsv = HERE / "sbr_matched_settings.tsv"
    for line in tsv.read_text().splitlines():
        label, mode, values = line.split("\t")
        vals = [float(v) for v in values.split(",")]
        for s in SCENES:
            specs.append((f"{label}_{s}", mode, s, vals))
    win, dry2, _ = margin_for(args.tool, args.work, specs)
    est_base = {}
    for s in SCENES:
        d0, sr0 = dry2[s]
        b0, _ = load_mono(Path(args.work) / "split" / f"unproc_{s}_background.wav")
        est_base[s] = compute_masking_margin(d0, d0 + b0, sr0, *DIALOGUE_RANGE, 0.0)["mean_margin_db"]
    best_margin = {s: max(float(r["basic_mean_margin_gain_db"]) for r in points[s]) for s in SCENES}
    with open(HERE / "sbr_margin_winners.tsv", "w") as f:
        f.write("setting\tscene\tsbr_db\ttrue_margin_gain_db\testimator_margin_gain_db\tbest_margin_gain_in_sweep_db\n")
        for i, m, s, _ in specs:
            mgain, sbr_db, d, b = win[i]
            # The estimator assumes Dialogue is unprocessed in the mix: only valid for Basic.
            est = (compute_masking_margin(dry2[s][0], d + b, dry2[s][1], *DIALOGUE_RANGE, 0.0)["mean_margin_db"]
                   - est_base[s]) if m == "basic" else float("nan")
            f.write(f"{i.rsplit('_', 1)[0]}\t{s}\t{sbr_db:.3f}\t{mgain:.3f}\t{est:.3f}\t{best_margin[s]:.3f}\n")
            est_txt = f"{est:+6.2f}" if m == "basic" else "   n/a"
            p(f"  {i.rsplit('_', 1)[0]:<12} {s:<14} SBR {sbr_db:+6.2f} dB  true margin gain {mgain:+6.2f} dB  "
              f"estimator {est_txt} dB  (best estimator margin gain in that scene's sweep: {best_margin[s]:+.2f} dB)")

    text = "\n".join(report)
    (HERE / "sbr_margin_crossover_summary.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
