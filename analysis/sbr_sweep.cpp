// Speech-to-background ratio (SBR) parameter sweep, after Johnson & Healy
// (2024), "The Optimal Speech-to-Background Ratio for Balancing Speech
// Recognition With Environmental Sound Recognition", Ear & Hearing 45(6).
//
// Metric (decided with the experimenter, 2026-10-15):
//   SBR = 10*log10( sum_active d^2 / sum_active b^2 )
//   d = processed Dialogue (incl. its key gain / makeup), both channels
//   b = processed sum of the other four stems (Music, Background, Safety,
//       Other), both channels
//   "active" = samples inside a 20 ms / 50 %-overlap frame where the RAW
//       Dialogue stem is within 40 dB of its own peak frame level - the
//       same activity gate as compute_metrics.py's masking margin.
// Broadband and unweighted, as in J&H. Scored against both J&H targets:
//   HI  +5.1 dB  (95 % window 4.7 to 5.8 dB)
//   NH +12.2 dB  (95 % window 2.8 to 22.4 dB)
//
// d and b come from two renders per point with identical processing state:
// the full mix, and the mix with Dialogue soloed (the sidechain detector
// reads the raw key regardless of solo, so the ducking is unchanged).
// b = mix - d exactly, since the mix is a plain sum (output WDRC bypassed).
//
// Sweeps (Latin Hypercube, same points evaluated on all three scenes):
//   basic        - Basic (full-band) sidechain ducking, Dialogue key.
//                  threshold, ratio, knee, attack, release, max reduction,
//                  key gain (safetyGainDb, limited to +-2 dB).
//   enhancement  - single-band group compression on all 5 groups.
//                  per-group threshold / ratio / makeup (makeup limited to
//                  +-2 dB), shared knee / attack / release. Also reports the
//                  SBR the makeup gains alone would give, so the share of
//                  the SBR change produced by the compression itself is
//                  visible (drc_shift_db).
//
// Build + run: see analysis/run_sbr_sweep.sh
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "Engine.h"

using namespace demo;

namespace {

constexpr int kBlock = 32; // gain-reduction telemetry is sampled once per block

struct Scene {
    std::string name;
    std::string dir;
    std::array<std::string, kNumClasses> files;
    std::array<std::vector<float>, kNumClasses> L, R;
    std::vector<unsigned char> active; // per sample
    size_t frames = 0;
    double sampleRate = 48000.0;
    double activeFraction = 0.0;
};

struct Range { const char* name; double lo, hi; };

const std::vector<Range> kBasicDims = {
    {"threshold_db", -70.0, -10.0},
    {"ratio", 1.5, 20.0},
    {"knee_db", 0.0, 24.0},
    {"attack_ms", 1.0, 50.0},
    {"release_ms", 20.0, 1000.0},
    {"max_reduction_db", 3.0, 30.0},
    {"key_gain_db", -2.0, 2.0},
};

std::vector<Range> enhancementDims() {
    static const char* cls[kNumClasses] = {"dialogue", "music", "background", "safety", "other"};
    static std::vector<std::string> names; // keep c_str() storage alive
    std::vector<Range> d;
    names.clear();
    names.reserve(kNumClasses * 3);
    for (int c = 0; c < kNumClasses; ++c) {
        names.push_back(std::string(cls[c]) + "_threshold_db");
        d.push_back({names.back().c_str(), -60.0, -10.0});
        names.push_back(std::string(cls[c]) + "_ratio");
        d.push_back({names.back().c_str(), 1.0, 10.0});
        names.push_back(std::string(cls[c]) + "_makeup_db");
        d.push_back({names.back().c_str(), -2.0, 2.0});
    }
    d.push_back({"knee_db", 0.0, 24.0});
    d.push_back({"attack_ms", 1.0, 50.0});
    d.push_back({"release_ms", 20.0, 1000.0});
    return d;
}

bool loadScene(Scene& s, const std::string& repo) {
    for (int c = 0; c < kNumClasses; ++c) {
        unsigned ch, sr;
        drwav_uint64 n;
        std::string path = repo + "/" + s.dir + "/" + s.files[c];
        float* d = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &n, nullptr);
        if (!d) { std::cerr << "cannot load " << path << "\n"; return false; }
        s.sampleRate = sr;
        s.L[c].resize(n);
        s.R[c].resize(n);
        for (drwav_uint64 i = 0; i < n; ++i) {
            s.L[c][i] = d[i * ch];
            s.R[c][i] = d[i * ch + (ch > 1 ? 1 : 0)];
        }
        drwav_free(d, nullptr);
    }
    s.frames = s.L[0].size();
    for (int c = 0; c < kNumClasses; ++c) s.frames = std::min(s.frames, s.L[c].size());
    for (int c = 0; c < kNumClasses; ++c) { s.L[c].resize(s.frames); s.R[c].resize(s.frames); }

