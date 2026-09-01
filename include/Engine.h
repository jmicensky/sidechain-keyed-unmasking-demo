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
#include "WdrcCompressor.h"

namespace demo {

// A single-tap fixed delay line, used to time-align the "dry" signal path
// with Resonance mode's STFT processing latency (see Engine::process()) -
// without this, blending an undelayed dry sample against a
// SpectralResonanceSuppressor::kLatencySamples-delayed ducked sample would
// smear the keyBlend crossfade across ~43ms of misalignment.
class DelayLine {
public:
    void prepare(int delaySamples) {
        buf_.assign(std::max(1, delaySamples), 0.0);
        writePos_ = 0;
    }
    double process(double x) {
        double delayed = buf_[writePos_];
        buf_[writePos_] = x;
        writePos_ = (writePos_ + 1) % buf_.size();
        return delayed;
    }

private:
    std::vector<double> buf_;
    size_t writePos_ = 0;
};

// Per-channel state: a 2-point crossover split per side (below the active
// unmask range / within it / above it - see Engine::updateUnmaskRange()),
// kept running continuously so filter state stays settled and mute/unmute
// never clicks, plus a smoothed audibility gain for mute/solo. Separate L/R
// splits preserve the stereo image instead of collapsing each stem to mono.
// Resonance mode's ducking is handled centrally by Engine's single shared
// SpectralResonanceSuppressor instead of per-channel filter state, since
// STFT analysis/resynthesis needs all 10 signals (5 classes x L/R) and the
// shared key spectrum processed together each hop.
struct ChannelStrip {
    LR4Split unmaskHighSplitL, unmaskHighSplitR; // splits off content above the active range
    LR4Split unmaskLowSplitL, unmaskLowSplitR;   // splits the remainder into below/within the range
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
        spectralSuppressor_.prepare(sampleRate, params_.attackMs, params_.releaseMs);
        spectralSuppressor_.setActivePeakCount(resonanceNumPeaks_);
        spectralSuppressor_.setBandwidthOctaves(resonanceBandwidthOctaves_);
        safetyGainLinear_ = std::pow(10.0, params_.safetyGainDb / 20.0);

        wdrcCompressor_.prepare(sampleRate);
        wdrcCompressor_.setThresholdDb(wdrcThresholdDb_);
        wdrcCompressor_.setRatio(wdrcRatio_);
        wdrcCompressor_.setMakeupGainDb(wdrcMakeupGainDb_);
        wdrcCompressor_.setBypassed(wdrcBypassed_);
        wdrcCompressor_.setAttackMs(wdrcAttackMs_);
        wdrcCompressor_.setReleaseMs(wdrcReleaseMs_);

        for (int c = 0; c < kNumClasses; ++c) {
            strips_[c].audibleGain.prepare(sampleRate, kAudibleRampMs);
            strips_[c].audibleGain.reset(1.0); // audible by default
            keyBlend_[c].prepare(sampleRate, kKeyRampMs);
            keyBlend_[c].reset(0.0);
            dryDelayL_[c].prepare(SpectralResonanceSuppressor::kLatencySamples);
            dryDelayR_[c].prepare(SpectralResonanceSuppressor::kLatencySamples);
            // Per-channel Advanced ducking starts matched to the shared
            // "summed bus" values (see setAdvancedDuckingMode()).
            channelThresholdDb_[c] = params_.thresholdDb;
            channelRatio_[c] = params_.ratio;
            syncPerChannelGainComputer(c);
        }
        updateUnmaskRange(keyChannel_); // Advanced mode's ducked band starts matching the default key channel
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
        // Advanced duck mode's ducked band follows the newly-selected key
        // channel's own frequency range - see updateUnmaskRange() and
        // Types.h's kUnmaskFrequencyRanges.
        updateUnmaskRange(channel);
    }

