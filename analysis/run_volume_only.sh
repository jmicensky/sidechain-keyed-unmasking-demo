#!/usr/bin/env bash
# Builds and runs the static volume-only comparison (analysis/volume_only.cpp)
# against the SBR-matched settings in analysis/sbr_matched_settings.tsv.
#   analysis/run_volume_only.sh [extra volume_only args, e.g. --threshold-db -58]
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
bin="$(mktemp -d)/volume_only"
c++ -std=c++17 -O3 -I"$repo/include" -I"$repo/third_party" "$repo/analysis/volume_only.cpp" -o "$bin"
mkdir -p "$repo/exports/volume_only"
cd "$repo"
"$bin" --repo . "$@"
