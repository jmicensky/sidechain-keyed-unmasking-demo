#!/usr/bin/env python3
"""Latin Hypercube parameter sweep over the Sidechain Compressor's 5
parameters (threshold, ratio, knee, attack, release), searching for
settings that maximize mean margin gain, STOI gain, and psychoacoustic
partial-loudness reduction - and specifically for a "sweet spot" where
STOI/loudness gains are high while margin gain stays comparatively low
(more intelligibility improvement than the raw level-based margin metric
would suggest - a proxy for "more intelligible while remaining
transparent").

Renders both Basic and Advanced (Summed-bus) for every sampled point via
the native CLI's --static mode (Construction Scene, Dialogue priority),
computing all 3 metrics for each render against a single shared
unprocessed baseline (computed once, since it doesn't depend on any swept
parameter). Uses ONE reusable temp WAV file per condition, overwritten
each iteration, rather than keeping N x 2 renders on disk (~9GB+ otherwise
for N=150).

Margin uses the same 0.85-octave-padded band as
compute_metrics.py's MARGIN_MEASUREMENT_PAD_OCTAVES. Partial-loudness uses
the same ISO 532-1 stationary-per-segment/100dB-SPL-calibration approach
as compute_partial_loudness.py - see that script's docstring for the full
methodology caveats (not repeated here).

Writes each row to the output CSV incrementally (flushed after every
point), so a partial run's results are still usable if interrupted.

Usage:
    python3 analysis/run_lhs_sweep.py --n 150 --out analysis/lhs_sweep_results.csv
"""
import argparse
import csv
import subprocess
import sys
import time
import warnings
from pathlib import Path

import numpy as np
from scipy.stats import qmc

sys.path.insert(0, str(Path(__file__).resolve().parent))
warnings.filterwarnings("ignore")  # mosqito/matplotlib import-time deprecation noise
from compute_metrics import (  # noqa: E402
    load_mono, CLASS_RANGES, DEFAULT_SAFETY_GAIN_DB,
    compute_masking_margin, compute_stoi, HAVE_PYSTOI,
)
from mosqito.sq_metrics import loudness_zwst_perseg  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI = REPO_ROOT / "demo_engine_cli"
SCENE = "Construction Scene"
KEY = "Dialogue"
KEY_SLUG = "dialogue"

# Search ranges - matches each control's UI slider/select range where one
# exists (knee 0-24, attack 1-50, release 20-400); threshold and ratio
# extend somewhat beyond what's been manually tested so far (threshold
# -60 to -18 vs. the -48 to -24 explored by hand; ratio up to 10, the
# engine's own clamp ceiling, vs. the UI dropdown's 1.5/4/8 discrete options).
PARAM_RANGES = {
    'threshold_db': (-60.0, -18.0),
    'ratio': (1.5, 10.0),
    'knee_db': (0.0, 24.0),
    'attack_ms': (1.0, 50.0),
    'release_ms': (20.0, 400.0),
}
PARAM_ORDER = ['threshold_db', 'ratio', 'knee_db', 'attack_ms', 'release_ms']

P_REF_PA = 20e-6
P_FULL_SCALE_PA = P_REF_PA * 10 ** (100 / 20)  # 0dBFS = 100dB SPL, matches compute_partial_loudness.py
NPERSEG = 8192
NOVERLAP = 4096
ACTIVE_FRACTION_OF_PEAK = 0.10


def specific_loudness(signal_linear, sr):
    signal_pa = signal_linear * P_FULL_SCALE_PA
    N, N_spec, _bark_axis, time_axis = loudness_zwst_perseg(
        signal_pa, sr, nperseg=NPERSEG, noverlap=NOVERLAP, field_type="free"
    )
    return N, N_spec, time_axis


def fraction_masked(dialogue_specific, masker_specific):
    masked = dialogue_specific <= masker_specific
    masked_loudness = np.sum(np.where(masked, dialogue_specific, 0.0), axis=0)
    total_loudness = np.sum(dialogue_specific, axis=0)
    with np.errstate(invalid="ignore", divide="ignore"):
        frac = np.where(total_loudness > 1e-9, masked_loudness / total_loudness, np.nan)
    return frac


def render(mode, params, out_path):
    cmd = [
        str(CLI), "--static", "--scene", SCENE, "--key", KEY, "--mode", mode,
        "--threshold", str(params['threshold_db']),
        "--ratio", str(params['ratio']),
        "--knee", str(params['knee_db']),
        "--attack", str(params['attack_ms']),
        "--release", str(params['release_ms']),
        "--out", str(out_path),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"CLI failed ({' '.join(cmd)}):\n{result.stderr}")


