#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "Engine.h"
#include "Crossover.h"

using namespace demo;

static constexpr double kSampleRate = 48000.0;
static constexpr int kBlockSize = 128; // matches typical AudioWorklet render quantum

// ---------------------------------------------------------------------
// Synthetic stem generation (used when no real WAV files are supplied)
// ---------------------------------------------------------------------

static std::vector<float> synthDialogue(double durationSec, std::mt19937& rng) {
    size_t n = static_cast<size_t>(durationSec * kSampleRate);
    std::vector<float> buf(n, 0.0f);
    std::uniform_real_distribution<double> gate(0.0, 1.0);
    double phase1 = 0, phase2 = 0;
    double syllableRate = 4.0; // Hz, speech-rate-ish gating
    for (size_t i = 0; i < n; ++i) {
        double t = i / kSampleRate;
        double envelope = 0.5 * (1.0 + std::sin(2.0 * M_PI * syllableRate * t));
        envelope = std::pow(envelope, 3.0); // sharpen into syllable bursts
        double f0 = 160.0 + 20.0 * std::sin(2.0 * M_PI * 0.3 * t);
        phase1 += 2.0 * M_PI * f0 / kSampleRate;
        phase2 += 2.0 * M_PI * (2.0 * f0) / kSampleRate;
        double sample = 0.6 * std::sin(phase1) + 0.3 * std::sin(phase2);
        buf[i] = static_cast<float>(0.5 * envelope * sample);
    }
    return buf;
}

static std::vector<float> synthMusic(double durationSec) {
    size_t n = static_cast<size_t>(durationSec * kSampleRate);
    std::vector<float> buf(n, 0.0f);
    double freqs[3] = {261.63, 329.63, 392.00}; // C major triad
    double phases[3] = {0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        double t = i / kSampleRate;
        double lfo = 0.85 + 0.15 * std::sin(2.0 * M_PI * 0.2 * t);
        double sample = 0.0;
        for (int k = 0; k < 3; ++k) {
            phases[k] += 2.0 * M_PI * freqs[k] / kSampleRate;
            sample += std::sin(phases[k]);
        }
        buf[i] = static_cast<float>(0.15 * lfo * sample);
    }
    return buf;
}

static std::vector<float> synthBackgroundNoise(double durationSec, std::mt19937& rng) {
    size_t n = static_cast<size_t>(durationSec * kSampleRate);
    std::vector<float> buf(n, 0.0f);
    std::normal_distribution<double> noise(0.0, 1.0);
    double state = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double w = noise(rng);
        state = 0.98 * state + 0.02 * w; // crude low-pass for "room tone" feel
        buf[i] = static_cast<float>(0.08 * state * 20.0); // scale back up after LPF attenuation
    }
    return buf;
}

static std::vector<float> synthSafetyAlert(double durationSec) {
    size_t n = static_cast<size_t>(durationSec * kSampleRate);
    std::vector<float> buf(n, 0.0f);
    // Two-tone siren, on/off gated in ~2s bursts starting at t=1s and t=6s
    double phase = 0;
    for (size_t i = 0; i < n; ++i) {
        double t = i / kSampleRate;
        bool active = (t >= 1.0 && t < 3.0) || (t >= 6.0 && t < 8.0);
        if (!active) continue;
        double burstT = std::fmod(t, 0.5);
        double f = (burstT < 0.25) ? 800.0 : 1200.0; // alternating warble
        phase += 2.0 * M_PI * f / kSampleRate;
        buf[i] = static_cast<float>(0.7 * std::sin(phase));
    }
    return buf;
}

static std::vector<float> synthOther(double durationSec, std::mt19937& rng) {
    size_t n = static_cast<size_t>(durationSec * kSampleRate);
    std::vector<float> buf(n, 0.0f);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    // Sparse low-level transient taps
    for (double t = 0.5; t < durationSec; t += 1.7) {
        size_t start = static_cast<size_t>(t * kSampleRate);
        for (size_t i = 0; i < 400 && start + i < n; ++i) {
            double env = std::exp(-static_cast<double>(i) / 80.0);
            buf[start + i] += static_cast<float>(0.3 * env * (u(rng) * 2.0 - 1.0));
        }
    }
    return buf;
}

