#!/usr/bin/env python3
"""Checks for sii.py. Run: python3 analysis/test_sii.py

1. The S3.5 core against the expected results listed in readme.doc of the sii.to
   C kit (Pavlovic, DevelopmentKit.zip): TO.TST -> 0.445 and TO_1.TST -> 0.438.
   The inputs below are those files' contents.
2. Rhebergen & Versfeld's prerequisite (2005, p. 2183): with stationary
   maskers the Extended SII equals the conventional SII.
"""
import numpy as np

from sii import (F_CENTER, N_BANDS, compute_sii, sii_from_spectra)

# TO.TST: line 1 speech, line 2 noise, line 3 threshold. TO_1.TST adds line 4, the importance.
TO_E = [90, 5, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, -10, -10, -10, -10]
TO_N = [10, -10, -10, 75, -10, -10, -10, -10, -10, -10, -10, -10, -10, -10, 10, 10, 10, 10]
TO_T = [90] + [0] * 17
TO_1_I = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.3, 0]


def test_reference_cases():
    s = sii_from_spectra(TO_E, TO_N, TO_T)
    s1 = sii_from_spectra(TO_E, TO_N, TO_T, importance=TO_1_I)
    print(f"TO.TST   {s:.4f} (expected 0.445)")
    print(f"TO_1.TST {s1:.4f} (expected 0.438)")
    assert round(s, 3) == 0.445
    assert round(s1, 3) == 0.438


def test_esii_equals_sii_for_stationary_signals():
    rng = np.random.default_rng(0)
    sr, dur = 16000, 6.0
    speech = 0.05 * rng.standard_normal(int(sr * dur))   # stationary "speech"
    noise = 0.05 * rng.standard_normal(int(sr * dur))    # stationary masker, 0 dB SNR
    r = compute_sii(speech, noise, sr)
    print(f"stationary: ESII {r['esii']:.4f}, static {r['static_sii']:.4f}")
    assert abs(r["esii"] - r["static_sii"]) < 0.01


def test_esii_exceeds_static_for_interrupted_masker():
    # Interrupted noise at 8 Hz, 50% duty: listening in the gaps, so ESII > static SII.
    rng = np.random.default_rng(1)
    sr, dur = 16000, 6.0
    n = int(sr * dur)
    speech = 0.05 * rng.standard_normal(n)
    gate = (np.floor(np.arange(n) / sr * 16) % 2 == 0).astype(float)
    noise = 0.1 * rng.standard_normal(n) * gate
    r = compute_sii(speech, noise, sr)
    print(f"interrupted: ESII {r['esii']:.4f}, static {r['static_sii']:.4f}")
    assert r["esii"] > r["static_sii"] + 0.05


if __name__ == "__main__":
    assert N_BANDS == len(F_CENTER) == 18
    test_reference_cases()
    test_esii_equals_sii_for_stationary_signals()
    test_esii_exceeds_static_for_interrupted_masker()
    print("all SII tests passed")
