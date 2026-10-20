// Volume-only baseline vs. the SBR-matched dynamics settings.
//
// Question: source separation lets us simply turn the background groups up
// or down. What does reaching the Johnson & Healy (2024) optimal SBR with
// plain volume sound like, compared with the dynamics processing (sidechain
// unmasking, group compression) at the same SBR?
//
// Volume-only (decided with the experimenter, 2026-10-16): ONE static gain on
// the summed background groups for the whole scene, no automation. Dialogue
// is unchanged. A static gain shifts SBR dB-for-dB, so the gain that lands
// the J&H SBR (below) on a value S is exactly
//     gain_db = SBR_unprocessed - S.
// Two sets of static conditions per scene:
//   static_HI / static_NH       - S = the J&H targets, +5.1 / +12.2 dB.
//   static_match_<setting>      - S = the J&H SBR that <setting> from
//                                 analysis/sbr_matched_settings.tsv actually
//                                 reaches in that scene, for exact same-SBR
//                                 A/B listening (the settings were matched on
//                                 the gated SBR, so they don't sit exactly on
//                                 the targets here).
//
// Every condition is scored identically:
//   J&H SBR - broadband, unweighted RMS ratio of processed Dialogue to the
//       processed background sum, pooled over utterance spans (J&H trimmed
//       each sentence's leading/trailing silence and measured the whole
//       sentence, pauses included). Also reported per utterance. Utterances
//       come from a voice-present detector on the dry Dialogue stem (mono
//       L+R, 10 ms RMS > --threshold-db, default -58 dBFS, updated every 32
//       samples): voice-present runs, gaps < --merge-ms merged, runs shorter
//       than --min-utt-ms dropped.
//   Gated SBR - sbr_sweep.cpp's metric (20 ms frames within 40 dB of the
//       Dialogue peak), for continuity with the existing tables.
//   Voice SBR - SBR over voice-present samples only.
//   Gap background change - level of the processed background sum relative
//       to the unprocessed background, over voice-ABSENT samples only (dB):
//       how much each approach turns the scene down while no one is talking.
//   Gain "work" on the background groups (mean over the four groups of each
//       group's gain trace in dB, sampled every 32 samples): range, total
//       gain travel (sum |dg| / duration, dB/s), 99th-percentile slew
//       (dB/s), share of time moving faster than 1 dB/s, and direction
//       reversals per second (turns of at least 1 dB from the last
//       peak/trough). Zero for the static conditions by construction.
//   Loudness - integrated LUFS (BS.1770-4); momentary (400 ms) loudness
//       standard deviation over integrated-gated blocks; short-term (3 s,
//       100 ms hop) standard deviation and loudness range (EBU LRA: 95th -
//       10th percentile, -20 LU relative gate). Listening renders (Construction
//       scene) are matched to the unprocessed mix's integrated LUFS.
//
// Scenes: Construction and Public Transit by default. Restaurant is left out
// (2026-10-20): its Dialogue stem never goes quiet (quieter voice energy
// ~20 dB below the main talker fills every pause), so it stands in for a
// source separator that misclassifies background speech as Dialogue rather
// than a clean test scene. Run it on purpose as that stress case with
// --scenes construction,publictransit,restaurant.
//
// Usage (see analysis/run_volume_only.sh):
//   volume_only --repo . [--scenes construction,publictransit] [--threshold-db -58]
//               [--merge-ms 400] [--min-utt-ms 500]
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>
#include "Engine.h"

using namespace demo;

