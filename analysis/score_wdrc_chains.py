#!/usr/bin/env python3
"""Class-based processing -> WDRC vs WDRC alone, with the WDRC fitting held
fixed (2026-11-08; see "WDRC chain test" in HANDOFF_speech_band_sii.md).

The WDRC is Engine's 6-band output compressor (makeup 0 dB), applied by
`sbr_matched_search --eval --wdrc`, which applies the WDRC's per-band gains
(computed from the real mix) to Dialogue and the background sum separately,
so SBR at the ear is exact. Two generic fittings stand in for a prescription:
threshold -50 dBFS (about 50 dB SPL at 0 dBFS = 100 dB SPL), 2.5:1, 6 dB knee,
attack 5 ms, release 80 ms (fast) or 1000 ms (slow).

Conditions per fitting and scene, all through the same WDRC:
  wdrc_only               the unprocessed mix (today's hearing aid)
  <process>  HI/NH        the existing matched-SBR settings
                          (sbr_matched_2scene_<process>_best.tsv), not re-tuned
  static_match_<process>  static gain on the background giving the same SBR
                          into the WDRC as <process>
  static     HI/NH        static gain landing the J&H target into the WDRC
Each output is loudness-matched to the unprocessed mix (no WDRC) and scored by
process_metrics. Gains are reported against wdrc_only of the same fitting
(<metric>_vs_wdrc) and against the unprocessed mix (<metric>_gain).

    python3 analysis/score_wdrc_chains.py --tool <sbr_matched_search> --work <dir>
Writes analysis/wdrc_chains.csv.
"""
import argparse
import csv
import os
import subprocess
import sys
from multiprocessing import Pool
from pathlib import Path

os.environ.setdefault("OMP_NUM_THREADS", "1")
import numpy as np  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import process_metrics as pm  # noqa: E402

SCENES = ["construction", "publictransit"]
TARGETS = {"HI": 5.1, "NH": 12.2}
PROCESSES = ["basic", "perchannel", "comp", "compmb"]
FITS = {"fast": "-50,2.5,6,5,80", "slow": "-50,2.5,6,5,1000"}
WDRC_COLS = ["sbr_out_db", "wdrc_mean_gr_db", "wdrc_mean_gr_active_db", "out_range_db", "dialogue_range_out_db"]


def run_tool(tool, lines, work, tag, wdrc=None, split=True):
    tsv, out = work / f"{tag}.in.tsv", work / f"{tag}.out.tsv"
    tsv.write_text("\n".join(lines) + "\n")
    cmd = [tool, "--repo", str(REPO), "--scenes", ",".join(SCENES), "--eval", str(tsv), "--out", str(out)]
    if split:
        cmd += ["--split-dir", str(work)]
    if wdrc:
        cmd += ["--wdrc", wdrc]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return {r["id"]: r for r in csv.DictReader(f, delimiter="\t")}


_BASE = {}


def _init(base):
    _BASE.update(base)


def _score(job):
    rid, scene, work = job
    return rid, pm.score(Path(work) / f"{rid}_dialogue.wav", Path(work) / f"{rid}_background.wav", _BASE[scene])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--out", default=str(HERE / "wdrc_chains.csv"))
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    # Process lines and their SBR into the WDRC (no WDRC, no split).
    proc_lines = []   # (condition, target, scene, mode, values)
    for proc in PROCESSES:
        for line in (HERE / f"sbr_matched_2scene_{proc}_best.tsv").read_text().splitlines():
            if line.strip():
                label, mode, values = line.split("\t")[:3]
                for s in SCENES:
                    proc_lines.append((proc, label.rsplit("_", 1)[1], s, mode, values))
    pre = run_tool(args.tool, [f"unproc_{s}\tnone\t{s}\t0" for s in SCENES] +
                   [f"{c}_{t}_{s}\t{m}\t{s}\t{v}" for c, t, s, m, v in proc_lines], work, "pre", split=False)
    sbr0 = {s: float(pre[f"unproc_{s}"]["sbr_db"]) for s in SCENES}

    # Reference: unprocessed, no WDRC.
    run_tool(args.tool, [f"ref_{s}\tnone\t{s}\t0" for s in SCENES], work, "ref")
    base = {s: pm.baseline(work / f"ref_{s}_dialogue.wav", work / f"ref_{s}_background.wav") for s in SCENES}
    with Pool(len(SCENES), initializer=_init, initargs=(base,)) as pool:
        ref_sc = {k[4:]: v for k, v in pool.map(_score, [(f"ref_{s}", s, str(work)) for s in SCENES])}

    # Conditions: (id, condition, target, scene, mode, values, sbr_in, static_gain)
    conds = []
    for s in SCENES:
        conds.append((f"wdrc_only_{s}", "wdrc_only", "", s, "none", "0", sbr0[s], ""))
        for t, val in TARGETS.items():
            g = sbr0[s] - val
            conds.append((f"static_{t}_{s}", "static", t, s, "static", repr(g), val, g))
    for c, t, s, m, v in proc_lines:
        sin = float(pre[f"{c}_{t}_{s}"]["sbr_db"])
        conds.append((f"{c}_{t}_{s}", c, t, s, m, v, sin, ""))
        g = sbr0[s] - sin
        conds.append((f"static_match_{c}_{t}_{s}", f"static_match_{c}", t, s, "static", repr(g), sin, g))

    rows = []
    for fit, spec in FITS.items():
        res = run_tool(args.tool, [f"{i}\t{m}\t{s}\t{v}" for i, _, _, s, m, v, _, _ in conds], work, f"wdrc_{fit}", wdrc=spec)
        with Pool(args.workers, initializer=_init, initargs=(base,)) as pool:
            sc = dict(pool.map(_score, [(i, s, str(work)) for i, _, _, s, _, _, _, _ in conds]))
        for i, cond, t, s, _, _, sin, g in conds:
            r = res[i]
            row = {"fit": fit, "scene": s, "condition": cond, "target": t, "static_gain_db": g,
                   "sbr_in_db": sin, **{k: float(r[k]) for k in WDRC_COLS}}
            row["sbr_wdrc_change_db"] = row["sbr_out_db"] - sin
            row.update(pm.with_gains(sc[i], ref_sc[s]))
            rows.append(row)
        for i, *_ in conds:
            for suffix in ("_dialogue.wav", "_background.wav"):
                (work / f"{i}{suffix}").unlink(missing_ok=True)
        # Against WDRC alone, same fitting and scene.
        for s in SCENES:
            w = next(r for r in rows if r["fit"] == fit and r["scene"] == s and r["condition"] == "wdrc_only")
            for r in rows:
                if r["fit"] == fit and r["scene"] == s:
                    for k in ["sbr_out_db", "esii", "static_sii", "estoi", "bg_level_change_db", "wdrc_mean_gr_db"]:
                        r[f"{k}_vs_wdrc"] = r[k] - w[k]
        for r in rows:
            if r["fit"] == fit:
                print(f"  {fit:<4} {r['scene']:<14} {r['condition']:<24} {r['target']:<3} SBR in {r['sbr_in_db']:+6.2f} "
                      f"at ear {r['sbr_out_db']:+6.2f}  WDRC GR {-r['wdrc_mean_gr_db']:5.2f}  out range "
                      f"{r['out_range_db']:4.1f}  ESII {r['esii']:.3f} ({r['esii_vs_wdrc']:+.3f} vs WDRC)  "
                      f"ESTOI {r['estoi']:.3f}  bg {r['bg_level_change_db']:+6.2f} dB", flush=True)

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows({k: (float(v) if isinstance(v, np.floating) else v) for k, v in r.items()} for r in rows)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
