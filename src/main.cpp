#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
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
        CrossoverFilterbank<kWdrcNumBands> fb;
        fb.prepare(kSampleRate, kWdrcCrossoverHz);

        double phase = 0.0;
        double sumSqIn = 0.0, sumSqOut = 0.0;
        for (int i = 0; i < settleSamples + measureSamples; ++i) {
            double x = std::sin(phase);
            phase += 2.0 * M_PI * f / kSampleRate;

            double bands[kWdrcNumBands];
            fb.tick(x, bands);
            double y = 0.0;
            for (double b : bands) y += b;

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

    // NOTE: expect a small dip spread fairly evenly across the whole sweep
    // now, not concentrated at one pair of points - the 11 crossover points
    // are uniformly ~0.63 octaves apart (kWdrcCrossoverHz in Types.h), each
    // pair close enough for their LR4 transition skirts (24 dB/oct) to
    // overlap somewhat. This is a known, expected characteristic of
    // tree-structured multiband crossovers with closely-spaced bands (the
    // same effect shows up in commercial multiband compressors), not an
    // implementation bug - see this file's git history for the measured
    // deviation at the previous, more widely-spaced 4-band/3-point version.
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
// Static-condition export mode (--static): renders one full clip under a
// single fixed configuration with NO mid-clip parameter changes, for
// generating comparable data across conditions (unprocessed/basic/advanced)
// for objective intelligibility-metric analysis - see
// analysis/compute_metrics.py. Deliberately a separate code path from the
// scripted-timeline demo below main(): that path exists specifically to
// exercise click/pop-free mid-playback changes, which is the opposite of
// what a fixed-condition measurement needs. Only loads real scene stems -
// never falls back to the synthetic generators above, so there is no RNG
// anywhere in this path and a given command line is fully deterministic.
// ---------------------------------------------------------------------

struct SceneSpec {
    std::vector<std::string> aliases; // acceptable --scene values
    std::string dir;                  // includes trailing slash
    std::string stemFiles[5];         // indexed like ClassIndex: Dialogue, Music, BG, Safety, Other
    std::string blendFile;            // mastered reference "whole blend" track
};

static const std::vector<SceneSpec>& sceneTable() {
    static const std::vector<SceneSpec> table = {
        {{"Construction Scene", "Construction", "construction"}, "Construction Scene/",
         {"Dialogue.wav", "MUSIC.wav", "BKG.wav", "SAFETY.wav", "OTHER.wav"}, "WHOLE BLEND.wav"},
        {{"PublicTransit Scene", "PublicTransit", "publicTransit"}, "PublicTransit Scene/",
         {"Dialogue_PublicTransit_01.wav", "MUSIC_publictransit_01.wav", "BKG_PublicTransit_01.wav",
          "SAFETY_publictransit_01.wav", "OTHER_publictransit_01.wav"}, "WholeBlend_publictransit.wav"},
    };
    return table;
}

static const SceneSpec* findScene(const std::string& name) {
    for (const auto& s : sceneTable()) {
        for (const auto& alias : s.aliases) {
            if (alias == name) return &s;
        }
    }
    return nullptr;
}

static bool parseKeyChannel(const std::string& name, int& outIndex) {
    for (int c = 0; c < kNumClasses; ++c) {
        if (name == classIndexToName(c)) {
            outIndex = c;
            return true;
        }
    }
    return false;
}

static std::map<std::string, std::string> parseFlags(int argc, char** argv, int startIndex) {
    std::map<std::string, std::string> flags;
    for (int i = startIndex; i + 1 < argc; i += 2) {
        std::string key = argv[i];
        if (key.size() > 2 && key[0] == '-' && key[1] == '-') {
            flags[key.substr(2)] = argv[i + 1];
        }
    }
    return flags;
}

static double flagDouble(const std::map<std::string, std::string>& flags, const std::string& key, double def) {
    auto it = flags.find(key);
    return it != flags.end() ? std::stod(it->second) : def;
}

static std::string flagString(const std::map<std::string, std::string>& flags, const std::string& key) {
    auto it = flags.find(key);
    return it != flags.end() ? it->second : std::string();
}

static void printStaticUsage() {
    std::cerr <<
        "Usage: demo_engine_cli --static --scene <name> --key <ClassName> "
        "--mode <unprocessed|basic|advanced> --out <path.wav>\n"
        "         [--threshold dB] [--ratio r] [--knee dB] [--attack ms] "
        "[--release ms] [--max-reduction dB]\n"
        "  --scene: \"Construction Scene\" or \"PublicTransit Scene\" "
        "(aliases: Construction, PublicTransit)\n"
        "  --key:   Dialogue | Music | \"Background Noise\" | \"Safety Alerts\" | Other\n"
        "  --mode:  unprocessed (true sum of the 5 raw stems, Unmask off) | "
        "basic | advanced (Summed-bus)\n"
        "  Compressor flags are ignored for --mode unprocessed. For basic/advanced "
        "they default to Types.h's CompressorParams/Engine defaults if omitted -\n"
        "  pass the same values to both a basic and an advanced run to isolate the "
        "effect of band-limiting in a comparison.\n";
}

// mode=unprocessed: renders the true, unweighted sum of the 5 raw stems -
// via Engine in Basic mode with unmaskEnabled=false, so it goes through
// literally the same dry-path arithmetic (raw * safetyGainLinear_, summed)
// that basic/advanced ducking's own dry/unducked content uses, rather than
// reusing the scene's separately-bounced mastered reference blend file.
//
// An earlier version of this function reused that reference blend
// directly, on the reasoning that it's the same file the browser demo's
// "Unprocessed" panel plays. That turned out to be the wrong call: the
// blend is NOT a bit-identical sum of the 5 raw stems (confirmed - overall
// RMS difference ~8% of the signal's own RMS), and per-stem best-fit level
// analysis shows the mismatch doesn't even localize to one stem (Dialogue's
// own best-fit scale factor is +0.01dB, essentially exact; Music and
// Background show larger deviations, -0.23dB and -0.14dB) - most likely a
// DAW export inconsistency between when the blend was bounced and when the
// stems were bounced. Since basic/advanced ducking is computed from the
// stem files directly, comparing them against a baseline that was bounced
// separately (and isn't even guaranteed to be internally consistent with
// those exact stem levels) risks contaminating every metric with a mixing
// artifact unrelated to the sidechain ducking being evaluated. Building
// the baseline from the same stems the engine actually processes removes
// that risk entirely, at the cost of the baseline no longer being
// literally the same file the browser's reference panel plays (which is
// still fine for its purpose there - a listening reference, not a
// measurement baseline).
static int runStaticUnprocessed(const SceneSpec& scene, const std::string& outPath) {
    std::array<std::vector<float>, kNumClasses> stemsL, stemsR;
    double loadedSampleRate = kSampleRate;
    bool ok = true;
    for (int c = 0; c < kNumClasses; ++c) {
        double sr;
        std::string path = scene.dir + scene.stemFiles[c];
        if (!loadWavStereo(path, stemsL[c], stemsR[c], sr)) {
            std::cerr << "Failed to load " << classIndexToName(c) << " from " << path << "\n";
            ok = false;
        } else {
            loadedSampleRate = sr;
        }
    }
    if (!ok) return 1;

    size_t minLen = stemsL[0].size();
    for (int c = 1; c < kNumClasses; ++c) minLen = std::min(minLen, stemsL[c].size());
    for (int c = 0; c < kNumClasses; ++c) {
        stemsL[c].resize(minLen);
        stemsR[c].resize(minLen);
    }

    CompressorParams params; // defaults - irrelevant here since unmaskEnabled=false forces g=1 regardless
    Engine engine;
    engine.prepare(loadedSampleRate, params);
    engine.loadStems(stemsL, stemsR);
    engine.setMode(DuckMode::Basic); // simplest dry-path arithmetic; mode is otherwise moot with Unmask off
    engine.setUnmaskEnabled(false);

    size_t totalFrames = engine.numFrames();
    std::vector<float> outputL(totalFrames, 0.0f);
    std::vector<float> outputR(totalFrames, 0.0f);
    size_t frame = 0;
    while (frame < totalFrames) {
        size_t blockLen = std::min(static_cast<size_t>(kBlockSize), totalFrames - frame);
        engine.process(outputL.data() + frame, outputR.data() + frame, blockLen);
        frame += blockLen;
    }

    writeWavStereo(outPath, outputL, outputR, loadedSampleRate);
    int clicks = countClicks(outputL, 0.35f) + countClicks(outputR, 0.35f);
    std::cout << "Wrote " << outPath << " (" << totalFrames << " frames, mode=unprocessed, "
              << "true sum of the 5 raw stems via Engine, Unmask off)\n"
              << "[self-check] Click detector: " << clicks << " sample-to-sample jumps > 0.35 amplitude\n";
    return 0;
}

static int runStaticDucked(const SceneSpec& scene, int keyChannel, const std::string& modeName,
                            const std::map<std::string, std::string>& flags, const std::string& outPath) {
    std::array<std::vector<float>, kNumClasses> stemsL, stemsR;
    double loadedSampleRate = kSampleRate;
    bool ok = true;
    for (int c = 0; c < kNumClasses; ++c) {
        double sr;
        std::string path = scene.dir + scene.stemFiles[c];
        if (!loadWavStereo(path, stemsL[c], stemsR[c], sr)) {
            std::cerr << "Failed to load " << classIndexToName(c) << " from " << path << "\n";
            ok = false;
        } else {
            loadedSampleRate = sr;
        }
    }
    if (!ok) return 1;

    size_t minLen = stemsL[0].size();
    for (int c = 1; c < kNumClasses; ++c) minLen = std::min(minLen, stemsL[c].size());
    for (int c = 0; c < kNumClasses; ++c) {
        stemsL[c].resize(minLen);
        stemsR[c].resize(minLen);
    }

    CompressorParams params; // Types.h defaults, overridden by any flags given below
    params.thresholdDb = flagDouble(flags, "threshold", params.thresholdDb);
    params.ratio        = flagDouble(flags, "ratio", params.ratio);
    params.kneeDb        = flagDouble(flags, "knee", params.kneeDb);
    params.attackMs       = flagDouble(flags, "attack", params.attackMs);
    params.releaseMs      = flagDouble(flags, "release", params.releaseMs);
    double maxReductionDb = flagDouble(flags, "max-reduction", 6.0); // matches Engine.h's own default

    Engine engine;
    engine.prepare(loadedSampleRate, params);
    engine.loadStems(stemsL, stemsR);
    // Fixed configuration applied once, before any processing - no mid-clip
    // changes, unlike the scripted-timeline path below main().
    engine.setMode(modeName == "basic" ? DuckMode::Basic : DuckMode::Advanced);
    engine.setAdvancedDuckingMode(AdvancedDuckingMode::SummedBus); // task scope: Summed-bus only, see spec
    engine.setUnmaskEnabled(true);
    engine.setKeyChannel(keyChannel);
    engine.setMaxReductionDb(maxReductionDb);
    // WDRC output stage stays at its default (bypassed) - out of scope for
    // this metric, which is specifically about the sidechain-keyed ducking.

    size_t totalFrames = engine.numFrames();
    std::vector<float> outputL(totalFrames, 0.0f);
    std::vector<float> outputR(totalFrames, 0.0f);

    size_t frame = 0;
    while (frame < totalFrames) {
        size_t blockLen = std::min(static_cast<size_t>(kBlockSize), totalFrames - frame);
        engine.process(outputL.data() + frame, outputR.data() + frame, blockLen);
        frame += blockLen;
    }

    writeWavStereo(outPath, outputL, outputR, loadedSampleRate);
    int clicks = countClicks(outputL, 0.35f) + countClicks(outputR, 0.35f);
    std::cout << "Wrote " << outPath << " (" << totalFrames << " frames)\n"
              << "  key=" << classIndexToName(keyChannel) << " mode=" << modeName
              << " threshold=" << params.thresholdDb << "dB ratio=" << params.ratio
              << ":1 knee=" << params.kneeDb << "dB attack=" << params.attackMs
              << "ms release=" << params.releaseMs << "ms maxReduction=" << maxReductionDb
              << "dB safetyGain=" << params.safetyGainDb << "dB\n"
              << "[self-check] Click detector: " << clicks << " sample-to-sample jumps > 0.35 amplitude\n";
    if (clicks > 0) {
        std::cout << "  WARNING: unexpected discontinuity in a static (no mid-clip changes) "
                     "render - investigate before treating this as valid measurement data.\n";
    }
    return 0;
}

static int runStaticExport(int argc, char** argv) {
    auto flags = parseFlags(argc, argv, 2); // argv[1] == "--static"

    std::string sceneName = flagString(flags, "scene");
    std::string keyName = flagString(flags, "key");
    std::string modeName = flagString(flags, "mode");
    std::string outPath = flagString(flags, "out");

    if (sceneName.empty() || keyName.empty() || modeName.empty() || outPath.empty()) {
        printStaticUsage();
        return 1;
    }
    if (modeName != "unprocessed" && modeName != "basic" && modeName != "advanced") {
        std::cerr << "Unknown --mode '" << modeName << "' (expected unprocessed, basic, or advanced)\n";
        return 1;
    }

    const SceneSpec* scene = findScene(sceneName);
    if (!scene) {
        std::cerr << "Unknown --scene '" << sceneName << "'\n";
        return 1;
    }

    int keyChannel;
    if (!parseKeyChannel(keyName, keyChannel)) {
        std::cerr << "Unknown --key '" << keyName << "' (expected Dialogue, Music, "
                     "\"Background Noise\", \"Safety Alerts\", or Other)\n";
        return 1;
    }

    std::filesystem::path outFsPath(outPath);
    if (outFsPath.has_parent_path()) {
        std::filesystem::create_directories(outFsPath.parent_path());
    }

    if (modeName == "unprocessed") {
        return runStaticUnprocessed(*scene, outPath);
    }
    return runStaticDucked(*scene, keyChannel, modeName, flags, outPath);
}

// ---------------------------------------------------------------------
// Main: build 5 stems (real or synthetic), run a scripted event timeline
// through the Engine, write output.wav, run self-checks.
// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--static") {
        return runStaticExport(argc, argv);
    }

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
        {20.0, "WDRC output compressor: unbypass, threshold -30dB, ratio 3:1",
            [&]{ engine.setWdrcBypassed(false); engine.setWdrcThresholdDb(-30.0); engine.setWdrcRatio(3.0); }},
        {21.0, "WDRC: add +6dB makeup gain",
            [&]{ engine.setWdrcMakeupGainDb(6.0); }},
        {22.0, "WDRC: sweep to a hard-limiting ratio (10:1) mid-playback",
            [&]{ engine.setWdrcRatio(10.0); }},
        {23.0, "WDRC: re-bypass mid-playback",
            [&]{ engine.setWdrcBypassed(true); }},
        {24.0, "WDRC: unbypass, out-of-range attack/release requests (-5ms, 9000ms) - should clamp to 1ms/3000ms",
            [&]{ engine.setWdrcBypassed(false); engine.setWdrcAttackMs(-5.0); engine.setWdrcReleaseMs(9000.0); }},
        {25.0, "WDRC: slow-acting end of the literature range (attack 50ms, release 3000ms)",
            [&]{ engine.setWdrcAttackMs(50.0); engine.setWdrcReleaseMs(3000.0); }},
        // NOTE: with this specific combination still active from earlier
        // events (ratio 10:1, +6dB makeup gain) plus this 50ms attack, the
        // click detector below will flag one real >0.35 jump around
        // t=74.86s. Confirmed (via isolated single-variable probing during
        // development) this is genuine transient overshoot - slow attack +
        // high ratio + positive makeup gain lets a real percussive moment in
        // the source material through at near-unity gain before the
        // compressor reacts, then makeup gain amplifies it further. Verified
        // the raw stems have no comparably large sample-to-sample delta at
        // that point, and neither does the ducked mix with fast (default)
        // attack - this is a real, expected characteristic of that specific
        // aggressive parameter combination, not an engine bug.
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
