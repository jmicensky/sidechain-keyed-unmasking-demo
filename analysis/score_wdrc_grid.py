#!/usr/bin/env python3
"""How the strength of the output WDRC fitting relates to the SBR it costs
(2026-11-08; see "WDRC chain test" in HANDOFF_speech_band_sii.md).

Grid of fittings: threshold -60/-50/-40/-30 dBFS x ratio 1.5/2.5/3.5 x
release 80 ms (fast) / 1000 ms (slow); knee 6 dB, attack 5 ms, makeup 0 dB.
Chains, through each fitting: WDRC alone; Basic and group single-band (the
matched HI/NH settings); and their static-gain matches (same SBR into the
WDRC). Renderer-only measures (no SII/ESTOI): SBR into and at the ear, WDRC
mean gain reduction, and the mix's dynamic range into and out of the WDRC
(99th-30th pct of 125 ms levels). The question is whether the SBR lost through
the WDRC follows mean gain reduction or dynamic-range reduction.

    python3 analysis/score_wdrc_grid.py --tool <sbr_matched_search> --work <dir>
Writes analysis/wdrc_grid.csv and analysis/wdrc_grid_summary.txt.
"""
import argparse
import csv
import itertools
import subprocess
from pathlib import Path

import numpy as np
from scipy.stats import pearsonr, spearmanr

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SCENES = ["construction", "publictransit"]
PROCESSES = ["basic", "comp"]
THRESHOLDS = [-60, -50, -40, -30]
RATIOS = [1.5, 2.5, 3.5]
RELEASES = {"fast": 80, "slow": 1000}


def run_tool(tool, lines, work, tag, wdrc=None):
    tsv, out = work / f"{tag}.in.tsv", work / f"{tag}.out.tsv"
    tsv.write_text("\n".join(lines) + "\n")
    cmd = [tool, "--repo", str(REPO), "--scenes", ",".join(SCENES), "--eval", str(tsv), "--out", str(out)]
    if wdrc:
        cmd += ["--wdrc", wdrc]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        return {r["id"]: r for r in csv.DictReader(f, delimiter="\t")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    procs = []
    for proc in PROCESSES:
        for line in (HERE / f"sbr_matched_2scene_{proc}_best.tsv").read_text().splitlines():
            if line.strip():
                label, mode, values = line.split("\t")[:3]
                for s in SCENES:
                    procs.append((proc, label.rsplit("_", 1)[1], s, mode, values))
    pre = run_tool(args.tool, [f"u_{s}\tnone\t{s}\t0" for s in SCENES] +
                   [f"{c}_{t}_{s}\t{m}\t{s}\t{v}" for c, t, s, m, v in procs], work, "pre")
    sbr0 = {s: float(pre[f"u_{s}"]["sbr_db"]) for s in SCENES}
    conds = [(f"wdrc_only_{s}", "wdrc_only", "", s, "none", "0") for s in SCENES]
    for c, t, s, m, v in procs:
        sin = float(pre[f"{c}_{t}_{s}"]["sbr_db"])
        conds.append((f"{c}_{t}_{s}", c, t, s, m, v))
        conds.append((f"static_match_{c}_{t}_{s}", f"static_match_{c}", t, s, "static", repr(sbr0[s] - sin)))
    sbr_in = {i: (sbr0[s] if m == "none" else float(pre[f"{c.replace('static_match_', '')}_{t}_{s}"]["sbr_db"]))
              for i, c, t, s, m, _ in conds}

    rows = []
    for speed, thr, ratio in itertools.product(RELEASES, THRESHOLDS, RATIOS):
        fit = f"{thr},{ratio},6,5,{RELEASES[speed]}"
        res = run_tool(args.tool, [f"{i}\t{m}\t{s}\t{v}" for i, _, _, s, m, v in conds], work, "grid", wdrc=fit)
        for i, c, t, s, _, _ in conds:
            r = res[i]
            rows.append({"speed": speed, "threshold_db": thr, "ratio": ratio, "scene": s, "condition": c,
                         "target": t, "sbr_in_db": sbr_in[i], "sbr_out_db": float(r["sbr_out_db"]),
                         "sbr_loss_db": sbr_in[i] - float(r["sbr_out_db"]),
                         "wdrc_mean_gr_db": -float(r["wdrc_mean_gr_db"]),
                         "in_range_db": float(r["in_range_db"]), "out_range_db": float(r["out_range_db"]),
                         "range_reduction_db": float(r["in_range_db"]) - float(r["out_range_db"])})
        print(f"{speed} thr {thr} ratio {ratio}: done", flush=True)
    with open(HERE / "wdrc_grid.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    out = []
    p = out.append
    p("SBR lost through the WDRC vs fitting strength, across the 24 fittings (and per speed, 12 each)")
    p("columns: Pearson r of SBR loss with mean GR / with range reduction  [Spearman in brackets]")
    groups = sorted({(r["condition"], r["target"], r["scene"]) for r in rows})
    for g in groups:
        for speed in (None, "fast", "slow"):
            rr = [r for r in rows if (r["condition"], r["target"], r["scene"]) == g and (speed is None or r["speed"] == speed)]
            loss = np.array([r["sbr_loss_db"] for r in rr])
            gr = np.array([r["wdrc_mean_gr_db"] for r in rr])
            rd = np.array([r["range_reduction_db"] for r in rr])
            p(f"  {g[0]:<20} {g[1]:<3} {g[2]:<14} {speed or 'all':<4}  loss {loss.min():+5.2f}..{loss.max():+5.2f} dB  "
              f"r(GR) {pearsonr(gr, loss)[0]:+.2f} [{spearmanr(gr, loss)[0]:+.2f}]  "
              f"r(range red.) {pearsonr(rd, loss)[0]:+.2f} [{spearmanr(rd, loss)[0]:+.2f}]")
    p("\nWDRC alone, per fitting: SBR loss dB / mean GR dB / range reduction dB")
    for s in SCENES:
        p(f"  {s}")
        for speed in RELEASES:
            for thr in THRESHOLDS:
                cells = []
                for ratio in RATIOS:
                    r = next(x for x in rows if x["condition"] == "wdrc_only" and x["scene"] == s and x["speed"] == speed
                             and x["threshold_db"] == thr and x["ratio"] == ratio)
                    cells.append(f"{ratio}:1 {r['sbr_loss_db']:+5.2f}/{r['wdrc_mean_gr_db']:5.2f}/{r['range_reduction_db']:5.2f}")
                p(f"    {speed} thr {thr:>4}  " + "   ".join(cells))
    text = "\n".join(out)
    (HERE / "wdrc_grid_summary.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
