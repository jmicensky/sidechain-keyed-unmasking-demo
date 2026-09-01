#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "Crossover.h"
#include "GainComputer.h"
#include "Smoother.h"
#include "Types.h"

namespace demo {

// One class's frequency window - the WDRC compressor only compresses within
// this range for whichever class is currently the unmask/key channel;
// content outside it passes through completely dry.
struct WdrcFreqRange {
    double lowHz, highHz;
};

// Indexed by ClassIndex (kDialogue, kMusic, kBackgroundNoise, kSafetyAlerts,
// kOther) - see Types.h. Chosen per-class so the compressor "sees" roughly
// the band that class's own content actually occupies, rather than
// compressing the whole spectrum regardless of what's currently unmasked.
inline constexpr std::array<WdrcFreqRange, kNumClasses> kWdrcChannelRanges = {{
    {400.0, 7000.0},   // kDialogue: voice
    {75.0, 12000.0},   // kMusic
    {60.0, 2000.0},    // kBackgroundNoise
    {300.0, 2000.0},   // kSafetyAlerts
    {700.0, 12000.0},  // kOther
}};

// A small-band-count WDRC-style (Wide Dynamic Range Compression) stereo
// compressor applied to the final decomposed mix - not to be confused with
// the sidechain-keyed ducking compressor elsewhere in the engine, which
// reacts to a *different* channel's level. This one is a self-contained
// insert on the mix bus, the way a hearing aid's WDRC stage compresses
// whatever's actually reaching the output.
//
// The compressor's active frequency window tracks whichever class is
// currently selected as the unmask/key channel (see setActiveChannel(),
// called from Engine::setKeyChannel()) - compression is focused on that
// class's own frequency range (kWdrcChannelRanges), not the whole spectrum.
// Content above and below the active range passes straight through
// unprocessed. Within the active range, it splits into 2 sub-bands at the
// range's log-frequency center, each compressing independently off its own
// level (same reasoning as the sidechain-keyed compressor's shared mono
// detector: an independent L/R detector per band would let the two
// channels compress by different amounts and wobble the stereo image) -
// still genuinely multiband within the focused range, without a wall of
// per-band controls.
class WdrcCompressor {
public:
    static constexpr int kNumBands = 2; // compressed sub-bands within the active range

    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        for (auto& ef : bandDetectors_) ef.prepare(sampleRate, attackMs_, releaseMs_);
        gainComputer_.prepare(params_);
        bypassBlend_.prepare(sampleRate, kBypassRampMs);
        bypassBlend_.reset(0.0); // starts bypassed
        setActiveChannel(activeChannel_);
    }

    void setBypassed(bool bypassed) { bypassBlend_.setTarget(bypassed ? 0.0 : 1.0); }

    // Reconfigures the active frequency window to classIndex's range (see
    // kWdrcChannelRanges) - call whenever the key/unmask channel selection
    // changes, so "focus the compression on the specific frequency range of
    // the unmasked channel" stays true live.
    void setActiveChannel(int classIndex) {
        activeChannel_ = classIndex;
        const WdrcFreqRange& range = kWdrcChannelRanges[classIndex];
        activeLowHz_ = range.lowHz;
        activeHighHz_ = range.highHz;
        double midHz = std::sqrt(activeLowHz_ * activeHighHz_); // geometric mean = log-frequency center

        highSplitL_.prepare(sampleRate_, activeHighHz_);
        highSplitR_.prepare(sampleRate_, activeHighHz_);
        lowSplitL_.prepare(sampleRate_, activeLowHz_);
        lowSplitR_.prepare(sampleRate_, activeLowHz_);
        midSplitL_.prepare(sampleRate_, midHz);
        midSplitR_.prepare(sampleRate_, midHz);
    }

    int activeChannel() const { return activeChannel_; }
    double activeLowHz() const { return activeLowHz_; }
    double activeHighHz() const { return activeHighHz_; }

    // Clamped to the range this project treats as "common" in hearing-aid
    // WDRC time-constant literature (Dillon's Hearing Aids textbook, Kates'
    // Digital Hearing Aids): fast/syllabic designs use attack ~5ms, release
    // ~50-200ms; slow-acting/AGC-o designs push release out to several
    // seconds. 1-50ms attack and 30-3000ms release covers both philosophies
    // without being unbounded.
    void setAttackMs(double attackMs) {
        attackMs_ = std::clamp(attackMs, 1.0, 50.0);
        for (auto& ef : bandDetectors_) ef.setTimes(attackMs_, releaseMs_);
    }

    void setReleaseMs(double releaseMs) {
        releaseMs_ = std::clamp(releaseMs, 30.0, 3000.0);
        for (auto& ef : bandDetectors_) ef.setTimes(attackMs_, releaseMs_);
    }

