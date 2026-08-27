#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>
#include "FFT.h"
#include "GainComputer.h"
#include "Smoother.h"
#include "Types.h"

namespace demo {

// STFT-based resonance suppressor for DuckMode::Resonance: analyzes the key
// channel's real magnitude spectrum, finds its N loudest mutually-separated
// spectral peaks, and applies a smooth per-bin gain-reduction mask - built
// and applied entirely in the frequency domain via windowed overlap-add -
// to each of the engine's 10 signals (5 classes x L/R).
//
// This replaces an earlier design that swept narrow IIR notch (peaking EQ)
// filters through the spectrum in the time domain. That's mechanically the
// same mechanism a phaser effect uses (moving resonant filter poles through
// frequency), which is exactly the "phasey"/swept character that design
// produced. Multiplying FFT bins by a real gain and reconstructing via
// square-root-Hann windowed overlap-add doesn't sweep any poles through
// frequency - there's no equivalent phase modulation to hear, which is how
// tools like Soothe2 avoid this artifact.
//
// Fixed 50%-overlap, square-root-Hann analysis+synthesis windowing -
// verified during development (see the FFT/STFT round-trip identity tests
// this class's design was validated against) to reconstruct an unmodified
// signal to numerical precision at a latency of exactly kFftSize samples.
class SpectralResonanceSuppressor {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kHopSize = kFftSize / 2; // 50% overlap
    static constexpr int kNumBins = kFftSize / 2 + 1; // 0..Nyquist inclusive
    static constexpr int kMaxPeaks = 12;
    static constexpr double kMinFreqHz = 120.0;
    static constexpr double kMaxFreqHz = 9600.0;
    static constexpr int kLatencySamples = kFftSize;
    // Extra headroom beyond a peak's own bump width when spacing peaks
    // apart, so adjacent gain-reduction bumps leave a clean gap instead of
    // stacking.
    static constexpr double kSeparationMarginFactor = 1.15;
    // How fast a peak's reported frequency may glide between hops, and how
    // its detected level is smoothed before computing gain - both tick once
    // per hop (~21ms at these settings), not once per sample; see tick().
    static constexpr double kFreqGlideMs = 120.0;

    static constexpr int kNumSignals = kNumClasses * 2; // 5 classes x L/R
    static constexpr int signalIndex(int classIndex, int side) { return classIndex * 2 + side; }

    void prepare(double sampleRate, double attackMs, double releaseMs) {
        sampleRate_ = sampleRate;
        hopRate_ = sampleRate_ / kHopSize;
        binHz_ = sampleRate_ / kFftSize;
        minBin_ = std::max(1, static_cast<int>(std::round(kMinFreqHz / binHz_)));
        maxBin_ = std::min(kNumBins - 2, static_cast<int>(std::round(kMaxFreqHz / binHz_)));

        window_.resize(kFftSize);
        for (int i = 0; i < kFftSize; ++i) {
            window_[i] = std::sqrt(0.5 - 0.5 * std::cos(2.0 * M_PI * i / kFftSize));
        }
        // Empirically-derived overlap-add normalization for this window/hop
        // combination (verified separately to be numerically exact for
        // sqrt-Hann at 50% hop - see development STFT identity test).
        std::vector<double> probe(kFftSize * 3, 0.0);
        for (int hop = 0; hop < 4; ++hop) {
            int base = hop * kHopSize;
            for (int i = 0; i < kFftSize; ++i) probe[base + i] += window_[i] * window_[i];
        }
        olaNorm_ = probe[kFftSize];

        keyInBuf_.assign(kFftSize, 0.0);
        for (int s = 0; s < kNumSignals; ++s) {
            sigInBuf_[s].assign(kFftSize, 0.0);
            sigOutAccum_[s].assign(kFftSize, 0.0);
        }
        hopCounter_ = 0;
        binGainLinear_.fill(1.0);

        for (int p = 0; p < kMaxPeaks; ++p) {
            freqLog2_[p].prepare(hopRate_, kFreqGlideMs);
            int seedBin = minBin_ + ((p + 1) * (maxBin_ - minBin_)) / (kMaxPeaks + 1);
            freqLog2_[p].reset(std::log2(seedBin * binHz_));
            freq_[p] = std::pow(2.0, freqLog2_[p].current());
            gainLinear_[p] = 1.0;
            levelFollower_[p].prepare(hopRate_, attackMs, releaseMs);
        }
        setBandwidthOctaves(bandwidthOctaves_);
    }

    void setActivePeakCount(int count) { activePeakCount_ = std::clamp(count, 1, kMaxPeaks); }

