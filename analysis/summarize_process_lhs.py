#!/usr/bin/env python3
"""Summary of the process LHS sweeps (process_lhs_<process>.csv, from
run_process_lhs.py): reachable SBR range and metric ranges per process and
scene, correlation of each metric gain with SBR gain, and Spearman rank
correlation of each parameter with SBR gain and Extended SII gain.

    python3 analysis/summarize_process_lhs.py
Writes analysis/process_lhs_summary.txt.
"""
import csv
from pathlib import Path

import numpy as np
from scipy.stats import pearsonr, spearmanr

HERE = Path(__file__).resolve().parent
PROCESSES = ["comp", "compmb", "basic", "perchannel"]
NAMES = {"comp": "Group single-band", "compmb": "Group multiband", "basic": "Basic sidechain", "perchannel": "Per-channel sidechain"}
SCENES = ["construction", "publictransit"]
COLS = [("sbr_gain_db", "SBR gain dB"), ("bg_level_change_db", "bg level dB"), ("esii_gain", "ESII gain"),
        ("static_sii_gain", "static SII gain"), ("estoi_gain", "ESTOI gain")]
NON_PARAMS = {"process", "point", "scene", "sbr_db", "sbr_gain_db", "match_gain_db"}


def main():
    out = []
    p = out.append
    for proc in PROCESSES:
        with open(HERE / f"process_lhs_{proc}.csv") as f:
            rows = list(csv.DictReader(f))
        params = list(rows[0])[3:list(rows[0]).index("sbr_db")]  # columns between scene and sbr_db
        p(f"\n=== {NAMES[proc]} ({proc}): {len(rows) // len(SCENES)} points per scene")
        for s in SCENES:
            r = [x for x in rows if x["scene"] == s]
            v = {k: np.array([float(x[k]) for x in r]) for k, _ in COLS}
            p(f"  {s}")
            for k, label in COLS:
                a = v[k]
                p(f"    {label:<16} min {a.min():+7.3f}  median {np.median(a):+7.3f}  max {a.max():+7.3f}")
            for k, label in COLS[1:]:
                p(f"    Pearson r(SBR gain, {label}) = {pearsonr(v['sbr_gain_db'], v[k])[0]:+.3f}")
            p("    Spearman rho of each parameter with SBR gain / ESII gain:")
            for k in params:
                x = np.array([float(row[k]) for row in r])
                p(f"      {k:<28} {spearmanr(x, v['sbr_gain_db'])[0]:+.2f} / {spearmanr(x, v['esii_gain'])[0]:+.2f}")
    text = "\n".join(out)
    (HERE / "process_lhs_summary.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
