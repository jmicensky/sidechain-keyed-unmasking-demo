// SBR-matched settings search: for each processing mode, find ONE setting
// that lands the Johnson & Healy (2024) speech-to-background ratio on target
// in all three scenes at once, so the modes can be compared at equal SBR
// ("if we achieve the same SBR, the different processing sounds like this").
//
// Modes (all with key gain and every makeup gain fixed at 0 dB, so the SBR
// change comes entirely from the dynamics):
//   basic     - Basic (full-band) sidechain ducking keyed on raw Dialogue.
//   comp      - single-band group compression on all 5 groups. Dialogue is
//               REQUIRED to compress (ratio 2-4:1, own syllabic attack/
//               release, mean gain reduction 1.5-6 dB in every scene) - the
//               intelligibility reason for compressing it - and the four
//               background groups have to supply the SBR.
//   combined  - comp, then Basic ducking of the four background groups,
//               keyed on raw (pre-compression) Dialogue: the Unity "S3"
//               design (unity/UNITY_HANDOFF.md section 3).
//
// Metric (same as sbr_sweep.cpp): SBR = 10*log10(sum d^2 / sum b^2) over
// samples where the raw Dialogue stem is active (20 ms frames within 40 dB
// of its peak); d = processed Dialogue, b = processed sum of the other four
// stems, both channels. Targets: HI +5.1 dB, NH +12.2 dB.
//
// Also reports Dialogue's dynamic range the way J&H's Fig. 5 measures it:
// 99th minus 30th percentile of 125 ms window levels (50 % overlap), over
// windows where Dialogue is active.
//
// Search: Latin Hypercube over each mode's space, then for each target an
// elitist local search minimizing
//   J = max_scene |SBR - target| + 0.02 * mean processing (dB of gain
//       reduction) + Dialogue-compression constraint penalty.
//
// The single-pass renderer below is used instead of Engine because Engine
// can't run Enhancement and Unmasking together; --validate checks it
// against Engine's own Basic and Enhancement output.
//
// Usage (see analysis/run_sbr_matched.sh):
//   sbr_matched_search --repo . --validate 1
//   sbr_matched_search --repo . --mode basic|comp|combined [--lhs N] [--gens G]
//                      [--scenes construction,publictransit] [--tag sbr_matched]
//   sbr_matched_search --repo . --render <settings.tsv> --render-dir exports/sbr_matched
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "Engine.h"

using namespace demo;

