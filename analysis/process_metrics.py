#!/usr/bin/env python3
"""Shared scoring for the process comparison (2026-11-05 restructure; see the
"Restructure" section of HANDOFF_speech_band_sii.md).

Every processed render is first loudness-matched to the unprocessed mix of
its scene (one gain on Dialogue and background together, integrated loudness
per ITU-R BS.1770-4), so processes without makeup gain (group compression)
are judged at the same playback level as the rest. Then:

  bg_level_change_db       background (non-Dialogue) bus loudness after the
                           match, minus the unprocessed background's
  dialogue_level_change_db the same for Dialogue
  esii, static_sii         sii.compute_sii on the matched Dialogue/background
  estoi                    pystoi Extended STOI, dry Dialogue vs matched mix
  speech_margin_db         compute_separated_margin (secondary; not in the
                           main tables)
  match_gain_db            the loudness-match gain that was applied

SBR is not computed here (the render tool reports it, and a common gain does
not change it). Inputs are the stereo float WAVs `sbr_matched_search --eval
--split-dir` writes: <id>_dialogue.wav and <id>_background.wav.
"""
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import lfilter

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from compute_metrics import HAVE_PYSTOI, compute_separated_margin, compute_stoi  # noqa: E402
from sii import compute_sii  # noqa: E402

METRICS = ["bg_level_change_db", "dialogue_level_change_db", "esii", "static_sii", "estoi",
           "speech_margin_db"]
# Metrics whose change from unprocessed is reported as <name>_gain. The two
# level changes are already relative to unprocessed.
GAIN_METRICS = ["esii", "static_sii", "estoi", "speech_margin_db"]

# BS.1770-4 K-weighting at 48 kHz (same coefficients as sbr_matched_search.cpp).
_K1 = ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
_K2 = ([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])


def integrated_lufs(x, sr):
    """Integrated loudness of a (n,) or (n, ch) signal, BS.1770-4 gating."""
    if sr != 48000:
        raise ValueError("K-weighting coefficients are for 48 kHz only")
    x = np.atleast_2d(np.asarray(x, dtype=np.float64).T).T
    z = lfilter(*_K2, lfilter(*_K1, x, axis=0), axis=0)
    p = np.cumsum(np.concatenate([np.zeros((1, z.shape[1])), z ** 2]), axis=0)
    block, hop = 19200, 4800
    starts = np.arange(0, len(z) - block + 1, hop)
    power = ((p[starts + block] - p[starts]) / block).sum(axis=1)
    loud = -0.691 + 10 * np.log10(np.maximum(power, 1e-30))
    keep = loud > -70
    if not keep.any():
        return -np.inf
    rel = -0.691 + 10 * np.log10(power[keep].mean()) - 10
    keep &= loud > rel
    return -0.691 + 10 * np.log10(power[keep].mean())


def load_stereo(path):
    x, sr = sf.read(str(path), dtype="float32", always_2d=True)
    return x.astype(np.float64), sr


def baseline(dialogue_wav, background_wav):
    """Reference levels and dry Dialogue for a scene, from its unprocessed split render."""
    d, sr = load_stereo(dialogue_wav)
    b, _ = load_stereo(background_wav)
    return {"sr": sr, "dry": d.mean(axis=1), "mix_lufs": integrated_lufs(d + b, sr),
            "bg_lufs": integrated_lufs(b, sr), "dlg_lufs": integrated_lufs(d, sr)}


def score(dialogue_wav, background_wav, base, with_estoi=True):
    d, sr = load_stereo(dialogue_wav)
    b, _ = load_stereo(background_wav)
    return score_arrays(d, b, sr, base, with_estoi)


def score_arrays(d, b, sr, base, with_estoi=True):
    """As score(), from stereo (n, 2) Dialogue and background arrays."""
    g_db = base["mix_lufs"] - integrated_lufs(d + b, sr)
    g = 10 ** (g_db / 20)
    out = {"match_gain_db": g_db,
           "bg_level_change_db": integrated_lufs(b, sr) + g_db - base["bg_lufs"],
           "dialogue_level_change_db": integrated_lufs(d, sr) + g_db - base["dlg_lufs"]}
    dm, bm = g * d.mean(axis=1), g * b.mean(axis=1)
    del d, b
    s = compute_sii(dm, bm, sr)
    out["esii"], out["static_sii"] = s["esii"], s["static_sii"]
    out["speech_margin_db"] = compute_separated_margin(dm, bm, sr)["mean_margin_db"]
    out["estoi"] = (compute_stoi(base["dry"], dm + bm, sr, extended=True)[0]
                    if with_estoi and HAVE_PYSTOI else float("nan"))
    return out


def with_gains(scores, base_scores):
    row = dict(scores)
    for k in GAIN_METRICS:
        row[f"{k}_gain"] = scores[k] - base_scores[k]
    return row