    // Advanced duck mode only - see Types.h's AdvancedDuckingMode doc
    // comment. Switching into PerChannel seeds every channel's independent
    // threshold/ratio from whatever the shared "summed bus" values are at
    // that exact instant, so they start matched and then diverge freely -
    // switching back to SummedBus doesn't erase those per-channel values,
    // it just stops using them until PerChannel is re-engaged (at which
    // point they're reseeded from the shared values again).
    void setAdvancedDuckingMode(AdvancedDuckingMode mode) {
        bool entering = (mode == AdvancedDuckingMode::PerChannel &&
                          advancedDuckingMode_ != AdvancedDuckingMode::PerChannel);
        advancedDuckingMode_ = mode;
        if (entering) {
            for (int c = 0; c < kNumClasses; ++c) {
                channelThresholdDb_[c] = params_.thresholdDb;
                channelRatio_[c] = params_.ratio;
                syncPerChannelGainComputer(c);
            }
        }
    }

    // Per-channel threshold/ratio, only used while
    // setAdvancedDuckingMode(PerChannel) is active (see above) - inert
    // otherwise. Ratio is clamped to [1, 10] (unity to a hard-limiting
    // 10:1), matching the UI's per-channel ratio knob range.
    void setChannelThresholdDb(int channel, double thresholdDb) {
        channelThresholdDb_[channel] = thresholdDb;
        syncPerChannelGainComputer(channel);
    }

