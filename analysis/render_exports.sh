#!/bin/bash
# Renders the full static-condition export sweep (2 scenes x 5 priority
# classes x 3 conditions = 30 WAVs) that analysis/compute_metrics.py
# expects, using src/main.cpp's --static mode (see HANDOFF.md / the
# task spec this was built from). Run from the repo root after building
# demo_engine_cli.
#
# Deliberately avoids bash associative arrays (`declare -A`) - macOS ships
# bash 3.2 (pre-GPLv3) as /bin/bash, which doesn't support them at all.
set -euo pipefail

CLI="./demo_engine_cli"
OUTDIR="exports"
mkdir -p "$OUTDIR"

# Sidechain Compressor settings held identical across every basic/advanced
# run so comparisons isolate the effect of band-limiting, not differing
# compressor tuning (see the task spec's acceptance criteria). Override by
# editing these or by exporting the same-named env vars before running.
THRESHOLD_DB="${THRESHOLD_DB:--30}"
RATIO="${RATIO:-4}"
KNEE_DB="${KNEE_DB:-6}"
ATTACK_MS="${ATTACK_MS:-5}"
RELEASE_MS="${RELEASE_MS:-120}"
MAX_REDUCTION_DB="${MAX_REDUCTION_DB:-6}"

scene_slug() {
    case "$1" in
        "Construction Scene") echo "construction" ;;
        "PublicTransit Scene") echo "publictransit" ;;
        *) echo "unknown_scene_$1" ;;
    esac
}

key_slug() {
    case "$1" in
        "Dialogue") echo "dialogue" ;;
        "Music") echo "music" ;;
        "Background Noise") echo "background" ;;
        "Safety Alerts") echo "safety" ;;
        "Other") echo "other" ;;
        *) echo "unknown_key_$1" ;;
    esac
}

SCENES=("Construction Scene" "PublicTransit Scene")
KEYS=("Dialogue" "Music" "Background Noise" "Safety Alerts" "Other")

for scene in "${SCENES[@]}"; do
    sslug="$(scene_slug "$scene")"
    for key in "${KEYS[@]}"; do
        kslug="$(key_slug "$key")"
        echo "=== $sslug / $kslug ==="

        "$CLI" --static --scene "$scene" --key "$key" --mode unprocessed \
            --out "$OUTDIR/${sslug}_${kslug}_unprocessed.wav"

        "$CLI" --static --scene "$scene" --key "$key" --mode basic \
            --threshold "$THRESHOLD_DB" --ratio "$RATIO" --knee "$KNEE_DB" \
            --attack "$ATTACK_MS" --release "$RELEASE_MS" --max-reduction "$MAX_REDUCTION_DB" \
            --out "$OUTDIR/${sslug}_${kslug}_basic.wav"

        "$CLI" --static --scene "$scene" --key "$key" --mode advanced \
            --threshold "$THRESHOLD_DB" --ratio "$RATIO" --knee "$KNEE_DB" \
            --attack "$ATTACK_MS" --release "$RELEASE_MS" --max-reduction "$MAX_REDUCTION_DB" \
            --out "$OUTDIR/${sslug}_${kslug}_advanced.wav"
    done
done

echo "Done. Rendered $(ls "$OUTDIR"/*.wav 2>/dev/null | wc -l | tr -d ' ') WAVs in $OUTDIR/"
