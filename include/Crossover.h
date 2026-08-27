#pragma once
#include <cmath>
#include "Types.h"

namespace demo {

// Plain coefficient set, so a caller can compute RBJ coefficients once (e.g.
// the trig-heavy peaking formula) and broadcast the same {b0,b1,b2,a1,a2} to
// several Biquad instances that each need independent filter state but
// identical response - see setCoeffs() below and ResonanceSuppressor.h.
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// RBJ cookbook peaking (bell) EQ. Positive gainDb boosts, negative cuts;
// 0 dB reduces exactly to the identity filter.
inline BiquadCoeffs computePeakingCoeffs(double sampleRate, double fc, double gainDb, double q) {
    double A = std::pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * fc / sampleRate;
    double cosw0 = std::cos(w0);
    double alpha = std::sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = (-2.0 * cosw0) / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = (-2.0 * cosw0) / a0;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}

// Standard RBJ cookbook biquad, transposed direct form II for numerical
// stability. Used as the building block for Butterworth/LR crossover stages.
class Biquad {
public:
    void setLowpass(double sampleRate, double fc, double q) {
        double w0 = 2.0 * M_PI * fc / sampleRate;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        b0_ = ((1.0 - cosw0) / 2.0) / a0;
        b1_ = (1.0 - cosw0) / a0;
        b2_ = b0_;
        a1_ = (-2.0 * cosw0) / a0;
        a2_ = (1.0 - alpha) / a0;
        reset();
    }

    void setHighpass(double sampleRate, double fc, double q) {
        double w0 = 2.0 * M_PI * fc / sampleRate;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        b0_ = ((1.0 + cosw0) / 2.0) / a0;
        b1_ = (-(1.0 + cosw0)) / a0;
        b2_ = b0_;
        a1_ = (-2.0 * cosw0) / a0;
        a2_ = (1.0 - alpha) / a0;
        reset();
    }

    // Constant 0 dB peak-gain bandpass (RBJ cookbook), used as an analysis
    // filter (see SpectralAnalyzer) rather than for audio shaping.
    void setBandpass(double sampleRate, double fc, double q) {
        double w0 = 2.0 * M_PI * fc / sampleRate;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        b0_ = alpha / a0;
        b1_ = 0.0;
        b2_ = -alpha / a0;
        a1_ = (-2.0 * cosw0) / a0;
        a2_ = (1.0 - alpha) / a0;
        reset();
    }

    // Adopts a precomputed coefficient set without touching filter state
    // (z1_/z2_) - lets a dynamic notch's center frequency/depth change every
    // sample without clicking, and lets several instances share one set of
    // (trig-heavy) coefficients instead of each recomputing them.
    void setCoeffs(const BiquadCoeffs& c) {
        b0_ = c.b0; b1_ = c.b1; b2_ = c.b2; a1_ = c.a1; a2_ = c.a2;
    }

    void reset() { z1_ = 0.0; z2_ = 0.0; }

    inline double tick(double x) {
        double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

private:
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

// A single Linkwitz-Riley 4th-order (24 dB/oct) two-way split.
// Built from two cascaded Butterworth 2nd-order stages per branch, which is
// the standard construction that lets low + high sum back to a flat,
// in-phase response (no polarity inversion needed at 4th order, unlike LR2).
class LR4Split {
public:
    void prepare(double sampleRate, double fc) {
        static constexpr double kQ = 0.70710678; // Butterworth Q
        lowA_.setLowpass(sampleRate, fc, kQ);
        lowB_.setLowpass(sampleRate, fc, kQ);
        highA_.setHighpass(sampleRate, fc, kQ);
        highB_.setHighpass(sampleRate, fc, kQ);
    }

    inline void tick(double x, double& low, double& high) {
        low = lowB_.tick(lowA_.tick(x));
        high = highB_.tick(highA_.tick(x));
    }

private:
    Biquad lowA_, lowB_, highA_, highB_;
};

// Splits a signal into 4 bands via a tree of LR4 two-way splits:
//   stage1: split at 1800 Hz -> loA, band4 (1800 - Nyquist)
//   stage2: split loA at 300 Hz -> loB, band3 (300 - 1800)
//   stage3: split loB at 100 Hz -> band1 (0 - 100), band2 (100 - 300)
// Each instance owns its own filter state, so give each audio channel that
// needs banding (i.e. each non-key channel in Advanced mode) its own
// CrossoverFilterbank instance rather than sharing one across channels.
class CrossoverFilterbank {
public:
    void prepare(double sampleRate) {
        stage1_.prepare(sampleRate, kCrossoverHigh);
        stage2_.prepare(sampleRate, kCrossoverMid);
        stage3_.prepare(sampleRate, kCrossoverLow);
    }

    // band[0..3] = band1 (low), band2 (low-mid), band3 (upper-mid, the
    // ducked band), band4 (high).
    inline void tick(double x, double band[4]) {
        double loA, band4;
        stage1_.tick(x, loA, band4);

        double loB, band3;
        stage2_.tick(loA, loB, band3);

        double band1, band2;
        stage3_.tick(loB, band1, band2);

        band[0] = band1;
        band[1] = band2;
        band[2] = band3;
        band[3] = band4;
    }

private:
    LR4Split stage1_, stage2_, stage3_;
};

} // namespace demo
