#pragma once
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
        for (auto& ef : bandDetectors_) ef.prepare(sampleRate, kAttackMs, kReleaseMs);
        gainComputer_.prepare(params_);
        bypassBlend_.prepare(sampleRate, kBypassRampMs);
        bypassBlend_.reset(0.0); // starts bypassed
    }

    void setBypassed(bool bypassed) { bypassBlend_.setTarget(bypassed ? 0.0 : 1.0); }

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
            return;
        }

        double bandsL[kNumBands], bandsR[kNumBands];
        filterbankL_.tick(inL, bandsL);
        filterbankR_.tick(inR, bandsR);

        double compressedL = 0.0, compressedR = 0.0;
        for (int b = 0; b < kNumBands; ++b) {
            double mono = 0.5 * (bandsL[b] + bandsR[b]);
            double levelDb = bandDetectors_[b].tick(mono);
            double g = gainComputer_.computeLinearGain(levelDb);
            compressedL += bandsL[b] * g;
            compressedR += bandsR[b] * g;
        }
        compressedL *= makeupGainLinear_;
        compressedR *= makeupGainLinear_;

        outL = (1.0 - blend) * inL + blend * compressedL;
        outR = (1.0 - blend) * inR + blend * compressedR;
    }

private:
    // Fixed WDRC-typical timing, not exposed as a control - the point of
    // this stage is experimenting with threshold/ratio/makeup gain shape,
    // not re-deriving attack/release (the sidechain compressor elsewhere
    // already covers that experimentation).
    static constexpr double kAttackMs = 5.0;
    static constexpr double kReleaseMs = 80.0;
    static constexpr double kBypassRampMs = 25.0;

    CrossoverFilterbank filterbankL_, filterbankR_;
    std::array<EnvelopeFollower, kNumBands> bandDetectors_;
    GainComputer gainComputer_;
    CompressorParams params_;
    double makeupGainLinear_ = 1.0;
    Smoother bypassBlend_;
};

} // namespace demo