namespace {

constexpr double kTargetsDb[2] = {5.1, 12.2};
const char* kTargetNames[2] = {"HI", "NH"};
constexpr size_t kBlock = 32; // gain telemetry / voice detector update interval
constexpr double kMovingDbPerSec = 1.0;
constexpr double kReversalDb = 1.0;

struct Options {
    std::string repo = ".";
    double thresholdDb = -58.0;
    double windowMs = 10.0;
    double mergeMs = 400.0;
    double minUttMs = 500.0;
};

// ---------------------------------------------------------------- scenes

struct Scene {
    std::string name, dir;
    std::array<std::string, kNumClasses> files;
    std::array<std::vector<float>, kNumClasses> L, R;
    std::vector<unsigned char> gate;   // sbr_sweep.cpp's 40 dB activity gate, per sample
    std::vector<unsigned char> voice;  // -58 dB detector, per block
    std::vector<std::pair<size_t, size_t>> utterances; // [start, end) in samples
    size_t frames = 0;
    double sampleRate = 48000.0;
};

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
    // active if within 40 dB of the loudest frame (sbr_sweep.cpp).
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
    s.gate.assign(s.frames, 0);
    for (size_t f = 0; f < levels.size(); ++f)
        if (levels[f] > peak - 40.0)
            std::fill(s.gate.begin() + f * hop, s.gate.begin() + f * hop + frameLen, 1);
    return true;
}

// Prefix sums of per-sample power, so any window's mean power is O(1).
struct Prefix {
    std::vector<double> p;
    double mean(size_t a, size_t b) const { return b > a ? (p[b] - p[a]) / double(b - a) : 0.0; }
};

Prefix prefix(const std::vector<double>& x) {
    Prefix pr;
    pr.p.assign(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) pr.p[i + 1] = pr.p[i] + x[i];
    return pr;
}

// Window ending at the end of block k: [end - win, end).
std::pair<size_t, size_t> blockWindow(size_t k, size_t win, size_t frames) {
    size_t end = std::min((k + 1) * kBlock, frames);
    return {end > win ? end - win : 0, end};
}

// Voice-present detector on the dry Dialogue stem, plus utterance segmentation.
void detectVoice(Scene& s, const Options& o) {
    std::vector<double> mono(s.frames);
    for (size_t i = 0; i < s.frames; ++i) {
        double m = 0.5 * (double(s.L[kDialogue][i]) + s.R[kDialogue][i]);
        mono[i] = m * m;
    }
    const Prefix pm = prefix(mono);
    const size_t win = static_cast<size_t>(o.windowMs * 1e-3 * s.sampleRate);
    const size_t nBlocks = (s.frames + kBlock - 1) / kBlock;
    s.voice.assign(nBlocks, 0);
    for (size_t k = 0; k < nBlocks; ++k) {
        auto [a, b] = blockWindow(k, win, s.frames);
        s.voice[k] = 10.0 * std::log10(std::max(pm.mean(a, b), 1e-20)) > o.thresholdDb;
    }

    // Voice-present runs -> utterances: merge short gaps, drop short runs.
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t k = 0; k < nBlocks;) {
        if (!s.voice[k]) { ++k; continue; }
        size_t j = k;
        while (j < nBlocks && s.voice[j]) ++j;
        runs.push_back({k * kBlock, std::min(j * kBlock, s.frames)});
        k = j;
    }
    const size_t merge = static_cast<size_t>(o.mergeMs * 1e-3 * s.sampleRate);
    const size_t minLen = static_cast<size_t>(o.minUttMs * 1e-3 * s.sampleRate);
    std::vector<std::pair<size_t, size_t>> merged;
    for (auto r : runs) {
        if (!merged.empty() && r.first - merged.back().second < merge) merged.back().second = r.second;
        else merged.push_back(r);
    }
    s.utterances.clear();
    for (auto r : merged) if (r.second - r.first >= minLen) s.utterances.push_back(r);
}

// ---------------------------------------------------------------- settings

struct Setting {
    std::string id;
    bool duck = false, comp = false;
    CompressorParams duckP;
    double maxReductionDb = 6.0;
    std::array<CompressorParams, kNumClasses> compP;
};

// Parameter order of analysis/sbr_matched_settings.tsv (sbr_matched_search.cpp dimsFor()).
std::vector<std::string> dimsFor(const std::string& mode) {
    std::vector<std::string> d;
    if (mode == "comp" || mode == "combined") {
        d = {"dialogue_threshold_db", "dialogue_ratio", "dialogue_attack_ms", "dialogue_release_ms"};
        for (const char* c : {"music", "background", "safety", "other"}) {
            d.push_back(std::string(c) + "_threshold_db");
            d.push_back(std::string(c) + "_ratio");
        }
        for (const char* n : {"comp_knee_db", "background_attack_ms", "background_release_ms"}) d.push_back(n);
    }
    if (mode == "basic" || mode == "combined")
        for (const char* n : {"duck_threshold_db", "duck_ratio", "duck_knee_db", "duck_attack_ms",
                              "duck_release_ms", "duck_max_reduction_db"})
            d.push_back(n);
    return d;
}

