#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "Types.h"
#include "Smoother.h"
#include "GainComputer.h"
#include "Crossover.h"
#include "ResonanceSuppressor.h"

namespace demo {

// Per-channel state: its own crossover filterbank per side (Advanced mode
// only needs this when the channel is non-key, but we keep it running
// continuously so filter state stays settled and mute/unmute never clicks)
// plus a smoothed audibility gain for mute/solo. Separate L/R filterbanks
// preserve the stereo image instead of collapsing each stem to mono. The
// notch filters (kNumPeaks per side, cascaded in series) are Resonance
// mode's equivalent of the crossover - every channel gets its own filter
// *state*, but every channel's notches share the same coefficients each
// sample (computed once from the shared SpectralAnalyzer, see
// Engine::process()).
struct ChannelStrip {
    CrossoverFilterbank crossoverL;
    CrossoverFilterbank crossoverR;
    std::array<Biquad, SpectralAnalyzer::kNumPeaks> notchL, notchR;
    Smoother audibleGain; // 0 = silent, 1 = fully audible
    bool muted = false;
    bool soloed = false;
};

// The full signal engine. Owns 5 preloaded stereo stem buffers, one shared
// sidechain detector, and per-channel processing/mixing state.
//
// The sidechain detector runs on a mono sum of the key channel (standard
// sidechain-compressor practice) so a single gain value g(n) is computed
// once per sample and applied identically to both L and R - if L and R each
// ran their own detector, the two channels could duck by different amounts
// and the stereo image would wobble, which is worse than not having stereo
// at all.
//
// Call order per block: fill each channel's raw samples for the block
// (via loadStems once up front), then call process() repeatedly, advancing
// sampleIndex_ internally.
class Engine {
public:
    void prepare(double sampleRate, const CompressorParams& params) {
        sampleRate_ = sampleRate;
        params_ = params;
        gainComputer_.prepare(params_);
        detector_.prepare(sampleRate, params_.attackMs, params_.releaseMs);
        resonanceAnalyzer_.prepare(sampleRate, params_.attackMs, params_.releaseMs);
        safetyGainLinear_ = std::pow(10.0, params_.safetyGainDb / 20.0);

        for (int c = 0; c < kNumClasses; ++c) {
            strips_[c].crossoverL.prepare(sampleRate);
            strips_[c].crossoverR.prepare(sampleRate);
            strips_[c].audibleGain.prepare(sampleRate, kAudibleRampMs);
            strips_[c].audibleGain.reset(1.0); // audible by default
            keyBlend_[c].prepare(sampleRate, kKeyRampMs);
            keyBlend_[c].reset(0.0);
        }
        recomputeAudibility();
    }

    // Load the 5 stereo stem buffers (L and R must all be the same length /
    // sample rate). A mono sum is precomputed once here for the sidechain
    // detector to read from during process().
    void loadStems(const std::array<std::vector<float>, kNumClasses>& left,
                    const std::array<std::vector<float>, kNumClasses>& right) {
        stemsL_ = left;
        stemsR_ = right;
        numFrames_ = stemsL_[0].size();

        for (int c = 0; c < kNumClasses; ++c) {
            stemsMono_[c].resize(numFrames_);
            for (size_t i = 0; i < numFrames_; ++i) {
                stemsMono_[c][i] = 0.5f * (stemsL_[c][i] + stemsR_[c][i]);
            }
        }
    }

    size_t numFrames() const { return numFrames_; }

    void setMode(DuckMode mode) { mode_ = mode; }

    void setUnmaskEnabled(bool enabled) {
        unmaskEnabled_ = enabled;
        updateKeyBlendTargets();
    }

    void setKeyChannel(int channel) {
        keyChannel_ = channel;
        updateKeyBlendTargets();
    }

    void setMute(int channel, bool muted) {
        strips_[channel].muted = muted;
        recomputeAudibility();
    }

    void setSolo(int channel, bool soloed) {
        // Solo is exclusive: engaging one clears any other.
        for (int c = 0; c < kNumClasses; ++c) {
            strips_[c].soloed = (c == channel) ? soloed : false;
        }
        recomputeAudibility();
    }

    // Live sidechain compressor parameter updates. gainComputer_ and the
    // attack/release-driven detectors (detector_, resonanceAnalyzer_) are
    // shared across all three DuckModes, so these apply to Basic/Advanced's
    // single-band detector *and* Resonance mode's per-peak detection with no
    // extra plumbing - there's only ever one set of compressor parameters.
    void setThresholdDb(double thresholdDb) {
        params_.thresholdDb = thresholdDb;
        gainComputer_.setParams(params_);
    }