    // Activity gate on the raw Dialogue stem (mono), 20 ms frames, 50 % overlap,
    // active if within 40 dB of the loudest frame (compute_metrics.py).
    const size_t frameLen = static_cast<size_t>(s.sampleRate * 0.020);
    const size_t hop = frameLen / 2;
    std::vector<double> levels;
    for (size_t start = 0; start + frameLen <= s.frames; start += hop) {
        double sum = 0.0;
        for (size_t i = start; i < start + frameLen; ++i) {
            double m = 0.5 * (s.L[kDialogue][i] + s.R[kDialogue][i]);
            sum += m * m;
        }
        levels.push_back(20.0 * std::log10(std::max(std::sqrt(sum / frameLen), 1e-9)));
    }
    double peak = *std::max_element(levels.begin(), levels.end());
    s.active.assign(s.frames, 0);
    for (size_t f = 0; f < levels.size(); ++f) {
        if (levels[f] > peak - 40.0) {
            size_t start = f * hop;
            std::fill(s.active.begin() + start, s.active.begin() + start + frameLen, 1);
        }
    }
    s.activeFraction = std::accumulate(s.active.begin(), s.active.end(), 0.0) / s.frames;
    return true;
}

struct SceneResult {
    double sbrDb = 0.0;
    double meanGainDb = 0.0;      // basic: mean duck gain over the clip; enhancement: mean over non-dialogue groups
    double engagedFraction = 0.0; // basic: fraction of clip with duck gain < -0.01 dB
    double dialogueGainDb = 0.0;  // enhancement: mean dialogue compressor gain
    double makeupOnlySbrDb = 0.0; // enhancement: SBR from makeup gains alone (no compression)
};

using Params = std::vector<double>;

void configure(Engine& e, const std::string& sweep, const Params& p) {
    if (sweep == "basic") {
        e.setProcessingTab(ProcessingTab::Unmasking);
        e.setMode(DuckMode::Basic);
        e.setKeyChannel(kDialogue);
        e.setMaxReductionDb(p[5]);
        e.setUnmaskEnabled(true);
    } else {
        e.setProcessingTab(ProcessingTab::Enhancement);
        e.setEnhancementBandMode(EnhancementBandMode::SingleBand);
        e.setEnhancementBypassed(false);
        for (int c = 0; c < kNumClasses; ++c) {
            e.setEnhancementThresholdDb(c, p[3 * c + 0]);
            e.setEnhancementRatio(c, p[3 * c + 1]);
            e.setEnhancementMakeupGainDb(c, p[3 * c + 2]);
            e.setEnhancementKneeDb(c, p[15]);
            e.setEnhancementAttackMs(c, p[16]);
            e.setEnhancementReleaseMs(c, p[17]);
        }
    }
}

CompressorParams compressorParams(const std::string& sweep, const Params& p) {
    CompressorParams cp;
    if (sweep == "basic") {
        cp.thresholdDb = p[0]; cp.ratio = p[1]; cp.kneeDb = p[2];
        cp.attackMs = p[3]; cp.releaseMs = p[4]; cp.safetyGainDb = p[6];
    }
    return cp;
}