    void setChannelRatio(int channel, double ratio) {
        channelRatio_[channel] = std::clamp(ratio, 1.0, 10.0);
        syncPerChannelGainComputer(channel);
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

    // Live sidechain compressor parameter updates. gainComputer_ is shared
    // across all three DuckModes, so these apply to Basic/Advanced's
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

    // Knee width in dB (W in GainComputer's Eq. 2). 0 = hard knee (gain
    // curve bends instantly at the threshold); larger = softer, wider
    // transition centered on the threshold. Shared by both Advanced ducking
    // modes - see Types.h's AdvancedDuckingMode - so the per-channel
    // GainComputers need re-syncing too, even though their threshold/ratio
    // are independent.
    void setKneeDb(double kneeDb) {
        params_.kneeDb = kneeDb;
        gainComputer_.setParams(params_);
        for (int c = 0; c < kNumClasses; ++c) syncPerChannelGainComputer(c);
    }

    // Resonance-mode-only settings (not shared with Basic/Advanced).
    void setResonanceNumPeaks(int count) {
        resonanceNumPeaks_ = std::clamp(count, 1, SpectralResonanceSuppressor::kMaxPeaks);
        spectralSuppressor_.setActivePeakCount(resonanceNumPeaks_);
    }

    // Width (in octaves) of the smooth gain-reduction bump applied around
    // each detected peak. Also re-derives the minimum spacing between peaks
    // so wider bumps automatically push peaks further apart and can't
    // overlap - see SpectralResonanceSuppressor::setBandwidthOctaves().
    void setResonanceBandwidthOctaves(double bandwidthOctaves) {
        resonanceBandwidthOctaves_ = std::clamp(bandwidthOctaves, 0.05, 4.0);
        spectralSuppressor_.setBandwidthOctaves(resonanceBandwidthOctaves_);
    }

    // Ceiling on how deep any single peak may cut, in dB (magnitude - pass
    // 24 for "at most -24dB", not -24).
    void setResonanceMaxReductionDb(double maxReductionDb) {
        resonanceMaxReductionDb_ = std::clamp(maxReductionDb, 0.0, 60.0);
    }

    // Output-bus WDRC compressor (see WdrcCompressor.h) - a separate stage
    // from the sidechain-keyed ducking compressor above. Applied once to
    // the final 5-channel mix, after ducking, not per-channel or per-mode.
    void setWdrcBypassed(bool bypassed) {
        wdrcBypassed_ = bypassed;
        wdrcCompressor_.setBypassed(bypassed);
    }

    void setWdrcThresholdDb(double thresholdDb) {
        wdrcThresholdDb_ = thresholdDb;
        wdrcCompressor_.setThresholdDb(thresholdDb);
    }

    void setWdrcRatio(double ratio) {
        wdrcRatio_ = ratio;
        wdrcCompressor_.setRatio(ratio);
    }

    void setWdrcMakeupGainDb(double makeupGainDb) {
        wdrcMakeupGainDb_ = makeupGainDb;
        wdrcCompressor_.setMakeupGainDb(makeupGainDb);
    }

    // Clamped inside WdrcCompressor to the "common" WDRC literature range
    // (1-50ms attack, 30-3000ms release) - see its setAttackMs()/
    // setReleaseMs() doc comment. Separate from the sidechain compressor's
    // setAttackMs()/setReleaseMs() below - the two stages don't share timing.
    void setWdrcAttackMs(double attackMs) {
        wdrcAttackMs_ = attackMs;
        wdrcCompressor_.setAttackMs(attackMs);
    }

    void setWdrcReleaseMs(double releaseMs) {
        wdrcReleaseMs_ = releaseMs;
        wdrcCompressor_.setReleaseMs(releaseMs);
    }

    void setAttackMs(double attackMs) {
        params_.attackMs = attackMs;
        detector_.setTimes(params_.attackMs, params_.releaseMs);
        spectralSuppressor_.setTimes(params_.attackMs, params_.releaseMs);
    }

    void setReleaseMs(double releaseMs) {
        params_.releaseMs = releaseMs;
        detector_.setTimes(params_.attackMs, params_.releaseMs);
        spectralSuppressor_.setTimes(params_.attackMs, params_.releaseMs);
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
            //     Resonance runs all 10 signals through the shared STFT
            //     suppressor at once (it needs every channel's spectrum
            //     alongside the key's to build/apply one shared per-bin
            //     gain mask each hop - see SpectralResonanceSuppressor).
            double keyRaw = stemsMono_[keyChannel_][sampleIndex_];
            bool keyMuted = strips_[keyChannel_].muted;
            double detectorInput = keyMuted ? 0.0 : keyRaw;

            double g = 1.0;
            // Only set (and only meaningful) outside Resonance mode - the
            // shared sidechain level in dB, used both by the SummedBus gain
            // (g, above) and by Advanced/PerChannel's independent per-
            // channel GainComputers below, which read the same detected
            // level but apply their own threshold/ratio to it.
            double sidechainLevelDb = 0.0;
            std::array<double, SpectralResonanceSuppressor::kNumSignals> spectralOut{};
            std::array<double, SpectralResonanceSuppressor::kNumSignals> dryDelayed{};

            if (mode_ == DuckMode::Resonance) {
                std::array<double, SpectralResonanceSuppressor::kNumSignals> spectralIn;
                for (int c = 0; c < kNumClasses; ++c) {
                    double rawL = stemsL_[c][sampleIndex_];
                    double rawR = stemsR_[c][sampleIndex_];
                    spectralIn[SpectralResonanceSuppressor::signalIndex(c, 0)] = rawL;
                    spectralIn[SpectralResonanceSuppressor::signalIndex(c, 1)] = rawR;
                    // Delayed to match the STFT path's algorithmic latency,
                    // so the dry/ducked keyBlend crossfade stays time-aligned.
                    dryDelayed[SpectralResonanceSuppressor::signalIndex(c, 0)] =
                        dryDelayL_[c].process(rawL * safetyGainLinear_);
                    dryDelayed[SpectralResonanceSuppressor::signalIndex(c, 1)] =
                        dryDelayR_[c].process(rawR * safetyGainLinear_);
                }
                spectralSuppressor_.tick(detectorInput, spectralIn.data(), spectralOut.data(),
                                          gainComputer_, unmaskEnabled_, kResonanceLevelCompensationDb,
                                          resonanceMaxReductionDb_);
                lastGainLinear_ = 1.0;
                for (int p = 0; p < resonanceNumPeaks_; ++p) {
                    lastGainLinear_ = std::min(lastGainLinear_, spectralSuppressor_.gainLinear(p));
                }
            } else {
                sidechainLevelDb = detector_.tick(detectorInput);
                g = unmaskEnabled_ ? gainComputer_.computeLinearGain(sidechainLevelDb) : 1.0;
                lastGainLinear_ = g;
            }

            // In Advanced + PerChannel mode, each non-key channel ducks by
            // its own independent gain instead of the shared g - tracked
            // here (deepest cut among the audibly-ducked, non-key channels)
            // so lastGainLinear_ still reports something meaningful for the
            // meter/gain-viz instead of the now-unused shared g.
            bool perChannelAdvanced = mode_ == DuckMode::Advanced &&
                                       advancedDuckingMode_ == AdvancedDuckingMode::PerChannel &&
                                       unmaskEnabled_;
            double minChannelGain = 1.0;

            // --- 2. Per-channel processing + smoothed mix, L and R run
            //     through identical per-sample math (same g, same keyBlend)
            //     but independent filter state so the stereo image survives.
            double mixL = 0.0, mixR = 0.0;
            for (int c = 0; c < kNumClasses; ++c) {
                double rawL = stemsL_[c][sampleIndex_];
                double rawR = stemsR_[c][sampleIndex_];
                double keyBlend = keyBlend_[c].tick(); // 1 = acting as key/dry

                double dryL, dryR, duckedL, duckedR;

                if (mode_ == DuckMode::Basic) {
                    dryL = rawL * safetyGainLinear_;
                    dryR = rawR * safetyGainLinear_;
                    duckedL = rawL * g;
                    duckedR = rawR * g;
                } else if (mode_ == DuckMode::Advanced) {
                    dryL = rawL * safetyGainLinear_;
                    dryR = rawR * safetyGainLinear_;
                    // Split into below/within/above the active unmask
                    // range (see updateUnmaskRange()); only the within-
                    // range content gets ducked by g, below/above pass
                    // straight through - the ducked band tracks the key
                    // channel's own frequency range instead of being fixed.
                    double loA_L, aboveL, belowL, inRangeL;
                    strips_[c].unmaskHighSplitL.tick(rawL, loA_L, aboveL);
                    strips_[c].unmaskLowSplitL.tick(loA_L, belowL, inRangeL);
                    double loA_R, aboveR, belowR, inRangeR;
                    strips_[c].unmaskHighSplitR.tick(rawR, loA_R, aboveR);
                    strips_[c].unmaskLowSplitR.tick(loA_R, belowR, inRangeR);
                    double gForChannel = g;
                    if (perChannelAdvanced) {
                        gForChannel = perChannelGainComputer_[c].computeLinearGain(sidechainLevelDb);
                        if (c != keyChannel_) minChannelGain = std::min(minChannelGain, gForChannel);
                    }
                    duckedL = belowL + aboveL + gForChannel * inRangeL;
                    duckedR = belowR + aboveR + gForChannel * inRangeR;
                } else { // Resonance
                    dryL = dryDelayed[SpectralResonanceSuppressor::signalIndex(c, 0)];
                    dryR = dryDelayed[SpectralResonanceSuppressor::signalIndex(c, 1)];
                    duckedL = spectralOut[SpectralResonanceSuppressor::signalIndex(c, 0)];
                    duckedR = spectralOut[SpectralResonanceSuppressor::signalIndex(c, 1)];
                }

                double outL = keyBlend * dryL + (1.0 - keyBlend) * duckedL;
                double outR = keyBlend * dryR + (1.0 - keyBlend) * duckedR;
                double audible = strips_[c].audibleGain.tick();
                mixL += audible * outL;
                mixR += audible * outR;
            }
            if (perChannelAdvanced) lastGainLinear_ = minChannelGain;

            // --- 3. Output-bus WDRC compressor, applied once to the final
            //     mix of all 5 channels - independent of duck mode and of
            //     the sidechain-keyed compressor above.
            double finalL, finalR;
            wdrcCompressor_.tick(mixL, mixR, finalL, finalR);

            outputL[i] = static_cast<float>(finalL);
            outputR[i] = static_cast<float>(finalR);
            ++sampleIndex_;
        }
    }

