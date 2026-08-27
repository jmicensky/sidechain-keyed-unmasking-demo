#pragma once
#include <algorithm>
#include <cmath>
#include "Types.h"

namespace demo {

// Implements Eq. 1: attack/release smoothed level estimate in dB.
class EnvelopeFollower {
public:
    void prepare(double sampleRate, double attackMs, double releaseMs) {
        sampleRate_ = sampleRate;
        setTimes(attackMs, releaseMs);
        levelDb_ = kFloorDb;
    }

    void setTimes(double attackMs, double releaseMs) {
        alphaAttack_  = std::exp(-1.0 / (0.001 * attackMs  * sampleRate_));
        alphaRelease_ = std::exp(-1.0 / (0.001 * releaseMs * sampleRate_));
    }

    // Feed one sample of the sidechain key signal (already gated to 0 if
    // the key channel is muted), return the smoothed level in dB.
    double tick(double sample) {
        double instDb = 20.0 * std::log10(std::fabs(sample) + kEpsilon);
        double alpha = (instDb >= levelDb_) ? alphaAttack_ : alphaRelease_;
        levelDb_ = alpha * levelDb_ + (1.0 - alpha) * instDb;
        levelDb_ = std::max(levelDb_, kFloorDb);
        return levelDb_;
    }

    double levelDb() const { return levelDb_; }

private:
    static constexpr double kEpsilon = 1e-9;
    static constexpr double kFloorDb = -120.0;

    double sampleRate_ = 48000.0;
    double alphaAttack_ = 0.0;
    double alphaRelease_ = 0.0;
    double levelDb_ = kFloorDb;
};

// Implements Eq. 2: soft-knee compressor gain computer.
// Converts a sidechain level (dB) into a linear gain multiplier g(n) in (0, 1].
class GainComputer {
public:
    void prepare(const CompressorParams& params) { params_ = params; }
    void setParams(const CompressorParams& params) { params_ = params; }

    double computeLinearGain(double levelDb) const {
        const double T = params_.thresholdDb;
        const double R = params_.ratio;
        const double W = params_.kneeDb;

        double gainDb;
        if (W < kMinKneeDb) {
            // Hard knee: the quadratic transition branch below divides by W,
            // so guard the W->0 limit explicitly rather than let it hit a
            // division by zero on a sample that lands exactly at T.
            gainDb = (levelDb <= T) ? 0.0 : (1.0 / R - 1.0) * (levelDb - T);
        } else if (levelDb < T - W / 2.0) {
            gainDb = 0.0;
        } else if (levelDb > T + W / 2.0) {
            gainDb = (1.0 / R - 1.0) * (levelDb - T);
        } else {
            double x = levelDb - T + W / 2.0;
            gainDb = (1.0 / R - 1.0) * (x * x) / (2.0 * W);
        }
        return std::pow(10.0, gainDb / 20.0);
    }

private:
    static constexpr double kMinKneeDb = 1e-6;
    CompressorParams params_;
};

} // namespace demo