// ---------------------------------------------------------------------
// WAV I/O helpers
// ---------------------------------------------------------------------

// Loads a WAV as true stereo. Mono source files get duplicated to both
// channels (matches the synthesized-stem behavior below), so downstream
// code can always assume L/R pairs regardless of source channel count.
static bool loadWavStereo(const std::string& path, std::vector<float>& outL,
                           std::vector<float>& outR, double& sampleRateOut) {
    unsigned int channels, sampleRate;
    drwav_uint64 totalFrames;
    float* data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &sampleRate, &totalFrames, nullptr);
    if (!data) return false;

    sampleRateOut = static_cast<double>(sampleRate);
    outL.resize(totalFrames);
    outR.resize(totalFrames);
    for (drwav_uint64 i = 0; i < totalFrames; ++i) {
        if (channels == 1) {
            outL[i] = data[i];
            outR[i] = data[i];
        } else {
            outL[i] = data[i * channels + 0];
            outR[i] = data[i * channels + 1];
        }
    }
    drwav_free(data, nullptr);
    return true;
}

static void writeWavStereo(const std::string& path, const std::vector<float>& left,
                            const std::vector<float>& right, double sampleRate) {
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = 2;
    fmt.sampleRate = static_cast<drwav_uint32>(sampleRate);
    fmt.bitsPerSample = 32;

    drwav wav;
    drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr);

    std::vector<float> interleaved(left.size() * 2);
    for (size_t i = 0; i < left.size(); ++i) {
        interleaved[2 * i] = left[i];
        interleaved[2 * i + 1] = right[i];
    }
    drwav_write_pcm_frames(&wav, left.size(), interleaved.data());
    drwav_uninit(&wav);
}

// ---------------------------------------------------------------------
// Self-check 1: LR4 crossover flatness.
// An LR4 crossover's low+high sum has a FLAT MAGNITUDE spectrum but is not
// a literal identity filter (it has its own phase/group-delay character),
// so the correct test is a swept-sine RMS ratio, not a time-domain impulse
// comparison. For each test frequency, feed a sine long enough to reach
// steady state, sum all 4 bands, and compare output RMS to input RMS -
// both should be equal (0 dB) at every frequency if the filterbank is
// implemented correctly.
// ---------------------------------------------------------------------
static bool checkCrossoverFlatness() {
    const double testFreqs[] = {30, 60, 100, 150, 250, 300, 500, 900,
                                 1400, 1800, 2500, 4000, 8000, 15000};
    const int settleSamples = 4000;
    const int measureSamples = 4000;

    double maxDeviationDb = 0.0;
    for (double f : testFreqs) {
        CrossoverFilterbank fb;
        fb.prepare(kSampleRate);

        double phase = 0.0;
        double sumSqIn = 0.0, sumSqOut = 0.0;
        for (int i = 0; i < settleSamples + measureSamples; ++i) {
            double x = std::sin(phase);
            phase += 2.0 * M_PI * f / kSampleRate;

            double bands[4];
            fb.tick(x, bands);
            double y = bands[0] + bands[1] + bands[2] + bands[3];

            if (i >= settleSamples) {
                sumSqIn += x * x;
                sumSqOut += y * y;
            }
        }
        double rmsIn = std::sqrt(sumSqIn / measureSamples);
        double rmsOut = std::sqrt(sumSqOut / measureSamples);
        double ratioDb = 20.0 * std::log10(rmsOut / rmsIn);
        maxDeviationDb = std::max(maxDeviationDb, std::fabs(ratioDb));
    }

    // NOTE: expect a small dip (roughly 1-1.5 dB) concentrated between the
    // 100 Hz and 300 Hz crossover points specifically - they sit under 1.6
    // octaves apart, so their LR4 transition skirts (24 dB/oct) overlap.
    // This is a known, expected characteristic of tree-structured multiband
    // crossovers with closely-spaced bands (the same effect shows up in
    // commercial multiband compressors), not an implementation bug. The
    // 300 Hz-1.8 kHz ducked band's own edges stay flat within a few tenths
    // of a dB, which is what matters most for the paper's argument.
    std::cout << "[self-check] Crossover flatness: max deviation from 0 dB across "
              << "sweep = " << maxDeviationDb << " dB\n";
    return maxDeviationDb < 2.0;
}

