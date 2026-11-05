#!/usr/bin/env python3
"""Speech Intelligibility Index (ANSI S3.5-1997, one-third-octave procedure)
and the Extended SII of Rhebergen & Versfeld (2005).

Sources
-------
* Section 4 of ANSI S3.5-1997 (R2017), ported from the reference MATLAB
  implementation by Hannes Muesch (SII.M, http://sii.to/siito/assets/siimatlab.zip).
  Every table below (band centres, internal noise, standard speech spectrum,
  band importance) is copied from SII.M. It agrees value for value with
  Chas Pavlovic's C kit (SOURCES/SII.C, http://sii.to/siito/assets/DevelopmentKit.zip).
  The sii.to programs may be used freely except in commercially distributed
  software. They are not part of the standard.
  test_sii.py checks this port against the C kit's test cases (TO.TST, TO_1.TST).
* Extended SII: K. S. Rhebergen and N. J. Versfeld, "A Speech Intelligibility
  Index-based approach to predict the speech reception threshold for sentences
  in fluctuating noise for normal-hearing listeners," JASA 117(4), 2181-2192
  (2005), doi:10.1121/1.1861713, pp. 2182-2183.
  - Speech and noise are filtered into bands separately, with a Hilbert
    envelope per band.
  - Rectangular windows that all end at the same instant.
  - An instantaneous SII every 9.4 ms, averaged over frames.

Choices that are ours, not the paper's (decided with the author, 2026-11-04;
see HANDOFF_speech_band_sii.md):
* One-third-octave bands. The paper used 21 critical bands but calls
  one-third-octave "equally valid".
* Window lengths. The paper gives only the ends: 35 ms at the lowest band
  and 9.4 ms at 8 kHz. Here they are interpolated linearly in log-frequency
  between the 160 Hz and 8 kHz bands. The paper's best single-window fit,
  12 ms, is available as `window_ms=12.0`.
* Band importance: average speech (Table 3), not the paper's SPIN.
* Silence gate: frames count only when Dialogue's in-band level is within
  40 dB of its peak, the same rule as compute_metrics.compute_separated_margin.
  The paper used stationary speech-shaped noise as speech and had no pauses.
* Calibration: 0 dBFS = 100 dB SPL, as in compute_partial_loudness.py, so a
  signal with RMS r sits at 100 + 20 log10(r) dB SPL. Levels are taken
  as equivalent free-field levels. There is no binaural correction and the
  threshold is 0 dB HL (the S3.5 defaults).
"""
import numpy as np
from scipy.fft import next_fast_len
from scipy.signal import butter, hilbert, sosfiltfilt

# ---------------------------------------------------------------- tables (SII.M)
# Band centre frequencies, one-third-octave procedure (Table 3).
F_CENTER = np.array([160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000,
                     2500, 3150, 4000, 5000, 6300, 8000], dtype=float)
# Reference internal noise spectrum level (Table 3).
INTERNAL_NOISE = np.array([0.6, -1.7, -3.9, -6.1, -8.2, -9.7, -10.8, -11.9, -12.5, -13.5,
                           -15.4, -17.7, -21.2, -24.2, -25.9, -23.6, -15.8, -7.1])
# Standard speech spectrum level, normal vocal effort (Table 3).
STANDARD_SPEECH_NORMAL = np.array([32.41, 34.48, 34.75, 33.98, 34.59, 34.27, 32.06, 28.30,
                                   25.01, 23.00, 20.15, 17.32, 13.18, 11.55, 9.33, 5.31,
                                   2.59, 1.13])
# Band importance (SII.M BIArr). Column 1 is average speech (Table 3), column 7 is SPIN (Table B.2).
IMPORTANCE = {
    "average": np.array([0.0083, 0.0095, 0.0150, 0.0289, 0.0440, 0.0578, 0.0653, 0.0711,
                         0.0818, 0.0844, 0.0882, 0.0898, 0.0868, 0.0844, 0.0771, 0.0527,
                         0.0364, 0.0185]),
    "spin": np.array([0, 0.0255, 0.0256, 0.036, 0.0362, 0.0514, 0.0616, 0.077, 0.0718,
                      0.0718, 0.1075, 0.0921, 0.1026, 0.0922, 0.0719, 0.0461, 0.0306, 0]),
}
N_BANDS = len(F_CENTER)
# Bandwidth used to convert band level to spectrum level. The standard's slope
# term (Eq. 7, "B + 10 log10(f) - 6.353") is band level written as spectrum
# level + 10 log10(0.2316 f); 10 log10(0.2316) = -6.353 dB. 0.2316 is the exact
# one-third-octave bandwidth ratio 2^(1/6) - 2^(-1/6).
BANDWIDTH_HZ = F_CENTER * (2 ** (1 / 6) - 2 ** (-1 / 6))

