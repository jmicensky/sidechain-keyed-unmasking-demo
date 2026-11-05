#!/usr/bin/env python3
"""Speech-band margin, Extended/static SII and ESTOI for every volume-only
comparison condition (2026-11-04; see HANDOFF_speech_band_sii.md).

Reads the split WAVs written by `volume_only --split-dir <dir>`
(<scene>_<condition>[_<target>]_dialogue.wav / _background.wav, at real
levels, not LUFS-matched). Scores them with the same functions the sweeps use,
and writes analysis/volume_only_speech.csv: one row per condition, with each
metric absolute and as the change from that scene's unprocessed condition.
Join it to volume_only_results.csv on scene + condition + target.

    analysis/run_volume_only.sh --split-dir <dir>
    python3 analysis/score_volume_only_speech.py --split-dir <dir>
"""
import argparse
import csv
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from compute_metrics import HAVE_PYSTOI, compute_separated_margin, compute_stoi, load_mono  # noqa: E402
from sii import compute_sii  # noqa: E402

METRICS = ["speech_margin_db", "esii", "static_sii", "estoi"]


def score(dlg, bg, dry, sr):
    m = compute_separated_margin(dlg, bg, sr)
    s = compute_sii(dlg, bg, sr)
    return {"speech_margin_db": m["mean_margin_db"], "esii": s["esii"], "static_sii": s["static_sii"],
            "estoi": compute_stoi(dry, dlg + bg, sr, extended=True)[0] if HAVE_PYSTOI else float("nan")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--split-dir", required=True)
    ap.add_argument("--out", default=str(HERE / "volume_only_speech.csv"))
    args = ap.parse_args()
    split = Path(args.split_dir)

    with open(HERE / "volume_only_results.csv") as f:
        conds = [(r["scene"], r["condition"], r["target"]) for r in csv.DictReader(f)]
    rows, base, dry = [], {}, {}
    for scene, cond, target in conds:
        stem = f"{scene}_{cond}" + (f"_{target}" if target else "")
        d, sr = load_mono(split / f"{stem}_dialogue.wav")
        b, _ = load_mono(split / f"{stem}_background.wav")
        if scene not in dry:   # the unprocessed condition comes first per scene
            assert cond == "unprocessed", f"expected unprocessed first for {scene}, got {cond}"
            dry[scene] = d
        sc = score(d, b, dry[scene], sr)
        if cond == "unprocessed":
            base[scene] = sc
        row = {"scene": scene, "condition": cond, "target": target}
        for k in METRICS:
            row[k] = sc[k]
            row[f"{k}_gain"] = sc[k] - base[scene][k]
        rows.append(row)
        print(f"{stem:<40} margin {row['speech_margin_db_gain']:+6.2f} dB  ESII {row['esii_gain']:+.3f}  "
              f"static SII {row['static_sii_gain']:+.3f}  ESTOI {row['estoi_gain']:+.3f}", flush=True)
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
