#!/usr/bin/env python3
"""Latin Hypercube sweep over Resonance duck mode's 7 parameters, scored with the
speech-band metrics (2026-11-04 revision; see HANDOFF_speech_band_sii.md).

Parameters: the 5 shared Sidechain Compressor controls (threshold, ratio, knee,
attack, release; Resonance applies the same gain law to each peak), plus
num_peaks (1-6) and bandwidth_octaves (0.05-4.0). Max reduction is not sampled
(author's decision); the CLI defaults apply, as in the earlier sweep.

The motivating question is unchanged: do a few wide peaks behave like a
moving-band multiband compressor, following wherever the key's energy is,
better than several narrow notches?

Metrics come from split_metrics.score_split: speech-band margin (150 Hz-8 kHz)
and Extended/static SII from separated key/non-key renders, plus ESTOI.

About the 2048-sample STFT delay: the separated files carry it together, so
margin and SII need no alignment. Only ESTOI, which compares the mix with
the dry stem, trims the mix by RESONANCE_LATENCY_SAMPLES.

The old caveat, that the margin was measured on a fixed class band Resonance
doesn't duck, now matters less: the margin uses the fixed speech band for
every mode, and SII weights the full 160 Hz-8 kHz range by band importance.

The old CSV (resonance_sweep_results_*.csv) is kept; this writes
resonance_speech_<scene>_<key>.csv.

Usage:
    python3 analysis/run_resonance_sweep.py --n 100
"""
import argparse
import csv
import shutil
import sys
import tempfile
import time
from pathlib import Path

from scipy.stats import qmc

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_metrics import CLASS_RANGES, HAVE_PYSTOI, SCENES, load_mono  # noqa: E402
from split_metrics import (METRICS, RESONANCE_LATENCY_SAMPLES, gain_columns,  # noqa: E402
                           render_split, score_split)

REPO_ROOT = Path(__file__).resolve().parent.parent

KEY_LABEL_BY_SLUG = {
    'dialogue': 'Dialogue', 'music': 'Music', 'background': 'Background Noise',
    'safety': 'Safety Alerts', 'other': 'Other',
}

# Shared-compressor ranges match run_lhs_sweep.py. num_peaks is capped at 6 to
# focus on the "few wide bands" hypothesis; bandwidth spans its full valid range.
PARAM_RANGES = {
    'threshold_db': (-60.0, -18.0),
    'ratio': (1.5, 10.0),
    'knee_db': (0.0, 24.0),
    'attack_ms': (1.0, 50.0),
    'release_ms': (20.0, 400.0),
    'num_peaks': (1.0, 6.0),
    'bandwidth_octaves': (0.05, 4.0),
}
PARAM_ORDER = list(PARAM_RANGES)


def cli_args(p):
    return ["--threshold", p['threshold_db'], "--ratio", p['ratio'], "--knee", p['knee_db'],
            "--attack", p['attack_ms'], "--release", p['release_ms'],
            "--peaks", int(p['num_peaks']), "--bandwidth", p['bandwidth_octaves']]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--n', type=int, default=100)
    ap.add_argument('--scene', default='construction', choices=['construction', 'publictransit'])
    ap.add_argument('--key', default='dialogue', choices=sorted(CLASS_RANGES))
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--out', default=None, help='Default: analysis/resonance_speech_<scene>_<key>.csv')
    ap.add_argument('--keep-renders', action='store_true')
    args = ap.parse_args()

    scene_dir = SCENES[args.scene]['dir']
    key_label = KEY_LABEL_BY_SLUG[args.key]
    out_path = REPO_ROOT / (args.out or f"analysis/resonance_speech_{args.scene}_{args.key}.csv")
    if out_path.exists():
        sys.exit(f"{out_path} already exists; pass a different --out or remove it first.")
    if not HAVE_PYSTOI:
        print("WARNING: pystoi not installed - ESTOI will be NaN.", file=sys.stderr)

    unit = qmc.LatinHypercube(d=len(PARAM_ORDER), seed=args.seed).random(n=args.n)
    scaled = qmc.scale(unit, [PARAM_RANGES[p][0] for p in PARAM_ORDER],
                       [PARAM_RANGES[p][1] for p in PARAM_ORDER])
    points = []
    for row in scaled:
        p = dict(zip(PARAM_ORDER, row))
        p['num_peaks'] = float(int(round(p['num_peaks'])))  # log the integer actually rendered
        points.append(p)

    tmp = Path(tempfile.mkdtemp(prefix=f"resonance_{args.scene}_"))
    print(f"Scene={scene_dir!r} key={key_label!r} n={args.n}; renders in {tmp}")
    dry, dry_sr = load_mono(REPO_ROOT / scene_dir / SCENES[args.scene]['stems'][args.key])
    base = score_split(render_split(scene_dir, key_label, 'unprocessed', tmp / 'unprocessed'), dry, dry_sr)
    print("Unprocessed: " + "  ".join(f"{k}={v:.4f}" for k, v in base.items()))

    fieldnames = PARAM_ORDER + [f"resonance_{k}{g}" for k in METRICS for g in ("", "_gain")]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    with open(out_path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for i, p in enumerate(points):
            prefix = render_split(scene_dir, key_label, 'resonance', tmp / 'resonance', cli_args(p))
            scores = score_split(prefix, dry, dry_sr, mix_latency_samples=RESONANCE_LATENCY_SAMPLES)
            row = {**p, **gain_columns('resonance', scores, base)}
            w.writerow(row)
            f.flush()
            el = time.time() - t0
            eta = el / (i + 1) * (args.n - i - 1) / 60
            print(f"[{i + 1}/{args.n}] {el / (i + 1):.1f} s/pt ETA {eta:.1f} min | peaks={int(p['num_peaks'])} "
                  f"bw={p['bandwidth_octaves']:.2f} margin {row['resonance_speech_margin_db_gain']:+.2f} dB "
                  f"ESII {row['resonance_esii_gain']:+.3f}", flush=True)

    if not args.keep_renders:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nDone. {args.n} rows -> {out_path}")


if __name__ == '__main__':
    main()
