#!/usr/bin/env bash
# One setting per Unity mixer snapshot (Unmasked / GroupCompress /
# GroupCompressed>Unmasked) that lands the J&H SBR on +5.0 dB (+-0.1) on the
# Construction scene: search, round, verify, render, LL Dynamics report, Unity doc.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
bin="$(mktemp -d)/sbr_matched"
c++ -std=c++17 -O3 -I"$repo/include" -I"$repo/third_party" "$repo/analysis/sbr_matched_search.cpp" -o "$bin"
cd "$repo"
"$bin" --repo . --mode basic --target 5.0 --ref-scene construction --tag sbr5 --lhs 800 --gens 20 --per-gen 30
"$bin" --repo . --mode comp --target 5.0 --ref-scene construction --tag sbr5 --lhs 1200 --gens 25 --per-gen 30
"$bin" --repo . --mode combined --target 5.0 --ref-scene construction --tag sbr5 --lhs 800 --gens 20 --per-gen 30
python3 analysis/finalize_sbr5.py "$bin"
# Verify the ROUNDED settings on all three scenes (Construction is the reference).
awk -F'\t' '{for (s = 0; s < 3; s++) { split("construction publictransit restaurant", n, " ");
  printf "%s\t%s\t%s\t%s\n", $1, $2, n[s + 1], $3 }}' analysis/sbr5_settings.tsv > analysis/sbr5_eval.in.tsv
"$bin" --repo . --eval analysis/sbr5_eval.in.tsv --out analysis/sbr5_eval.tsv
mkdir -p exports/sbr5_construction paper-audio/sbr5-construction
"$bin" --repo . --only-scene construction --render analysis/sbr5_settings.tsv --render-dir exports/sbr5_construction
for f in exports/sbr5_construction/*.wav; do
  ffmpeg -y -loglevel error -i "$f" -codec:a libmp3lame -b:a 128k "paper-audio/sbr5-construction/$(basename "${f%.wav}").mp3"
done
"$bin" --repo . --only-scene construction --tag sbr5 --target 5.0 --report analysis/sbr5_settings.tsv
python3 analysis/write_unity_sbr5_handoff.py
