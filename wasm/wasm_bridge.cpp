// Thin extern "C" wrapper exposing demo::Engine to WebAssembly. Kept
// deliberately dumb: no ownership cleverness, just create/configure/process
// calls a JS-side AudioWorkletProcessor can drive one render quantum at a
// time. See web/engine-worklet.js for the caller.
#include <emscripten/emscripten.h>
#include <vector>
#include "Engine.h"

using namespace demo;

extern "C" {

EMSCRIPTEN_KEEPALIVE
Engine* engine_create() {
    return new Engine();
}

EMSCRIPTEN_KEEPALIVE
void engine_destroy(Engine* e) {
    delete e;
}

EMSCRIPTEN_KEEPALIVE
void engine_prepare(Engine* e, double sampleRate) {
    CompressorParams params; // literature-placeholder defaults from Types.h
    e->prepare(sampleRate, params);
}

// Scratch heap buffer helpers so JS can stage stem/output data before/after
// crossing into the Engine's own std::vector-owned storage.
EMSCRIPTEN_KEEPALIVE
float* engine_alloc(int numFloats) {
    return new float[numFloats];
}

EMSCRIPTEN_KEEPALIVE
void engine_free(float* p) {
    delete[] p;
}

// All 5 stems must already be same-length stereo (interleaved-free, separate
// L/R) float buffers in WASM heap memory (see engine_alloc). Copies into the
// Engine's internal storage, so the scratch buffers can be freed immediately
// after this call.
EMSCRIPTEN_KEEPALIVE
void engine_load_stems(Engine* e,
                        float* dialogueL, float* dialogueR,
                        float* musicL, float* musicR,
                        float* bgL, float* bgR,
                        float* safetyL, float* safetyR,
                        float* otherL, float* otherR,
                        int numFrames) {
    std::array<std::vector<float>, kNumClasses> left, right;
    left[kDialogue]        = std::vector<float>(dialogueL, dialogueL + numFrames);
    right[kDialogue]       = std::vector<float>(dialogueR, dialogueR + numFrames);
    left[kMusic]           = std::vector<float>(musicL, musicL + numFrames);
    right[kMusic]          = std::vector<float>(musicR, musicR + numFrames);
    left[kBackgroundNoise] = std::vector<float>(bgL, bgL + numFrames);
    right[kBackgroundNoise]= std::vector<float>(bgR, bgR + numFrames);
    left[kSafetyAlerts]    = std::vector<float>(safetyL, safetyL + numFrames);
    right[kSafetyAlerts]   = std::vector<float>(safetyR, safetyR + numFrames);
    left[kOther]           = std::vector<float>(otherL, otherL + numFrames);
    right[kOther]          = std::vector<float>(otherR, otherR + numFrames);
    e->loadStems(left, right);
}

EMSCRIPTEN_KEEPALIVE
int engine_num_frames(Engine* e) {
    return static_cast<int>(e->numFrames());
}

// 0 = Basic, 1 = Advanced, 2 = Resonance (see Types.h DuckMode).
EMSCRIPTEN_KEEPALIVE
void engine_set_mode(Engine* e, int mode) {
    DuckMode m = DuckMode::Advanced;
    if (mode == 0) m = DuckMode::Basic;
    else if (mode == 2) m = DuckMode::Resonance;
    e->setMode(m);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_unmask_enabled(Engine* e, int enabled) {
    e->setUnmaskEnabled(enabled != 0);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_key_channel(Engine* e, int channel) {
    e->setKeyChannel(channel);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_mute(Engine* e, int channel, int muted) {
    e->setMute(channel, muted != 0);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_solo(Engine* e, int channel, int soloed) {
    e->setSolo(channel, soloed != 0);
}

// Live sidechain compressor parameter updates - shared across all three
// DuckModes (see Engine::setThresholdDb() doc comment), so these affect
// Resonance mode's per-peak detection too, not just Basic/Advanced.
EMSCRIPTEN_KEEPALIVE
void engine_set_threshold_db(Engine* e, double thresholdDb) {
    e->setThresholdDb(thresholdDb);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_ratio(Engine* e, double ratio) {
    e->setRatio(ratio);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_knee_db(Engine* e, double kneeDb) {
    e->setKneeDb(kneeDb);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_attack_ms(Engine* e, double attackMs) {
    e->setAttackMs(attackMs);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_release_ms(Engine* e, double releaseMs) {
    e->setReleaseMs(releaseMs);
}

// Output-bus WDRC compressor (see Engine::setWdrc*() doc comment) - applied
// once to the final 5-channel mix, independent of duck mode.
EMSCRIPTEN_KEEPALIVE
void engine_set_wdrc_bypassed(Engine* e, int bypassed) {
    e->setWdrcBypassed(bypassed != 0);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_wdrc_threshold_db(Engine* e, double thresholdDb) {
    e->setWdrcThresholdDb(thresholdDb);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_wdrc_ratio(Engine* e, double ratio) {
    e->setWdrcRatio(ratio);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_wdrc_makeup_gain_db(Engine* e, double makeupGainDb) {
    e->setWdrcMakeupGainDb(makeupGainDb);
}

// Resonance-mode-only settings (see Engine::setResonance*() doc comments) -
// not shared with Basic/Advanced.
EMSCRIPTEN_KEEPALIVE
void engine_set_resonance_num_peaks(Engine* e, int count) {
    e->setResonanceNumPeaks(count);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_resonance_bandwidth_octaves(Engine* e, double bandwidthOctaves) {
    e->setResonanceBandwidthOctaves(bandwidthOctaves);
}

EMSCRIPTEN_KEEPALIVE
void engine_set_resonance_max_reduction_db(Engine* e, double maxReductionDb) {
    e->setResonanceMaxReductionDb(maxReductionDb);
}

EMSCRIPTEN_KEEPALIVE
void engine_reset_playhead(Engine* e) {
    e->resetPlayhead();
}

EMSCRIPTEN_KEEPALIVE
int engine_playhead(Engine* e) {
    return static_cast<int>(e->playhead());
}

EMSCRIPTEN_KEEPALIVE
double engine_last_gain_linear(Engine* e) {
    return e->lastGainLinear();
}

// How many dynamic notches Resonance mode is currently using (live, set via
// engine_set_resonance_num_peaks) - lets JS size its display arrays without
// hardcoding the count.
EMSCRIPTEN_KEEPALIVE
int engine_resonance_num_peaks(Engine* e) {
    return e->resonanceNumPeaks();
}

// Resonance mode's dynamic notch centers/depths, for drawing the actual
// moving notches in the UI rather than a fixed band. Only meaningful while
// in Resonance mode (see Engine::resonance*() doc comments). peakIndex must
// be in [0, engine_resonance_num_peaks()).
EMSCRIPTEN_KEEPALIVE
double engine_resonance_freq(Engine* e, int peakIndex) { return e->resonanceFreq(peakIndex); }

EMSCRIPTEN_KEEPALIVE
double engine_resonance_gain_linear(Engine* e, int peakIndex) { return e->resonanceGainLinear(peakIndex); }

EMSCRIPTEN_KEEPALIVE
void engine_process(Engine* e, float* outputL, float* outputR, int numFrames) {
    e->process(outputL, outputR, static_cast<size_t>(numFrames));
}

} // extern "C"
