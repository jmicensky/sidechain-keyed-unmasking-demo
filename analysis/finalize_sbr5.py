#!/usr/bin/env python3
"""Rounds the +5.0 dB (Construction) search winners to slider precision.

Reads analysis/sbr5_{basic,comp,combined}_best.tsv and writes
analysis/sbr5_settings.tsv (input to sbr_matched_search --eval/--render/--report).
Rounding moves the SBR slightly, so the rounded setting then gets one 0.1 dB-step
trim (-1.0 to +1.0 dB) on the same thresholds the LL Dynamics transfer trims -
the duck threshold (Basic, Combined) or the four background compressor
thresholds together (Group compression) - whichever lands Construction closest
to +5.00 dB.

    python3 analysis/finalize_sbr5.py <sbr_matched_search binary>
"""
import subprocess
import sys
import csv
from pathlib import Path

A = Path(__file__).resolve().parent
MODES = ["basic", "comp", "combined"]


def rounding(name):
    if name.endswith("_ratio"):
        return 2
    if name.endswith("release_ms"):
        return 0
    return 1


def dims_for(mode):
    with open(A / f"sbr5_{mode}.csv") as f:
        header = next(csv.reader(f))
    names = []
    for h in header[2:]:
        if h.startswith(("construction_", "publictransit_", "restaurant_")):
            break
        names.append(h)
    return names


TARGET = 5.0
TRIM_DIMS = {
    "basic": ["duck_threshold_db"],
    "comp": ["music_threshold_db", "background_threshold_db", "safety_threshold_db", "other_threshold_db"],
    "combined": ["duck_threshold_db"],
}


def construction_sbr(tool, candidates):
    """candidates: list of (id, mode, values) -> {id: SBR on Construction}."""
    tmp = A / "sbr5_trim.in.tsv"
    tmp.write_text("\n".join(f"{i}\t{m}\tconstruction\t{','.join(repr(v) for v in vals)}"
                             for i, m, vals in candidates) + "\n")
    out = A / "sbr5_trim.tsv"
    subprocess.run([tool, "--repo", str(A.parent), "--only-scene", "construction", "--eval", str(tmp),
                    "--out", str(out)], check=True, stdout=subprocess.DEVNULL)
    with open(out) as f:
        res = {r["id"]: float(r["sbr_db"]) for r in csv.DictReader(f, delimiter="\t")}
    tmp.unlink()
    out.unlink()
    return res


def main():
    tool = sys.argv[1]
    lines = []
    for mode in MODES:
        _, _, values = (A / f"sbr5_{mode}_best.tsv").read_text().strip().split("\t")
        names = dims_for(mode)
        vals = [round(float(v), rounding(n)) for n, v in zip(names, values.split(","))]
        cands = []
        for k in range(-10, 11):
            trimmed = [round(v + 0.1 * k, 1) if n in TRIM_DIMS[mode] else v for n, v in zip(names, vals)]
            cands.append((f"{mode}_{k}", mode, trimmed))
        sbr = construction_sbr(tool, cands)
        best = min(cands, key=lambda c: (abs(sbr[c[0]] - TARGET), abs(int(c[0].split("_")[-1]))))
        k = int(best[0].split("_")[-1])
        print(f"  {mode:<9} rounded: {sbr[mode + '_0']:+.3f} dB -> trim {0.1 * k:+.1f} dB on {', '.join(TRIM_DIMS[mode])}"
              f" -> {sbr[best[0]]:+.3f} dB")
        lines.append(f"{mode}_T\t{mode}\t" + ",".join(repr(v) for v in best[2]))
    (A / "sbr5_settings.tsv").write_text("\n".join(lines) + "\n")
    print("wrote analysis/sbr5_settings.tsv")


if __name__ == "__main__":
    main()