namespace {

constexpr double kTargetsDb[2] = {5.1, 12.2};
const char* kTargetNames[2] = {"HI", "NH"};
constexpr double kDialogueGrMinDb = 1.5; // Dialogue must compress at least this much (mean)...
constexpr double kDialogueGrMaxDb = 6.0; // ...and at most this much, in every scene
constexpr double kProcessingWeight = 0.02;

// ---------------------------------------------------------------- scenes

struct Scene {
    std::string name, dir;
    std::array<std::string, kNumClasses> files;
    std::array<std::vector<float>, kNumClasses> L, R;
    std::vector<unsigned char> active;
    size_t frames = 0;
    double sampleRate = 48000.0;
    double unprocessedSbrDb = 0.0;
    double rawDialogueRangeDb = 0.0;
    double unprocessedMixRms = 0.0;
};

constexpr size_t kChunk = 3000; // 62.5 ms at 48 kHz; a 125 ms window = 2 chunks

// 99th - 30th percentile of 125 ms (50 %-overlap) window levels, over windows
// whose samples are mostly speech-active. chunkPow = per-chunk mean power.
double dynamicRangeDb(const std::vector<double>& chunkPow, const std::vector<double>& chunkActive) {
    std::vector<double> levels;
    for (size_t k = 0; k + 1 < chunkPow.size(); ++k) {
        if (chunkActive[k] + chunkActive[k + 1] < 1.0) continue; // < 50 % active
        double p = 0.5 * (chunkPow[k] + chunkPow[k + 1]);
        levels.push_back(10.0 * std::log10(std::max(p, 1e-20)));
    }
    if (levels.size() < 10) return 0.0;
    std::sort(levels.begin(), levels.end());
    auto pct = [&](double q) { return levels[static_cast<size_t>(q * (levels.size() - 1))]; };
    return pct(0.99) - pct(0.30);
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

    const size_t frameLen = static_cast<size_t>(s.sampleRate * 0.020), hop = frameLen / 2;
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
    for (size_t f = 0; f < levels.size(); ++f)
        if (levels[f] > peak - 40.0)
            std::fill(s.active.begin() + f * hop, s.active.begin() + f * hop + frameLen, 1);
    return true;
}

// ---------------------------------------------------------------- settings

struct Setting {
    bool duck = false, comp = false;
    // Ducking (Basic, Dialogue key, key gain 0 dB).
    CompressorParams duckP;
    double maxReductionDb = 6.0;
    // Group compression, makeup 0 dB everywhere. [0] = Dialogue.
    std::array<CompressorParams, kNumClasses> compP;
};

struct Dim { std::string name; double lo, hi; };

std::vector<Dim> dimsFor(const std::string& mode) {
    std::vector<Dim> d;
    if (mode == "comp" || mode == "combined") {
        d.push_back({"dialogue_threshold_db", -45.0, -15.0});
        d.push_back({"dialogue_ratio", 2.0, 4.0});
        d.push_back({"dialogue_attack_ms", 1.0, 20.0});
        d.push_back({"dialogue_release_ms", 30.0, 300.0});
        for (const char* c : {"music", "background", "safety", "other"}) {
            d.push_back({std::string(c) + "_threshold_db", -65.0, -20.0});
            d.push_back({std::string(c) + "_ratio", 1.5, 10.0});
        }
        d.push_back({"comp_knee_db", 0.0, 24.0});
        d.push_back({"background_attack_ms", 1.0, 50.0});
        d.push_back({"background_release_ms", 20.0, 1000.0});
    }
    if (mode == "basic" || mode == "combined") {
        d.push_back({"duck_threshold_db", -70.0, -10.0});
        d.push_back({"duck_ratio", 1.5, 20.0});
        d.push_back({"duck_knee_db", 0.0, 24.0});
        d.push_back({"duck_attack_ms", 1.0, 50.0});
        d.push_back({"duck_release_ms", 20.0, 1000.0});
        d.push_back({"duck_max_reduction_db", 3.0, 30.0});
    }
    return d;
}

Setting toSetting(const std::string& mode, const std::vector<Dim>& dims, const std::vector<double>& v) {
    std::map<std::string, double> p;
    for (size_t i = 0; i < dims.size(); ++i) p[dims[i].name] = v[i];
    Setting s;
    s.comp = mode == "comp" || mode == "combined";
    s.duck = mode == "basic" || mode == "combined";
    if (s.comp) {
        const char* names[kNumClasses] = {"dialogue", "music", "background", "safety", "other"};
        for (int c = 0; c < kNumClasses; ++c) {
            CompressorParams cp;
            cp.thresholdDb = p[std::string(names[c]) + "_threshold_db"];
            cp.ratio = p[std::string(names[c]) + "_ratio"];
            cp.kneeDb = p["comp_knee_db"];
            cp.attackMs = c == kDialogue ? p["dialogue_attack_ms"] : p["background_attack_ms"];
            cp.releaseMs = c == kDialogue ? p["dialogue_release_ms"] : p["background_release_ms"];
            s.compP[c] = cp;
        }
    }
    if (s.duck) {
        s.duckP.thresholdDb = p["duck_threshold_db"];
        s.duckP.ratio = p["duck_ratio"];
        s.duckP.kneeDb = p["duck_knee_db"];
        s.duckP.attackMs = p["duck_attack_ms"];
        s.duckP.releaseMs = p["duck_release_ms"];
        s.maxReductionDb = p["duck_max_reduction_db"];
    }
    return s;
}

// ---------------------------------------------------------------- renderer

struct SceneResult {
    double sbrDb = 0.0;
    double meanDuckDb = 0.0;        // mean duck gain over the clip (<= 0)
    double duckEngagedFrac = 0.0;
    double meanDialogueGrDb = 0.0;  // mean Dialogue compressor gain (<= 0)
    double meanBackgroundGrDb = 0.0;// mean over the 4 background groups (<= 0)
    double dialogueRangeDb = 0.0;   // J&H 99th-30th percentile, processed Dialogue
};

// Single pass: Dialogue d and background sum b are produced separately, so no
// solo re-render is needed. If outL/outR are given, the mix d + b is written.
SceneResult render(const Scene& sc, const Setting& st, std::vector<float>* outL = nullptr,
                   std::vector<float>* outR = nullptr) {
    SceneResult r;
    EnvelopeFollower det;
    GainComputer gc;
    det.prepare(sc.sampleRate, st.duckP.attackMs, st.duckP.releaseMs);
    gc.prepare(st.duckP);
    const double floorLin = std::pow(10.0, -std::clamp(st.maxReductionDb, 0.0, 40.0) / 20.0);
    std::array<SingleBandEnhancementCompressor, kNumClasses> comp;
    for (int c = 0; c < kNumClasses; ++c) {
        comp[c].prepare(sc.sampleRate, st.compP[c]);
        comp[c].setMakeupGainDb(0.0);
    }

    double dPow = 0.0, bPow = 0.0;
    double sumDuckDb = 0.0, sumDlgDb = 0.0, sumBgDb = 0.0;
    size_t engaged = 0, ticks = 0;
    const size_t nChunks = sc.frames / kChunk;
    std::vector<double> chunkPow(nChunks, 0.0), chunkActive(nChunks, 0.0);

    for (size_t i = 0; i < sc.frames; ++i) {
        double g = 1.0;
        if (st.duck) {
            // Engine's detector reads a float mono sum (Engine::loadStems).
            double keyIn = static_cast<float>(0.5f * (sc.L[kDialogue][i] + sc.R[kDialogue][i]));
            g = std::max(gc.computeLinearGain(det.tick(keyIn)), floorLin);
        }
        double dL = sc.L[kDialogue][i], dR = sc.R[kDialogue][i];
        if (st.comp) comp[kDialogue].tick(dL, dR, dL, dR);
        double bL = 0.0, bR = 0.0;
        for (int c = 1; c < kNumClasses; ++c) {
            double yL = sc.L[c][i], yR = sc.R[c][i];
            if (st.comp) comp[c].tick(yL, yR, yL, yR);
            bL += g * yL;
            bR += g * yR;
        }
        if (sc.active[i]) {
            dPow += dL * dL + dR * dR;
            bPow += bL * bL + bR * bR;
        }
        size_t k = i / kChunk;
        if (k < nChunks) {
            chunkPow[k] += 0.5 * (dL * dL + dR * dR) / kChunk;
            chunkActive[k] += sc.active[i] / static_cast<double>(kChunk);
        }
        if ((i & 31) == 0) { // telemetry every 32 samples
            if (st.duck) {
                sumDuckDb += 20.0 * std::log10(std::max(g, 1e-6));
                engaged += g < 0.999;
            }
            if (st.comp) {
                sumDlgDb += comp[kDialogue].lastGainReductionDb();
                double bg = 0.0;
                for (int c = 1; c < kNumClasses; ++c) bg += comp[c].lastGainReductionDb();
                sumBgDb += bg / (kNumClasses - 1);
            }
            ++ticks;
        }
        if (outL) { (*outL)[i] = static_cast<float>(dL + bL); (*outR)[i] = static_cast<float>(dR + bR); }
    }
    r.sbrDb = 10.0 * std::log10(dPow / std::max(bPow, 1e-30));
    r.meanDuckDb = sumDuckDb / ticks;
    r.duckEngagedFrac = static_cast<double>(engaged) / ticks;
    r.meanDialogueGrDb = sumDlgDb / ticks;
    r.meanBackgroundGrDb = sumBgDb / ticks;
    r.dialogueRangeDb = dynamicRangeDb(chunkPow, chunkActive);
    return r;
}

// Same processing as render(), but writes processed Dialogue (d) and the processed
// background sum (b) to separate buffers.
void renderSplit(const Scene& sc, const Setting& st, std::vector<float>& dLo, std::vector<float>& dRo,
                 std::vector<float>& bLo, std::vector<float>& bRo) {
    EnvelopeFollower det;
    GainComputer gc;
    det.prepare(sc.sampleRate, st.duckP.attackMs, st.duckP.releaseMs);
    gc.prepare(st.duckP);
    const double floorLin = std::pow(10.0, -std::clamp(st.maxReductionDb, 0.0, 40.0) / 20.0);
    std::array<SingleBandEnhancementCompressor, kNumClasses> comp;
    for (int c = 0; c < kNumClasses; ++c) { comp[c].prepare(sc.sampleRate, st.compP[c]); comp[c].setMakeupGainDb(0.0); }
    for (size_t i = 0; i < sc.frames; ++i) {
        double g = 1.0;
        if (st.duck) {
            double keyIn = static_cast<float>(0.5f * (sc.L[kDialogue][i] + sc.R[kDialogue][i]));
            g = std::max(gc.computeLinearGain(det.tick(keyIn)), floorLin);
        }
        double dL = sc.L[kDialogue][i], dR = sc.R[kDialogue][i];
        if (st.comp) comp[kDialogue].tick(dL, dR, dL, dR);
        double bL = 0.0, bR = 0.0;
        for (int c = 1; c < kNumClasses; ++c) {
            double yL = sc.L[c][i], yR = sc.R[c][i];
            if (st.comp) comp[c].tick(yL, yR, yL, yR);
            bL += g * yL; bR += g * yR;
        }
        dLo[i] = float(dL); dRo[i] = float(dR); bLo[i] = float(bL); bRo[i] = float(bR);
    }
}

// ---------------------------------------------------------------- objective

struct Eval {
    std::vector<double> v;
    std::vector<SceneResult> scenes; // one per scene in the run
    double J[2] = {0, 0};
    double err[2] = {0, 0};
    double processing = 0.0, penalty = 0.0;
};

void score(Eval& e, const Setting& st) {
    double proc = 0.0, pen = 0.0;
    for (auto& s : e.scenes) {
        proc += -s.meanDuckDb - s.meanBackgroundGrDb - s.meanDialogueGrDb;
        if (st.comp) {
            double gr = -s.meanDialogueGrDb; // positive dB of reduction
            if (gr < kDialogueGrMinDb) pen += kDialogueGrMinDb - gr;
            if (gr > kDialogueGrMaxDb) pen += gr - kDialogueGrMaxDb;
        }
    }
    e.processing = proc / e.scenes.size();
    e.penalty = pen;
    for (int t = 0; t < 2; ++t) {
        double worst = 0.0;
        for (auto& s : e.scenes) worst = std::max(worst, std::fabs(s.sbrDb - kTargetsDb[t]));
        e.err[t] = worst;
        e.J[t] = worst + kProcessingWeight * e.processing + pen;
    }
}

void evaluateBatch(std::vector<Eval>& batch, const std::string& mode, const std::vector<Dim>& dims,
                   const std::vector<Scene>& scenes, int threads) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (size_t i = next++; i < batch.size(); i = next++) {
                Setting st = toSetting(mode, dims, batch[i].v);
                batch[i].scenes.resize(scenes.size());
                for (size_t s = 0; s < scenes.size(); ++s) batch[i].scenes[s] = render(scenes[s], st);
                score(batch[i], st);
            }
        });
    for (auto& th : pool) th.join();
}