FULL_SCALE_DB_SPL = 100.0     # 0 dBFS = 100 dB SPL
ESII_HOP_MS = 9.4             # Rhebergen & Versfeld (2005): an SII every 9.4 ms
ESII_WINDOW_LOW_MS = 35.0     # lowest band
ESII_WINDOW_HIGH_MS = 9.4     # 8 kHz band
ACTIVE_FLOOR_DB_BELOW_PEAK = 40.0  # same gate as compute_separated_margin


# ------------------------------------------------------------------- core (Sec. 4)
def sii_from_spectra(E, N=None, T=None, importance="average"):
    """SII from equivalent speech/noise spectrum levels and threshold (dB HL).

    E, N, T are 18-band arrays; they may also be (n_frames, 18) arrays, in which
    case one SII per row is returned. A direct port of SII.M, Section 4.
    """
    E = np.atleast_2d(np.asarray(E, dtype=float))
    N = np.full_like(E, -50.0) if N is None else np.broadcast_to(np.asarray(N, float), E.shape)
    T = np.zeros_like(E) if T is None else np.broadcast_to(np.asarray(T, float), E.shape)
    I = IMPORTANCE[importance] if isinstance(importance, str) else np.asarray(importance, float)

    V = E - 24.0                                                  # 4.3.2.1 Eq. 5
    B = np.maximum(V, N)                                          # 4.3.2.2
    C = 0.6 * (B + 10 * np.log10(F_CENTER) - 6.353) - 80.0        # 4.3.2.3 Eq. 7
    Z = np.empty_like(E)                                          # 4.3.2.4-5 Eq. 9
    Z[:, 0] = B[:, 0]
    for i in range(1, N_BANDS):
        spread = B[:, :i] + 3.32 * C[:, :i] * np.log10(0.89 * F_CENTER[i] / F_CENTER[:i])
        Z[:, i] = 10 * np.log10(10 ** (0.1 * N[:, i]) + np.sum(10 ** (0.1 * spread), axis=1))
    X = INTERNAL_NOISE + T                                        # 4.4 Eq. 10
    D = np.maximum(Z, X)                                          # 4.5
    L = np.minimum(1.0, 1 - (E - STANDARD_SPEECH_NORMAL - 10) / 160)  # 4.6 Eq. 11
    K = np.clip((E - D + 15) / 30, 0.0, 1.0)                      # 4.7.1 Eq. 12
    S = np.sum(I * L * K, axis=1)                                 # Eq. 13-14
    return S if S.size > 1 else float(S[0])


# ------------------------------------------------------------- signal analysis
def _band_sos(sr):
    """Order-3 Butterworth band-pass per one-third-octave band (6th-order overall)."""
    sos = []
    for fc in F_CENTER:
        lo, hi = fc * 2 ** (-1 / 6), fc * 2 ** (1 / 6)
        sos.append(butter(3, [lo, min(hi, 0.49 * sr)], btype="bandpass", fs=sr, output="sos"))
    return sos


def band_power_envelopes(signal, sr, sos=None):
    """(18, n) instantaneous band power, |Hilbert analytic|^2 of each band, in Pa^2-like
    units of full scale (1.0 = 0 dBFS). Zero-phase filtering keeps speech and noise
    bands aligned with each other and with the input."""
    sos = _band_sos(sr) if sos is None else sos
    n = len(signal)
    n_fft = next_fast_len(n)
    out = np.empty((N_BANDS, n))
    for b, s in enumerate(sos):
        band = sosfiltfilt(s, signal)
        out[b] = np.abs(hilbert(band, N=n_fft)[:n]) ** 2
    return out


