#pragma once
#include <cmath>

namespace demo {

// A simple one-pole exponential ramp toward a target value.
// Used anywhere a UI/control change (mute, solo, key selection) needs to
// become an audio-rate parameter without an audible step discontinuity.
class Smoother {
public:
    void prepare(double sampleRate, double timeMs) {
        sampleRate_ = sampleRate;
        setTimeMs(timeMs);
    }

    void setTimeMs(double timeMs) {
        timeMs_ = timeMs;
        // Standard one-pole coefficient for a given time constant.
        coeff_ = std::exp(-1.0 / (0.001 * timeMs_ * sampleRate_));
    }

    // Immediately jump to a value with no ramp (e.g. at startup).
    void reset(double value) {
        current_ = value;
        target_ = value;
    }

    void setTarget(double target) { target_ = target; }

    // Advance by one sample, return the new current value.
    double tick() {
        current_ = target_ + coeff_ * (current_ - target_);
        return current_;
    }

    double current() const { return current_; }
    double target() const { return target_; }
    bool isSettled(double epsilon = 1e-5) const {
        return std::fabs(current_ - target_) < epsilon;
    }

private:
    double sampleRate_ = 48000.0;
    double timeMs_ = 10.0;
    double coeff_ = 0.0;
    double current_ = 0.0;
    double target_ = 0.0;
};

} // namespace demo
