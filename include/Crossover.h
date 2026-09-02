#pragma once
#include <array>
#include <cmath>
#include "Types.h"

namespace demo {

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

// Splits a signal into NumBands bands via a tree of LR4 two-way splits,
// built from NumBands-1 crossover points (ascending, low to high): each
// stage peels the top band off the current low remainder, working from the
// highest crossover point down to the lowest, e.g. for 4 bands with points
// [fc0, fc1, fc2]:
//   stage[2]: split at fc2 -> lo, band[3] (fc2 - Nyquist)
//   stage[1]: split lo at fc1 -> lo, band[2] (fc1 - fc2)
//   stage[0]: split lo at fc0 -> band[0] (0 - fc0), band[1] (fc0 - fc1)
// Each instance owns its own filter state, so give each audio channel that
// needs banding (i.e. each non-key channel in Advanced mode) its own
// CrossoverFilterbank instance rather than sharing one across channels.
template <int NumBands>
class CrossoverFilterbank {
public:
    static_assert(NumBands >= 2, "CrossoverFilterbank needs at least 2 bands");
    static constexpr int kNumCrossovers = NumBands - 1;

    // crossoverHz[0..kNumCrossovers-1], ascending low to high.
    void prepare(double sampleRate, const double (&crossoverHz)[kNumCrossovers]) {
        for (int i = 0; i < kNumCrossovers; ++i) {
            stages_[i].prepare(sampleRate, crossoverHz[i]);
        }
    }

    // band[0] = lowest band, band[NumBands-1] = highest band.
    inline void tick(double x, double band[NumBands]) {
        double current = x;
        for (int i = kNumCrossovers - 1; i >= 1; --i) {
            double lo, hi;
            stages_[i].tick(current, lo, hi);
            band[i + 1] = hi;
            current = lo;
        }
        stages_[0].tick(current, band[0], band[1]);
    }

private:
    std::array<LR4Split, kNumCrossovers> stages_;
};

} // namespace demo