std::vector<Setting> loadSettings(const std::string& path) {
    std::vector<Setting> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string id, mode, csv;
        if (!(ls >> id >> mode >> csv)) continue;
        std::vector<double> v;
        std::stringstream cs(csv);
        for (std::string tok; std::getline(cs, tok, ',');) v.push_back(std::stod(tok));
        auto dims = dimsFor(mode);
        if (v.size() != dims.size()) { std::cerr << "bad setting " << id << "\n"; continue; }
        std::map<std::string, double> p;
        for (size_t i = 0; i < dims.size(); ++i) p[dims[i]] = v[i];
        Setting s;
        s.id = id;
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
        out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------- renderers

// One processed condition: Dialogue d, background sum b, and the mean (over the
// four background groups) gain trace in dB, one value per kBlock samples.
struct Render {
    std::string condition, target;
    std::vector<float> dL, dR, bL, bR;
    std::vector<double> gainDb;
    std::vector<double> dialogueGainDb; // empty unless Dialogue is processed
};

void allocate(Render& r, size_t frames) {
    r.dL.resize(frames); r.dR.resize(frames); r.bL.resize(frames); r.bR.resize(frames);
    r.gainDb.assign((frames + kBlock - 1) / kBlock, 0.0);
}

Render renderUnprocessed(const Scene& sc) {
    Render r;
    r.condition = "unprocessed";
    allocate(r, sc.frames);
    for (size_t i = 0; i < sc.frames; ++i) {
        r.dL[i] = sc.L[kDialogue][i]; r.dR[i] = sc.R[kDialogue][i];
        double bL = 0.0, bR = 0.0;
        for (int c = 1; c < kNumClasses; ++c) { bL += sc.L[c][i]; bR += sc.R[c][i]; }
        r.bL[i] = float(bL); r.bR[i] = float(bR);
    }
    return r;
}

// One static gain on the summed background for the whole scene.
Render renderStatic(const Scene& sc, const std::string& condition, const std::string& target, double gainDb) {
    Render r = renderUnprocessed(sc);
    r.condition = condition;
    r.target = target;
    const float g = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    for (size_t i = 0; i < sc.frames; ++i) { r.bL[i] *= g; r.bR[i] *= g; }
    std::fill(r.gainDb.begin(), r.gainDb.end(), gainDb);
    return r;
}

// sbr_matched_search.cpp renderSplit(), plus gain telemetry.
Render renderSetting(const Scene& sc, const Setting& st) {
    Render r;
    r.condition = st.id.substr(0, st.id.rfind('_'));
    r.target = st.id.substr(st.id.rfind('_') + 1);
    allocate(r, sc.frames);
    if (st.comp) r.dialogueGainDb.assign(r.gainDb.size(), 0.0);
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
        r.dL[i] = float(dL); r.dR[i] = float(dR); r.bL[i] = float(bL); r.bR[i] = float(bR);
        if (i % kBlock == 0) {
            const double duckDb = 20.0 * std::log10(std::max(g, 1e-6));
            double groups = 0.0;
            for (int c = 1; c < kNumClasses; ++c) groups += duckDb + (st.comp ? comp[c].lastGainReductionDb() : 0.0);
            r.gainDb[i / kBlock] = groups / (kNumClasses - 1);
            if (st.comp) r.dialogueGainDb[i / kBlock] = comp[kDialogue].lastGainReductionDb();
        }
    }
    return r;
}

// ---------------------------------------------------------------- metrics

