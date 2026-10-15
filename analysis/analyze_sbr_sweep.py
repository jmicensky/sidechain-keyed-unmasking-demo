#!/usr/bin/env python3
"""Summarizes sbr_sweep.cpp's CSVs against the Johnson & Healy (2024) targets.

For each sweep (basic / enhancement), scene, and target (HI +5.1 dB, NH
+12.2 dB): how many points land on target, the best point, and - among
on-target points - the one doing the least processing. Also finds the
single setting that best fits all three scenes at once, and how strongly
each parameter drives the SBR change (Pearson r against SBR gain).

Standard library only (the analysis venv with numpy is not required).

    python3 analysis/analyze_sbr_sweep.py > analysis/sbr_sweep_findings.txt
"""
import csv
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCENES = ["construction", "publictransit", "restaurant"]
TARGETS = {"HI": (5.1, 4.7, 5.8), "NH": (12.2, 2.8, 22.4)}
ON_TARGET_DB = 0.5  # |SBR - target| tolerance for "on target"


def load(name):
    with open(HERE / f"sbr_sweep_{name}.csv") as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def pearson(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy) if sxx and syy else 0.0


def fmt_params(row, names):
    return ", ".join(f"{n}={row[n]:.1f}" if abs(row[n]) >= 10 else f"{n}={row[n]:.2f}" for n in names)


def cost(row, sweep, scene):
    """Lower = less processing, used to pick among on-target points."""
    if sweep == "basic":
        return row[f"{scene}_engaged_frac"]          # least time ducking
    return -row[f"{scene}_mean_others_gain_db"] - row[f"{scene}_mean_dialogue_gain_db"]  # least total gain reduction


def report(sweep, rows, baseline):
    param_names = [k for k in rows[0] if k != "point" and not any(k.startswith(s + "_") and k.split("_", 1)[1]
                   in ("sbr_db", "mean_duck_db", "engaged_frac", "mean_others_gain_db", "mean_dialogue_gain_db",
                       "makeup_only_sbr_db", "drc_shift_db") for s in SCENES)]
    print("=" * 78)
    print(f"SWEEP: {sweep}  ({len(rows)} Latin Hypercube points)")
    print("=" * 78)

    for scene in SCENES:
        sbrs = [r[f"{scene}_sbr_db"] for r in rows]
        print(f"\n--- {scene}: unprocessed SBR {baseline[scene]:+.2f} dB; swept range "
              f"{min(sbrs):+.2f} to {max(sbrs):+.2f} dB")
        for tname, (t, lo, hi) in TARGETS.items():
            on = [r for r in rows if abs(r[f"{scene}_sbr_db"] - t) <= ON_TARGET_DB]
            in_window = sum(lo <= r[f"{scene}_sbr_db"] <= hi for r in rows)
            best = min(rows, key=lambda r: abs(r[f"{scene}_sbr_db"] - t))
            print(f"  {tname} target {t:+.1f} dB: {len(on)} points within +-{ON_TARGET_DB} dB, "
                  f"{in_window} inside J&H window [{lo}, {hi}]")
            if on:
                pick = min(on, key=lambda r: cost(r, sweep, scene))
                extra = (f"engaged {100 * pick[f'{scene}_engaged_frac']:.0f}%, mean duck "
                         f"{pick[f'{scene}_mean_duck_db']:.1f} dB" if sweep == "basic" else
                         f"DRC-only shift {pick[f'{scene}_drc_shift_db']:+.2f} dB "
                         f"(makeup alone gives {pick[f'{scene}_makeup_only_sbr_db']:+.2f} dB)")
                print(f"    least-processing on-target point #{int(pick['point'])}: SBR "
                      f"{pick[f'{scene}_sbr_db']:+.2f} dB, {extra}")
                print(f"      {fmt_params(pick, param_names)}")
                # Same, but with the static gains held near 0 dB, so the SBR change is
                # produced by the dynamics rather than by makeup / key gain.
                static = [r for r in on if max(abs(r[n]) for n in param_names
                                                if n.endswith("key_gain_db") or n.endswith("makeup_db")) <= 0.5]
                if static:
                    pick = min(static, key=lambda r: cost(r, sweep, scene))
                    print(f"    ...with all key/makeup gains within +-0.5 dB: point #{int(pick['point'])}, SBR "
                          f"{pick[f'{scene}_sbr_db']:+.2f} dB")
                    print(f"      {fmt_params(pick, param_names)}")
                else:
                    print("    ...no on-target point has all key/makeup gains within +-0.5 dB")
            else:
                print(f"    closest point #{int(best['point'])}: SBR {best[f'{scene}_sbr_db']:+.2f} dB")

    print("\n--- One setting for all three scenes (minimize worst-scene |SBR - target|)")
    for tname, (t, _, _) in TARGETS.items():
        best = min(rows, key=lambda r: max(abs(r[f"{s}_sbr_db"] - t) for s in SCENES))
        worst = max(abs(best[f"{s}_sbr_db"] - t) for s in SCENES)
        per = ", ".join(f"{s} {best[f'{s}_sbr_db']:+.2f}" for s in SCENES)
        print(f"  {tname} {t:+.1f} dB: point #{int(best['point'])}, worst-scene error {worst:.2f} dB ({per})")
        print(f"      {fmt_params(best, param_names)}")

    print("\n--- What drives SBR gain (Pearson r of each parameter vs. SBR - unprocessed, mean of 3 scenes)")
    gains = [sum(r[f"{s}_sbr_db"] - baseline[s] for s in SCENES) / 3 for r in rows]
    rs = sorted(((pearson([r[p] for r in rows], gains), p) for p in param_names), key=lambda x: -abs(x[0]))
    for rv, p in rs:
        print(f"  {p:<24} r = {rv:+.3f}")

    if sweep == "enhancement":
        print("\n--- How much of the SBR change is the compression itself (vs. the +-2 dB makeup gains)")
        for scene in SCENES:
            shifts = sorted(r[f"{scene}_drc_shift_db"] for r in rows)
            print(f"  {scene:<14} DRC-only shift: min {shifts[0]:+.2f}, median {shifts[len(shifts) // 2]:+.2f}, "
                  f"max {shifts[-1]:+.2f} dB")
    print()


def main():
    baseline = {"construction": 1.31, "publictransit": -4.50, "restaurant": -2.71}
    print("Speech-to-background ratio sweeps vs. Johnson & Healy (2024) optimal SBR")
    print("SBR = speech-active RMS(processed Dialogue) / RMS(processed sum of the other 4 stems)")
    print("Targets: HI +5.1 dB (95% window 4.7-5.8), NH +12.2 dB (95% window 2.8-22.4)")
    print(f"'On target' = within +-{ON_TARGET_DB} dB. Unprocessed SBRs (same gate): "
          + ", ".join(f"{k} {v:+.2f} dB" for k, v in baseline.items()) + "\n")
    for sweep in ("basic", "enhancement"):
        path = HERE / f"sbr_sweep_{sweep}.csv"
        if path.exists():
            report(sweep, load(sweep), baseline)


if __name__ == "__main__":
    main()