    void setThresholdDb(double thresholdDb) {
        params_.thresholdDb = thresholdDb;
        gainComputer_.setParams(params_);
    }

    void setRatio(double ratio) {
        params_.ratio = ratio;
        gainComputer_.setParams(params_);
    }

    void setMakeupGainDb(double makeupGainDb) {
        makeupGainLinear_ = std::pow(10.0, makeupGainDb / 20.0);
    }

    void tick(double inL, double inR, double& outL, double& outR) {
        double blend = bypassBlend_.tick();
        if (blend < 1e-6) {
            // Fully bypassed: skip the filter/detector work entirely rather
            // than run it at zero blend, so a bypassed WDRC stage costs
            // nothing and can never subtly color the signal.
            outL = inL;
            outR = inR;
            lastGainReductionDb_ = 0.0;
            return;
        }

        // Three-way split per side: below the active range, the active
        // range itself, and above it - the same LR4-split-tree technique
        // Advanced duck mode's fixed 4-band CrossoverFilterbank uses, just
        // with the two crossover points set from the active channel's
        // range instead of fixed constants.
        double loA_L, aboveL, loA_R, aboveR;
        highSplitL_.tick(inL, loA_L, aboveL);
        highSplitR_.tick(inR, loA_R, aboveR);

        double belowL, inRangeL, belowR, inRangeR;
        lowSplitL_.tick(loA_L, belowL, inRangeL);
        lowSplitR_.tick(loA_R, belowR, inRangeR);

        double subLowL, subHighL, subLowR, subHighR;
        midSplitL_.tick(inRangeL, subLowL, subHighL);
        midSplitR_.tick(inRangeR, subLowR, subHighR);

        double bandsL[kNumBands] = {subLowL, subHighL};
        double bandsR[kNumBands] = {subLowR, subHighR};

        double compressedInRangeL = 0.0, compressedInRangeR = 0.0;
        double minGainDb = 0.0; // deepest cut among the 2 sub-bands, for the meter
        for (int b = 0; b < kNumBands; ++b) {
            double mono = 0.5 * (bandsL[b] + bandsR[b]);
            // Any one sub-band only carries a fraction of the active
            // range's energy, which itself is only a fraction of the full
            // mix's energy (same reasoning as Resonance mode's per-band
            // detector - see Engine.h's kResonanceLevelCompensationDb), so
            // a threshold calibrated against full-mix loudness would rarely
            // be crossed without this compensation. +10dB is an empirical
            // match, not a physically exact correction.
            double levelDb = bandDetectors_[b].tick(mono) + kLevelCompensationDb;
            double g = gainComputer_.computeLinearGain(levelDb);
            minGainDb = std::min(minGainDb, 20.0 * std::log10(std::max(g, 1e-6)));
            compressedInRangeL += bandsL[b] * g;
            compressedInRangeR += bandsR[b] * g;
        }
        lastGainReductionDb_ = minGainDb;
        // Makeup gain applies only to the compressed (in-range) content,
        // not the dry above/below-range content passing through untouched -
        // it's restoring level for what was actually compressed.
        compressedInRangeL *= makeupGainLinear_;
        compressedInRangeR *= makeupGainLinear_;

        double compressedL = belowL + aboveL + compressedInRangeL;
        double compressedR = belowR + aboveR + compressedInRangeR;

        outL = (1.0 - blend) * inL + blend * compressedL;
        outR = (1.0 - blend) * inR + blend * compressedR;
    }

    // Deepest per-band cut from the most recently processed sample, in dB
    // (0 = no reduction, negative = reduction amount) - for a gain-reduction
    // meter. Always 0 while bypassed.
    double lastGainReductionDb() const { return lastGainReductionDb_; }

private:
    static constexpr double kBypassRampMs = 25.0;
    static constexpr double kLevelCompensationDb = 10.0; // see tick()

    double sampleRate_ = 48000.0;
    LR4Split highSplitL_, highSplitR_;
    LR4Split lowSplitL_, lowSplitR_;
    LR4Split midSplitL_, midSplitR_;
    std::array<EnvelopeFollower, kNumBands> bandDetectors_;
    GainComputer gainComputer_;
    CompressorParams params_;
    double makeupGainLinear_ = 1.0;
    Smoother bypassBlend_;
    double lastGainReductionDb_ = 0.0;
    // Defaults land in the "fast/syllabic" end of the literature range.
    double attackMs_ = 5.0;
    double releaseMs_ = 80.0;
    int activeChannel_ = kDialogue; // matches Engine's default key channel
    double activeLowHz_ = 400.0;
    double activeHighHz_ = 7000.0;
};

} // namespace demo
