# Sidechain-Keyed Unmasking Demo Engine — Handoff Document

**Status:** Working browser demo (C++/WASM engine + AudioWorklet UI), deployed to
GitHub Pages, plus a native CLI harness for standalone DSP testing/validation.
**Live demo:** https://jmicensky.github.io/sidechain-keyed-unmasking-demo/web/index.html
**Companion paper:** "Sidechain-Keyed Multiband Compression for Safety-Sound
Unmasking in Hearing Aid Dynamic Range Processing"

---

## 1. What this is

A real-time-safe C++ audio engine implementing the sidechain-keyed ducking
architecture described in the paper, built both as a native CLI test harness
and as a WebAssembly module driving a browser AudioWorklet UI. It takes five
pre-separated stem recordings (Dialogue, Music, Background Noise, Safety
Alerts, Other) — from a source separator, in the paper's framing — and lets
you interactively:

- Select any one class as the **Unmask key** — the other four are ducked
  based on that class's envelope (or, in Resonance mode, its spectral peaks).
- Choose one of three duck modes:
  - **Basic** — full-spectrum ducking, one shared gain applied everywhere.
  - **Advanced** — band-limited ducking, restricted to the *currently-keyed
    class's own frequency range* (e.g. 400 Hz–7 kHz while Dialogue is the
    key, 60 Hz–2 kHz while Background Noise is the key — see
    `kUnmaskFrequencyRanges` in `Types.h`). The ducked band visibly moves
    with the key selection, both in a UI badge and in the Gain Reduction
    graph. Within Advanced mode, a second choice governs *how* the
    threshold/ratio law is applied:
    - **Summed-bus** (default) — one shared threshold/ratio ducks every
      non-key channel identically.
    - **Per-channel** — each of the 5 channels gets its own independently
      adjustable threshold/ratio (still driven by the same shared detector
      level), with a live gain-reduction meter per channel so you can watch
      the effect of tuning each one.
  - **Resonance** — STFT-based dynamic notch ducking: finds the key signal's
    N loudest, mutually-separated spectral peaks each hop and ducks only
    those frequencies in the other channels, tracking wherever the key's
    energy actually moves (loosely modeled on resonance suppressors like
    Soothe2).
- Tune the shared **Sidechain Compressor** (threshold, knee, ratio, attack,
  release, and a **Max Reduction** ceiling — pull this back for a more
  transparent, -2 to -6 dB result instead of letting the raw law cut
  arbitrarily deep) that drives Basic and both Advanced sub-modes.
- Tune **Resonance Mode**'s own settings (peak count, bandwidth in octaves,
  and its own separate Max Reduction ceiling).
- Apply an **Output Compressor (WDRC)** — a 6-band, fixed-crossover
  (100/299/894/2675/8000 Hz) compressor on the final summed mix, independent
  of duck mode, with its own threshold/ratio/makeup-gain/attack/release and
  a bypass switch (bypassed by default). Was originally targeted at 12
  bands (to approximate a higher-end commercial hearing aid) but this
  engine's nested-crossover filterbank measurably can't do 12 bands this
  closely spaced without real coloration — see `WdrcCompressor.h`'s and
  `Types.h`'s comments for the full band-count story.
- **Mute** or **Solo** any of the five channels independently, mixer-style
  (solo is exclusive — engaging one clears any other).
- Switch between three full **scenes** (Construction, Public Transit,
  Restaurant), each with its own five real stems. There is no separate
  "unprocessed" reference player - leaving Unmask disabled on the
  Decomposed Mix already plays the raw stem sum, so a second dedicated
  player would just duplicate that (an earlier version had one; removed as
  redundant).

All control changes (key selection, mode, mute, solo, every slider) are
designed to be click/pop-free even when changed mid-playback, which is the
reason the architecture looks more involved than a simple "if unmask: apply
gain" branch — filter/envelope state is kept continuously running and
crossfaded rather than hard-switched.

## 2. Directory layout

