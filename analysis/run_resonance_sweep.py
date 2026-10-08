#!/usr/bin/env python3
"""Latin Hypercube parameter sweep over Resonance duck mode's 7 parameters:
the same 5 Sidechain Compressor parameters Basic/Advanced use (threshold,
ratio, knee, attack, release - Resonance reuses this exact GainComputer/
EnvelopeFollower-driven law per peak, see Engine.h's setAttackMs()/
setReleaseMs()), plus Resonance's own num_peaks (1-12, how many spectral
peaks it tracks) and bandwidth_octaves (0.05-4.0, how wide each peak's
gain-reduction bump is).

Motivation: a hunch that a SMALL number of WIDE-bandwidth peaks (1-2 bands,
each covering a wide swath of spectrum) might work more like a moving-band
multiband compressor - tracking wherever the key's energy currently is,
rather than a fixed class-specific band the way Advanced mode does - than
Resonance mode's current default framing (several narrow, precise notches,
e.g. 4 peaks x 1.16 octaves). This sweep searches num_peaks x
bandwidth_octaves jointly with the shared compressor parameters to see
where Resonance mode's own optimum actually sits, rather than assuming the
hunch is right going in.

Renders Resonance mode only (no Basic/Advanced comparison here - see
run_lhs_sweep.py for that) via the native CLI's --static mode (added in
this same session - main.cpp's --static path did not support
--mode resonance before this script existed).

IMPORTANT METRIC CAVEAT (already flagged in HANDOFF.md before this script
was written): mean margin gain is measured on the priority class's fixed
nominal frequency band (CLASS_RANGES/kUnmaskFrequencyRanges), the same as
Basic/Advanced. Resonance mode does NOT duck within a fixed band - it
ducks wherever the key signal's own spectral peaks currently are, which
may or may not overlap that nominal band at any given moment. Margin gain
is still computed here for a consistent point of comparison against
Basic/Advanced's own numbers, but treat it as a weaker signal for
Resonance specifically than it is for Basic/Advanced. STOI gain and
partial-loudness reduction are both computed on the full broadband signal
(no band restriction), so they don't have this caveat.

Usage:
    python3 analysis/run_resonance_sweep.py --n 100
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
    load_mono, CLASS_RANGES, SCENES, DEFAULT_SAFETY_GAIN_DB,
    compute_masking_margin, compute_stoi, HAVE_PYSTOI,
)
from mosqito.sq_metrics import loudness_zwst_perseg  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI = REPO_ROOT / "demo_engine_cli"

KEY_LABEL_BY_SLUG = {
    'dialogue': 'Dialogue', 'music': 'Music', 'background': 'Background Noise',
    'safety': 'Safety Alerts', 'other': 'Other',
}

SCENE = None
KEY = None
KEY_SLUG = None

# Shared-compressor ranges match run_lhs_sweep.py's PARAM_RANGES exactly,
# for comparability. num_peaks capped at 6 (not the engine's full 1-12
# ceiling) to keep the search focused on the "few wide bands" hypothesis
# region vs. a handful of denser alternatives, rather than diluting it
# across the engine's whole range including settings nobody's proposing;
# bandwidth spans its full valid range (0.05-4.0 octaves) since "how wide"
# is the other half of the hypothesis being tested.
PARAM_RANGES = {
    'threshold_db': (-60.0, -18.0),
    'ratio': (1.5, 10.0),
    'knee_db': (0.0, 24.0),
    'attack_ms': (1.0, 50.0),
    'release_ms': (20.0, 400.0),
    'num_peaks': (1.0, 6.0),
    'bandwidth_octaves': (0.05, 4.0),
}
PARAM_ORDER = ['threshold_db', 'ratio', 'knee_db', 'attack_ms', 'release_ms',
               'num_peaks', 'bandwidth_octaves']

P_REF_PA = 20e-6
P_FULL_SCALE_PA = P_REF_PA * 10 ** (100 / 20)
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
    # dialogue_specific is precomputed once from the full-length dry signal
    # (see main()); masker_specific is shorter for Resonance-mode
    # conditions specifically, since evaluate_condition() trims the mix's
    # front by the algorithmic latency before this is ever called - so
    # these two won't always already be the same frame count, unlike in
    # run_lhs_sweep.py (Basic/Advanced never trim mix, so it never needed
    # this truncation).
    n_frames = min(dialogue_specific.shape[-1], masker_specific.shape[-1])
    dialogue_specific = dialogue_specific[..., :n_frames]
    masker_specific = masker_specific[..., :n_frames]
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
    if mode == 'resonance':
        cmd += [
            "--peaks", str(int(round(params['num_peaks']))),
            "--bandwidth", str(params['bandwidth_octaves']),
        ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"CLI failed ({' '.join(cmd)}):\n{result.stderr}")


def evaluate_condition(dry, dry_sr, spec_dialogue, active, low_hz, high_hz,
                        unproc_mean_margin, unproc_stoi, unproc_pct_masked,
                        wav_path):
    mix, mix_sr = load_mono(wav_path)

    # Resonance mode has a fixed, deterministic algorithmic latency of
    # exactly kFftSize samples (ResonanceSuppressor.h's kLatencySamples,
    # Engine.h's resonanceLatencySamples()) - confirmed directly by a wide
    # (+/-100ms) cross-correlation search independently landing on exactly
    # this value. compute_stoi()'s own lag search only covers +/-20ms
    # (compute_metrics.py's estimate_lag_samples default), which does NOT
    # reach 2048 samples (42.7ms @ 48kHz) - and compute_masking_margin() has
    # no lag correction at all. Without this shift, every metric below
    # compares temporally-misaligned signals, which is what produced the
    # first version of this sweep's nonsensical results (margin gain far
    # exceeding anything Basic/Advanced ever produced, STOI/loudness
    # uniformly and severely negative regardless of settings) - not a real
    # finding about Resonance mode's quality. Trimming mix's front by this
    # exact known amount (rather than a generic cross-correlation search,
    # which risks locking onto the wrong peak in a wider window) is more
    # precise since the true value is deterministic, not content-dependent.
    RESONANCE_LATENCY_SAMPLES = 2048
    mix = mix[RESONANCE_LATENCY_SAMPLES:]

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
    # active's frame count matches the full-length dry signal (computed
    # once in main()); frac may be shorter after the latency trim above, so
    # truncate active to match before using it as a boolean mask.
    pct_masked = 100 * np.nanmean(frac[active[:len(frac)]])
    masked_reduction_pp = unproc_pct_masked - pct_masked

    return mean_margin_gain, stoi_gain, masked_reduction_pp


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--n', type=int, default=100)
    parser.add_argument('--out', default=None,
                         help='Defaults to analysis/resonance_sweep_results_<scene>_<key>.csv')
    parser.add_argument('--scene', default='construction', choices=sorted(SCENES.keys()))
    parser.add_argument('--key', default='dialogue', choices=sorted(CLASS_RANGES.keys()))
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    global SCENE, KEY, KEY_SLUG
    SCENE = SCENES[args.scene]['dir']
    KEY = KEY_LABEL_BY_SLUG[args.key]
    KEY_SLUG = args.key

    if args.out is None:
        args.out = f'analysis/resonance_sweep_results_{args.scene}_{args.key}.csv'

    if not HAVE_PYSTOI:
        print("WARNING: pystoi not installed - STOI gain will be NaN for every row.", file=sys.stderr)

    d = len(PARAM_ORDER)
    sampler = qmc.LatinHypercube(d=d, seed=args.seed)
    unit_samples = sampler.random(n=args.n)
    bounds_lo = np.array([PARAM_RANGES[k][0] for k in PARAM_ORDER])
    bounds_hi = np.array([PARAM_RANGES[k][1] for k in PARAM_ORDER])
    scaled = qmc.scale(unit_samples, bounds_lo, bounds_hi)

    tmp_dir = Path('/tmp/resonance_sweep')
    tmp_dir.mkdir(exist_ok=True)

    print(f"Scene={SCENE!r} Key={KEY!r} (slug={KEY_SLUG})")
    print(f"Rendering shared unprocessed baseline...")
    unprocessed_path = tmp_dir / 'unprocessed.wav'
    dummy_params = {'threshold_db': -30.0, 'ratio': 4.0, 'knee_db': 6.0, 'attack_ms': 5.0,
                     'release_ms': 120.0, 'num_peaks': 4.0, 'bandwidth_octaves': 1.16}
    render('unprocessed', dummy_params, unprocessed_path)

    dry_path = REPO_ROOT / SCENE / SCENES[args.scene]['stems'][args.key]
    dry, dry_sr = load_mono(dry_path)
    unproc, unproc_sr = load_mono(unprocessed_path)
    low_hz, high_hz = CLASS_RANGES[KEY_SLUG]

    m_unproc = compute_masking_margin(dry, unproc, unproc_sr, low_hz, high_hz, DEFAULT_SAFETY_GAIN_DB)
    unproc_mean_margin = m_unproc['mean_margin_db']
    unproc_stoi = float('nan')
    if HAVE_PYSTOI:
        unproc_stoi, _lag = compute_stoi(dry, unproc, unproc_sr, extended=True)

    print(f"Computing dry {KEY} specific loudness (shared across all points)...")
    N_dialogue, spec_dialogue, _t = specific_loudness(dry, dry_sr)
    peak_N = np.max(N_dialogue)
    active = N_dialogue > (ACTIVE_FRACTION_OF_PEAK * peak_N)

    n_unproc = min(len(dry), len(unproc))
    unproc_masker_alone = unproc[:n_unproc] - dry[:n_unproc]
    _N, unproc_masker_specific, _t2 = specific_loudness(unproc_masker_alone, unproc_sr)
    unproc_frac = fraction_masked(spec_dialogue, unproc_masker_specific)
    unproc_pct_masked = 100 * np.nanmean(unproc_frac[active])

    print(f"Baseline: mean_margin={unproc_mean_margin:.2f}dB stoi={unproc_stoi:.3f} pct_masked={unproc_pct_masked:.1f}%")
    print(f"Starting sweep: n={args.n} mode=resonance\n")

    out_path = REPO_ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = PARAM_ORDER + ['mean_margin_gain_db', 'stoi_gain', 'masked_reduction_pp']

    t_start = time.time()
    with open(out_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        for i, row_vals in enumerate(scaled):
            params = dict(zip(PARAM_ORDER, row_vals))
            params['num_peaks'] = float(int(round(params['num_peaks'])))  # log the rounded int actually rendered
            row = dict(params)

            cond_path = tmp_dir / 'resonance.wav'
            render('resonance', params, cond_path)
            mg, sg, mr = evaluate_condition(
                dry, dry_sr, spec_dialogue, active, low_hz, high_hz,
                unproc_mean_margin, unproc_stoi, unproc_pct_masked, cond_path
            )
            row['mean_margin_gain_db'] = mg
            row['stoi_gain'] = sg
            row['masked_reduction_pp'] = mr

            writer.writerow(row)
            f.flush()

            elapsed = time.time() - t_start
            rate = (i + 1) / elapsed
            eta_min = (args.n - i - 1) / rate / 60 if rate > 0 else float('nan')
            print(f"[{i+1}/{args.n}] thr={params['threshold_db']:.1f} ratio={params['ratio']:.2f} "
                  f"knee={params['knee_db']:.1f} atk={params['attack_ms']:.1f} rel={params['release_ms']:.1f} "
                  f"peaks={int(params['num_peaks'])} bw={params['bandwidth_octaves']:.2f}oct "
                  f"| elapsed={elapsed/60:.1f}min ETA={eta_min:.1f}min")

    print(f"\nDone. Saved {args.n} rows to {out_path}")


if __name__ == '__main__':
    main()
