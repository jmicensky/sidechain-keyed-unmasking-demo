#!/bin/bash
# Compiles the Engine + wasm_bridge.cpp to a self-contained ES module
# (wasm binary embedded as base64 via SINGLE_FILE, so the AudioWorklet
# never has to fetch a second file). Run from anywhere; paths are relative
# to this script's location.
set -euo pipefail
cd "$(dirname "$0")"

export PATH="/opt/homebrew/Cellar/emscripten/6.0.8/bin:/opt/homebrew/bin:$PATH"
hash -r

mkdir -p ../web

em++ wasm_bridge.cpp \
  -O3 -std=c++17 \
  -I../include -I../third_party \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createEngineModule \
  -sSINGLE_FILE=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=worker \
  -sEXPORTED_FUNCTIONS=_engine_create,_engine_destroy,_engine_prepare,_engine_alloc,_engine_free,_engine_load_stems,_engine_num_frames,_engine_set_mode,_engine_set_unmask_enabled,_engine_set_key_channel,_engine_set_mute,_engine_set_solo,_engine_set_threshold_db,_engine_set_ratio,_engine_set_knee_db,_engine_set_attack_ms,_engine_set_release_ms,_engine_reset_playhead,_engine_playhead,_engine_last_gain_linear,_engine_resonance_num_peaks,_engine_resonance_freq,_engine_resonance_gain_linear,_engine_process,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAP32 \
  -o ../web/engine.mjs

echo "Built web/engine.mjs ($(du -h ../web/engine.mjs | cut -f1))"