    void setRatio(double ratio) {
        params_.ratio = ratio;
        gainComputer_.setParams(params_);
    }

    void setAttackMs(double attackMs) {
        params_.attackMs = attackMs;
        detector_.setTimes(params_.attackMs, params_.releaseMs);
        resonanceAnalyzer_.setTimes(params_.attackMs, params_.releaseMs);
    }

    void setReleaseMs(double releaseMs) {
        params_.releaseMs = releaseMs;
        detector_.setTimes(params_.attackMs, params_.releaseMs);
        resonanceAnalyzer_.setTimes(params_.attackMs, params_.releaseMs);
    }

    // Process one block into separate L/R buffers (each must have room for
    // numFramesToProcess samples), matching how Web Audio hands per-channel
    // Float32Arrays to an AudioWorklet rather than interleaved data.
    void process(float* outputL, float* outputR, size_t numFramesToProcess) {
        for (size_t i = 0; i < numFramesToProcess; ++i) {
            if (sampleIndex_ >= numFrames_) {
                outputL[i] = 0.0f;
                outputR[i] = 0.0f;
                continue;
            }

            // --- 1. Sidechain detector: current key channel's mono-summed
            //     raw sample, gated to silence if that channel is muted.
            //     Basic/Advanced use a single-band envelope+gain (g);
            //     Resonance uses the multi-peak spectral analyzer instead
            //     and builds one shared notch coefficient set per peak for
            //     step 2.
            double keyRaw = stemsMono_[keyChannel_][sampleIndex_];
            bool keyMuted = strips_[keyChannel_].muted;
            double detectorInput = keyMuted ? 0.0 : keyRaw;

            double g = 1.0;
            std::array<BiquadCoeffs, SpectralAnalyzer::kNumPeaks> notchCoeffs;
            if (mode_ == DuckMode::Resonance) {
                resonanceAnalyzer_.tick(detectorInput);
                double minGain = 1.0;
                for (int p = 0; p < SpectralAnalyzer::kNumPeaks; ++p) {
                    // A single ~0.5-octave analysis band inherently captures
                    // far less energy than the full-band detector
                    // Basic/Advanced use for the same real signal (energy is
                    // spread across all 32 bands), so the same
                    // CompressorParams threshold would almost never trigger
                    // here without this compensation. +12 dB is an
                    // empirical match (measured against the real
                    // Construction Scene stems) for comparable trigger
                    // sensitivity across modes - not a physically exact
                    // correction.
                    double level = resonanceAnalyzer_.levelDb(p) + kResonanceLevelCompensationDb;
                    double gp = unmaskEnabled_ ? gainComputer_.computeLinearGain(level) : 1.0;
                    resonanceFreq_[p] = resonanceAnalyzer_.freq(p);
                    resonanceGainLinear_[p] = gp;
                    double gainDb = 20.0 * std::log10(std::max(gp, 1e-6));
                    notchCoeffs[p] = computePeakingCoeffs(sampleRate_, resonanceFreq_[p], gainDb, kResonanceQ);
                    minGain = std::min(minGain, gp);
                }
                lastGainLinear_ = minGain; // deepest of the notch cuts, for the single-number readout
            } else {
                double levelDb = detector_.tick(detectorInput);
                g = unmaskEnabled_ ? gainComputer_.computeLinearGain(levelDb) : 1.0;
                lastGainLinear_ = g;
            }

            // --- 2. Per-channel processing + smoothed mix, L and R run
            //     through identical per-sample math (same g, same keyBlend)
            //     but independent filter state so the stereo image survives.
            double mixL = 0.0, mixR = 0.0;
            for (int c = 0; c < kNumClasses; ++c) {
                double rawL = stemsL_[c][sampleIndex_];
                double rawR = stemsR_[c][sampleIndex_];
                double keyBlend = keyBlend_[c].tick(); // 1 = acting as key/dry

                double dryL = rawL * safetyGainLinear_;
                double dryR = rawR * safetyGainLinear_;
                double duckedL, duckedR;

                if (mode_ == DuckMode::Basic) {
                    duckedL = rawL * g;
                    duckedR = rawR * g;
                } else if (mode_ == DuckMode::Advanced) {
                    double bandsL[4], bandsR[4];
                    strips_[c].crossoverL.tick(rawL, bandsL);
                    strips_[c].crossoverR.tick(rawR, bandsR);
                    duckedL = bandsL[0] + bandsL[1] + g * bandsL[2] + bandsL[3];
                    duckedR = bandsR[0] + bandsR[1] + g * bandsR[2] + bandsR[3];
                } else { // Resonance: kNumPeaks cascaded dynamic notches, same coeffs on both sides
                    duckedL = rawL;
                    duckedR = rawR;
                    for (int p = 0; p < SpectralAnalyzer::kNumPeaks; ++p) {
                        strips_[c].notchL[p].setCoeffs(notchCoeffs[p]);
                        strips_[c].notchR[p].setCoeffs(notchCoeffs[p]);
                        duckedL = strips_[c].notchL[p].tick(duckedL);
                        duckedR = strips_[c].notchR[p].tick(duckedR);
                    }
                }

                double outL = keyBlend * dryL + (1.0 - keyBlend) * duckedL;
                double outR = keyBlend * dryR + (1.0 - keyBlend) * duckedR;
                double audible = strips_[c].audibleGain.tick();
                mixL += audible * outL;
                mixR += audible * outR;
            }

            outputL[i] = static_cast<float>(mixL);
            outputR[i] = static_cast<float>(mixR);
            ++sampleIndex_;
        }
    }