SceneResult evaluate(Engine& e, const Scene& s, const std::string& sweep, const Params& p,
                     std::vector<float>& mixL, std::vector<float>& mixR) {
    SceneResult r;
    const CompressorParams cp = compressorParams(sweep, p);
    std::vector<float> outL(kBlock), outR(kBlock);

    // Pass 1: full mix, plus gain-reduction telemetry.
    e.prepare(s.sampleRate, cp);
    e.setSolo(kDialogue, false);
    configure(e, sweep, p);
    e.resetPlayhead();
    double sumGainDb = 0.0, sumDlgGainDb = 0.0;
    size_t engagedBlocks = 0, blocks = 0;
    for (size_t i = 0; i < s.frames; i += kBlock) {
        size_t n = std::min<size_t>(kBlock, s.frames - i);
        e.process(mixL.data() + i, mixR.data() + i, n);
        if (sweep == "basic") {
            double g = e.lastGainLinear();
            sumGainDb += 20.0 * std::log10(std::max(g, 1e-6));
            engagedBlocks += g < 0.999;
        } else {
            double others = 0.0;
            for (int c = 1; c < kNumClasses; ++c)
                others += 20.0 * std::log10(std::max(e.enhancementChannelGainLinear(c), 1e-6));
            sumGainDb += others / (kNumClasses - 1);
            sumDlgGainDb += 20.0 * std::log10(std::max(e.enhancementChannelGainLinear(kDialogue), 1e-6));
        }
        ++blocks;
    }
    r.meanGainDb = sumGainDb / blocks;
    r.engagedFraction = static_cast<double>(engagedBlocks) / blocks;
    r.dialogueGainDb = sumDlgGainDb / blocks;

    // Pass 2: Dialogue soloed, same processing. b = mix - d.
    e.prepare(s.sampleRate, cp);
    configure(e, sweep, p);
    e.setSolo(kDialogue, true);
    e.resetPlayhead();
    double dPow = 0.0, bPow = 0.0;
    for (size_t i = 0; i < s.frames; i += kBlock) {
        size_t n = std::min<size_t>(kBlock, s.frames - i);
        e.process(outL.data(), outR.data(), n);
        for (size_t j = 0; j < n; ++j) {
            if (!s.active[i + j]) continue;
            double dl = outL[j], dr = outR[j];
            double bl = mixL[i + j] - dl, br = mixR[i + j] - dr;
            dPow += dl * dl + dr * dr;
            bPow += bl * bl + br * br;
        }
    }
    r.sbrDb = 10.0 * std::log10(dPow / std::max(bPow, 1e-30));

    if (sweep == "enhancement") {
        // SBR if each group only had its static makeup gain (no compression).
        double gd = std::pow(10.0, p[2] / 20.0);
        double gm[kNumClasses];
        for (int c = 0; c < kNumClasses; ++c) gm[c] = std::pow(10.0, p[3 * c + 2] / 20.0);
        double dp = 0.0, bp = 0.0;
        for (size_t i = 0; i < s.frames; ++i) {
            if (!s.active[i]) continue;
            double dl = gd * s.L[kDialogue][i], dr = gd * s.R[kDialogue][i];
            double bl = 0.0, br = 0.0;
            for (int c = 1; c < kNumClasses; ++c) { bl += gm[c] * s.L[c][i]; br += gm[c] * s.R[c][i]; }
            dp += dl * dl + dr * dr;
            bp += bl * bl + br * br;
        }
        r.makeupOnlySbrDb = 10.0 * std::log10(dp / std::max(bp, 1e-30));
    }
    return r;
}

double unprocessedSbr(const Scene& s) {
    double dp = 0.0, bp = 0.0;
    for (size_t i = 0; i < s.frames; ++i) {
        if (!s.active[i]) continue;
        double dl = s.L[kDialogue][i], dr = s.R[kDialogue][i];
        double bl = 0.0, br = 0.0;
        for (int c = 1; c < kNumClasses; ++c) { bl += s.L[c][i]; br += s.R[c][i]; }
        dp += dl * dl + dr * dr;
        bp += bl * bl + br * br;
    }
    return 10.0 * std::log10(dp / bp);
}

