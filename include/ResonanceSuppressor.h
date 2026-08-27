#pragma once
#include <array>
#include <cmath>
#include "Crossover.h"
#include "GainComputer.h"
#include "Smoother.h"
#include "Types.h"

namespace demo {

// Analyzes a mono signal across a bank of narrow, log-spaced bands to find
// its kNumPeaks loudest, mutually-separated spectral regions - the sidechain
// "detector" for DuckMode::Resonance. Instead of ducking a fixed band (as
// Advanced mode does), this lets the caller duck wherever the key signal's
// energy actually is: its fundamental plus its next few loudest harmonics,
// loosely modeled on resonance suppressors like Soothe2 (which track many
// more peaks via full FFT analysis - this is a deliberately simpler
// fixed-band-count version for the demo).
class SpectralAnalyzer {
public:
    static constexpr int kNumBands = 32;
    static constexpr int kNumPeaks = 4;
    static constexpr double kMinFreqHz = 120.0;
    static constexpr double kMaxFreqHz = 9600.0;
    // Each picked peak must be at least this many bands apart from every
    // other already-picked peak (~0.6 octaves at this band spacing) so each
    // one is a genuinely different resonance, not just the next bin over
    // from one already picked.
    static constexpr int kMinPeakSeparationBands = 3;
    // How fast a reported peak frequency may glide when the loudest band in
    // its slot changes - keeps a moving notch sweeping smoothly instead of
    // jumping between discrete band centers.
    static constexpr double kFreqGlideMs = 100.0;
    static constexpr double kBandQ = 3.0; // analysis-filter width, not the applied notch width

    void prepare(double sampleRate, double attackMs, double releaseMs) {
        for (int b = 0; b < kNumBands; ++b) {
            double t = static_cast<double>(b) / (kNumBands - 1);
            double freq = kMinFreqHz * std::pow(kMaxFreqHz / kMinFreqHz, t);
            bandFreqs_[b] = freq;
            bandFilters_[b].setBandpass(sampleRate, freq, kBandQ);
            bandLevels_[b].prepare(sampleRate, attackMs, releaseMs);
        }
        for (int p = 0; p < kNumPeaks; ++p) {
            // Spread initial positions across the range so they don't all
            // start piled on the same frequency before real signal arrives.
            int seedBand = ((p + 1) * kNumBands) / (kNumPeaks + 1);
            freqLog2_[p].prepare(sampleRate, kFreqGlideMs);
            freqLog2_[p].reset(std::log2(bandFreqs_[seedBand]));
            freq_[p] = std::pow(2.0, freqLog2_[p].current());
            levelDb_[p] = -120.0;
        }
    }

    void tick(double keyInput) {
        for (int b = 0; b < kNumBands; ++b) {
            double filtered = bandFilters_[b].tick(keyInput);
            levels_[b] = bandLevels_[b].tick(filtered);
        }

        std::array<int, kNumPeaks> pickedIdx;
        pickedIdx.fill(-1);
        for (int p = 0; p < kNumPeaks; ++p) {
            int bestIdx = -1;
            double bestLevel = -1e9;
            for (int b = 0; b < kNumBands; ++b) {
                bool tooClose = false;
                for (int q = 0; q < p; ++q) {
                    if (std::abs(b - pickedIdx[q]) < kMinPeakSeparationBands) {
                        tooClose = true;
                        break;
                    }
                }
                if (tooClose) continue;
                if (levels_[b] > bestLevel) {
                    bestLevel = levels_[b];
                    bestIdx = b;
                }
            }
            if (bestIdx < 0) {
                // No band satisfies separation from every previous pick -
                // shouldn't happen at kNumBands=32/kNumPeaks=4, but fall
                // back to the global loudest band rather than leaving this
                // peak undefined.
                bestIdx = 0;
                bestLevel = levels_[0];
                for (int b = 1; b < kNumBands; ++b) {
                    if (levels_[b] > bestLevel) { bestLevel = levels_[b]; bestIdx = b; }
                }
            }

            pickedIdx[p] = bestIdx;
            freqLog2_[p].setTarget(std::log2(bandFreqs_[bestIdx]));
            freq_[p] = std::pow(2.0, freqLog2_[p].tick());
            levelDb_[p] = bestLevel;
        }
    }

    double freq(int peakIndex) const { return freq_[peakIndex]; }
    double levelDb(int peakIndex) const { return levelDb_[peakIndex]; }

    // Live attack/release update, so the UI can change compressor speed
    // without re-preparing (and thereby resetting) the whole analyzer.
    void setTimes(double attackMs, double releaseMs) {
        for (auto& ef : bandLevels_) ef.setTimes(attackMs, releaseMs);
    }

private:
    std::array<double, kNumBands> bandFreqs_{};
    std::array<Biquad, kNumBands> bandFilters_;
    std::array<EnvelopeFollower, kNumBands> bandLevels_;
    std::array<double, kNumBands> levels_{};

    std::array<Smoother, kNumPeaks> freqLog2_;
    std::array<double, kNumPeaks> freq_{};
    std::array<double, kNumPeaks> levelDb_{};
};

} // namespace demo