double sbrDb(const Render& r, size_t a, size_t b, const std::vector<unsigned char>* gate = nullptr) {
    double dp = 0.0, bp = 0.0;
    for (size_t i = a; i < b; ++i) {
        if (gate && !(*gate)[i]) continue;
        dp += double(r.dL[i]) * r.dL[i] + double(r.dR[i]) * r.dR[i];
        bp += double(r.bL[i]) * r.bL[i] + double(r.bR[i]) * r.bR[i];
    }
    return 10.0 * std::log10(std::max(dp, 1e-30) / std::max(bp, 1e-30));
}

std::vector<double> kweight(const std::vector<float>& x) {
    // BS.1770-4 K-weighting, 48 kHz coefficients (sbr_matched_search.cpp).
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
}

double loud(double p) { return -0.691 + 10.0 * std::log10(std::max(p, 1e-30)); }

// Mean K-weighted power of windows of `block` samples every `hop` samples.
std::vector<double> windowPowers(const std::vector<double>& zl, const std::vector<double>& zr, size_t block, size_t hop) {
    std::vector<double> power;
    for (size_t start = 0; start + block <= zl.size(); start += hop) {
        double s = 0.0;
        for (size_t i = start; i < start + block; ++i) s += zl[i] * zl[i] + zr[i] * zr[i];
        power.push_back(s / block);
    }
    return power;
}

struct Loudness { double integrated = 0.0, momentaryStd = 0.0, shortTermStd = 0.0, lra = 0.0; };

Loudness loudness(const std::vector<float>& L, const std::vector<float>& R, double sr) {
    Loudness out;
    auto zl = kweight(L), zr = kweight(R);
    auto stdOf = [](const std::vector<double>& v) {
        double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size(), var = 0.0;
        for (double x : v) var += (x - m) * (x - m);
        return std::sqrt(var / v.size());
    };
    // Integrated: 400 ms blocks, 75 % overlap, -70 LUFS absolute, -10 LU relative gate.
    auto mom = windowPowers(zl, zr, size_t(0.4 * sr), size_t(0.1 * sr));
    double sum = 0.0; size_t n = 0;
    for (double p : mom) if (loud(p) > -70.0) { sum += p; ++n; }
    double rel = loud(sum / std::max<size_t>(n, 1)) - 10.0;
    sum = 0.0; n = 0;
    for (double p : mom) if (loud(p) > -70.0 && loud(p) > rel) { sum += p; ++n; }
    out.integrated = loud(sum / std::max<size_t>(n, 1));
    std::vector<double> momLv;
    for (double p : mom) if (loud(p) > -70.0 && loud(p) > rel) momLv.push_back(loud(p));
    if (momLv.size() > 1) out.momentaryStd = stdOf(momLv);
    // Short-term: 3 s windows, 100 ms hop. LRA per EBU Tech 3342.
    auto st = windowPowers(zl, zr, size_t(3.0 * sr), size_t(0.1 * sr));
    std::vector<double> lv;
    sum = 0.0; n = 0;
    for (double p : st) if (loud(p) > -70.0) { lv.push_back(loud(p)); sum += p; ++n; }
    if (lv.size() < 2) return out;
    out.shortTermStd = stdOf(lv);
    double relSt = loud(sum / n) - 20.0;
    std::vector<double> gated;
    for (double v : lv) if (v > relSt) gated.push_back(v);
    std::sort(gated.begin(), gated.end());
    auto pct = [&](double q) { return gated[static_cast<size_t>(q * (gated.size() - 1))]; };
    out.lra = pct(0.95) - pct(0.10);
    return out;
}

struct Work {
    double meanDb = 0.0, minDb = 0.0, maxDb = 0.0, p5Db = 0.0, p95Db = 0.0;
    double travelDbPerSec = 0.0, slewP99DbPerSec = 0.0, movingFrac = 0.0, reversalsPerSec = 0.0;
};