std::vector<Params> latinHypercube(const std::vector<Range>& dims, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<Params> pts(n, Params(dims.size()));
    for (size_t d = 0; d < dims.size(); ++d) {
        std::vector<int> perm(n);
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), rng);
        for (int i = 0; i < n; ++i) {
            double t = (perm[i] + u(rng)) / n;
            pts[i][d] = dims[d].lo + t * (dims[d].hi - dims[d].lo);
        }
    }
    return pts;
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    const std::string repo = a.count("--repo") ? a["--repo"] : ".";
    const std::string sweep = a.count("--sweep") ? a["--sweep"] : "basic";
    const int n = a.count("--n") ? std::stoi(a["--n"]) : 400;
    const unsigned seed = a.count("--seed") ? std::stoul(a["--seed"]) : 42u;
    const std::string out = a.count("--out") ? a["--out"] : ("analysis/sbr_sweep_" + sweep + ".csv");
    const int threads = a.count("--threads") ? std::stoi(a["--threads"])
                                             : std::max(1u, std::thread::hardware_concurrency());
    if (sweep != "basic" && sweep != "enhancement") { std::cerr << "--sweep basic|enhancement\n"; return 1; }

    std::vector<Scene> scenes = {
        {"construction", "Construction Scene", {"Dialogue.wav", "MUSIC.wav", "BKG.wav", "SAFETY.wav", "OTHER.wav"}},
        {"publictransit", "PublicTransit Scene", {"Dialogue_PublicTransit_01.wav", "MUSIC_publictransit_01.wav",
                                                   "BKG_PublicTransit_01.wav", "SAFETY_publictransit_01.wav",
                                                   "OTHER_publictransit_01.wav"}},
        {"restaurant", "Restaurant Scene", {"Dialogue.wav", "MUSIC.wav", "BGV.wav", "Safety.wav", "OTHER.wav"}},
    };

    const std::vector<Range> dims = sweep == "basic" ? kBasicDims : enhancementDims();
    const std::vector<Params> points = latinHypercube(dims, n, seed);
    std::vector<std::vector<SceneResult>> results(n, std::vector<SceneResult>(scenes.size()));

    for (size_t si = 0; si < scenes.size(); ++si) {
        Scene& s = scenes[si];
        if (!loadScene(s, repo)) return 1;
        std::cout << s.name << ": " << s.frames / s.sampleRate << " s, Dialogue active "
                  << 100.0 * s.activeFraction << " %, unprocessed SBR " << unprocessedSbr(s) << " dB\n";

        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                auto e = std::make_unique<Engine>();
                e->prepare(s.sampleRate, CompressorParams{});
                e->loadStems(s.L, s.R);
                std::vector<float> mixL(s.frames), mixR(s.frames);
                for (int i = next++; i < n; i = next++) {
                    results[i][si] = evaluate(*e, s, sweep, points[i], mixL, mixR);
                    if ((i + 1) % 50 == 0) std::cout << "  " << s.name << " " << (i + 1) << "/" << n << "\n" << std::flush;
                }
            });
        }
        for (auto& th : pool) th.join();
        // Free this scene's audio before loading the next.
        for (int c = 0; c < kNumClasses; ++c) { s.L[c] = {}; s.R[c] = {}; }
        s.active = {};
    }

    std::ofstream f(out);
    f << "point";
    for (auto& d : dims) f << "," << d.name;
    for (auto& s : scenes) {
        f << "," << s.name << "_sbr_db";
        if (sweep == "basic") f << "," << s.name << "_mean_duck_db," << s.name << "_engaged_frac";
        else f << "," << s.name << "_mean_others_gain_db," << s.name << "_mean_dialogue_gain_db,"
               << s.name << "_makeup_only_sbr_db," << s.name << "_drc_shift_db";
    }
    f << "\n";
    f.precision(6);
    for (int i = 0; i < n; ++i) {
        f << i;
        for (double v : points[i]) f << "," << v;
        for (size_t si = 0; si < scenes.size(); ++si) {
            const SceneResult& r = results[i][si];
            f << "," << r.sbrDb;
            if (sweep == "basic") f << "," << r.meanGainDb << "," << r.engagedFraction;
            else f << "," << r.meanGainDb << "," << r.dialogueGainDb << "," << r.makeupOnlySbrDb << ","
                   << (r.sbrDb - r.makeupOnlySbrDb);
        }
        f << "\n";
    }
    std::cout << "wrote " << out << "\n";
    return 0;
}
