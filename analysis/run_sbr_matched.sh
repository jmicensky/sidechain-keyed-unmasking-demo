#!/usr/bin/env bash
# Full SBR-matched pipeline: search each mode for one setting that hits the
# Johnson & Healy targets in all three scenes, export the rounded settings
# (web presets + Unity table), render level-matched examples, encode MP3s,
# then measure levels / gain reduction / LL Dynamics-matched settings and
# write the Unity handoff (unity/SBR_SETTINGS_HANDOFF.md).
# Roughly 30-60 min on a 10-core Mac.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
bin="$(mktemp -d)/sbr_matched"
c++ -std=c++17 -O3 -I"$repo/include" -I"$repo/third_party" "$repo/analysis/sbr_matched_search.cpp" -o "$bin"
cd "$repo"
"$bin" --repo . --validate 1
"$bin" --repo . --mode basic --lhs 800
"$bin" --repo . --mode comp --lhs 1500
"$bin" --repo . --mode combined --lhs 2000
python3 analysis/export_sbr_presets.py
mkdir -p exports/sbr_matched
"$bin" --repo . --render analysis/sbr_matched_settings.tsv --render-dir exports/sbr_matched
python3 analysis/export_sbr_examples.py exports/sbr_matched
"$bin" --repo . --report analysis/sbr_matched_settings.tsv
python3 analysis/write_unity_sbr_handoff.py
