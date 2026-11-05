#!/usr/bin/env python3
"""Scores the Johnson & Healy SBR-matched setting of each process, and the
volume-only baseline next to them (2026-11-05 restructure; see the
"Restructure" section of HANDOFF_speech_band_sii.md).

Settings: analysis/sbr_matched_2scene_<process>_best.tsv (searched on
Construction + Public Transit). Conditions per scene and target (HI/NH):
  <process>               the matched setting, rendered by sbr_matched_search
  static_match_<process>  volume only: one static gain on the background sum
                          that gives the same SBR as <process> in that scene
  static                  volume only, landing exactly on the J&H target
SBR here is the gated SBR the search matched on (sbr_matched_search); a
static gain moves it dB for dB, so the static gains are exact. All conditions
are scored by process_metrics after loudness matching to the unprocessed mix.

    python3 analysis/score_matched_processes.py --tool <sbr_matched_search> --work <scratch>
        [--processes basic perchannel comp]
Writes analysis/process_matched.csv.
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
import process_metrics as pm  # noqa: E402

SCENES = ["construction", "publictransit"]
TARGETS = {"HI": 5.1, "NH": 12.2}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--processes", nargs="+", default=["basic", "perchannel", "comp"])
    ap.add_argument("--out", default=str(HERE / "process_matched.csv"))
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    lines, specs = [f"unproc_{s}\tnone\t{s}\t0" for s in SCENES], []
    for proc in args.processes:
        for line in (HERE / f"sbr_matched_2scene_{proc}_best.tsv").read_text().splitlines():
            if not line.strip():
                continue
            label, mode, values = line.split("\t")[:3]
            target = label.rsplit("_", 1)[1]
            for s in SCENES:
                specs.append((proc, target, s, f"{label}_{s}"))
                lines.append(f"{label}_{s}\t{mode}\t{s}\t{values}")
    tsv, res = work / "matched.in.tsv", work / "matched.out.tsv"
    tsv.write_text("\n".join(lines) + "\n")
    subprocess.run([args.tool, "--repo", str(REPO), "--scenes", ",".join(SCENES), "--eval", str(tsv),
                    "--out", str(res), "--split-dir", str(work)], check=True, stdout=subprocess.DEVNULL)
    with open(res) as f:
        sbr = {r["id"]: float(r["sbr_db"]) for r in csv.DictReader(f, delimiter="\t")}

    rows = []

    def add(scene, cond, target, sbr_db, sc, base_sc, static_gain_db=""):
        row = {"scene": scene, "condition": cond, "target": target, "sbr_db": sbr_db,
               "sbr_gain_db": sbr_db - sbr[f"unproc_{scene}"], "static_gain_db": static_gain_db,
               **pm.with_gains(sc, base_sc)}
        rows.append({k: (float(v) if isinstance(v, np.floating) else v) for k, v in row.items()})
        print(f"  {scene:<14} {cond:<24} {target:<3} SBR {sbr_db:+6.2f}  bg {sc['bg_level_change_db']:+6.2f} dB  "
              f"ESII {row['esii_gain']:+.3f}  static SII {row['static_sii_gain']:+.3f}  ESTOI {row['estoi_gain']:+.3f}",
              flush=True)

    for s in SCENES:
        dw, bw = work / f"unproc_{s}_dialogue.wav", work / f"unproc_{s}_background.wav"
        base = pm.baseline(dw, bw)
        d, sr = pm.load_stereo(dw)
        b, _ = pm.load_stereo(bw)
        base_sc = pm.score_arrays(d, b, sr, base)
        s0 = sbr[f"unproc_{s}"]
        add(s, "unprocessed", "", s0, base_sc, base_sc)
        for t, val in TARGETS.items():
            g = s0 - val
            add(s, "static", t, val, pm.score_arrays(d, b * 10 ** (g / 20), sr, base), base_sc, g)
        for proc, t, scene, rid in specs:
            if scene != s:
                continue
            add(s, proc, t, sbr[rid], pm.score(work / f"{rid}_dialogue.wav", work / f"{rid}_background.wav", base),
                base_sc)
            g = s0 - sbr[rid]
            add(s, f"static_match_{proc}", t, sbr[rid], pm.score_arrays(d, b * 10 ** (g / 20), sr, base), base_sc, g)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
