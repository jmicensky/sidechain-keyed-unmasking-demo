#!/usr/bin/env python3
"""Render one setting with `demo_engine_cli --static --split-out` and score it.

Used by run_lhs_sweep.py, run_resonance_sweep.py and
compute_folder_export_metrics.py, so every sweep computes the speech-band
metrics the same way:

  speech_margin_db / speech_worst_margin_db
      compute_metrics.compute_separated_margin on <prefix>_key.wav /
      <prefix>_nonkey.wav, over SPEECH_BAND_HZ (150 Hz-8 kHz).
  esii / static_sii
      sii.compute_sii on the same pair (Extended SII is the main value).
  estoi
      Extended STOI of the full mix against the dry key stem, as before
      (compute_metrics.compute_stoi, with lag search).

The separated files are aligned with each other (the key output comes from a
second render with the non-key stems zeroed), so margin and SII need no lag
search, even through Resonance mode's 2048-sample STFT delay.
"""
import subprocess
from pathlib import Path

from compute_metrics import (HAVE_PYSTOI, SPEECH_BAND_HZ, compute_separated_margin,
                             compute_stoi, load_mono)
from sii import compute_sii

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI = REPO_ROOT / "demo_engine_cli"

METRICS = ("speech_margin_db", "speech_worst_margin_db", "estoi", "esii", "static_sii")


def render_split(scene_dir, key_label, mode, prefix, extra_args=()):
    """Write <prefix>_mix.wav, <prefix>_key.wav and <prefix>_nonkey.wav."""
    prefix = Path(prefix)
    cmd = [str(CLI), "--static", "--scene", scene_dir, "--key", key_label, "--mode", mode,
           "--out", f"{prefix}_mix.wav", "--split-out", str(prefix), *map(str, extra_args)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"CLI failed ({' '.join(cmd)}):\n{r.stderr}")
    return prefix


RESONANCE_LATENCY_SAMPLES = 2048  # ResonanceSuppressor.h kLatencySamples (STFT size)


def score_split(prefix, dry, dry_sr, with_estoi=True, mix_latency_samples=0):
    """Score a split render against the dry key stem. Returns a dict keyed by METRICS.

    mix_latency_samples trims the front of the mix before ESTOI; pass
    RESONANCE_LATENCY_SAMPLES for Resonance mode. compute_stoi's lag search
    only reaches +/-20 ms, short of the 42.7 ms STFT delay. Margin and SII
    need no trim: key and non-key are delayed together.
    """
    key, sr = load_mono(f"{prefix}_key.wav")
    nonkey, sr2 = load_mono(f"{prefix}_nonkey.wav")
    assert sr == sr2 == dry_sr, (sr, sr2, dry_sr)
    out = dict.fromkeys(METRICS, float("nan"))
    m = compute_separated_margin(key, nonkey, sr, SPEECH_BAND_HZ)
    if m:
        out["speech_margin_db"] = m["mean_margin_db"]
        out["speech_worst_margin_db"] = m["worst_margin_db"]
    s = compute_sii(key, nonkey, sr)
    if s:
        out["esii"] = s["esii"]
        out["static_sii"] = s["static_sii"]
    if with_estoi and HAVE_PYSTOI:
        mix, _ = load_mono(f"{prefix}_mix.wav")
        out["estoi"], _lag = compute_stoi(dry, mix[mix_latency_samples:], sr, extended=True)
    return out


def gain_columns(mode, scores, baseline):
    """Absolute and change-from-unprocessed columns for one mode."""
    row = {}
    for k in METRICS:
        row[f"{mode}_{k}"] = scores[k]
        row[f"{mode}_{k}_gain"] = scores[k] - baseline[k]
    return row