Work work(const std::vector<double>& g, double sr) {
    Work w;
    if (g.size() < 2) return w;
    const double dt = kBlock / sr, duration = g.size() * dt;
    std::vector<double> sorted = g;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](const std::vector<double>& v, double q) { return v[static_cast<size_t>(q * (v.size() - 1))]; };
    w.meanDb = std::accumulate(g.begin(), g.end(), 0.0) / g.size();
    w.minDb = sorted.front(); w.maxDb = sorted.back();
    w.p5Db = pct(sorted, 0.05); w.p95Db = pct(sorted, 0.95);
    std::vector<double> slew;
    double travel = 0.0; size_t moving = 0, reversals = 0;
    // Reversal = the gain turns around by at least kReversalDb from its last
    // peak/trough (hysteresis, so small wiggles don't count).
    int dir = 0; double hi = g[0], lo = g[0], extreme = g[0];
    for (size_t k = 1; k < g.size(); ++k) {
        double d = g[k] - g[k - 1];
        travel += std::fabs(d);
        slew.push_back(std::fabs(d) / dt);
        moving += std::fabs(d) / dt > kMovingDbPerSec;
        if (dir == 0) { // no direction yet: wait for the first 1 dB move
            hi = std::max(hi, g[k]); lo = std::min(lo, g[k]);
            if (g[k] <= hi - kReversalDb) { dir = -1; extreme = g[k]; }
            else if (g[k] >= lo + kReversalDb) { dir = 1; extreme = g[k]; }
        } else if (dir > 0) {
            if (g[k] > extreme) extreme = g[k];
            else if (g[k] <= extreme - kReversalDb) { ++reversals; dir = -1; extreme = g[k]; }
        } else {
            if (g[k] < extreme) extreme = g[k];
            else if (g[k] >= extreme + kReversalDb) { ++reversals; dir = 1; extreme = g[k]; }
        }
    }
    std::sort(slew.begin(), slew.end());
    w.travelDbPerSec = travel / duration;
    w.slewP99DbPerSec = pct(slew, 0.99);
    w.movingFrac = double(moving) / (g.size() - 1);
    w.reversalsPerSec = reversals / duration;
    return w;
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

