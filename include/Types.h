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
//   Advanced   - band-limited multiband ducking (300 Hz - 1.8 kHz only)
//   Resonance  - dynamic notch ducking that tracks wherever the key signal's
//                energy actually is (its two loudest, separated spectral
//                peaks - fundamental + next-loudest harmonic), rather than a
//                fixed band. Loosely modeled on resonance suppressors like
//                Soothe2. See ResonanceSuppressor.h.
enum class DuckMode { Basic, Advanced, Resonance };

// Fixed crossover edges for Advanced mode, from Section 3.2 of the paper.
constexpr double kCrossoverLow  = 100.0;   // f_c1
constexpr double kCrossoverMid  = 300.0;   // f_c2
constexpr double kCrossoverHigh = 1800.0;  // f_c3

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