def evaluate_condition(dry, dry_sr, spec_dialogue, active, low_hz, high_hz,
                        unproc_mean_margin, unproc_stoi, unproc_pct_masked,
                        wav_path):
    mix, mix_sr = load_mono(wav_path)
    n = min(len(dry), len(mix))

    margin = compute_masking_margin(dry, mix, mix_sr, low_hz, high_hz, DEFAULT_SAFETY_GAIN_DB)
    mean_margin_gain = (margin['mean_margin_db'] - unproc_mean_margin) if margin else float('nan')

    stoi_gain = float('nan')
    if HAVE_PYSTOI:
        score, _lag = compute_stoi(dry, mix, mix_sr, extended=True)
        stoi_gain = score - unproc_stoi

    masker_alone = mix[:n] - dry[:n]
    _N, masker_specific, _t = specific_loudness(masker_alone, mix_sr)
    frac = fraction_masked(spec_dialogue, masker_specific)
    pct_masked = 100 * np.nanmean(frac[active])
    masked_reduction_pp = unproc_pct_masked - pct_masked

    return mean_margin_gain, stoi_gain, masked_reduction_pp


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--n', type=int, default=150)
    parser.add_argument('--out', default='analysis/lhs_sweep_results.csv')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--modes', nargs='+', default=['basic', 'advanced'], choices=['basic', 'advanced'])
    args = parser.parse_args()

    if not HAVE_PYSTOI:
        print("WARNING: pystoi not installed - STOI gain will be NaN for every row.", file=sys.stderr)

    sampler = qmc.LatinHypercube(d=5, seed=args.seed)
    unit_samples = sampler.random(n=args.n)
    bounds_lo = np.array([PARAM_RANGES[k][0] for k in PARAM_ORDER])
    bounds_hi = np.array([PARAM_RANGES[k][1] for k in PARAM_ORDER])
    scaled = qmc.scale(unit_samples, bounds_lo, bounds_hi)

    tmp_dir = Path('/tmp/lhs_sweep')
    tmp_dir.mkdir(exist_ok=True)

    print(f"Rendering shared unprocessed baseline...")
    unprocessed_path = tmp_dir / 'unprocessed.wav'
    dummy_params = {'threshold_db': -30.0, 'ratio': 4.0, 'knee_db': 6.0, 'attack_ms': 5.0, 'release_ms': 120.0}
    render('unprocessed', dummy_params, unprocessed_path)

    dry_path = REPO_ROOT / SCENE / 'Dialogue.wav'
    dry, dry_sr = load_mono(dry_path)
    unproc, unproc_sr = load_mono(unprocessed_path)
    low_hz, high_hz = CLASS_RANGES[KEY_SLUG]

    m_unproc = compute_masking_margin(dry, unproc, unproc_sr, low_hz, high_hz, DEFAULT_SAFETY_GAIN_DB)
    unproc_mean_margin = m_unproc['mean_margin_db']
    unproc_stoi = float('nan')
    if HAVE_PYSTOI:
        unproc_stoi, _lag = compute_stoi(dry, unproc, unproc_sr, extended=True)

    print("Computing dry Dialogue specific loudness (shared across all points)...")
    N_dialogue, spec_dialogue, _t = specific_loudness(dry, dry_sr)
    peak_N = np.max(N_dialogue)
    active = N_dialogue > (ACTIVE_FRACTION_OF_PEAK * peak_N)

    n_unproc = min(len(dry), len(unproc))
    unproc_masker_alone = unproc[:n_unproc] - dry[:n_unproc]
    _N, unproc_masker_specific, _t2 = specific_loudness(unproc_masker_alone, unproc_sr)
    unproc_frac = fraction_masked(spec_dialogue, unproc_masker_specific)
    unproc_pct_masked = 100 * np.nanmean(unproc_frac[active])

    print(f"Baseline: mean_margin={unproc_mean_margin:.2f}dB stoi={unproc_stoi:.3f} pct_masked={unproc_pct_masked:.1f}%")
    print(f"Starting sweep: n={args.n} modes={args.modes}\n")

    out_path = REPO_ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = PARAM_ORDER + [f'{m}_{metric}' for m in args.modes for metric in
                                 ('mean_margin_gain_db', 'stoi_gain', 'masked_reduction_pp')]

    t_start = time.time()
    with open(out_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        for i, row_vals in enumerate(scaled):
            params = dict(zip(PARAM_ORDER, row_vals))
            row = dict(params)

            for mode in args.modes:
                cond_path = tmp_dir / f'{mode}.wav'
                render(mode, params, cond_path)
                mg, sg, mr = evaluate_condition(
                    dry, dry_sr, spec_dialogue, active, low_hz, high_hz,
                    unproc_mean_margin, unproc_stoi, unproc_pct_masked, cond_path
                )
                row[f'{mode}_mean_margin_gain_db'] = mg
                row[f'{mode}_stoi_gain'] = sg
                row[f'{mode}_masked_reduction_pp'] = mr

            writer.writerow(row)
            f.flush()

            elapsed = time.time() - t_start
            rate = (i + 1) / elapsed
            eta_min = (args.n - i - 1) / rate / 60 if rate > 0 else float('nan')
            print(f"[{i+1}/{args.n}] thr={params['threshold_db']:.1f} ratio={params['ratio']:.2f} "
                  f"knee={params['knee_db']:.1f} atk={params['attack_ms']:.1f} rel={params['release_ms']:.1f}  "
                  f"| elapsed={elapsed/60:.1f}min ETA={eta_min:.1f}min")

    print(f"\nDone. Saved {args.n} rows to {out_path}")


if __name__ == '__main__':
    main()
