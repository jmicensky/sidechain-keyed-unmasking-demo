#pragma once
#include <array>
#include <cstddef>

namespace demo {

// The five predetermined classes / buses from the thesis demo design.
enum ClassIndex : int {
    kDialogue = 0,
    kMusic = 1,
    kBackgroundNoise = 2,
    kSafetyAlerts = 3,
    kOther = 4,
    kNumClasses = 5
};

inline const char* classIndexToName(int c) {
    switch (c) {
        case kDialogue:        return "Dialogue";
        case kMusic:           return "Music";
        case kBackgroundNoise: return "Background Noise";
        case kSafetyAlerts:    return "Safety Alerts";
        case kOther:           return "Other";
        default:               return "Unknown";
    }
}

// Three ducking modes:
//   Basic      - full-spectrum sidechain compression (single band, no crossover)
//   Advanced   - band-limited multiband ducking: only the currently-unmasked
//                (key) channel's own frequency range gets ducked in the
//                other channels - see kUnmaskFrequencyRanges below. The
//                ducked band moves with the key channel selection, not fixed.
//   Resonance  - dynamic notch ducking that tracks wherever the key signal's
//                energy actually is (its two loudest, separated spectral
//                peaks - fundamental + next-loudest harmonic), rather than a
//                fixed band. Loosely modeled on resonance suppressors like
//                Soothe2. See ResonanceSuppressor.h.
enum class DuckMode { Basic, Advanced, Resonance };

// Fixed crossover edges used only by the standalone crossover-flatness self-
// check (see main.cpp's checkCrossoverFlatness()) and by the WDRC output
// compressor's fixed band split (see WdrcCompressor.h) - NOT by Advanced
// duck mode, which uses kUnmaskFrequencyRanges below instead.
//
// 6 bands, not the originally-targeted 12: this project's literature
// review didn't turn up one citable "the" channel count for commercial
// hearing aids (they vary roughly 8-24+ by manufacturer/model), and a
// build/measure pass found this engine's crossover architecture - N-1
// nested Linkwitz-Riley 4th-order two-way splits - genuinely cannot
// support 12 bands within a realistic 100 Hz-8 kHz range without audible
// coloration: each individual LR4 split reconstructs its own input
// essentially exactly, but small per-split ripple compounds across nested
// stages, and 12 log-spaced bands in this range only allows ~0.63 octaves
// between crossover points (measured max deviation 10.7 dB - both a linear
// cascade and a balanced binary tree were tried, same order of magnitude
// either way, so it's a spacing problem inherent to this filter approach,
// not a fixable topology bug). Measured deviation vs. band count at this
// same frequency range (4->0.14dB, 5->0.66dB, 6->1.50dB, 7->2.58dB,
// 8->3.94dB, ..., 12->10.69dB) put 6 bands right at the same ~1.5dB
// coloration the original 4-band/100-300-1800Hz version already shipped
// with and was accepted - a real improvement over 4 bands within this
// architecture's actual limits. A true 12-band (or higher) implementation
// would need a different filterbank principle entirely - e.g. FFT/STFT
// bin-grouping, which reconstructs exactly regardless of band count (see
// FFT.h / ResonanceSuppressor.h for the STFT infrastructure this project
// already has for Resonance duck mode) - not attempted here.
//
// The 5 crossover points below are log-uniformly spaced from 100 Hz to
// 8 kHz (~1.58 octaves apart, close to the original 100/300 Hz pair's own
// spacing), leaving band 0 open below 100 Hz and band 5 open from 8 kHz to
// Nyquist, the same "outer bands open-ended" convention the original
// 3-point/4-band version used.
constexpr int kWdrcNumBands = 6;
constexpr double kWdrcCrossoverHz[kWdrcNumBands - 1] = {
    100.0, 299.1, 894.4, 2675.0, 8000.0,
};

// One class's approximate frequency range, used by Advanced duck mode to
// decide which band to duck: whichever class is currently the unmask/key
// channel, the OTHER channels get ducked specifically within that class's
// own range (kUnmaskFrequencyRanges[keyChannel]), not a fixed band -
// "focus the ducking on the specific frequency range of the unmasked
// channel" is the whole point of Advanced mode's multiband design.
struct UnmaskFreqRange {
    double lowHz, highHz;
};

// Indexed by ClassIndex.
inline constexpr UnmaskFreqRange kUnmaskFrequencyRanges[kNumClasses] = {
    {400.0, 7000.0},   // kDialogue: voice
    {75.0, 12000.0},   // kMusic
    {60.0, 2000.0},    // kBackgroundNoise
    {300.0, 2000.0},   // kSafetyAlerts
    {700.0, 12000.0},  // kOther
};

// How Advanced duck mode's within-range gain is computed:
//   SummedBus   - one shared GainComputer (the "Sidechain Compressor" panel's
//                 Threshold/Ratio/Knee) drives every non-key channel's
//                 in-range content identically. Mathematically equivalent to
//                 summing the non-key channels into one bus and compressing
//                 that once (gain is linear, so per-channel-then-sum and
//                 sum-then-compress give the same result when every channel
//                 shares one gain) - the default, and the lighter-weight
//                 option (effectively one compressor).
//   PerChannel  - each of the 5 channels gets its own threshold/ratio (still
//                 driven by the same shared sidechain detector level/knee/
//                 attack/release - only the compression law's threshold and
//                 ratio go independent per channel).
enum class AdvancedDuckingMode { SummedBus, PerChannel };

// Fixed compressor parameters, from Section 3.3 (Eq. 2), matched to the
// literature values discussed for the paper (Kowalewski et al. 2018 /
// Chen et al. 2021 style fast-acting release, adjust here if the paper's
// final chosen values differ).
struct CompressorParams {
    double thresholdDb = -30.0;   // T
    double ratio       = 4.0;     // R
    double kneeDb       = 6.0;    // W
    double attackMs     = 5.0;    // tau_A
    double releaseMs    = 120.0;  // tau_R
    double safetyGainDb = 0.0;    // g_safety, applied to the key/dry path
};

} // namespace demo
