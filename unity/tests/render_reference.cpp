// Parity-test reference renderer: drives the real C++ Engine (include/)
// through each scenario ParityTest.cs reproduces with the Unity C# port,
// and writes raw float32 interleaved-stereo files the C# harness reads.
//
//   render_reference <sceneDir> <outDir> <seconds>
//
// Writes <outDir>/stem<c>.f32 (the 5 stems, trimmed) and
// <outDir>/ref_<scenario>.f32 (engine output) for every scenario below.
// Parameter values are chosen to be exactly representable as float, since
// the Unity-side ProcessingSettings stores floats.
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "Engine.h"

using namespace demo;

static const char* kStemFiles[kNumClasses] = {"Dialogue.wav", "MUSIC.wav", "BKG.wav", "SAFETY.wav", "OTHER.wav"};

static void writeF32(const std::string& path, const std::vector<float>& interleaved) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(interleaved.data(), sizeof(float), interleaved.size(), f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 4) { std::cerr << "usage: render_reference <sceneDir> <outDir> <seconds>\n"; return 1; }
    std::string sceneDir = argv[1], outDir = argv[2];
    double seconds = std::stod(argv[3]);

    std::array<std::vector<float>, kNumClasses> L, R;
    double sampleRate = 48000.0;
    size_t frames = 0;
    for (int c = 0; c < kNumClasses; ++c) {
        unsigned int ch, sr;
        drwav_uint64 total;
        float* d = drwav_open_file_and_read_pcm_frames_f32((sceneDir + "/" + kStemFiles[c]).c_str(), &ch, &sr, &total, nullptr);
        if (!d) { std::cerr << "failed to load " << kStemFiles[c] << "\n"; return 1; }
        sampleRate = sr;
        frames = std::min<size_t>(total, static_cast<size_t>(seconds * sr));
        L[c].resize(frames);
        R[c].resize(frames);
        std::vector<float> inter(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            L[c][i] = d[i * ch];
            R[c][i] = d[i * ch + (ch > 1 ? 1 : 0)];
            inter[2 * i] = L[c][i];
            inter[2 * i + 1] = R[c][i];
        }
        drwav_free(d, nullptr);
        writeF32(outDir + "/stem" + std::to_string(c) + ".f32", inter);
    }
    for (int c = 0; c < kNumClasses; ++c) { L[c].resize(frames); R[c].resize(frames); }

    auto render = [&](const std::string& name, CompressorParams params, const std::function<void(Engine&)>& configure) {
        Engine e;
        e.prepare(sampleRate, params);
        e.loadStems(L, R);
        configure(e);
        std::vector<float> outL(frames), outR(frames);
        const size_t block = 128;
        for (size_t i = 0; i < frames; i += block) {
            size_t n = std::min(block, frames - i);
            e.process(outL.data() + i, outR.data() + i, n);
        }
        std::vector<float> inter(frames * 2);
        for (size_t i = 0; i < frames; ++i) { inter[2 * i] = outL[i]; inter[2 * i + 1] = outR[i]; }
        writeF32(outDir + "/ref_" + name + ".f32", inter);
        std::cout << "rendered " << name << "\n";
    };

    // Sidechain settings shared by the Unmasking scenarios (near the thesis's
    // "Balanced" tier, rounded to float-exact values).
    CompressorParams sc;
    sc.thresholdDb = -40.5; sc.ratio = 5.5; sc.kneeDb = 22.5; sc.attackMs = 16.5; sc.releaseMs = 347.0;

    render("unprocessed", sc, [](Engine& e) { e.setMode(DuckMode::Basic); });

    render("basic_dialogue", sc, [](Engine& e) {
        e.setMode(DuckMode::Basic);
        e.setKeyChannel(kDialogue);
        e.setUnmaskEnabled(true);
    });

    render("advanced_dialogue", sc, [](Engine& e) {
        e.setMode(DuckMode::Advanced);
        e.setKeyChannel(kDialogue);
        e.setUnmaskEnabled(true);
        e.setMaxReductionDb(9.0);
    });

    CompressorParams scSafety = sc;
    scSafety.thresholdDb = -36.0;
    scSafety.safetyGainDb = 3.0;
    render("advanced_safety_keygain", scSafety, [](Engine& e) {
        e.setMode(DuckMode::Advanced);
        e.setKeyChannel(kSafetyAlerts);
        e.setUnmaskEnabled(true);
    });

    // Enhancement: deliberately different params per class.
    auto configureEnhancement = [](Engine& e) {
        const double thr[kNumClasses] = {-30.0, -36.0, -42.0, -24.0, -33.0};
        const double ratio[kNumClasses] = {3.0, 4.0, 6.0, 2.5, 5.0};
        const double knee[kNumClasses] = {6.0, 12.0, 0.0, 8.0, 4.0};
        const double atk[kNumClasses] = {5.0, 10.0, 2.0, 20.0, 8.0};
        const double rel[kNumClasses] = {120.0, 250.0, 60.0, 400.0, 90.0};
        const double makeup[kNumClasses] = {3.0, 0.0, -2.0, 6.0, 1.5};
        for (int c = 0; c < kNumClasses; ++c) {
            e.setEnhancementThresholdDb(c, thr[c]);
            e.setEnhancementRatio(c, ratio[c]);
            e.setEnhancementKneeDb(c, knee[c]);
            e.setEnhancementAttackMs(c, atk[c]);
            e.setEnhancementReleaseMs(c, rel[c]);
            e.setEnhancementMakeupGainDb(c, makeup[c]);
        }
        e.setProcessingTab(ProcessingTab::Enhancement);
    };
    render("enh_single", sc, [&](Engine& e) {
        configureEnhancement(e);
        e.setEnhancementBandMode(EnhancementBandMode::SingleBand);
    });
    render("enh_multi", sc, [&](Engine& e) {
        configureEnhancement(e);
        e.setEnhancementBandMode(EnhancementBandMode::Multiband);
    });

    render("advanced_wdrc", sc, [](Engine& e) {
        e.setMode(DuckMode::Advanced);
        e.setKeyChannel(kDialogue);
        e.setUnmaskEnabled(true);
        e.setWdrcThresholdDb(-30.0);
        e.setWdrcRatio(3.0);
        e.setWdrcMakeupGainDb(4.0);
        e.setWdrcAttackMs(5.0);
        e.setWdrcReleaseMs(150.0);
        e.setWdrcBypassed(false);
    });

    std::cout << "frames=" << frames << " sampleRate=" << sampleRate << "\n";
    return 0;
}
