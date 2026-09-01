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
constexpr double kCrossoverLow  = 100.0;   // f_c1
constexpr double kCrossoverMid  = 300.0;   // f_c2
constexpr double kCrossoverHigh = 1800.0;  // f_c3

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
