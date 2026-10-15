#!/usr/bin/env bash
# Builds the C++ reference renderer and the C# parity test, renders the
# reference scenarios from the real Construction Scene stems, and checks the
# Unity C# port against them. Needs: clang++ (Xcode CLT) and Mono (csc + mono).
#
#   unity/tests/run_parity_test.sh [seconds] [renderDir]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
seconds="${1:-30}"
render_dir="${2:-}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

c++ -std=c++17 -O2 -I"$repo/include" -I"$repo/third_party" "$here/render_reference.cpp" -o "$work/render_reference"
"$work/render_reference" "$repo/Construction Scene" "$work" "$seconds"

csc -nologo -langversion:9 -optimize+ -out:"$work/ParityTest.exe" \
    "$here/ParityTest.cs" "$here/UnityAttributeStubs.cs" \
    "$repo/unity/Runtime/Dsp/DspPrimitives.cs" "$repo/unity/Runtime/Dsp/WdrcCompressor.cs" \
    "$repo/unity/Runtime/ProcessingSettings.cs" "$repo/unity/Runtime/UnmaskingBus.cs" \
    "$repo/unity/Runtime/SourceTapProcessor.cs"
mono "$work/ParityTest.exe" "$work" $render_dir
