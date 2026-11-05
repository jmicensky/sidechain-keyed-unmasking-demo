#!/usr/bin/env python3
"""LHS sweep of one process for the process comparison (2026-11-05 restructure;
see the "Restructure" section of HANDOFF_speech_band_sii.md).

Processes (renderer: sbr_matched_search, validated against Engine):
  basic       Basic sidechain (summed bus, full band)          6 params
  perchannel  full-band sidechain, per-stem threshold/ratio    12 params
  comp        group compression, single-band                   15 params

Basic and Per-channel use the same ranges as the earlier Basic LHS sweep
(run_lhs_sweep.py, max reduction 3-12 dB); Per-channel samples threshold and
ratio once per background stem. Group compression uses the ranges of
sbr_matched_search's comp mode (Dialogue compressed 2-4:1).

The same LHS points (scipy qmc, seed 42) are rendered in every scene, so the
scenes are paired. Each point is scored by process_metrics.score after
loudness matching to the unprocessed mix. Rows are appended as they finish;
an interrupted run resumes where it stopped.

    python3 analysis/run_process_lhs.py --process basic --n 50 --tool <sbr_matched_search> --work <scratch>
Writes analysis/process_lhs_<process>.csv (one row per point and scene).
"""
import argparse
import csv
import os
import subprocess
import sys
from multiprocessing import Pool
from pathlib import Path

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
import numpy as np  # noqa: E402
from scipy.stats import qmc  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import process_metrics as pm  # noqa: E402

SCENES = ["construction", "publictransit"]
STEMS = ["music", "background", "safety", "other"]
SIDECHAIN_SHARED = [("knee_db", 0.0, 24.0), ("attack_ms", 1.0, 50.0), ("release_ms", 20.0, 400.0),
                    ("max_reduction_db", 3.0, 12.0)]
# Order = the tool's dims order for that mode (dimsFor in sbr_matched_search.cpp).
RANGES = {
    "basic": [("threshold_db", -60.0, -18.0), ("ratio", 1.5, 10.0)] + SIDECHAIN_SHARED,
    "perchannel": [p for s in STEMS for p in ((f"{s}_threshold_db", -60.0, -18.0), (f"{s}_ratio", 1.5, 10.0))]
                  + SIDECHAIN_SHARED,
    "comp": [("dialogue_threshold_db", -45.0, -15.0), ("dialogue_ratio", 2.0, 4.0),
             ("dialogue_attack_ms", 1.0, 20.0), ("dialogue_release_ms", 30.0, 300.0)]
            + [p for s in STEMS for p in ((f"{s}_threshold_db", -65.0, -20.0), (f"{s}_ratio", 1.5, 10.0))]
            + [("knee_db", 0.0, 24.0), ("background_attack_ms", 1.0, 50.0), ("background_release_ms", 20.0, 1000.0)],
}


def lhs_points(process, n, seed):
    r = RANGES[process]
    u = qmc.LatinHypercube(d=len(r), seed=seed).random(n)
    lo, hi = np.array([x[1] for x in r]), np.array([x[2] for x in r])
    return lo + u * (hi - lo)


def render(tool, lines, work, tag):
    tsv, out = work / f"{tag}.in.tsv", work / f"{tag}.out.tsv"
    tsv.write_text("\n".join(lines) + "\n")
    subprocess.run([tool, "--repo", str(REPO), "--scenes", ",".join(SCENES), "--eval", str(tsv),
                    "--out", str(out), "--split-dir", str(work)], check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return {r["id"]: float(r["sbr_db"]) for r in csv.DictReader(f, delimiter="\t")}


_BASE = {}


def _score(job):
    rid, scene, work = job
    return rid, pm.score(Path(work) / f"{rid}_dialogue.wav", Path(work) / f"{rid}_background.wav", _BASE[scene])


def _init(base):
    _BASE.update(base)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--process", required=True, choices=list(RANGES))
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--batch", type=int, default=8, help="points per render batch")
    ap.add_argument("--out")
    args = ap.parse_args()
    out = Path(args.out) if args.out else HERE / f"process_lhs_{args.process}.csv"
    work = Path(args.work) / args.process
    work.mkdir(parents=True, exist_ok=True)
    names = [x[0] for x in RANGES[args.process]]
    pts = lhs_points(args.process, args.n, args.seed)

    # Unprocessed reference per scene.
    sbr0 = render(args.tool, [f"unproc_{s}\tnone\t{s}\t0" for s in SCENES], work, "unproc")
    base = {s: pm.baseline(work / f"unproc_{s}_dialogue.wav", work / f"unproc_{s}_background.wav") for s in SCENES}
    with Pool(len(SCENES), initializer=_init, initargs=(base,)) as pool:
        base_scores = dict(pool.map(_score, [(f"unproc_{s}", s, str(work)) for s in SCENES]))
    for s in SCENES:
        bs = base_scores[f"unproc_{s}"]
        print(f"unprocessed {s}: SBR {sbr0[f'unproc_{s}']:+.2f} dB  ESII {bs['esii']:.3f}  "
              f"static SII {bs['static_sii']:.3f}  ESTOI {bs['estoi']:.3f}", flush=True)

    fields = (["process", "point", "scene"] + names + ["sbr_db", "sbr_gain_db", "match_gain_db"]
              + pm.METRICS + [f"{k}_gain" for k in pm.GAIN_METRICS])
    done = set()
    if out.exists():
        with open(out) as f:
            done = {(int(r["point"]), r["scene"]) for r in csv.DictReader(f)}
        print(f"resuming: {len(done)} rows already in {out.name}", flush=True)
    else:
        with open(out, "w", newline="") as f:
            csv.DictWriter(f, fieldnames=fields).writeheader()

    todo = [i for i in range(args.n) if any((i, s) not in done for s in SCENES)]
    with Pool(args.workers, initializer=_init, initargs=(base,)) as pool:
        for b0 in range(0, len(todo), args.batch):
            batch = todo[b0:b0 + args.batch]
            jobs = [(i, s) for i in batch for s in SCENES if (i, s) not in done]
            lines = [f"p{i}_{s}\t{args.process}\t{s}\t" + ",".join(repr(float(x)) for x in pts[i]) for i, s in jobs]
            sbr = render(args.tool, lines, work, "batch")
            scores = dict(pool.map(_score, [(f"p{i}_{s}", s, str(work)) for i, s in jobs]))
            with open(out, "a", newline="") as f:
                w = csv.DictWriter(f, fieldnames=fields)
                for i, s in jobs:
                    rid = f"p{i}_{s}"
                    row = {"process": args.process, "point": i, "scene": s,
                           **dict(zip(names, (float(x) for x in pts[i]))),
                           "sbr_db": sbr[rid], "sbr_gain_db": sbr[rid] - sbr0[f"unproc_{s}"],
                           **pm.with_gains(scores[rid], base_scores[f"unproc_{s}"])}
                    w.writerow({k: (float(v) if isinstance(v, np.floating) else v) for k, v in row.items()})
            for i, s in jobs:
                for suffix in ("_dialogue.wav", "_background.wav"):
                    (work / f"p{i}_{s}{suffix}").unlink(missing_ok=True)
            print(f"{args.process}: {min(b0 + args.batch, len(todo))}/{len(todo)} points", flush=True)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