    void resetPlayhead() { sampleIndex_ = 0; }
    size_t playhead() const { return sampleIndex_; }

    // Sidechain-computed gain from the most recently processed sample (1.0 =
    // no reduction). In Basic mode this is the gain applied to the whole
    // signal; in Advanced mode it's the gain applied only within the
    // 300 Hz-1.8 kHz ducked band; in Resonance mode it's the deepest of the
    // notch cuts (see kCrossoverMid/kCrossoverHigh, and the resonance*()
    // getters below for the full per-notch state).
    double lastGainLinear() const { return lastGainLinear_; }

    // Resonance mode's dynamic notch centers (Hz) and depths (linear gain,
    // 1.0 = no cut) from the most recently processed sample - only
    // meaningful while setMode(DuckMode::Resonance) is active. peakIndex
    // ranges over [0, SpectralAnalyzer::kNumPeaks). Exposed primarily so the
    // UI can draw the notches at their real, moving frequencies instead of
    // a fixed band.
    static constexpr int kNumResonancePeaks = SpectralAnalyzer::kNumPeaks;
    double resonanceFreq(int peakIndex) const { return resonanceFreq_[peakIndex]; }
    double resonanceGainLinear(int peakIndex) const { return resonanceGainLinear_[peakIndex]; }

private:
    static constexpr double kAudibleRampMs = 8.0;
    static constexpr double kKeyRampMs = 30.0;
    // Notch bandwidth applied in Resonance mode. RBJ peaking-filter Q, so
    // lower = wider/gentler. 1.2 is roughly a 1.1-octave-wide cut, versus
    // the earlier 4.0's ~0.36 octaves (a much more surgical notch).
    static constexpr double kResonanceQ = 1.2;
    static constexpr double kResonanceLevelCompensationDb = 12.0; // see process()

    void recomputeAudibility() {
        bool anySoloed = false;
        for (int c = 0; c < kNumClasses; ++c) anySoloed |= strips_[c].soloed;

        for (int c = 0; c < kNumClasses; ++c) {
            bool audible = anySoloed ? strips_[c].soloed : !strips_[c].muted;
            strips_[c].audibleGain.setTarget(audible ? 1.0 : 0.0);
        }
    }

    void updateKeyBlendTargets() {
        for (int c = 0; c < kNumClasses; ++c) {
            bool isKey = unmaskEnabled_ && (c == keyChannel_);
            keyBlend_[c].setTarget(isKey ? 1.0 : 0.0);
        }
    }

    double sampleRate_ = 48000.0;
    DuckMode mode_ = DuckMode::Advanced;
    bool unmaskEnabled_ = false;
    int keyChannel_ = kSafetyAlerts;
    double safetyGainLinear_ = 1.0;
    CompressorParams params_;

    EnvelopeFollower detector_;
    GainComputer gainComputer_;
    SpectralAnalyzer resonanceAnalyzer_;

    std::array<ChannelStrip, kNumClasses> strips_;
    std::array<Smoother, kNumClasses> keyBlend_;
    std::array<std::vector<float>, kNumClasses> stemsL_;
    std::array<std::vector<float>, kNumClasses> stemsR_;
    std::array<std::vector<float>, kNumClasses> stemsMono_; // sidechain detector input only
    size_t numFrames_ = 0;
    size_t sampleIndex_ = 0;
    double lastGainLinear_ = 1.0;
    std::array<double, SpectralAnalyzer::kNumPeaks> resonanceFreq_{};
    std::array<double, SpectralAnalyzer::kNumPeaks> resonanceGainLinear_{};
};

} // namespace demo