    void setBandwidthOctaves(double bandwidthOctaves) {
        bandwidthOctaves_ = bandwidthOctaves;
        minSeparationOctaves_ = bandwidthOctaves_ * kSeparationMarginFactor;
    }

    void setTimes(double attackMs, double releaseMs) {
        for (int p = 0; p < kMaxPeaks; ++p) levelFollower_[p].setTimes(hopRate_, attackMs, releaseMs);
    }

    // Called once per sample. keyInput is the mono key signal (already
    // mute-gated). signalsIn/signalsOut are kNumSignals-length arrays: this
    // sample's raw input, and this sample's (STFT-latency-delayed) output,
    // for every channel/side. Internally accumulates samples and does the
    // actual analyze/mask/resynthesize work once per hop.
    void tick(double keyInput, const double* signalsIn, double* signalsOut,
              const GainComputer& gainComputer, bool unmaskEnabled, double levelCompensationDb,
              double maxReductionDb) {
        int tailPos = kFftSize - kHopSize + hopCounter_;
        keyInBuf_[tailPos] = keyInput;
        for (int s = 0; s < kNumSignals; ++s) sigInBuf_[s][tailPos] = signalsIn[s];

        for (int s = 0; s < kNumSignals; ++s) signalsOut[s] = sigOutAccum_[s][hopCounter_];

        ++hopCounter_;
        if (hopCounter_ >= kHopSize) {
            hopCounter_ = 0;
            processHop(gainComputer, unmaskEnabled, levelCompensationDb, maxReductionDb);
        }
    }

    double freq(int peakIndex) const { return freq_[peakIndex]; }
    double gainLinear(int peakIndex) const { return gainLinear_[peakIndex]; }

private:
    // Attack/release smoothing in the dB domain directly (unlike
    // GainComputer.h's EnvelopeFollower, which expects a linear audio
    // sample and does its own dB conversion - our input here is already a
    // dB magnitude read from an FFT bin, so that conversion would be wrong
    // applied a second time).
    struct DbLevelFollower {
        double levelDb = -120.0;
        double alphaAttack = 0.0, alphaRelease = 0.0;

        void prepare(double hopRate, double attackMs, double releaseMs) {
            setTimes(hopRate, attackMs, releaseMs);
            levelDb = -120.0;
        }
        void setTimes(double hopRate, double attackMs, double releaseMs) {
            alphaAttack = std::exp(-1.0 / (0.001 * attackMs * hopRate));
            alphaRelease = std::exp(-1.0 / (0.001 * releaseMs * hopRate));
        }
        double tick(double instDb) {
            double alpha = (instDb >= levelDb) ? alphaAttack : alphaRelease;
            levelDb = alpha * levelDb + (1.0 - alpha) * instDb;
            return levelDb;
        }
    };

    void processHop(const GainComputer& gainComputer, bool unmaskEnabled, double levelCompensationDb,
                     double maxReductionDb) {
        std::vector<std::complex<double>> keyFrame(kFftSize);
        for (int i = 0; i < kFftSize; ++i) keyFrame[i] = keyInBuf_[i] * window_[i];
        FFT::transform(keyFrame, false);

        std::array<double, kNumBins> magDb{};
        for (int b = 0; b < kNumBins; ++b) {
            magDb[b] = 20.0 * std::log10(std::abs(keyFrame[b]) / kFftSize + 1e-12);
        }

        pickPeaksAndBuildMask(magDb, gainComputer, unmaskEnabled, levelCompensationDb, maxReductionDb);

        for (int s = 0; s < kNumSignals; ++s) {
            std::vector<std::complex<double>> frame(kFftSize);
            for (int i = 0; i < kFftSize; ++i) frame[i] = sigInBuf_[s][i] * window_[i];
            FFT::transform(frame, false);
            for (int b = 0; b < kNumBins; ++b) {
                frame[b] *= binGainLinear_[b];
                if (b > 0 && b < kFftSize - b) frame[kFftSize - b] *= binGainLinear_[b];
            }
            FFT::transform(frame, true);

            // Pop the oldest hopSize samples (finalized - no future frame
            // can still contribute to them), shift, THEN add this hop's new
            // contribution across the whole window. Order matters here -
            // adding before popping was the bug caught during development.
            auto& accum = sigOutAccum_[s];
            std::copy(accum.begin() + kHopSize, accum.end(), accum.begin());
            std::fill(accum.end() - kHopSize, accum.end(), 0.0);
            for (int i = 0; i < kFftSize; ++i) {
                accum[i] += frame[i].real() * window_[i] / olaNorm_;
            }
        }

        std::copy(keyInBuf_.begin() + kHopSize, keyInBuf_.end(), keyInBuf_.begin());
        for (int s = 0; s < kNumSignals; ++s) {
            std::copy(sigInBuf_[s].begin() + kHopSize, sigInBuf_[s].end(), sigInBuf_[s].begin());
        }
    }