std::vector<std::vector<double>> latinHypercube(const std::vector<Dim>& dims, int n, std::mt19937& rng) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<std::vector<double>> pts(n, std::vector<double>(dims.size()));
    for (size_t d = 0; d < dims.size(); ++d) {
        std::vector<int> perm(n);
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), rng);
        for (int i = 0; i < n; ++i) pts[i][d] = dims[d].lo + (perm[i] + u(rng)) / n * (dims[d].hi - dims[d].lo);
    }
    return pts;
}

// ---------------------------------------------------------------- output

// Scene names in CSV column order; set in main() from the scenes actually run.
std::vector<std::string> gSceneNames = {"construction", "publictransit", "restaurant"};

void writeEvalHeader(std::ostream& f, const std::vector<Dim>& dims,
                     const char* errCols = "worst_err_hi_db,worst_err_nh_db") {
    f << "stage,target";
    for (auto& d : dims) f << "," << d.name;
    for (const auto& s : gSceneNames)
        f << "," << s << "_sbr_db," << s << "_mean_duck_db," << s << "_duck_engaged_frac," << s
          << "_dialogue_gr_db," << s << "_background_gr_db," << s << "_dialogue_range_db";
    f << "," << errCols << ",processing_db,penalty\n";
}

void writeEval(std::ostream& f, const std::string& stage, const std::string& target, const Eval& e) {
    f << stage << "," << target;
    for (double x : e.v) f << "," << x;
    for (auto& s : e.scenes)
        f << "," << s.sbrDb << "," << s.meanDuckDb << "," << s.duckEngagedFrac << "," << s.meanDialogueGrDb << ","
          << s.meanBackgroundGrDb << "," << s.dialogueRangeDb;
    f << "," << e.err[0] << "," << e.err[1] << "," << e.processing << "," << e.penalty << "\n";
}

void writeWavFloat(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, double sr) {
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = 2;
    fmt.sampleRate = static_cast<drwav_uint32>(sr);
    fmt.bitsPerSample = 32;
    drwav wav;
    drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr);
    std::vector<float> inter(L.size() * 2);
    for (size_t i = 0; i < L.size(); ++i) { inter[2 * i] = L[i]; inter[2 * i + 1] = R[i]; }
    drwav_write_pcm_frames(&wav, L.size(), inter.data());
    drwav_uninit(&wav);
}

double rms(const std::vector<float>& L, const std::vector<float>& R) {
    double s = 0.0;
    for (size_t i = 0; i < L.size(); ++i) s += double(L[i]) * L[i] + double(R[i]) * R[i];
    return std::sqrt(s / (2.0 * L.size()));
}

// ---------------------------------------------------------------- validation

// Engine-based SBR (two renders, solo), as in sbr_sweep.cpp.
double engineSbr(const Scene& sc, const std::string& mode, const Setting& st) {
    auto run = [&](bool soloDialogue, std::vector<float>& L, std::vector<float>& R) {
        Engine e;
        CompressorParams cp = st.duck ? st.duckP : CompressorParams{};
        e.prepare(sc.sampleRate, cp);
        e.loadStems(sc.L, sc.R);
        if (mode == "basic") {
            e.setMode(DuckMode::Basic);
            e.setKeyChannel(kDialogue);
            e.setMaxReductionDb(st.maxReductionDb);
            e.setUnmaskEnabled(true);
        } else {
            e.setProcessingTab(ProcessingTab::Enhancement);
            for (int c = 0; c < kNumClasses; ++c) {
                e.setEnhancementThresholdDb(c, st.compP[c].thresholdDb);
                e.setEnhancementRatio(c, st.compP[c].ratio);
                e.setEnhancementKneeDb(c, st.compP[c].kneeDb);
                e.setEnhancementAttackMs(c, st.compP[c].attackMs);
                e.setEnhancementReleaseMs(c, st.compP[c].releaseMs);
            }
        }
        if (soloDialogue) e.setSolo(kDialogue, true);
        L.resize(sc.frames);
        R.resize(sc.frames);
        for (size_t i = 0; i < sc.frames; i += 128) e.process(L.data() + i, R.data() + i, std::min<size_t>(128, sc.frames - i));
    };
    std::vector<float> mL, mR, dL, dR;
    run(false, mL, mR);
    run(true, dL, dR);
    double dp = 0.0, bp = 0.0;
    for (size_t i = 0; i < sc.frames; ++i) {
        if (!sc.active[i]) continue;
        double bl = mL[i] - dL[i], br = mR[i] - dR[i];
        dp += double(dL[i]) * dL[i] + double(dR[i]) * dR[i];
        bp += bl * bl + br * br;
    }
    return 10.0 * std::log10(dp / bp);
}


// ---------------------------------------------------------------- handoff report

