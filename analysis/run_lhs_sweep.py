#!/usr/bin/env python3
"""Latin Hypercube sweep over the Sidechain Compressor's parameters, scored with
the speech-band metrics (2026-11-04 revision; see HANDOFF_speech_band_sii.md).

Sampled: threshold, ratio, knee, attack, release, and max reduction (3-12 dB).
With --fixed-threshold, threshold is held constant and max reduction stays at
the old 6 dB (the CLI default the earlier sweeps used), so only ratio, knee,
attack and release are sampled.

Every point renders Basic and Advanced (Summed-bus) through `demo_engine_cli
--static --split-out`, and split_metrics.score_split scores each render:
  - speech-band margin, 150 Hz-8 kHz, from separated key/non-key renders;
  - Extended SII (main value) and static SII;
  - ESTOI against the dry key stem.
Each metric is written both absolute and as the change from the unprocessed
mix (`_gain`), which is rendered and scored once per run.

The old metrics (padded-band margin by power subtraction, and mosqito
partial loudness) are gone. Their CSVs (lhs_sweep_results*.csv) are kept,
and the script that wrote them is in git history before this revision.
Outputs go to new files (lhs_speech_*.csv), so the two can't be confused.

Rows are flushed as they are written, so an interrupted run is still usable.
Each run renders into its own temporary folder, so scenes can run in parallel.

Usage:
    python3 analysis/run_lhs_sweep.py --scene construction --n 100
    python3 analysis/run_lhs_sweep.py --scene publictransit --n 100
    python3 analysis/run_lhs_sweep.py --scene construction --n 100 --fixed-threshold -35
"""
import argparse
import csv
import shutil
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from scipy.stats import qmc

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_metrics import CLASS_RANGES, HAVE_PYSTOI, SCENES, load_mono  # noqa: E402
from split_metrics import METRICS, gain_columns, render_split, score_split  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent

# CLI --key expects display names (src/main.cpp printStaticUsage), not slugs.
KEY_LABEL_BY_SLUG = {
    'dialogue': 'Dialogue', 'music': 'Music', 'background': 'Background Noise',
    'safety': 'Safety Alerts', 'other': 'Other',
}

# Search ranges. Knee, attack and release follow the UI slider ranges; threshold
# and ratio extend past what was tested by hand (ratio up to the engine's clamp).
# The old ranges are kept, and max reduction is new (author's decision 2026-11-04).
PARAM_RANGES = {
    'threshold_db': (-60.0, -18.0),
    'ratio': (1.5, 10.0),
    'knee_db': (0.0, 24.0),
    'attack_ms': (1.0, 50.0),
    'release_ms': (20.0, 400.0),
    'max_reduction_db': (3.0, 12.0),
}
PARAM_ORDER = list(PARAM_RANGES)
OLD_MAX_REDUCTION_DB = 6.0   # CLI/Engine.h default, used by every earlier sweep


def cli_args(params):
    return ["--threshold", params['threshold_db'], "--ratio", params['ratio'],
            "--knee", params['knee_db'], "--attack", params['attack_ms'],
            "--release", params['release_ms'], "--max-reduction", params['max_reduction_db']]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--n', type=int, default=100)
    ap.add_argument('--scene', default='construction', choices=['construction', 'publictransit'],
                    help='Restaurant is excluded from these results (see HANDOFF_speech_band_sii.md).')
    ap.add_argument('--key', default='dialogue', choices=sorted(CLASS_RANGES))
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--modes', nargs='+', default=['basic', 'advanced'], choices=['basic', 'advanced'])
    ap.add_argument('--fixed-threshold', type=float, default=None,
                    help='Hold threshold constant (and max reduction at 6 dB); sample ratio, '
                         'knee, attack and release only.')
    ap.add_argument('--out', default=None,
                    help='Default: analysis/lhs_speech_<scene>_<key>[_thr<T>].csv')
    ap.add_argument('--keep-renders', action='store_true', help='Keep the temp folder of WAVs.')
    args = ap.parse_args()

    scene_dir = SCENES[args.scene]['dir']
    key_label = KEY_LABEL_BY_SLUG[args.key]
    if args.out is None:
        tag = f"_thr{abs(args.fixed_threshold):g}" if args.fixed_threshold is not None else ""
        args.out = f"analysis/lhs_speech_{args.scene}_{args.key}{tag}.csv"
    out_path = REPO_ROOT / args.out
    if out_path.exists():
        sys.exit(f"{out_path} already exists; pass a different --out or remove it first.")
    if not HAVE_PYSTOI:
        print("WARNING: pystoi not installed - ESTOI will be NaN.", file=sys.stderr)

    fixed = {}
    if args.fixed_threshold is not None:
        fixed = {'threshold_db': args.fixed_threshold, 'max_reduction_db': OLD_MAX_REDUCTION_DB}
    free = [p for p in PARAM_ORDER if p not in fixed]
    unit = qmc.LatinHypercube(d=len(free), seed=args.seed).random(n=args.n)
    scaled = qmc.scale(unit, [PARAM_RANGES[p][0] for p in free], [PARAM_RANGES[p][1] for p in free])
    points = [{**fixed, **dict(zip(free, row))} for row in scaled]

    tmp = Path(tempfile.mkdtemp(prefix=f"lhs_{args.scene}_"))
    print(f"Scene={scene_dir!r} key={key_label!r} n={args.n} modes={args.modes}")
    print(f"Sampling {free}; fixed {fixed or 'nothing'}; renders in {tmp}")

    dry, dry_sr = load_mono(REPO_ROOT / scene_dir / SCENES[args.scene]['stems'][args.key])
    base = score_split(render_split(scene_dir, key_label, 'unprocessed', tmp / 'unprocessed'), dry, dry_sr)
    print("Unprocessed: " + "  ".join(f"{k}={v:.4f}" for k, v in base.items()))

    fieldnames = PARAM_ORDER + [f"{m}_{k}{g}" for m in args.modes for k in METRICS for g in ("", "_gain")]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    with open(out_path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for i, params in enumerate(points):
            row = {p: params[p] for p in PARAM_ORDER}
            for mode in args.modes:
                prefix = render_split(scene_dir, key_label, mode, tmp / mode, cli_args(params))
                row.update(gain_columns(mode, score_split(prefix, dry, dry_sr), base))
            w.writerow(row)
            f.flush()
            el = time.time() - t0
            eta = el / (i + 1) * (args.n - i - 1) / 60
            summary = "  ".join(f"{m}: margin {row[f'{m}_speech_margin_db_gain']:+.2f} dB, "
                                f"ESII {row[f'{m}_esii_gain']:+.3f}" for m in args.modes)
            print(f"[{i + 1}/{args.n}] {el / (i + 1):.1f} s/pt ETA {eta:.1f} min | {summary}", flush=True)

    if not args.keep_renders:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nDone. {args.n} rows -> {out_path}")


if __name__ == '__main__':
    main()
