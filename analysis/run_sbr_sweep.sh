#!/usr/bin/env bash
# Builds and runs the Johnson & Healy SBR sweeps (analysis/sbr_sweep.cpp),
# then writes analysis/sbr_sweep_findings.txt. ~10 min on a 10-core Mac.
#   analysis/run_sbr_sweep.sh [basicPoints] [enhancementPoints]
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
bin="$(mktemp -d)/sbr_sweep"
c++ -std=c++17 -O3 -I"$repo/include" -I"$repo/third_party" "$repo/analysis/sbr_sweep.cpp" -o "$bin"
cd "$repo"
"$bin" --sweep basic --n "${1:-1000}" --out analysis/sbr_sweep_basic.csv
"$bin" --sweep enhancement --n "${2:-1500}" --out analysis/sbr_sweep_enhancement.csv
python3 analysis/analyze_sbr_sweep.py > analysis/sbr_sweep_findings.txt
echo "wrote analysis/sbr_sweep_findings.txt"