def esii_window_ms(window_ms=None):
    """Per-band window length (ms). Default: log-frequency interpolation 35 -> 9.4 ms."""
    if window_ms is not None:
        return np.full(N_BANDS, float(window_ms))
    frac = np.log(F_CENTER / F_CENTER[0]) / np.log(F_CENTER[-1] / F_CENTER[0])
    return ESII_WINDOW_LOW_MS + frac * (ESII_WINDOW_HIGH_MS - ESII_WINDOW_LOW_MS)


def windowed_band_levels(power_env, sr, window_ms=None, hop_ms=ESII_HOP_MS):
    """(n_frames, 18) band levels in dB SPL. Each frame ends at the same sample
    in every band; band b averages the preceding window_ms[b] (rectangular)."""
    w = np.maximum(1, np.round(esii_window_ms(window_ms) * sr / 1000).astype(int))
    hop = max(1, int(round(hop_ms * sr / 1000)))
    n = power_env.shape[1]
    ends = np.arange(w.max(), n + 1, hop)          # first frame: every window full
    csum = np.concatenate([np.zeros((N_BANDS, 1)), np.cumsum(power_env, axis=1)], axis=1)
    mean_pow = np.empty((len(ends), N_BANDS))
    for b in range(N_BANDS):
        mean_pow[:, b] = (csum[b, ends] - csum[b, ends - w[b]]) / w[b]
    return FULL_SCALE_DB_SPL + 10 * np.log10(np.maximum(mean_pow, 1e-20))


def band_to_spectrum_level(band_db):
    return band_db - 10 * np.log10(BANDWIDTH_HZ)


def compute_sii(speech_out, noise_out, sr, window_ms=None, importance="average",
                threshold_hl=None, return_frames=False):
    """Extended and static SII from SEPARATED renders (<prefix>_key.wav / _nonkey.wav).

    Returns a dict:
      esii         mean instantaneous SII over speech-active frames (main value)
      static_sii   SII of the long-term band levels over those same frames
      active_frames, total_frames
    The gate keeps frames where the speech's summed band level (160 Hz-8 kHz
    one-third-octave bands) is within 40 dB of its peak.
    """
    n = min(len(speech_out), len(noise_out))
    sos = _band_sos(sr)
    sp_pow = band_power_envelopes(speech_out[:n], sr, sos)
    nz_pow = band_power_envelopes(noise_out[:n], sr, sos)
    sp_db = windowed_band_levels(sp_pow, sr, window_ms)
    nz_db = windowed_band_levels(nz_pow, sr, window_ms)
    if len(sp_db) == 0:
        return None
    total_db = 10 * np.log10(np.sum(10 ** (sp_db / 10), axis=1))
    active = total_db > total_db.max() - ACTIVE_FLOOR_DB_BELOW_PEAK
    inst = np.atleast_1d(sii_from_spectra(band_to_spectrum_level(sp_db[active]),
                                          band_to_spectrum_level(nz_db[active]),
                                          threshold_hl, importance))
    # Static: long-term (power-mean) band levels over the active frames.
    lt = lambda db: 10 * np.log10(np.mean(10 ** (db[active] / 10), axis=0))
    static = sii_from_spectra(band_to_spectrum_level(lt(sp_db)),
                              band_to_spectrum_level(lt(nz_db)), threshold_hl, importance)
    out = {"esii": float(np.mean(inst)), "static_sii": float(static),
           "active_frames": int(np.sum(active)), "total_frames": int(len(active))}
    if return_frames:
        out["inst_sii"] = inst
    return out


if __name__ == "__main__":
    import argparse
    import time
    from compute_metrics import load_mono

    p = argparse.ArgumentParser(description="Extended + static SII from separated renders")
    p.add_argument("speech_wav")
    p.add_argument("noise_wav")
    p.add_argument("--window-ms", type=float, default=None,
                   help="fixed window for every band (paper's single-window fit: 12)")
    a = p.parse_args()
    s, sr = load_mono(a.speech_wav)
    nz, sr2 = load_mono(a.noise_wav)
    assert sr == sr2
    t0 = time.time()
    r = compute_sii(s, nz, sr, window_ms=a.window_ms)
    print({k: round(v, 4) if isinstance(v, float) else v for k, v in r.items()},
          f"({time.time() - t0:.1f} s)")
