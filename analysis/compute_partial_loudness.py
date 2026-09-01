#!/usr/bin/env python3
"""Psychoacoustic partial-loudness / masked-threshold estimate for the
parameter sweep in "UnMasking Exports/" (see compute_folder_export_metrics.py
for the masking-margin/STOI metrics this complements).

Uses MOSQITO's ISO 532-1 (Zwicker) stationary loudness model, applied
per-segment (mosqito.sq_metrics.loudness_zwst_perseg) to get specific
loudness N'(bark, t) in sones/bark for:
  - the dry Dialogue stem alone
  - the "masker" (everything else) in each condition, reconstructed by
    time-domain subtraction: masker_alone = mix - dry_dialogue. This is
    exact (not an approximation) for this engine's architecture: the key
    channel (Dialogue, since it's the priority class in every one of these
    runs) always takes the dry/unducked path regardless of duck mode, so
    subtracting it out of any condition's mix isolates exactly what that
    condition did to the other four channels.

METHODOLOGY CAVEATS (read before citing this as a rigorous partial-loudness
result):
  1. Calibration: WAV samples have no absolute SPL reference. This script
     assumes 0 dBFS (full-scale peak) = 100 dB SPL, a common convention for
     "hot" consumer playback level, and converts samples to Pascals
     accordingly (see P_FULL_SCALE_PA). The ISO 532-1 loudness model is
     level-dependent (compressive growth, equal-loudness contours), so the
     *absolute* percentages below are calibration-dependent. The *relative*
     comparison (unprocessed vs. basic vs. advanced, same calibration
     throughout) should be far more robust to this assumption than the
     absolute numbers are.
  2. Stationary vs. time-varying: uses the STATIONARY per-segment method
     (loudness_zwst_perseg) rather than the full ISO 532-1 time-varying
     model (loudness_zwtv), for computational tractability (~2s vs. ~54s
     per 80s file) - each segment is treated as if steady-state, so
     temporal masking/integration effects between segments are not
     modeled.
  3. Masking-threshold proxy: "masked" here means Dialogue's specific
     loudness in a given critical band and time frame is less than or
     equal to the masker's specific loudness in that same band/frame -
     i.e. the masker's own excitation is used as an approximate masking
     threshold. This is NOT the full Moore & Glasberg partial-loudness /
     masking-index model (which also models spread beyond simple
     co-located excitation and an explicit masking-index offset above
     threshold-in-quiet). It is a simpler, directionally-meaningful proxy,
     not a substitute for a validated partial-loudness implementation if
     the paper needs one.

Usage:
    python3 analysis/compute_partial_loudness.py \
        [--exports-dir "UnMasking Exports"] [--out analysis/partial_loudness_results.csv]
"""
import argparse
import csv
import sys
import warnings
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_folder_export_metrics import parse_settings, SCENE_DIR_BY_LABEL, SCENE_STEMS  # noqa: E402

warnings.filterwarnings("ignore")  # mosqito/matplotlib import-time deprecation noise, not from our computation

from mosqito.sq_metrics import loudness_zwst_perseg  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent

# 0 dBFS peak assumed = 100 dB SPL - see module docstring caveat 1.
P_REF_PA = 20e-6
P_FULL_SCALE_PA = P_REF_PA * 10 ** (100 / 20)

NPERSEG = 8192
NOVERLAP = 4096

# Fraction of the dry Dialogue stem's own peak overall loudness N(t) above
# which a frame counts as "active" - see module docstring; analogous to but
# not identical to compute_metrics.py's dB-domain activity gate.
ACTIVE_FRACTION_OF_PEAK = 0.10


def load_mono_f64(path):
    data, sr = sf.read(str(path), dtype="float32")
    mono = data if data.ndim == 1 else data.mean(axis=1)
    return mono.astype(np.float64), sr


def specific_loudness(signal_linear, sr):
    """Returns (N, N_specific, time_axis) for a full-scale-normalized [-1,1] signal."""
    signal_pa = signal_linear * P_FULL_SCALE_PA
    N, N_spec, _bark_axis, time_axis = loudness_zwst_perseg(
        signal_pa, sr, nperseg=NPERSEG, noverlap=NOVERLAP, field_type="free"
    )
    return N, N_spec, time_axis