    void resetPlayhead() { sampleIndex_ = 0; }
    size_t playhead() const { return sampleIndex_; }

    // Sidechain-computed gain from the most recently processed sample (1.0 =
    // no reduction). In Basic mode this is the gain applied to the whole
    // signal; in Advanced mode it's the gain applied only within the active
    // unmask range (see unmaskLowHz()/unmaskHighHz()); in Resonance mode
    // it's the deepest of the per-peak cuts (see the resonance*() getters
    // below for the full per-peak state).
    double lastGainLinear() const { return lastGainLinear_; }

    // Resonance mode's dynamic peak centers (Hz) and depths (linear gain,
    // 1.0 = no cut) from the most recently processed hop - only meaningful
    // while setMode(DuckMode::Resonance) is active. peakIndex ranges over
    // [0, resonanceNumPeaks()). Exposed primarily so the UI can draw the
    // notches at their real, moving frequencies instead of a fixed band.
    // kMaxResonancePeaks is the compile-time upper bound (array size);
    // resonanceNumPeaks() is the live, user-set count actually in use.
    static constexpr int kMaxResonancePeaks = SpectralResonanceSuppressor::kMaxPeaks;
    int resonanceNumPeaks() const { return resonanceNumPeaks_; }
    double resonanceFreq(int peakIndex) const { return spectralSuppressor_.freq(peakIndex); }
    double resonanceGainLinear(int peakIndex) const { return spectralSuppressor_.gainLinear(peakIndex); }