```
demo_engine/
├── CMakeLists.txt          CMake build for the CLI (file present; g++ direct
│                            build is what's actually been verified — see §6)
├── include/
│   ├── Types.h              Shared constants, enums, parameter structs -
│   │                        per-key-channel frequency ranges, DuckMode,
│   │                        AdvancedDuckingMode, CompressorParams
│   ├── Smoother.h            Generic click-free parameter ramp
│   ├── GainComputer.h        Envelope follower + soft-knee gain computer (Eqs. 1–2)
│   ├── Crossover.h           Biquad + LR4 crossover split + N-band filterbank (template)
│   ├── FFT.h                 Minimal radix-2 Cooley-Tukey FFT (power-of-two sizes)
│   ├── ResonanceSuppressor.h STFT-based dynamic-notch suppressor (Resonance mode)
│   ├── WdrcCompressor.h      Output-bus 6-band WDRC-style compressor
│   └── Engine.h              Top-level signal flow, ties everything together
├── src/
│   └── main.cpp              CLI: scripted-timeline demo mode (default) and
│                              --static single-fixed-condition export mode
├── third_party/
│   └── dr_wav.h               Single-header WAV read/write (mackron/dr_libs, MIT)
├── wasm/
│   ├── wasm_bridge.cpp        extern "C" wrapper exposing Engine to WebAssembly
│   └── build.sh                em++ build script -> web/engine.mjs (SINGLE_FILE)
├── web/
│   ├── index.html              Page markup + all CSS
│   ├── app.js                  Main-thread UI logic, waveform/gain-viz drawing
│   ├── engine-worklet.js       AudioWorkletProcessor driving the WASM engine
│   └── engine.mjs              Built WASM module (checked in, regenerate via
│                                wasm/build.sh after any include/ or wasm/ change)
├── analysis/
│   ├── render_exports.sh       Renders the full --static export sweep -> exports/
│   ├── compute_metrics.py      Masking-margin + STOI metrics from those exports
│   └── results.csv             Saved results table from the last full sweep run
├── exports/                     --static mode's rendered WAVs (gitignored, ~930MB -
│                                regenerate via analysis/render_exports.sh)
├── Construction Scene/          Real stems + reference blend (~135MB, tracked in git)
├── PublicTransit Scene/         Real stems + reference blend (~146MB, tracked in git)
└── Restaurant Scene/            Real stems, no reference blend (~111MB, tracked in git)
```

## 3. Module-by-module

### `Types.h`
- `ClassIndex` enum: fixes the five classes to indices 0–4
  (`kDialogue, kMusic, kBackgroundNoise, kSafetyAlerts, kOther`). Any code
  that needs to iterate "all classes" loops `0..kNumClasses`.
- `DuckMode`: `Basic` (full-band) / `Advanced` (band-limited, per-key-channel
  range) / `Resonance` (dynamic spectral-peak notches).
- `AdvancedDuckingMode`: `SummedBus` / `PerChannel` — see §1.
- `kUnmaskFrequencyRanges[kNumClasses]`: the frequency range Advanced mode
  ducks other channels within when each class is the key — Dialogue
  400Hz–7kHz, Music 75Hz–12kHz, Background Noise 60Hz–2kHz, Safety Alerts
  300Hz–2kHz, Other 700Hz–12kHz.
- `kWdrcNumBands` (6) / `kWdrcCrossoverHz` (100/299/894/2675/8000 Hz) —
  **only** used by the standalone crossover-flatness self-check in
  `main.cpp` and by `WdrcCompressor`'s fixed band split. **Not** used by
  Advanced duck mode (which uses `kUnmaskFrequencyRanges` and a dynamic
  2-point split instead). See the comment above these constants in
  `Types.h` for why 6 bands, not the originally-targeted 12.
- `CompressorParams` (threshold, ratio, knee, attack/release, safety gain) —
  the Sidechain Compressor's defaults; still adjustable live via the UI/CLI
  rather than hardcoded to a single literature-locked value.

### `Smoother.h`
One-pole exponential ramp toward a target value, parameterized by a time
constant in ms. Used everywhere a UI action needs to become a smooth
audio-rate change instead of a step: per-channel mute/solo audibility
(`kAudibleRampMs = 8ms`), per-channel key/non-key blend (`kKeyRampMs =
30ms`), and the WDRC bypass ramp.