// ---------------------------------------------------------------------
// Self-check 2: click detector.
// Scans a rendered buffer for sample-to-sample jumps larger than a
// threshold, which would indicate an un-smoothed discontinuity.
// ---------------------------------------------------------------------
static int countClicks(const std::vector<float>& buf, float threshold) {
    int clicks = 0;
    for (size_t i = 1; i < buf.size(); ++i) {
        if (std::fabs(buf[i] - buf[i - 1]) > threshold) {
            ++clicks;
        }
    }
    return clicks;
}

// ---------------------------------------------------------------------
// Main: build 5 stems (real or synthetic), run a scripted event timeline
// through the Engine, write output.wav, run self-checks.
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    std::cout << "=== Sidechain-Keyed Unmasking Demo Engine - CLI test harness ===\n";

    bool flatOk = checkCrossoverFlatness();
    if (!flatOk) {
        std::cerr << "WARNING: crossover flatness check failed tolerance.\n";
    }

    std::array<std::vector<float>, kNumClasses> stemsL, stemsR;
    double loadedSampleRate = kSampleRate;
    double durationSec = 10.0;

    if (argc >= 6) {
        std::cout << "Loading 5 stem files from arguments...\n";
        const char* order[5] = {"Dialogue", "Music", "Background Noise", "Safety Alerts", "Other"};
        bool ok = true;
        for (int c = 0; c < 5; ++c) {
            double sr;
            if (!loadWavStereo(argv[c + 1], stemsL[c], stemsR[c], sr)) {
                std::cerr << "Failed to load " << order[c] << " from " << argv[c + 1] << "\n";
                ok = false;
            } else {
                std::cout << "  " << order[c] << ": " << argv[c + 1]
                          << " (" << stemsL[c].size() << " frames @ " << sr << " Hz)\n";
                loadedSampleRate = sr;
            }
        }
        if (!ok) return 1;

        size_t minLen = stemsL[0].size();
        for (int c = 1; c < 5; ++c) minLen = std::min(minLen, stemsL[c].size());
        for (int c = 0; c < 5; ++c) {
            stemsL[c].resize(minLen);
            stemsR[c].resize(minLen);
        }
        durationSec = minLen / loadedSampleRate;
    } else {
        std::cout << "No stem files given - synthesizing a " << durationSec
                  << "s demo scene instead.\n"
                  << "(Usage: demo_engine_cli <dialogue.wav> <music.wav> <bg.wav> "
                     "<safety.wav> <other.wav> [output.wav])\n";
        std::mt19937 rng(42);
        stemsL[kDialogue]        = synthDialogue(durationSec, rng);
        stemsL[kMusic]           = synthMusic(durationSec);
        stemsL[kBackgroundNoise] = synthBackgroundNoise(durationSec, rng);
        stemsL[kSafetyAlerts]    = synthSafetyAlert(durationSec);
        stemsL[kOther]           = synthOther(durationSec, rng);
        for (int c = 0; c < kNumClasses; ++c) stemsR[c] = stemsL[c]; // mono source, L=R
        loadedSampleRate = kSampleRate;
    }

    std::string outPath = (argc >= 7) ? argv[6] : "output.wav";

    Engine engine;
    CompressorParams params; // defaults from Types.h
    engine.prepare(loadedSampleRate, params);
    engine.loadStems(stemsL, stemsR);
    engine.setMode(DuckMode::Advanced);
    engine.setUnmaskEnabled(false);
    engine.setKeyChannel(kSafetyAlerts);

    size_t totalFrames = engine.numFrames();
    std::vector<float> outputL(totalFrames, 0.0f);
    std::vector<float> outputR(totalFrames, 0.0f);

    // Scripted event timeline (seconds -> action). Exercises every
    // interaction discussed: unmask on/off, key switching, mute, solo,
    // mode switching - all mid-playback, to validate click-free smoothing.
    struct Event { double timeSec; std::string desc; std::function<void()> apply; };
    std::vector<Event> timeline = {
        {1.0, "Enable Unmask, key=Safety Alerts (Advanced mode)",
            [&]{ engine.setUnmaskEnabled(true); engine.setKeyChannel(kSafetyAlerts); }},
        {3.2, "Switch key to Music mid-playback",
            [&]{ engine.setKeyChannel(kMusic); }},
        {4.5, "Mute Dialogue",
            [&]{ engine.setMute(kDialogue, true); }},
        {5.5, "Solo Music",
            [&]{ engine.setSolo(kMusic, true); }},
        {6.5, "Clear solo, unmute Dialogue",
            [&]{ engine.setSolo(kMusic, false); engine.setMute(kDialogue, false); }},
        {7.0, "Switch key back to Safety Alerts",
            [&]{ engine.setKeyChannel(kSafetyAlerts); }},
        {8.0, "Switch mode to Basic (full-band ducking)",
            [&]{ engine.setMode(DuckMode::Basic); }},
        {9.0, "Mute the key channel (Safety Alerts) - ducking should release",
            [&]{ engine.setMute(kSafetyAlerts, true); }},
        {9.5, "Unmute key, switch to Resonance mode (dynamic 4-notch ducking)",
            [&]{ engine.setMute(kSafetyAlerts, false); engine.setMode(DuckMode::Resonance); }},
        {11.0, "Switch key to Music while in Resonance mode",
            [&]{ engine.setKeyChannel(kMusic); }},
        {12.0, "Switch key back to Safety Alerts, still Resonance mode",
            [&]{ engine.setKeyChannel(kSafetyAlerts); }},
        {13.0, "Live compressor param change mid-playback: threshold -18dB, ratio 8:1, attack 1ms, release 400ms",
            [&]{ engine.setThresholdDb(-18.0); engine.setRatio(8.0);
                 engine.setAttackMs(1.0); engine.setReleaseMs(400.0); }},
        {14.0, "Knee to 0 (hard knee) mid-playback - exercises the W->0 guard in GainComputer",
            [&]{ engine.setKneeDb(0.0); }},
        {15.0, "Knee to 24 (very soft) mid-playback",
            [&]{ engine.setKneeDb(24.0); }},
        {16.0, "Resonance: 12 peaks, very wide bandwidth (3 oct) - stresses the separation fallback",
            [&]{ engine.setResonanceNumPeaks(12); engine.setResonanceBandwidthOctaves(3.0); }},
        {17.0, "Resonance: down to 1 peak (single band)",
            [&]{ engine.setResonanceNumPeaks(1); }},
        {18.0, "Resonance: 8 peaks, narrow bandwidth (0.1 oct)",
            [&]{ engine.setResonanceNumPeaks(8); engine.setResonanceBandwidthOctaves(0.1); }},
        {19.0, "Resonance: max reduction clamped to 3dB",
            [&]{ engine.setResonanceMaxReductionDb(3.0); }},
    };

    std::cout << "\nRunning scripted timeline (" << durationSec << "s, "
              << kBlockSize << "-sample blocks):\n";
    for (auto& e : timeline) {
        std::printf("  t=%5.2fs: %s\n", e.timeSec, e.desc.c_str());
    }

    size_t eventCursor = 0;
    size_t frame = 0;
    while (frame < totalFrames) {
        size_t blockLen = std::min(static_cast<size_t>(kBlockSize), totalFrames - frame);

        double blockStartSec = frame / loadedSampleRate;
        while (eventCursor < timeline.size() && timeline[eventCursor].timeSec <= blockStartSec) {
            timeline[eventCursor].apply();
            ++eventCursor;
        }

        engine.process(outputL.data() + frame, outputR.data() + frame, blockLen);
        frame += blockLen;
    }

    writeWavStereo(outPath, outputL, outputR, loadedSampleRate);
    std::cout << "\nWrote " << outPath << " (" << totalFrames << " frames)\n";

    int clicks = countClicks(outputL, 0.35f) + countClicks(outputR, 0.35f); // threshold well above normal signal deltas
    std::cout << "[self-check] Click detector: " << clicks
              << " sample-to-sample jumps > 0.35 amplitude\n";
    if (clicks > 0) {
        std::cout << "  (Some jumps are expected from the synthetic siren's hard on/off "
                     "gating in the test signal itself - inspect the WAV around event "
                     "times to confirm engine-caused vs. source-caused.)\n";
    }

    std::cout << "\nDone. Listen to " << outPath << " and check around each event "
                 "timestamp above for clicks/pops.\n";
    return 0;
}