    void pickPeaksAndBuildMask(const std::array<double, kNumBins>& magDb, const GainComputer& gainComputer,
                                bool unmaskEnabled, double levelCompensationDb, double maxReductionDb) {
        std::array<double, kNumBins> binGainDb{};
        binGainDb.fill(0.0);

        std::array<int, kMaxPeaks> pickedBin;
        pickedBin.fill(-1);

        for (int p = 0; p < activePeakCount_; ++p) {
            int bestBin = -1;
            double bestLevel = -1e9;
            for (int b = minBin_; b <= maxBin_; ++b) {
                double freqB = b * binHz_;
                bool tooClose = false;
                for (int q = 0; q < p; ++q) {
                    if (pickedBin[q] < 0) continue;
                    double freqQ = pickedBin[q] * binHz_;
                    if (std::fabs(std::log2(freqB / freqQ)) < minSeparationOctaves_) { tooClose = true; break; }
                }
                if (tooClose) continue;
                if (magDb[b] > bestLevel) { bestLevel = magDb[b]; bestBin = b; }
            }
            if (bestBin < 0) {
                // No bin satisfies separation from every previous pick -
                // happens when the requested peak count/bandwidth genuinely
                // doesn't fit the analysis range. Fall back to the global
                // loudest bin rather than leaving this peak undefined.
                bestBin = minBin_;
                bestLevel = magDb[minBin_];
                for (int b = minBin_ + 1; b <= maxBin_; ++b) {
                    if (magDb[b] > bestLevel) { bestLevel = magDb[b]; bestBin = b; }
                }
            }
            pickedBin[p] = bestBin;

            double freqHz = bestBin * binHz_;
            freqLog2_[p].setTarget(std::log2(freqHz));
            freq_[p] = std::pow(2.0, freqLog2_[p].tick());

            double level = levelFollower_[p].tick(bestLevel) + levelCompensationDb;
            double g = unmaskEnabled ? gainComputer.computeLinearGain(level) : 1.0;
            double gDb = 20.0 * std::log10(std::max(g, 1e-6));
            gDb = std::max(gDb, -maxReductionDb);
            gainLinear_[p] = std::pow(10.0, gDb / 20.0);

            // Smooth raised-cosine bump around this peak's frequency,
            // spanning +/- bandwidth/2 octaves, tapering to 0dB at the
            // edges to avoid a hard-edged mask (which would ring in the
            // time domain). Where two peaks' bumps reach the same bin, keep
            // the deeper cut rather than summing, so near-adjacent bumps
            // can't stack.
            double halfWidth = bandwidthOctaves_ / 2.0;
            for (int b = minBin_; b <= maxBin_; ++b) {
                double dOct = std::log2((b * binHz_) / freq_[p]);
                if (std::fabs(dOct) > halfWidth) continue;
                double t = (dOct + halfWidth) / bandwidthOctaves_; // 0..1
                double shape = 0.5 * (1.0 + std::cos(M_PI * (2.0 * t - 1.0))); // 1 at center, 0 at edges
                binGainDb[b] = std::min(binGainDb[b], shape * gDb);
            }
        }

        for (int b = minBin_; b <= maxBin_; ++b) {
            binGainLinear_[b] = std::pow(10.0, binGainDb[b] / 20.0);
        }
    }

    double sampleRate_ = 48000.0;
    double hopRate_ = 46.875;
    double binHz_ = 1.0;
    int minBin_ = 1, maxBin_ = 1;
    double olaNorm_ = 1.0;
    std::vector<double> window_;

    std::vector<double> keyInBuf_;
    std::array<std::vector<double>, kNumSignals> sigInBuf_;
    std::array<std::vector<double>, kNumSignals> sigOutAccum_;
    int hopCounter_ = 0;

    std::array<double, kNumBins> binGainLinear_{};

    int activePeakCount_ = 4;
    double bandwidthOctaves_ = 1.16;
    double minSeparationOctaves_ = 1.16 * kSeparationMarginFactor;

    std::array<Smoother, kMaxPeaks> freqLog2_;
    std::array<double, kMaxPeaks> freq_{};
    std::array<double, kMaxPeaks> gainLinear_{};
    std::array<DbLevelFollower, kMaxPeaks> levelFollower_;
};

} // namespace demo