void mix(const Render& r, std::vector<float>& L, std::vector<float>& R, double gain = 1.0) {
    L.resize(r.dL.size()); R.resize(r.dL.size());
    for (size_t i = 0; i < L.size(); ++i) {
        L[i] = float(gain * (double(r.dL[i]) + r.bL[i]));
        R[i] = float(gain * (double(r.dR[i]) + r.bR[i]));
    }
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    Options o;
    if (a.count("--repo")) o.repo = a["--repo"];
    if (a.count("--threshold-db")) o.thresholdDb = std::stod(a["--threshold-db"]);
    if (a.count("--merge-ms")) o.mergeMs = std::stod(a["--merge-ms"]);
    if (a.count("--min-utt-ms")) o.minUttMs = std::stod(a["--min-utt-ms"]);
    const std::string analysis = o.repo + "/analysis";
    const std::string renderDir = o.repo + "/exports/volume_only";

    std::vector<Setting> settings = loadSettings(analysis + "/sbr_matched_settings.tsv");
    if (settings.empty()) { std::cerr << "no settings in analysis/sbr_matched_settings.tsv\n"; return 1; }

    std::vector<Scene> allScenes = {
        {"construction", "Construction Scene", {"Dialogue.wav", "MUSIC.wav", "BKG.wav", "SAFETY.wav", "OTHER.wav"}},
        {"publictransit", "PublicTransit Scene", {"Dialogue_PublicTransit_01.wav", "MUSIC_publictransit_01.wav",
                                                   "BKG_PublicTransit_01.wav", "SAFETY_publictransit_01.wav",
                                                   "OTHER_publictransit_01.wav"}},
        {"restaurant", "Restaurant Scene", {"Dialogue.wav", "MUSIC.wav", "BGV.wav", "Safety.wav", "OTHER.wav"}},
    };
    std::vector<Scene> scenes;
    {
        std::stringstream ss(a.count("--scenes") ? a["--scenes"] : "construction,publictransit");
        for (std::string name; std::getline(ss, name, ',');) {
            auto it = std::find_if(allScenes.begin(), allScenes.end(), [&](const Scene& s) { return s.name == name; });
            if (it == allScenes.end()) { std::cerr << "unknown scene " << name << "\n"; return 1; }
            scenes.push_back(*it);
        }
    }

    std::ofstream res(analysis + "/volume_only_results.csv");
    res << "scene,condition,target,target_db,static_gain_db,jh_sbr_pooled_db,jh_sbr_mean_db,jh_sbr_median_db,"
           "jh_sbr_min_db,jh_sbr_max_db,jh_hi_window_frac,jh_nh_window_frac,gated_sbr_db,voice_sbr_db,gap_background_change_db,"
           "gain_mean_db,gain_min_db,gain_max_db,gain_p5_db,gain_p95_db,travel_db_per_s,slew_p99_db_per_s,"
           "moving_frac,reversals_per_s,dialogue_travel_db_per_s,integrated_lufs,lufs_match_db,"
           "momentary_std_lu,short_term_std_lu,lra_lu\n";
    std::ofstream utt(analysis + "/volume_only_utterances.csv");
    utt << "scene,condition,target,utterance,start_s,end_s,jh_sbr_db\n";
    std::ofstream det(analysis + "/volume_only_detector.csv");
    det << "scene,duration_s,voice_present_frac,voice_onsets_per_s,gate40_frac,utterances,utterance_mean_s\n";

    for (Scene& sc : scenes) {
        if (!loadScene(sc, o.repo)) return 1;
        detectVoice(sc, o);
        const double duration = sc.frames / sc.sampleRate;
        size_t onsets = 0;
        for (size_t k = 1; k < sc.voice.size(); ++k) onsets += sc.voice[k] && !sc.voice[k - 1];
        double uttLen = 0.0;
        for (auto u : sc.utterances) uttLen += (u.second - u.first) / sc.sampleRate;
        det << sc.name << "," << duration << ","
            << std::accumulate(sc.voice.begin(), sc.voice.end(), 0.0) / sc.voice.size() << ","
            << onsets / duration << "," << std::accumulate(sc.gate.begin(), sc.gate.end(), 0.0) / sc.frames << ","
            << sc.utterances.size() << "," << uttLen / std::max<size_t>(sc.utterances.size(), 1) << "\n";
        std::cout << sc.name << ": " << duration << " s, voice present "
                  << 100.0 * std::accumulate(sc.voice.begin(), sc.voice.end(), 0.0) / sc.voice.size() << " %, "
                  << sc.utterances.size() << " utterances\n";

        Render unproc = renderUnprocessed(sc);
        double gapRaw = 0.0;
        for (size_t i = 0; i < sc.frames; ++i)
            if (!sc.voice[i / kBlock]) gapRaw += double(unproc.bL[i]) * unproc.bL[i] + double(unproc.bR[i]) * unproc.bR[i];
        std::vector<float> uL, uR;
        mix(unproc, uL, uR);
        const double refLufs = loudness(uL, uR, sc.sampleRate).integrated;

        // Returns the pooled J&H SBR. staticGainDb is NAN for non-static conditions.
        auto score = [&](Render& r, double targetDb, double staticGainDb, bool listen) {
            std::vector<double> u;
            for (size_t n = 0; n < sc.utterances.size(); ++n) {
                auto [s0, s1] = sc.utterances[n];
                double v = sbrDb(r, s0, s1);
                u.push_back(v);
                utt << sc.name << "," << r.condition << "," << r.target << "," << n << ","
                    << s0 / sc.sampleRate << "," << s1 / sc.sampleRate << "," << v << "\n";
            }
            // Pooled: one ratio over all utterance samples together.
            double dp = 0.0, bp = 0.0;
            for (auto [s0, s1] : sc.utterances)
                for (size_t i = s0; i < s1; ++i) {
                    dp += double(r.dL[i]) * r.dL[i] + double(r.dR[i]) * r.dR[i];
                    bp += double(r.bL[i]) * r.bL[i] + double(r.bR[i]) * r.bR[i];
                }
            const double pooled = 10.0 * std::log10(dp / std::max(bp, 1e-30));
            std::vector<double> us = u;
            std::sort(us.begin(), us.end());
            size_t inHi = 0, inNh = 0;
            for (double v : u) { inHi += v >= 4.7 && v <= 5.8; inNh += v >= 2.8 && v <= 22.4; }
            Work w = work(r.gainDb, sc.sampleRate);
            double dlgTravel = r.dialogueGainDb.empty() ? 0.0 : work(r.dialogueGainDb, sc.sampleRate).travelDbPerSec;
            std::vector<float> mL, mR;
            mix(r, mL, mR);
            Loudness ld = loudness(mL, mR, sc.sampleRate);
            const double matchDb = refLufs - ld.integrated;
            double vd = 0.0, vb = 0.0, gap = 0.0;
            for (size_t i = 0; i < sc.frames; ++i) {
                if (!sc.voice[i / kBlock]) { gap += double(r.bL[i]) * r.bL[i] + double(r.bR[i]) * r.bR[i]; continue; }
                vd += double(r.dL[i]) * r.dL[i] + double(r.dR[i]) * r.dR[i];
                vb += double(r.bL[i]) * r.bL[i] + double(r.bR[i]) * r.bR[i];
            }
            res << sc.name << "," << r.condition << "," << r.target << ",";
            if (!std::isnan(targetDb)) res << targetDb;
            res << ",";
            if (!std::isnan(staticGainDb)) res << staticGainDb;
            res << "," << pooled << ","
                << (u.empty() ? 0.0 : std::accumulate(u.begin(), u.end(), 0.0) / u.size()) << ","
                << (us.empty() ? 0.0 : us[us.size() / 2]) << "," << (us.empty() ? 0.0 : us.front()) << ","
                << (us.empty() ? 0.0 : us.back()) << "," << double(inHi) / std::max<size_t>(u.size(), 1) << ","
                << double(inNh) / std::max<size_t>(u.size(), 1) << "," << sbrDb(r, 0, sc.frames, &sc.gate) << ","
                << 10.0 * std::log10(vd / std::max(vb, 1e-30)) << ",";
            if (gapRaw > 0.0) res << 10.0 * std::log10(std::max(gap, 1e-30) / gapRaw);
            res << ","
                << w.meanDb << "," << w.minDb << "," << w.maxDb << "," << w.p5Db << "," << w.p95Db << ","
                << w.travelDbPerSec << "," << w.slewP99DbPerSec << "," << w.movingFrac << ","
                << w.reversalsPerSec << "," << dlgTravel << "," << ld.integrated << "," << matchDb << ","
                << ld.momentaryStd << "," << ld.shortTermStd << "," << ld.lra << "\n";
            if (listen) {
                std::string name = renderDir + "/" + sc.name + "_" + r.condition + (r.target.empty() ? "" : "_" + r.target);
                mix(r, mL, mR, std::pow(10.0, matchDb / 20.0));
                writeWavFloat(name + "_lufs_matched.wav", mL, mR, sc.sampleRate);
            }
            return pooled;
        };

        const bool listenScene = sc.name == "construction";
        const double sbr0 = score(unproc, NAN, NAN, listenScene);
        for (int t = 0; t < 2; ++t) {
            Render r = renderStatic(sc, "static", kTargetNames[t], sbr0 - kTargetsDb[t]);
            score(r, kTargetsDb[t], sbr0 - kTargetsDb[t], listenScene);
        }
        for (const Setting& st : settings) {
            const double targetDb = st.id.substr(st.id.rfind('_') + 1) == "HI" ? kTargetsDb[0] : kTargetsDb[1];
            Render r = renderSetting(sc, st);
            const double reached = score(r, targetDb, NAN, listenScene);
            Render m = renderStatic(sc, "static_match_" + r.condition, r.target, sbr0 - reached);
            score(m, reached, sbr0 - reached, listenScene);
        }
        for (int c = 0; c < kNumClasses; ++c) { sc.L[c] = {}; sc.R[c] = {}; }
    }
    std::cout << "wrote analysis/volume_only_results.csv, volume_only_utterances.csv, "
                 "volume_only_detector.csv; renders in exports/volume_only/\n";
    return 0;
}