### `GainComputer.h`
Two classes implementing the paper's math directly:
- `EnvelopeFollower::tick()` — Eq. 1, attack/release smoothed dB level
  estimate of the sidechain key signal.
- `GainComputer::computeLinearGain()` — Eq. 2, soft-knee compressor curve
  (threshold/ratio/knee only — no time constants, those live in
  `EnvelopeFollower`), converted to a linear multiplier. Instantiated once
  for the shared Sidechain Compressor, once per channel for Advanced
  Per-channel mode, and reused inside `SpectralResonanceSuppressor` and
  `WdrcCompressor`.

### `Crossover.h`
- `Biquad` — RBJ cookbook lowpass/highpass, transposed Direct Form II.
- `LR4Split` — one Linkwitz-Riley 4th-order (24 dB/oct) two-way split.
  `.prepare(sampleRate, fc)` resets internal filter state, which is fine for
  discrete, infrequent UI-driven reconfiguration (e.g. switching key
  channel) but would click if called every sample. Advanced duck mode builds
  its per-key-channel band by cascading two of these per channel (high-edge
  split, then low-edge split on the low branch) to isolate below/within/above
  the active range.
- `CrossoverFilterbank<NumBands>` — a templated N-band split built from
  NumBands-1 nested `LR4Split`s, used now only by `WdrcCompressor` (N=6,
  `kWdrcCrossoverHz`) and by `main.cpp`'s `checkCrossoverFlatness()`
  self-test (**not** by Advanced duck mode, which moved to the dynamic
  `LR4Split`-based approach above).
  **Known characteristic, not a bug:** summing all bands is flat within a
  few tenths of a dB almost everywhere, but dips up to ~1.5 dB - a standard
  property of tree-structured multiband crossovers with closely-spaced
  bands (each individual LR4 split reconstructs its own input essentially
  exactly, but small per-split ripple compounds across nested stages the
  closer neighboring crossover points sit). **This is also a hard ceiling,
  not just a cosmetic characteristic:** measured directly by band count at
  this same 100 Hz-8 kHz range, deviation grows from 0.14 dB at 4 bands to
  10.7 dB at 12 bands - a true 12-band (or higher) WDRC was the original
  target but is not achievable with this nested-crossover approach without
  a different filterbank principle (e.g. FFT/STFT bin-grouping, which
  reconstructs exactly regardless of band count - see `FFT.h`/
  `ResonanceSuppressor.h` for the STFT infrastructure already in this
  project for Resonance mode; not attempted for WDRC).

### `FFT.h`
Minimal iterative radix-2 Cooley-Tukey FFT, in place, power-of-two sizes
only. Verified during development against identity round-trip, single-bin
sinusoid detection, and Parseval energy conservation.

### `ResonanceSuppressor.h`
`SpectralResonanceSuppressor` implements Resonance duck mode as true STFT
processing: fixed 2048-sample FFT, 50% overlap, square-root-Hann
analysis+synthesis windowing (verified to reconstruct an unmodified signal
to numerical precision). Each hop it analyzes the key channel's real
magnitude spectrum, finds up to `kMaxPeaks` (12) loudest mutually-separated
peaks (live-adjustable count, 1–12), and builds a smooth per-bin gain mask
applied to all 10 signals (5 classes × L/R) via overlap-add. This replaced
an earlier design that swept narrow IIR notch filters through the spectrum
in the time domain — mechanically the same mechanism a phaser effect uses,
which produced an audible "phasey"/swept character. Multiplying FFT bins by
a real gain has no equivalent phase modulation to hear. Fixed algorithmic
latency of `kFftSize` (2048) samples, compensated for elsewhere in `Engine`
via a matched dry-path delay line so the key/dry crossfade stays
time-aligned.