// Integrated loudness, ITU-R BS.1770-4 (K-weighting, 400 ms blocks, 75 % overlap,
// -70 LUFS absolute and -10 LU relative gates). Coefficients are the standard's
// 48 kHz values, so this is only valid at 48 kHz (all scenes are).
double integratedLufs(const std::vector<float>& L, const std::vector<float>& R) {
    auto kweight = [](const std::vector<float>& x) {
        const double b1[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
        const double a1[3] = {1.0, -1.69065929318241, 0.73248077421585};
        const double b2[3] = {1.0, -2.0, 1.0};
        const double a2[3] = {1.0, -1.99004745483398, 0.99007225036621};
        std::vector<double> y(x.size());
        double s1[2] = {0, 0}, s2[2] = {0, 0};
        for (size_t i = 0; i < x.size(); ++i) {
            double v = x[i];
            double o1 = b1[0] * v + s1[0];
            s1[0] = b1[1] * v - a1[1] * o1 + s1[1];
            s1[1] = b1[2] * v - a1[2] * o1;
            double o2 = b2[0] * o1 + s2[0];
            s2[0] = b2[1] * o1 - a2[1] * o2 + s2[1];
            s2[1] = b2[2] * o1 - a2[2] * o2;
            y[i] = o2;
        }
        return y;
    };
    auto zl = kweight(L), zr = kweight(R);
    const size_t block = 19200, hop = 4800; // 400 ms / 100 ms at 48 kHz
    std::vector<double> power;
    for (size_t start = 0; start + block <= zl.size(); start += hop) {
        double s = 0.0;
        for (size_t i = start; i < start + block; ++i) s += zl[i] * zl[i] + zr[i] * zr[i];
        power.push_back(s / block);
    }
    auto loud = [](double p) { return -0.691 + 10.0 * std::log10(std::max(p, 1e-30)); };
    double sum = 0.0; size_t n = 0;
    for (double p : power) if (loud(p) > -70.0) { sum += p; ++n; }
    if (!n) return -INFINITY;
    double rel = loud(sum / n) - 10.0;
    sum = 0.0; n = 0;
    for (double p : power) if (loud(p) > -70.0 && loud(p) > rel) { sum += p; ++n; }
    return loud(sum / n);
}

struct GrStats { double meanAll = 0.0, meanActive = 0.0, max = 0.0; }; // positive dB

// Accumulates per-sample gain reduction (positive dB) the way LL Dynamics' stats do.
struct GrAccumulator {
    double sumAll = 0.0, sumActive = 0.0, mx = 0.0;
    size_t n = 0, nActive = 0;
    void add(double grDb, bool active) {
        sumAll += grDb; ++n; mx = std::max(mx, grDb);
        if (active) { sumActive += grDb; ++nActive; }
    }
    GrStats stats() const { return {sumAll / std::max<size_t>(n, 1), sumActive / std::max<size_t>(nActive, 1), mx}; }
};

// Web-engine gain reduction per stage: duck (applies to channels 1-4) and each
// channel's own group compressor.
struct WebGr { GrStats duck; std::array<GrStats, kNumClasses> comp; };

WebGr webGainReduction(const Scene& sc, const Setting& st) {
    EnvelopeFollower det;
    GainComputer gc;
    det.prepare(sc.sampleRate, st.duckP.attackMs, st.duckP.releaseMs);
    gc.prepare(st.duckP);
    const double floorLin = std::pow(10.0, -std::clamp(st.maxReductionDb, 0.0, 40.0) / 20.0);
    std::array<SingleBandEnhancementCompressor, kNumClasses> comp;
    for (int c = 0; c < kNumClasses; ++c) comp[c].prepare(sc.sampleRate, st.compP[c]);
    GrAccumulator duck;
    std::array<GrAccumulator, kNumClasses> cacc;
    for (size_t i = 0; i < sc.frames; ++i) {
        bool act = sc.active[i];
        if (st.duck) {
            double keyIn = static_cast<float>(0.5f * (sc.L[kDialogue][i] + sc.R[kDialogue][i]));
            double g = std::max(gc.computeLinearGain(det.tick(keyIn)), floorLin);
            duck.add(-20.0 * std::log10(g), act);
        }
        if (st.comp)
            for (int c = 0; c < kNumClasses; ++c) {
                double l, r;
                comp[c].tick(sc.L[c][i], sc.R[c][i], l, r);
                cacc[c].add(-comp[c].lastGainReductionDb(), act);
            }
    }
    WebGr w;
    w.duck = duck.stats();
    for (int c = 0; c < kNumClasses; ++c) w.comp[c] = cacc[c].stats();
    return w;
}

// ---- LL Dynamics emulation: mirrors Modified_Johnson_Healy/NativePlugins/LLDynamics
// (LLDynamicsDSP.h PeakEnvelope / GainComputer, LLDynamicsProcessor.cpp MODE_COMPRESSOR
// and MODE_SIDECHAIN with the Loudness Gate off): float math, stereo-linked peak
// max(|L|,|R|), one-pole envelope on the LINEAR peak, soft knee, Max GR clamp.
struct LldParams { float threshold, ratio, knee, attack, release, maxGr; };

struct LldEnvelope {
    float env = 0.0f, a = 0.0f, r = 0.0f;
    LldEnvelope(float atk, float rel, float sr) {
        a = atk <= 0.0f ? 0.0f : std::exp(-1.0f / (0.001f * atk * sr));
        r = rel <= 0.0f ? 0.0f : std::exp(-1.0f / (0.001f * rel * sr));
    }
    float tick(float x) { const float c = x > env ? a : r; env = x + c * (env - x); return env; }
};

inline float lldGainDb(float levelDb, const LldParams& p) {
    const float slope = 1.0f / std::max(p.ratio, 1.0f) - 1.0f;
    float gr;
    if (p.knee <= 0.0f) gr = levelDb <= p.threshold ? 0.0f : slope * (levelDb - p.threshold);
    else {
        const float h = 0.5f * p.knee;
        if (levelDb < p.threshold - h) gr = 0.0f;
        else if (levelDb > p.threshold + h) gr = slope * (levelDb - p.threshold);
        else { const float d = levelDb - (p.threshold - h); gr = slope * d * d / (2.0f * p.knee); }
    }
    return std::max(gr, -p.maxGr);
}

inline float lldToDb(float x) { return 20.0f * std::log10(std::max(x, 1.0e-9f)); }

// Compressor mode on channel c (key = its own peak) or Sidechain mode (key = Dialogue's
// peak, delayed by lag samples to model the Priority->Sidechain block offset).
// Fills gain (linear, per sample) if given; returns GR stats.
GrStats lldRun(const Scene& sc, int keyChannel, const LldParams& p, int lag, std::vector<float>* gain) {
    LldEnvelope env(p.attack, p.release, static_cast<float>(sc.sampleRate));
    GrAccumulator acc;
    for (size_t i = 0; i < sc.frames; ++i) {
        float peak = 0.0f;
        if (i >= static_cast<size_t>(lag)) {
            size_t k = i - lag;
            peak = std::max(std::fabs(sc.L[keyChannel][k]), std::fabs(sc.R[keyChannel][k]));
        }
        const float grDb = lldGainDb(lldToDb(env.tick(peak)), p);
        acc.add(-grDb, sc.active[i]);
        if (gain) (*gain)[i] = std::pow(10.0f, grDb * 0.05f);
    }
    return acc.stats();
}

// Threshold that makes LL Dynamics' mean gain reduction (whole clip) equal targetDb.
// Mean GR falls monotonically as the threshold rises, so bisect.
float matchLldThreshold(const Scene& sc, int keyChannel, LldParams p, double targetDb) {
    float lo = -120.0f, hi = 0.0f;
    for (int it = 0; it < 30; ++it) {
        p.threshold = 0.5f * (lo + hi);
        if (lldRun(sc, keyChannel, p, 0, nullptr).meanAll > targetDb) lo = p.threshold; else hi = p.threshold;
    }
    return 0.5f * (lo + hi);
}

// A full LL Dynamics parameter set for one setting: one Compressor per group
// (Comp/Combined) and one Sidechain on the four background groups (Basic/Combined).
struct LldSetting {
    bool duck = false, comp = false;
    LldParams duckP{};
    std::array<LldParams, kNumClasses> compP{};
};

struct LldChainResult {
    double sbrDb = 0.0;
    GrStats duck;
    std::array<GrStats, kNumClasses> comp;
};

// Dialogue: Compressor only (its Priority effect sits before it, so the key is raw
// Dialogue). Background groups: Compressor, then Sidechain.
LldChainResult lldChain(const Scene& s, const LldSetting& ls, int lag) {
    LldChainResult r;
    std::array<std::vector<float>, kNumClasses> cg;
    std::vector<float> dg;
    if (ls.comp)
        for (int c = 0; c < kNumClasses; ++c) { cg[c].resize(s.frames); r.comp[c] = lldRun(s, c, ls.compP[c], 0, &cg[c]); }
    if (ls.duck) { dg.resize(s.frames); r.duck = lldRun(s, kDialogue, ls.duckP, lag, &dg); }
    double dp = 0.0, bp = 0.0;
    for (size_t i = 0; i < s.frames; ++i) {
        if (!s.active[i]) continue;
        double gd = ls.comp ? cg[0][i] : 1.0;
        double dl = gd * s.L[0][i], dr = gd * s.R[0][i], bl = 0.0, br = 0.0;
        double gk = ls.duck ? dg[i] : 1.0;
        for (int c = 1; c < kNumClasses; ++c) {
            double g = (ls.comp ? cg[c][i] : 1.0) * gk;
            bl += g * s.L[c][i]; br += g * s.R[c][i];
        }
        dp += dl * dl + dr * dr; bp += bl * bl + br * br;
    }
    r.sbrDb = 10.0 * std::log10(dp / bp);
    return r;
}

// Shifts the "trim" thresholds by delta: the Sidechain threshold when ducking is in
// the chain, otherwise the four background Compressor thresholds.
LldSetting trimmed(LldSetting ls, float delta) {
    if (ls.duck) ls.duckP.threshold += delta;
    else for (int c = 1; c < kNumClasses; ++c) ls.compP[c].threshold += delta;
    return ls;
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    const std::string repo = a.count("--repo") ? a["--repo"] : ".";
    const int threads = a.count("--threads") ? std::stoi(a["--threads"]) : std::max(1u, std::thread::hardware_concurrency());

    std::vector<Scene> scenes = {
        {"construction", "Construction Scene", {"Dialogue.wav", "MUSIC.wav", "BKG.wav", "SAFETY.wav", "OTHER.wav"}},
        {"publictransit", "PublicTransit Scene", {"Dialogue_PublicTransit_01.wav", "MUSIC_publictransit_01.wav",
                                                   "BKG_PublicTransit_01.wav", "SAFETY_publictransit_01.wav",
                                                   "OTHER_publictransit_01.wav"}},
        {"restaurant", "Restaurant Scene", {"Dialogue.wav", "MUSIC.wav", "BGV.wav", "Safety.wav", "OTHER.wav"}},
    };
    if (a.count("--scenes")) {
        // Restrict every stage to these scenes, e.g. --scenes construction,publictransit
        // (Restaurant left out as a separator-misclassification case, 2026-10-20).
        std::vector<Scene> keep;
        std::stringstream ss(a["--scenes"]);
        for (std::string name; std::getline(ss, name, ',');) {
            auto it = std::find_if(scenes.begin(), scenes.end(), [&](const Scene& sc) { return sc.name == name; });
            if (it == scenes.end()) { std::cerr << "unknown scene " << name << "\n"; return 1; }
            keep.push_back(*it);
        }
        scenes = std::move(keep);
    }
    for (auto& s : scenes) {
        if (!loadScene(s, repo)) return 1;
        Setting none;
        std::vector<float> L(s.frames), R(s.frames);
        SceneResult r = render(s, none, &L, &R);
        s.unprocessedSbrDb = r.sbrDb;
        s.rawDialogueRangeDb = r.dialogueRangeDb;
        s.unprocessedMixRms = rms(L, R);
        std::cout << s.name << ": unprocessed SBR " << r.sbrDb << " dB, raw Dialogue range (99-30th pct) "
                  << r.dialogueRangeDb << " dB\n";
    }

    if (a.count("--only-scene")) {
        // Restrict --report / --render / --eval to one scene (e.g. the reference scene).
        std::vector<Scene> keep;
        for (auto& s : scenes) if (s.name == a["--only-scene"]) keep.push_back(std::move(s));
        if (keep.empty()) { std::cerr << "unknown --only-scene\n"; return 1; }
        scenes = std::move(keep);
    }
    const std::string outPrefix = "analysis/" + (a.count("--tag") ? a["--tag"] : std::string("sbr_matched"));
    gSceneNames.clear();
    for (auto& s : scenes) gSceneNames.push_back(s.name);

    if (a.count("--validate")) {
        std::mt19937 rng(7);
        for (std::string mode : {"basic", "comp"}) {
            auto dims = dimsFor(mode);
            auto pts = latinHypercube(dims, 3, rng);
            for (auto& v : pts) {
                Setting st = toSetting(mode, dims, v);
                for (size_t s = 0; s < scenes.size(); ++s) {
                    double mine = render(scenes[s], st).sbrDb, ref = engineSbr(scenes[s], mode, st);
                    std::printf("  validate %-5s %-14s single-pass %+8.4f dB  Engine %+8.4f dB  diff %.5f dB\n",
                                mode.c_str(), scenes[s].name.c_str(), mine, ref, mine - ref);
                }
            }
        }
        return 0;
    }



    if (a.count("--eval")) {
        // Lines: id <TAB> mode <TAB> scene <TAB> v1,v2,... (that mode's dims order).
        // Writes SBR + Dialogue range per line; with --split-dir, also writes the
        // processed Dialogue and processed background-sum as separate float WAVs
        // (<id>_dialogue.wav / <id>_background.wav) for the margin metric.
        const std::string splitDir = a.count("--split-dir") ? a["--split-dir"] : "";
        std::ifstream in(a["--eval"]);
        std::ofstream outf(a.count("--out") ? a["--out"] : "analysis/sbr_eval.tsv");
        outf << "id\tscene\tsbr_db\tdialogue_range_db\n";
        std::vector<std::tuple<std::string, std::string, std::string, std::vector<double>>> lines;
        for (std::string line; std::getline(in, line);) {
            std::istringstream ls(line);
            std::string id, mode, scene, csv;
            std::getline(ls, id, '\t'); std::getline(ls, mode, '\t'); std::getline(ls, scene, '\t'); std::getline(ls, csv, '\t');
            std::vector<double> v;
            std::istringstream cs(csv);
            for (std::string x; std::getline(cs, x, ',');) v.push_back(std::stod(x));
            lines.emplace_back(id, mode, scene, v);
        }
        std::vector<std::string> results(lines.size());
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                for (size_t i = next++; i < lines.size(); i = next++) {
                    auto& [id, mode, sceneName, v] = lines[i];
                    const Scene* sc = nullptr;
                    for (auto& s : scenes) if (s.name == sceneName) sc = &s;
                    if (!sc) continue;
                    Setting st = mode == "none" ? Setting{} : toSetting(mode, dimsFor(mode), v);
                    SceneResult r = render(*sc, st);
                    if (!splitDir.empty()) {
                        std::vector<float> dL(sc->frames), dR(sc->frames), bL(sc->frames), bR(sc->frames);
                        renderSplit(*sc, st, dL, dR, bL, bR);
                        writeWavFloat(splitDir + "/" + id + "_dialogue.wav", dL, dR, sc->sampleRate);
                        writeWavFloat(splitDir + "/" + id + "_background.wav", bL, bR, sc->sampleRate);
                    }
                    std::ostringstream os;
                    os << id << "\t" << sceneName << "\t" << r.sbrDb << "\t" << r.dialogueRangeDb << "\n";
                    results[i] = os.str();
                }
            });
        for (auto& th : pool) th.join();
        for (auto& r : results) outf << r;
        return 0;
    }

    if (a.count("--report")) {
        // Everything the Unity handoff needs, for each rounded setting in the TSV:
        // input levels, web gain reduction, LL Dynamics-equivalent thresholds, and the
        // SBR an LL Dynamics chain with those thresholds produces on the same stems.
        std::vector<std::tuple<std::string, std::string, std::vector<double>>> jobs;
        std::ifstream in(a["--report"]);
        for (std::string line; std::getline(in, line);) {
            std::istringstream ls(line);
            std::string label, mode, csv;
            std::getline(ls, label, '\t'); std::getline(ls, mode, '\t'); std::getline(ls, csv, '\t');
            std::vector<double> v;
            std::istringstream cs(csv);
            for (std::string x; std::getline(cs, x, ',');) v.push_back(std::stod(x));
            jobs.emplace_back(label, mode, v);
        }
        const char* chName[kNumClasses] = {"Dialogue", "Music", "Background", "Safety", "Other"};
        const int lagBlock = a.count("--lag") ? std::stoi(a["--lag"]) : 1024;

        std::ofstream lv(outPrefix + "_input_levels.tsv");
        lv << "scene\tchannel\trms_dbfs\trms_active_dbfs\tpeak_dbfs\tlufs_integrated\n";
        for (auto& s : scenes)
            for (int c = 0; c < kNumClasses; ++c) {
                double sum = 0.0, sumA = 0.0; size_t nA = 0; float pk = 0.0f;
                for (size_t i = 0; i < s.frames; ++i) {
                    double e = 0.5 * (double(s.L[c][i]) * s.L[c][i] + double(s.R[c][i]) * s.R[c][i]);
                    sum += e;
                    if (s.active[i]) { sumA += e; ++nA; }
                    pk = std::max({pk, std::fabs(s.L[c][i]), std::fabs(s.R[c][i])});
                }
                lv << s.name << "\t" << chName[c] << "\t" << 10 * std::log10(sum / s.frames) << "\t"
                   << 10 * std::log10(sumA / nA) << "\t" << 20 * std::log10(pk) << "\t"
                   << integratedLufs(s.L[c], s.R[c]) << "\n";
            }

        std::ofstream gr(outPrefix + "_gain_reduction.tsv");
        gr << "setting\tscene\tstage\tchannel\tweb_threshold_db\tratio\tknee_db\tattack_ms\trelease_ms\t"
              "web_mean_gr_db\tweb_mean_gr_active_db\tweb_max_gr_db\tlld_threshold_db\tlld_max_gr_param_db\t"
              "lld_mean_gr_db\tlld_mean_gr_active_db\tlld_max_gr_db\n";
        std::ofstream chk(outPrefix + "_lld_check.tsv");
        chk << "setting\tscene\tweb_sbr_db\tlld_sbr_db_lag0\tlld_sbr_db_lag" << lagBlock << "\n";

        std::ofstream fin(outPrefix + "_lld_settings.tsv");
        fin << "setting\ttarget_sbr_db\ttrim_db\tstage\tchannel\tthreshold_db\tratio\tknee_db\tattack_ms\t"
               "release_ms\tmax_gr_db\tmakeup_db";
        for (auto& s : scenes) fin << "\t" << s.name << "_mean_gr_db\t" << s.name << "_mean_gr_active_db\t" << s.name << "_max_gr_db";
        fin << "\n";
        std::ofstream finSbr(outPrefix + "_lld_sbr.tsv");
        finSbr << "setting\ttarget_sbr_db\ttrim_db\tscene\tweb_sbr_db\tlld_sbr_db_lag0\tlld_sbr_db_lag" << lagBlock
               << "\tlld_trimmed_sbr_db_lag" << lagBlock << "\n";

        for (auto& [label, mode, v] : jobs) {
            Setting st = toSetting(mode, dimsFor(mode), v);
            LldSetting single;
            single.duck = st.duck;
            single.comp = st.comp;
            std::vector<double> webSbr;
            for (auto& s : scenes) {
                WebGr w = webGainReduction(s, st);
                webSbr.push_back(render(s, st).sbrDb);
                std::array<std::vector<float>, kNumClasses> compGain;
                auto row = [&](const char* stage, const char* ch, const CompressorParams& cp, const GrStats& web,
                               LldParams lp, const GrStats& lld) {
                    gr << label << "\t" << s.name << "\t" << stage << "\t" << ch << "\t" << cp.thresholdDb << "\t"
                       << cp.ratio << "\t" << cp.kneeDb << "\t" << cp.attackMs << "\t" << cp.releaseMs << "\t"
                       << web.meanAll << "\t" << web.meanActive << "\t" << web.max << "\t" << lp.threshold << "\t"
                       << lp.maxGr << "\t" << lld.meanAll << "\t" << lld.meanActive << "\t" << lld.max << "\n";
                };
                if (st.comp)
                    for (int c = 0; c < kNumClasses; ++c) {
                        const CompressorParams& cp = st.compP[c];
                        // Web group compression has no gain-reduction ceiling: use LLD's maximum.
                        LldParams lp{0.0f, float(cp.ratio), float(cp.kneeDb), float(cp.attackMs), float(cp.releaseMs), 60.0f};
                        lp.threshold = matchLldThreshold(s, c, lp, w.comp[c].meanAll);
                        if (&s == &scenes.front()) { single.compP[c] = lp; single.compP[c].threshold = 0.0f; }
                        single.compP[c].threshold += lp.threshold / scenes.size();
                        compGain[c].resize(s.frames);
                        GrStats lld = lldRun(s, c, lp, 0, &compGain[c]);
                        row("compressor", chName[c], cp, w.comp[c], lp, lld);
                    }
                std::vector<float> duck0, duckLag;
                if (st.duck) {
                    LldParams lp{0.0f, float(st.duckP.ratio), float(st.duckP.kneeDb), float(st.duckP.attackMs),
                                 float(st.duckP.releaseMs), float(st.maxReductionDb)};
                    lp.threshold = matchLldThreshold(s, kDialogue, lp, w.duck.meanAll);
                    if (&s == &scenes.front()) { single.duckP = lp; single.duckP.threshold = 0.0f; }
                    single.duckP.threshold += lp.threshold / scenes.size();
                    duck0.resize(s.frames); duckLag.resize(s.frames);
                    GrStats lld = lldRun(s, kDialogue, lp, 0, &duck0);
                    lldRun(s, kDialogue, lp, lagBlock, &duckLag);
                    row("sidechain", "Music/Background/Safety/Other", st.duckP, w.duck, lp, lld);
                }
                // SBR of the LL Dynamics chain: Dialogue = compressor only (Priority sits
                // before it); others = compressor then sidechain duck.
                auto lldSbr = [&](const std::vector<float>* duck) {
                    double dp = 0.0, bp = 0.0;
                    for (size_t i = 0; i < s.frames; ++i) {
                        if (!s.active[i]) continue;
                        double gd = st.comp ? compGain[0][i] : 1.0;
                        double dl = gd * s.L[0][i], dr = gd * s.R[0][i], bl = 0.0, br = 0.0;
                        double gk = duck ? (*duck)[i] : 1.0;
                        for (int c = 1; c < kNumClasses; ++c) {
                            double g = (st.comp ? compGain[c][i] : 1.0) * gk;
                            bl += g * s.L[c][i]; br += g * s.R[c][i];
                        }
                        dp += dl * dl + dr * dr; bp += bl * bl + br * br;
                    }
                    return 10.0 * std::log10(dp / bp);
                };
                chk << label << "\t" << s.name << "\t" << render(s, st).sbrDb << "\t"
                    << lldSbr(st.duck ? &duck0 : nullptr) << "\t" << lldSbr(st.duck ? &duckLag : nullptr) << "\n";
                std::cout << "  reported " << label << " / " << s.name << "\n" << std::flush;
            }

            // One trim (dB) on top of the scene-averaged thresholds so the LL Dynamics
            // chain, with a one-block sidechain lag, lands on the target SBR in all scenes.
            const double target = a.count("--target") ? std::stod(a["--target"])
                                                      : (label.find("_NH") != std::string::npos ? 12.2 : 5.1);
            auto worstErr = [&](float delta) {
                double worst = 0.0;
                for (auto& s : scenes) worst = std::max(worst, std::fabs(lldChain(s, trimmed(single, delta), lagBlock).sbrDb - target));
                return worst;
            };
            float lo = -20.0f, hi = 20.0f; // golden-section search on the worst-scene error
            const float gr = 0.618034f;
            float x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
            double f1 = worstErr(x1), f2 = worstErr(x2);
            for (int it = 0; it < 30; ++it) {
                if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - gr * (hi - lo); f1 = worstErr(x1); }
                else { lo = x1; x1 = x2; f1 = f2; x2 = lo + gr * (hi - lo); f2 = worstErr(x2); }
            }
            float trim = std::round(0.5f * (lo + hi) * 10.0f) / 10.0f;
            LldSetting fs = trimmed(single, trim);
            for (auto& p : fs.compP) p.threshold = std::round(p.threshold * 10.0f) / 10.0f;
            fs.duckP.threshold = std::round(fs.duckP.threshold * 10.0f) / 10.0f;

            std::vector<LldChainResult> res;
            for (size_t si = 0; si < scenes.size(); ++si) {
                const Scene& s = scenes[si];
                res.push_back(lldChain(s, fs, lagBlock));
                finSbr << label << "\t" << target << "\t" << trim << "\t" << s.name << "\t" << webSbr[si] << "\t"
                       << lldChain(s, single, 0).sbrDb << "\t" << lldChain(s, single, lagBlock).sbrDb << "\t"
                       << res.back().sbrDb << "\n";
            }
            auto finRow = [&](const char* stage, const char* ch, const LldParams& p, auto statOf) {
                fin << label << "\t" << target << "\t" << trim << "\t" << stage << "\t" << ch << "\t" << p.threshold << "\t"
                    << p.ratio << "\t" << p.knee << "\t" << p.attack << "\t" << p.release << "\t" << p.maxGr << "\t0";
                for (auto& r : res) { GrStats g = statOf(r); fin << "\t" << g.meanAll << "\t" << g.meanActive << "\t" << g.max; }
                fin << "\n";
            };
            if (fs.comp)
                for (int c = 0; c < kNumClasses; ++c)
                    finRow("Compressor", chName[c], fs.compP[c], [c](const LldChainResult& r) { return r.comp[c]; });
            if (fs.duck)
                finRow("Sidechain", "Music/Background/Safety/Other", fs.duckP, [](const LldChainResult& r) { return r.duck; });
            std::cout << "  " << label << ": LL Dynamics trim " << trim << " dB\n" << std::flush;
        }
        return 0;
    }

    if (a.count("--render")) {
        // settings.tsv lines: label <TAB> mode <TAB> v1,v2,... (dims order of that mode)
        const std::string dir = a.count("--render-dir") ? a["--render-dir"] : "exports/sbr_matched";
        std::ifstream in(a["--render"]);
        std::string line;
        std::vector<std::tuple<std::string, std::string, std::vector<double>>> jobs = {{"S0_unprocessed", "none", {}}};
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string label, mode, csv;
            std::getline(ls, label, '\t');
            std::getline(ls, mode, '\t');
            std::getline(ls, csv, '\t');
            std::vector<double> v;
            std::istringstream cs(csv);
            for (std::string x; std::getline(cs, x, ',');) v.push_back(std::stod(x));
            jobs.emplace_back(label, mode, v);
        }
        std::ofstream stats(dir + "/render_stats.tsv");
        stats << "scene\tlabel\tsbr_db\tdialogue_range_db\tlevel_match_db\tpeak_dbfs\n";
        for (auto& s : scenes) {
            // Every render in a scene is matched to one common RMS (J&H scaled each mixture
            // to the same RMS), so the comparison isn't a loudness comparison. That RMS is
            // the unprocessed mix's, lowered just enough that no render peaks above -1 dBFS.
            struct Out { std::string label; SceneResult r; std::vector<float> L, R; double gain; float peak; };
            std::vector<Out> outs;
            double worstPeakDb = -INFINITY;
            for (auto& [label, mode, v] : jobs) {
                Setting st = mode == "none" ? Setting{} : toSetting(mode, dimsFor(mode), v);
                Out o{label, {}, std::vector<float>(s.frames), std::vector<float>(s.frames), 0.0, 0.0f};
                o.r = render(s, st, &o.L, &o.R);
                o.gain = s.unprocessedMixRms / rms(o.L, o.R);
                float pk = 0.0f;
                for (size_t i = 0; i < s.frames; ++i) pk = std::max({pk, std::fabs(o.L[i]), std::fabs(o.R[i])});
                worstPeakDb = std::max(worstPeakDb, 20.0 * std::log10(pk * o.gain));
                outs.push_back(std::move(o));
            }
            const double offsetDb = std::min(0.0, -1.0 - worstPeakDb);
            for (auto& o : outs) {
                double gain = o.gain * std::pow(10.0, offsetDb / 20.0);
                float peak = 0.0f;
                for (size_t i = 0; i < s.frames; ++i) {
                    o.L[i] *= gain; o.R[i] *= gain;
                    peak = std::max({peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
                }
                std::string path = dir + "/" + s.name + "_" + o.label + ".wav";
                writeWavFloat(path, o.L, o.R, s.sampleRate);
                stats << s.name << "\t" << o.label << "\t" << o.r.sbrDb << "\t" << o.r.dialogueRangeDb << "\t"
                      << 20 * std::log10(gain) << "\t" << 20 * std::log10(std::max(peak, 1e-9f)) << "\n";
                std::printf("  %-14s %-16s SBR %+6.2f dB  Dialogue range %5.1f dB  gain %+6.2f dB  peak %6.2f dBFS\n",
                            s.name.c_str(), o.label.c_str(), o.r.sbrDb, o.r.dialogueRangeDb, 20 * std::log10(gain),
                            20 * std::log10(std::max(peak, 1e-9f)));
            }
            std::printf("  %-14s all renders at unprocessed RMS %+.2f dB\n", s.name.c_str(), offsetDb);
        }
        return 0;
    }

    const std::string mode = a.count("--mode") ? a["--mode"] : "basic";
    const int lhsN = a.count("--lhs") ? std::stoi(a["--lhs"]) : 1000;
    const int gens = a.count("--gens") ? std::stoi(a["--gens"]) : 20;
    const int perGen = a.count("--per-gen") ? std::stoi(a["--per-gen"]) : 30;
    const auto dims = dimsFor(mode);
    std::mt19937 rng(a.count("--seed") ? std::stoul(a["--seed"]) : 42u);

    if (a.count("--target")) {
        // Fixed-target mode: one setting whose SBR on ONE reference scene is within
        // tolerance of --target (other scenes are rendered and logged for reference
        // only). Objective: error beyond +-0.05 dB is heavily penalized, so inside
        // that band the gentlest processing wins; Dialogue-compression constraint
        // applied on the reference scene.
        const double T = std::stod(a["--target"]);
        const std::string ref = a.count("--ref-scene") ? a["--ref-scene"] : "construction";
        size_t ri = 0;
        while (ri < scenes.size() && scenes[ri].name != ref) ++ri;
        if (ri == scenes.size()) { std::cerr << "unknown --ref-scene " << ref << "\n"; return 1; }
        auto refScore = [&](Eval& e) {
            const SceneResult& r = e.scenes[ri];
            Setting st = toSetting(mode, dims, e.v);
            double pen = 0.0;
            if (st.comp) {
                double gr = -r.meanDialogueGrDb;
                if (gr < kDialogueGrMinDb) pen += kDialogueGrMinDb - gr;
                if (gr > kDialogueGrMaxDb) pen += gr - kDialogueGrMaxDb;
            }
            double proc = -r.meanDuckDb - r.meanBackgroundGrDb - r.meanDialogueGrDb;
            e.err[0] = std::fabs(r.sbrDb - T);
            e.err[1] = 0.0;
            e.processing = proc;
            e.penalty = pen;
            // The Dialogue-compression constraint is as hard as the SBR tolerance (same weight).
            e.J[0] = 20.0 * std::max(e.err[0] - 0.05, 0.0) + kProcessingWeight * proc + 20.0 * pen;
        };
        const std::string tag = a.count("--tag") ? a["--tag"] : "sbr5";
        std::ofstream log("analysis/" + tag + "_" + mode + ".csv");
        log.precision(6);
        writeEvalHeader(log, dims, "ref_err_db,unused");
        std::vector<Eval> pop;
        for (auto& v : latinHypercube(dims, lhsN, rng)) pop.push_back(Eval{v});
        evaluateBatch(pop, mode, dims, scenes, threads);
        for (auto& e : pop) { refScore(e); writeEval(log, "lhs", "T", e); }
        std::vector<Eval> elite = pop;
        auto byJ = [](const Eval& x, const Eval& y) { return x.J[0] < y.J[0]; };
        std::sort(elite.begin(), elite.end(), byJ);
        elite.resize(10);
        std::normal_distribution<double> nrm(0.0, 1.0);
        for (int g = 0; g < gens; ++g) {
            const double sigma = 0.08 * std::pow(0.85, g);
            std::uniform_int_distribution<size_t> pick(0, elite.size() - 1);
            std::vector<Eval> kids;
            for (int k = 0; k < perGen; ++k) {
                Eval child{elite[pick(rng)].v};
                for (size_t d = 0; d < dims.size(); ++d)
                    child.v[d] = std::clamp(child.v[d] + sigma * (dims[d].hi - dims[d].lo) * nrm(rng), dims[d].lo, dims[d].hi);
                kids.push_back(child);
            }
            evaluateBatch(kids, mode, dims, scenes, threads);
            for (auto& e : kids) { refScore(e); writeEval(log, "refine", "T", e); }
            elite.insert(elite.end(), kids.begin(), kids.end());
            std::sort(elite.begin(), elite.end(), byJ);
            elite.resize(10);
        }
        const Eval& b = elite.front();
        writeEval(log, "best", "T", b);
        std::printf("\n%s / %s %+.2f dB: SBR error %.3f dB, processing %.1f dB, penalty %.2f\n", mode.c_str(),
                    ref.c_str(), T, b.err[0], b.processing, b.penalty);
        for (size_t si = 0; si < scenes.size(); ++si) {
            const SceneResult& r = b.scenes[si];
            std::printf("  %-14s SBR %+6.2f  duck mean %6.2f dB engaged %3.0f%%  Dialogue GR %5.2f  background GR %6.2f  "
                        "Dialogue range %5.1f dB%s\n", scenes[si].name.c_str(), r.sbrDb, r.meanDuckDb,
                        100 * r.duckEngagedFrac, r.meanDialogueGrDb, r.meanBackgroundGrDb, r.dialogueRangeDb,
                        si == ri ? "  <- reference" : "");
        }
        std::ofstream best("analysis/" + tag + "_" + mode + "_best.tsv");
        best << mode << "_T\t" << mode << "\t";
        for (size_t d = 0; d < dims.size(); ++d) best << b.v[d] << (d + 1 < dims.size() ? "," : "\n");
        return 0;
    }

    std::ofstream log(outPrefix + "_" + mode + ".csv");
    log.precision(6);
    writeEvalHeader(log, dims);

    std::vector<Eval> lhs;
    for (auto& v : latinHypercube(dims, lhsN, rng)) lhs.push_back(Eval{v});
    evaluateBatch(lhs, mode, dims, scenes, threads);
    for (auto& e : lhs) writeEval(log, "lhs", "", e);
    std::cout << mode << ": " << lhsN << " LHS points evaluated\n";

    std::ofstream best(outPrefix + "_" + mode + "_best.tsv");
    for (int t = 0; t < 2; ++t) {
        std::vector<Eval> elite = lhs;
        std::sort(elite.begin(), elite.end(), [&](const Eval& x, const Eval& y) { return x.J[t] < y.J[t]; });
        elite.resize(10);
        for (int g = 0; g < gens; ++g) {
            const double sigma = 0.08 * std::pow(0.85, g);
            std::vector<Eval> kids;
            std::uniform_int_distribution<size_t> pick(0, elite.size() - 1);
            std::normal_distribution<double> nrm(0.0, 1.0);
            for (int k = 0; k < perGen; ++k) {
                Eval child{elite[pick(rng)].v};
                for (size_t d = 0; d < dims.size(); ++d)
                    child.v[d] = std::clamp(child.v[d] + sigma * (dims[d].hi - dims[d].lo) * nrm(rng), dims[d].lo, dims[d].hi);
                kids.push_back(child);
            }
            evaluateBatch(kids, mode, dims, scenes, threads);
            for (auto& e : kids) writeEval(log, "refine", kTargetNames[t], e);
            elite.insert(elite.end(), kids.begin(), kids.end());
            std::sort(elite.begin(), elite.end(), [&](const Eval& x, const Eval& y) { return x.J[t] < y.J[t]; });
            elite.resize(10);
        }
        const Eval& b = elite.front();
        std::printf("\n%s / %s target %+.1f dB: worst-scene error %.2f dB, processing %.1f dB, penalty %.2f\n",
                    mode.c_str(), kTargetNames[t], kTargetsDb[t], b.err[t], b.processing, b.penalty);
        for (size_t s = 0; s < scenes.size(); ++s) {
            const SceneResult& r = b.scenes[s];
            std::printf("  %-14s SBR %+6.2f (unproc %+6.2f)  duck mean %6.2f dB engaged %3.0f%%  "
                        "Dialogue GR %5.2f dB  background GR %6.2f dB  Dialogue range %5.1f dB (raw %5.1f)\n",
                        scenes[s].name.c_str(), r.sbrDb, scenes[s].unprocessedSbrDb, r.meanDuckDb,
                        100 * r.duckEngagedFrac, r.meanDialogueGrDb, r.meanBackgroundGrDb, r.dialogueRangeDb,
                        scenes[s].rawDialogueRangeDb);
        }
        std::printf("  ");
        for (size_t d = 0; d < dims.size(); ++d) std::printf("%s=%.2f%s", dims[d].name.c_str(), b.v[d], d + 1 < dims.size() ? ", " : "\n");
        best << mode << "_" << kTargetNames[t] << "\t" << mode << "\t";
        for (size_t d = 0; d < dims.size(); ++d) best << b.v[d] << (d + 1 < dims.size() ? "," : "\n");
        writeEval(log, "best", kTargetNames[t], b);
    }
    return 0;
}