    // Resonance mode's fixed algorithmic latency (STFT analysis/resynthesis
    // window), in samples at the engine's prepared sample rate. 0 in
    // Basic/Advanced (they're sample-accurate with no lookahead).
    static constexpr int resonanceLatencySamples() { return SpectralResonanceSuppressor::kLatencySamples; }

    // Deepest per-band cut the WDRC stage applied on the most recently
    // processed sample, in dB (0 = no reduction). Always 0 while bypassed -
    // for a small gain-reduction meter next to the WDRC controls.
    double wdrcGainReductionDb() const { return wdrcCompressor_.lastGainReductionDb(); }

    // Advanced duck mode's currently active ducked band - tracks the key
    // channel (see setKeyChannel()/updateUnmaskRange()). For the UI to show
    // plainly that the band changed along with the key channel selection.
    double unmaskLowHz() const { return unmaskLowHz_; }
    double unmaskHighHz() const { return unmaskHighHz_; }

private:
    static constexpr double kAudibleRampMs = 8.0;
    static constexpr double kKeyRampMs = 30.0;
    // A single ~1-octave-wide analysis bump inherently captures far less
    // energy than the full-band detector Basic/Advanced use for the same
    // real signal, so the same CompressorParams threshold would almost
    // never trigger here without this compensation. +12 dB is an empirical
    // match (measured against the real Construction Scene stems) for
    // comparable trigger sensitivity across modes - not a physically exact
    // correction.
    static constexpr double kResonanceLevelCompensationDb = 12.0;

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

