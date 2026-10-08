#pragma once
#include <algorithm>
#include <cmath>
#include "GainComputer.h"
#include "Types.h"

namespace demo {

// Single-band Enhancement compressor: one instance per channel, detecting
// and compressing that channel's OWN signal - no sidechain/key
// relationship to any other channel at all (the defining difference from
// every duck mode elsewhere in this engine, which all react to a
// *different* channel's level). Reuses the exact same GainComputer/
// EnvelopeFollower math the Sidechain Compressor already uses.
//
// The detector reads a mono sum of L+R (same reasoning as every other
// detector in this project - WdrcCompressor, the main sidechain
// detector): an independent L/R detector would let the two channels
// compress by different amounts and wobble the stereo image.
class SingleBandEnhancementCompressor {
public:
    void prepare(double sampleRate, const CompressorParams& params) {
        detector_.prepare(sampleRate, params.attackMs, params.releaseMs);
        gainComputer_.prepare(params);
    }

    void setParams(const CompressorParams& params) {
        detector_.setTimes(params.attackMs, params.releaseMs);
        gainComputer_.setParams(params);
    }

    // Static makeup gain, applied after compression - same convention as
    // WdrcCompressor::setMakeupGainDb() (not a factor in the gain-reduction
    // meter, which only reports the compressor's own cut).
    void setMakeupGainDb(double makeupGainDb) {
        makeupGainLinear_ = std::pow(10.0, makeupGainDb / 20.0);
    }

    void tick(double inL, double inR, double& outL, double& outR) {
        double mono = 0.5 * (inL + inR);
        double levelDb = detector_.tick(mono);
        double g = gainComputer_.computeLinearGain(levelDb);
        lastGainReductionDb_ = 20.0 * std::log10(std::max(g, 1e-6));
        outL = inL * g * makeupGainLinear_;
        outR = inR * g * makeupGainLinear_;
    }

    // 0 = no reduction, negative = reduction amount - for a gain-reduction
    // meter, same convention as WdrcCompressor::lastGainReductionDb().
    double lastGainReductionDb() const { return lastGainReductionDb_; }

private:
    EnvelopeFollower detector_;
    GainComputer gainComputer_;
    double lastGainReductionDb_ = 0.0;
    double makeupGainLinear_ = 1.0;
};

} // namespace demo