def fraction_masked(dialogue_specific, masker_specific):
    """Per-frame fraction of Dialogue's specific loudness sitting in bands
    where the masker's specific loudness meets or exceeds it."""
    masked = dialogue_specific <= masker_specific
    masked_loudness = np.sum(np.where(masked, dialogue_specific, 0.0), axis=0)
    total_loudness = np.sum(dialogue_specific, axis=0)
    with np.errstate(invalid="ignore", divide="ignore"):
        frac = np.where(total_loudness > 1e-9, masked_loudness / total_loudness, np.nan)
    return frac


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exports-dir", default="UnMasking Exports")
    parser.add_argument("--out", default="analysis/partial_loudness_results.csv")
    args = parser.parse_args()

    export_dir = Path(args.exports_dir)
    if not export_dir.is_absolute():
        export_dir = REPO_ROOT / export_dir

    folders = sorted(d for d in export_dir.iterdir() if d.is_dir() and (d / "settings.txt").exists())
    if not folders:
        print(f"No export folders found in {export_dir}", file=sys.stderr)
        sys.exit(1)

    # All 9 runs share the same scene/key (Construction/Dialogue) - verified
    # once rather than assumed, since this script hardcodes Dialogue-only.
    first_settings = parse_settings((folders[0] / "settings.txt").read_text())
    scene_dir = SCENE_DIR_BY_LABEL[first_settings["scene_label"]]
    key_label = first_settings["key_label"]
    if key_label != "Dialogue":
        print(f"WARNING: this script assumes Dialogue as priority; first folder has key={key_label}", file=sys.stderr)

    dry_path = REPO_ROOT / scene_dir / SCENE_STEMS[scene_dir][key_label]
    dry, dry_sr = load_mono_f64(dry_path)
    print(f"Computing dry {key_label} specific loudness ({len(dry)/dry_sr:.1f}s)...")
    N_dialogue, spec_dialogue, time_axis = specific_loudness(dry, dry_sr)
    peak_N = np.max(N_dialogue)
    active = N_dialogue > (ACTIVE_FRACTION_OF_PEAK * peak_N)
    print(f"  peak N = {peak_N:.3f} sones, active frames = {np.sum(active)}/{len(active)} "
          f"({100*np.sum(active)/len(active):.1f}%)")

    unprocessed_masker_specific = None  # shared across all runs, computed once
    rows = []

    for folder in folders:
        settings = parse_settings((folder / "settings.txt").read_text())
        row = {"folder": folder.name, **{k: v for k, v in settings.items() if k not in ("scene_label", "key_label")}}
        print(f"\n{folder.name}")

        # unprocessed: identical across every run (same reference blend), compute once
        if unprocessed_masker_specific is None:
            mix, mix_sr = load_mono_f64(folder / "unprocessed.wav")
            n = min(len(mix), len(dry))
            masker_alone = mix[:n] - dry[:n]
            _N, unprocessed_masker_specific, _t = specific_loudness(masker_alone, mix_sr)
        frac = fraction_masked(spec_dialogue, unprocessed_masker_specific)
        row["unprocessed_pct_masked"] = 100 * np.nanmean(frac[active])
        print(f"  unprocessed: {row['unprocessed_pct_masked']:.1f}% of Dialogue's loudness masked")

        for condition, fname in [("basic", "basic.wav"), ("processed", None)]:
            if fname is None:
                matches = sorted(folder.glob("processed_*.wav"))
                if not matches:
                    continue
                fname = matches[0].name
            mix, mix_sr = load_mono_f64(folder / fname)
            n = min(len(mix), len(dry))
            masker_alone = mix[:n] - dry[:n]
            _N, masker_specific, _t = specific_loudness(masker_alone, mix_sr)
            frac = fraction_masked(spec_dialogue, masker_specific)
            row[f"{condition}_pct_masked"] = 100 * np.nanmean(frac[active])
            print(f"  {condition}: {row[f'{condition}_pct_masked']:.1f}% of Dialogue's loudness masked")

        row["masked_reduction_pp"] = row["unprocessed_pct_masked"] - row["processed_pct_masked"]
        print(f"  -> reduction in masked loudness (advanced vs unprocessed): {row['masked_reduction_pp']:+.1f} pp")
        rows.append(row)

    out_path = REPO_ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys())
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