    // Reconfigures every channel's Advanced-mode split points to the given
    // class's frequency range (Types.h's kUnmaskFrequencyRanges), so the
    // ducked band tracks whichever channel is currently unmasked. All 5
    // strips get the same two edges - only the filter *state* is
    // per-channel, matching the reasoning already used for the shared
    // sidechain gain g: the point is ducking every OTHER channel within the
    // key channel's own range, not each channel's own range.
    void updateUnmaskRange(int channel) {
        const UnmaskFreqRange& range = kUnmaskFrequencyRanges[channel];
        unmaskLowHz_ = range.lowHz;
        unmaskHighHz_ = range.highHz;
        for (int c = 0; c < kNumClasses; ++c) {
            strips_[c].unmaskHighSplitL.prepare(sampleRate_, unmaskHighHz_);
            strips_[c].unmaskHighSplitR.prepare(sampleRate_, unmaskHighHz_);
            strips_[c].unmaskLowSplitL.prepare(sampleRate_, unmaskLowHz_);
            strips_[c].unmaskLowSplitR.prepare(sampleRate_, unmaskLowHz_);
        }
    }

    // Pushes channel c's independent threshold/ratio, plus the shared knee
    // (GainComputer has no use for attack/release - those only matter to
    // the shared EnvelopeFollower detector), into its own GainComputer.
    void syncPerChannelGainComputer(int c) {
        CompressorParams p = params_;
        p.thresholdDb = channelThresholdDb_[c];
        p.ratio = channelRatio_[c];
        perChannelGainComputer_[c].setParams(p);
    }

    double sampleRate_ = 48000.0;
    DuckMode mode_ = DuckMode::Advanced;
    bool unmaskEnabled_ = false;
    int keyChannel_ = kDialogue;
    double safetyGainLinear_ = 1.0;
    CompressorParams params_;
    double unmaskLowHz_ = kUnmaskFrequencyRanges[kDialogue].lowHz;
    double unmaskHighHz_ = kUnmaskFrequencyRanges[kDialogue].highHz;

    // Advanced duck mode's SummedBus-vs-PerChannel choice (Types.h) and the
    // per-channel state it uses when PerChannel is active - see
    // setAdvancedDuckingMode()/setChannelThresholdDb()/setChannelRatio().
    AdvancedDuckingMode advancedDuckingMode_ = AdvancedDuckingMode::SummedBus;
    std::array<GainComputer, kNumClasses> perChannelGainComputer_;
    std::array<double, kNumClasses> channelThresholdDb_{};
    std::array<double, kNumClasses> channelRatio_{};

    // Resonance-mode-only settings. Defaults match the values this project
    // shipped with before these became live-adjustable.
    int resonanceNumPeaks_ = 4;
    double resonanceBandwidthOctaves_ = 1.16;
    double resonanceMaxReductionDb_ = 24.0;

    // Output-bus WDRC settings. Starts bypassed so loading the app doesn't
    // change existing default behavior; threshold/ratio default to a mild,
    // typical WDRC starting point (gentler than the ducking compressor's
    // punchier defaults - WDRC is meant to gently compress most normal-level
    // material, not just react to loud transients).
    bool wdrcBypassed_ = true;
    double wdrcThresholdDb_ = -24.0;
    double wdrcRatio_ = 2.0;
    double wdrcMakeupGainDb_ = 0.0;
    double wdrcAttackMs_ = 5.0;
    double wdrcReleaseMs_ = 80.0;

    EnvelopeFollower detector_;
    GainComputer gainComputer_;
    SpectralResonanceSuppressor spectralSuppressor_;
    WdrcCompressor wdrcCompressor_;

    std::array<ChannelStrip, kNumClasses> strips_;
    std::array<Smoother, kNumClasses> keyBlend_;
    std::array<DelayLine, kNumClasses> dryDelayL_, dryDelayR_;
    std::array<std::vector<float>, kNumClasses> stemsL_;
    std::array<std::vector<float>, kNumClasses> stemsR_;
    std::array<std::vector<float>, kNumClasses> stemsMono_; // sidechain detector input only
    size_t numFrames_ = 0;
    size_t sampleIndex_ = 0;
    double lastGainLinear_ = 1.0;
};

} // namespace demo