### `WdrcCompressor.h`
A self-contained 6-band (fixed 100/299/894/2675/8000 Hz split, reusing
`CrossoverFilterbank<6>`) stereo compressor applied once to the final mix,
after ducking — not to be confused with the sidechain-keyed ducking
compressor above, which reacts to a *different* channel's level. One shared
threshold/ratio/makeup-gain across all 6 bands (still genuinely multiband:
each band's own detector reacts independently). Starts bypassed by default.
Originally targeted at 12 bands; see `Crossover.h`'s entry above for why 6
is this architecture's actual ceiling.

### `Engine.h`
The integration point (~600 lines). Key design decisions, worth
understanding before modifying:

- **Ducking is applied per-channel, then summed — not applied to the
  pre-summed mix.** Mathematically identical to processing the sum (filtering
  and multiplication by a shared gain both distribute over addition), but
  doing it per-channel is what lets Mute/Solo work correctly on the output
  *after* ducking, and is what makes Advanced Per-channel mode's independent
  per-channel gains possible at all.
- Each `ChannelStrip` owns its own pair of `LR4Split`s (`unmaskHighSplitL/R`,
  `unmaskLowSplitL/R`) so filter state persists correctly across mute/unmute
  and key changes — reconfigured only when the key channel actually changes
  (`updateUnmaskRange()`), not every sample.
- Solo is **exclusive** (only one channel soloed at a time) — see
  `Engine::setSolo()`.
- Advanced Per-channel mode: switching into it seeds every channel's
  independent threshold/ratio from whatever the shared Sidechain Compressor
  values are *at that instant* (`setAdvancedDuckingMode()`), so they start
  matched and diverge freely from there; switching back to Summed-bus
  doesn't discard the diverged values, it just stops using them until
  Per-channel is re-engaged, at which point they're reseeded again.
- The **Max Reduction** ceiling (`maxReductionDb_`, `clampGainLinear()`)
  floors the linear gain wherever the shared threshold/ratio law computes it
  directly — Basic mode, Advanced-SummedBus, and each channel's independent
  gain in Advanced-PerChannel. It deliberately never touches Resonance
  mode's own, separate `resonanceMaxReductionDb_`.

Per-sample signal flow inside `Engine::process()`:
1. Read the current key channel's raw sample; gate to silence if that
   channel is muted (muting the key also stops ducking, via feeding the
   envelope follower/spectral analysis zero rather than branching).
2. **Resonance mode**: feed all 10 signals (5 classes × L/R) plus the key
   into `SpectralResonanceSuppressor::tick()`, which does its own internal
   STFT-domain gain computation and returns ducked output directly.
   **Basic/Advanced modes**: run the envelope follower + shared
   `GainComputer` on the (possibly gated) key sample to get `g(n)`, then
   clamp it to the Max Reduction ceiling.
3. For each of the 5 channels: compute its dry path (`raw * safetyGain`) and
   its ducked path — Basic: `raw * g`; Advanced: split into below/within/above
   the active unmask range via the channel's `LR4Split` pair, multiply only
   the within-range content by `g` (Summed-bus) or that channel's own
   independently-computed, independently-clamped gain (Per-channel);
   Resonance: read from the STFT suppressor's output, time-aligned via a
   delay line on the dry side.
4. Blend dry vs. ducked per channel using that channel's smoothed
   `keyBlend` (1 = acting as key/dry, 0 = part of the ducked group).
5. Multiply by that channel's smoothed `audibleGain` (mute/solo) and sum
   into the mix.
6. Run the summed mix through the output-bus `WdrcCompressor` (independent
   of duck mode, bypassable).

### `main.cpp` (CLI test harness)
- **Synthesizes 5 placeholder stems** if no WAV paths are given (fake
  syllable-gated dialogue tone, a sustained chord pad, filtered noise, a
  two-tone siren, sparse decaying transient taps). Or point it at real
  files with zero code changes:
  ```
  ./demo_engine_cli dialogue.wav music.wav bg.wav safety.wav other.wav out.wav
  ```
- **Scripted event timeline** — drives the Engine through a long sequence of
  mid-playback control changes (enable Unmask, switch key mid-playback,
  mute/solo/clear, switch duck mode, live compressor/knee/resonance/WDRC
  parameter changes including out-of-range clamping checks) at fixed
  timestamps, so a single run exercises essentially every interaction path.
- **Two automated self-checks**, both run every time:
  - `checkCrossoverFlatness()` — swept-sine RMS-ratio test of the 6-band
    `CrossoverFilterbank<6>` (the one `WdrcCompressor` and the self-check
    itself use; **not** Advanced duck mode's per-key-channel splits, which
    aren't exercised by this particular check).
  - `countClicks()` — scans the rendered output for sample-to-sample jumps
    above a threshold (0.35). When run against the real Construction Scene
    stems, the detected jumps (2, both within 19 samples of each other) are
    a known, already-investigated pre-existing WDRC transient-overshoot
    event (slow attack + high ratio +
    positive makeup gain letting a real percussive moment through at
    t≈74.86s) — not an engine bug.
- **`--static` mode** — a separate code path (not the scripted timeline
  above) for objective-measurement work: renders one full clip under a
  single fixed configuration with no mid-clip changes at all.
  ```
  ./demo_engine_cli --static --scene "Construction Scene" --key Dialogue \
    --mode advanced --threshold -30 --ratio 4 --knee 6 --attack 5 \
    --release 120 --max-reduction 6 --out exports/construction_dialogue_advanced.wav
  ```
  `--mode` is `unprocessed` (reuses the scene's mastered reference blend
  file directly — confirmed it's *not* a bit-identical sum of the 5 raw
  stems, so this avoids computing a second, slightly different baseline),
  `basic`, or `advanced` (Advanced-mode ducking, forced to
  `AdvancedDuckingMode::SummedBus` — Per-channel and Resonance are out of
  scope for this path). Only loads real scene stems, never the synthetic
  generators, so it's fully deterministic. See `analysis/` below for what
  consumes its output.

### `wasm/wasm_bridge.cpp` + `wasm/build.sh`
Thin `extern "C"` wrapper exposing `Engine` to WebAssembly — one function per
`Engine` setter/getter, plus `engine_alloc`/`engine_free`/`engine_process`
scratch-buffer helpers so JS can stage stem/output data across the WASM
boundary. `build.sh` compiles it (with `Engine.h` and everything it includes)
via `em++` into a single self-contained ES module (`-sSINGLE_FILE=1` embeds
the wasm binary as base64, so the AudioWorklet never needs a second fetch)
at `web/engine.mjs`. **Must be rerun after any `include/` or
`wasm_bridge.cpp` change** — `engine.mjs` is checked in but not
auto-regenerated. Requires Emscripten (pinned path in the script:
`/opt/homebrew/Cellar/emscripten/6.0.8/bin`, adjust if your machine differs).

### `web/` (browser UI)
- `index.html` — all markup + CSS. Layout: a load bar, then a single
  "Decomposed Mix" panel (per-channel waveforms/mute/solo/per-channel
  threshold-ratio-meter knobs, then the control stack: Unmask/Key/Mode/
  Advanced-ducking-mode, a two-column row with Sidechain Compressor +
  Resonance Mode controls on the left and the Gain Reduction graph on the
  right (sized to match their combined height), then the WDRC section). No
  separate "Unprocessed" reference panel - it played the scene's whole-mix
  blend file for A/B comparison, which was redundant with just leaving
  Unmask disabled on this same panel; removed rather than kept as a
  second, weaker control for the same thing. (Also required for Restaurant
  Scene, which has no whole-mix blend file to begin with.)
- `app.js` — decodes all WAVs, draws waveform overviews, owns all control
  wiring (postMessage to the worklet), and draws the Gain Reduction EQ-style
  graph (mirrors `kUnmaskFrequencyRanges` in a JS-side `UNMASK_CHANNEL_RANGES`
  table so range-dependent UI updates instantly on dropdown change, even
  before a scene is loaded or Play is ever pressed — the engine only reports
  live state while `process()` is actively running).
- `engine-worklet.js` — the `AudioWorkletProcessor`; loads the WASM module,
  drives `engine_process()` one 128-frame render quantum at a time, and
  throttles state-reporting postMessages back to the main thread to ~47 Hz
  (every 8th quantum) to avoid flooding it.

### `analysis/` (objective intelligibility metrics)
Consumes `--static` mode's exports to produce comparable, exportable data
proving (or not) that sidechain ducking improves priority-class audibility.
Scope: Basic and Advanced (Summed-bus) duck modes only — Resonance's
ducking doesn't occupy a fixed frequency band, so a fixed-band
masking-margin measurement doesn't represent what it does.
- `render_exports.sh` — renders the full 3-scene × 5-priority-class ×
  3-condition sweep (45 WAVs) into `exports/` (gitignored — regenerate
  locally, ~930MB, rather than pulling it from git). Written without bash
  associative arrays (`declare -A`) because macOS ships bash 3.2 as
  `/bin/bash`, which doesn't support them.
- `compute_metrics.py` (needs `numpy`, `scipy`, `soundfile`, `pystoi`) —
  two metrics per (scene, priority class, condition):
  1. **In-band masking margin** — both the dry priority stem and the mix
     export are zero-phase band-passed and framed (20ms/50% overlap), and
     the masker level is estimated via power subtraction
     (`mix_power − gSafety² × priority_power`, incoherent-signal
     assumption). Gated to frames where the priority stem is actually
     active; reports mean and worst-case margin. **Not** measured on the
     priority class's full nominal `kUnmaskFrequencyRanges` band —
     `MARGIN_MEASUREMENT_PAD_OCTAVES` (0.85) pads inward from both edges
     first (Dialogue's nominal 400Hz–7kHz becomes ~721Hz–3.9kHz measured).
     Reason: this script's own analysis bandpass filter, applied to the
     rendered output, interacts with Advanced mode's internal LR4
     crossover (a different filter shape) right at the shared nominal
     edges — confirmed via the engine's own reported gain that Basic and
     Advanced apply bit-identical gain within the class's core band when
     settings match, so the ~0.9–1.0dB gap the unpadded measurement showed
     between them was a filter-interaction artifact, not real. 0.85
     octaves of padding closes it to within 0.13dB across a 9-run
     parameter sweep — a real, deliberate reduction in measured bandwidth,
     not a free fix.
  2. **STOI/ESTOI** (Dialogue only) — via `pystoi`, with a small
     cross-correlation lag search (±20ms) to correct for Advanced mode's
     crossover group delay before scoring. Operates on the full-band
     signal directly, so unaffected by the margin-padding note above.
  Results print to stdout and save to `analysis/results.csv`. The
  `unprocessed` condition inside each export is the true sum of the 5 raw
  stems (Basic mode, Unmask off) — not each scene's separately-bounced
  reference blend, which was found to disagree with the raw stem sum by
  ~8% RMS overall (likely a DAW export inconsistency) and didn't localize
  to any one stem.
- **Finding from the full sweep (both scenes, shared -30dB/4:1/6dB
  compressor settings):** Dialogue-as-priority clearly showed the
  hypothesized direction on both metrics and both scenes (unprocessed
  worst, basic/advanced both improved — e.g. Construction: mean margin
  -9.51dB → -7.51dB/-7.48dB, STOI 0.393 → 0.434/0.426). Other priority
  classes showed weak or mixed movement. Root-caused, not a script bug: a
  threshold-crossing diagnostic showed Music/Safety/Background/Other's own
  levels rarely reach anywhere near -30dBFS in either scene's real stems
  (Dialogue peaks around -9dB; the others mostly sit -18dB to -32dB), so a
  single shared threshold barely engages ducking when those classes are
  the key. This is itself a real finding — it's the concrete data-backed
  case for using Advanced Per-channel mode's independent per-class
  threshold/ratio (§1) rather than one shared threshold across all five
  classes.

## 4. Deployment

Static GitHub Pages site (`.nojekyll` present so the `web/` and scene
folders serve as-is) — no server-side component. The three scene folders'
real stems (~390MB total) are checked into git and fetched directly by the
browser. `git push` to `main` triggers a Pages rebuild automatically; poll
`gh api repos/jmicensky/sidechain-keyed-unmasking-demo/pages/builds/latest`
for `"status":"built"` to confirm a deploy landed.

## 5. Things intentionally deferred / known limitations

- **`CompressorParams` defaults are starting points**, not literature-locked
  final values — they're live-adjustable in the UI/CLI by design, so this
  may be a non-issue depending on how the paper's final methodology section
  reads, but worth double-checking against the literature discussion
  (Kowalewski et al. 2018 / Chen et al. 2021) before treating any specific
  default as "the" recommended value.
- **The crossover-summing dip** (§3, `Crossover.h`) is documented and
  understood but not addressed — it only affects `WdrcCompressor`'s 6-band
  split now (Advanced duck mode moved off this filterbank entirely), so its
  practical relevance shrank; revisit only if it turns out audible with the
  WDRC stage engaged on real material. Also worth remembering: a true
  12-band WDRC (the original target) is NOT achievable within this
  nested-crossover architecture at all (measured 10.7dB coloration) - only
  a different filterbank principle (FFT/STFT bin-grouping) would get there,
  not a further tweak to this one.
- **CMake build path unverified** — everything here has been built/tested
  via `g++`/`em++` directly (see §6); `CMakeLists.txt` exists and should
  work but hasn't actually been run in this environment.
- **No automated per-channel "recommended value" study yet.** Advanced
  Per-channel mode's independent threshold/ratio knobs exist so this can be
  explored, and the first `analysis/` sweep (§3) now gives a concrete,
  data-backed reason why it matters: a single shared -30dB threshold works
  well for Dialogue but barely engages for classes whose real-world levels
  sit well below it. Actually deriving recommended per-class values
  (Summed-bus as the primary methodology, Per-channel to help derive
  per-category starting points, per the paper goal) still hasn't been done.
- **Browser/device testing has been Chromium-via-Playwright only** in this
  environment — no manual cross-browser (Safari/Firefox) or real-device
  audio-hardware listening pass has been done as part of this work.

## 6. Building and running

```bash
# Native CLI (verified working):
g++ -std=c++17 -O2 -Iinclude -Ithird_party src/main.cpp -o demo_engine_cli -lm

# CMake (file present, not yet verified in practice):
mkdir build && cd build && cmake .. && cmake --build .

# Run with synthesized test stems:
./demo_engine_cli

# Run with real stems + write output for listening:
./demo_engine_cli "Construction Scene/Dialogue.wav" "Construction Scene/MUSIC.wav" \
  "Construction Scene/BKG.wav" "Construction Scene/SAFETY.wav" "Construction Scene/OTHER.wav" out.wav

# Rebuild the WASM module after any include/ or wasm/ change (needs Emscripten):
bash wasm/build.sh

# Serve the browser demo locally:
python3 -m http.server 8765   # from the repo root
# then open http://localhost:8765/web/index.html
```

Native CLI output is a stereo WAV (mono engine output duplicated to both
channels) at whatever sample rate the input stems used (48 kHz for the
synthesized test scene and all three real scenes' stems).

## 7. Suggested next steps

1. Lock in final `CompressorParams` values from the literature discussion,
   if the paper's methodology calls for a single fixed recommendation
   (§5) — update `Types.h`'s defaults accordingly.
2. Use Advanced Per-channel mode to derive and document recommended
   per-class threshold/ratio starting points, then decide whether those
   become new defaults or stay as user-adjustable knobs. The §3 `analysis/`
   finding (shared -30dB threshold barely engages for anything but
   Dialogue) is the concrete starting point for this — rerun
   `render_exports.sh`/`compute_metrics.py` with per-class thresholds tuned
   closer to each class's own real levels and compare against the
   shared-threshold baseline in `results.csv`.
3. A critical listen across all three scenes with real headphones/speakers,
   specifically around the Max Reduction default (6dB) and the WDRC
   transient-overshoot event documented in §3 (`main.cpp`), to confirm both
   read as intended rather than just measuring correctly.
4. Manual cross-browser pass (Safari, Firefox) — everything so far has been
   verified via Chromium/Playwright and native CLI DSP probes only.
5. Decide whether the 100–300 Hz `WdrcCompressor` crossover interaction
   (§5) is audible enough on real material to be worth addressing.
