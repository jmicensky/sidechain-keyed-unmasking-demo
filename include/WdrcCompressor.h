#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "Crossover.h"
#include "GainComputer.h"
#include "Smoother.h"
#include "Types.h"

namespace demo {

// A small-band-count WDRC-style (Wide Dynamic Range Compression) stereo
// compressor applied to the final decomposed mix - not to be confused with
// the sidechain-keyed ducking compressor elsewhere in the engine, which
// reacts to a *different* channel's level. This one is a self-contained
// insert on the mix bus, the way a hearing aid's WDRC stage compresses
// whatever's actually reaching the output.
//
// Real hearing-aid WDRC typically runs many more bands, each with its own
// independently-tunable threshold/ratio (fit to an audiogram). This is a
// deliberately simpler 4-band version - reusing the same LR4 crossover
// split Advanced duck mode already uses - with one shared
// threshold/ratio/makeup gain across all bands, so it's still genuinely
// multiband (each band compresses based on its own level, independently)
// without a wall of per-band controls to tune.
//
// Each band's detector reads a mono sum of L+R (same reasoning as the
// engine's main sidechain detector): an independent L/R detector per band
// would let the two channels compress by different amounts and wobble the
// stereo image.
class WdrcCompressor {
public:
    static constexpr int kNumBands = 4;

    void prepare(double sampleRate) {
        filterbankL_.prepare(sampleRate);
        filterbankR_.prepare(sampleRate);
        for (auto& ef : bandDetectors_) ef.prepare(sampleRate, attackMs_, releaseMs_);
        gainComputer_.prepare(params_);
        bypassBlend_.prepare(sampleRate, kBypassRampMs);
        bypassBlend_.reset(0.0); // starts bypassed
    }

    void setBypassed(bool bypassed) { bypassBlend_.setTarget(bypassed ? 0.0 : 1.0); }

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
            // Fully bypassed: skip the filterbank/detector work entirely
            // rather than run it at zero blend, so a bypassed WDRC stage
            // costs nothing and can never subtly color the signal.
            outL = inL;
            outR = inR;
            lastGainReductionDb_ = 0.0;
            return;
        }

        double bandsL[kNumBands], bandsR[kNumBands];
        filterbankL_.tick(inL, bandsL);
        filterbankR_.tick(inR, bandsR);

        double compressedL = 0.0, compressedR = 0.0;
        double minGainDb = 0.0; // deepest cut among the 4 bands, for the meter
        for (int b = 0; b < kNumBands; ++b) {
            double mono = 0.5 * (bandsL[b] + bandsR[b]);
            // Any one band only carries a fraction of the full mix's energy
            // (measured on the real Construction Scene stems: 2.5-19dB
            // quieter than the full-mix level, depending on the band and
            // how much content actually lives there), so a threshold
            // calibrated against full-mix loudness would rarely be crossed
            // by any individual band - the same issue Resonance mode's
            // per-band detector had (see Engine.h's
            // kResonanceLevelCompensationDb). +10dB is an empirical match
            // for comparable sensitivity, not a physically exact correction.
            double levelDb = bandDetectors_[b].tick(mono) + kLevelCompensationDb;
            double g = gainComputer_.computeLinearGain(levelDb);
            minGainDb = std::min(minGainDb, 20.0 * std::log10(std::max(g, 1e-6)));
            compressedL += bandsL[b] * g;
            compressedR += bandsR[b] * g;
        }
        lastGainReductionDb_ = minGainDb;
        compressedL *= makeupGainLinear_;
        compressedR *= makeupGainLinear_;

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

    CrossoverFilterbank filterbankL_, filterbankR_;
    std::array<EnvelopeFollower, kNumBands> bandDetectors_;
    GainComputer gainComputer_;
    CompressorParams params_;
    double makeupGainLinear_ = 1.0;
    Smoother bypassBlend_;
    double lastGainReductionDb_ = 0.0;
    // Defaults land in the "fast/syllabic" end of the literature range.
    double attackMs_ = 5.0;
    double releaseMs_ = 80.0;
};

} // namespace demo
