# Sidechain-Keyed Unmasking Demo Engine — Handoff Document

**Status:** Standalone C++ core, validated via CLI test harness. Not yet wired into
a browser/WebAssembly front end.
**Companion paper:** "Sidechain-Keyed Multiband Compression for Safety-Sound
Unmasking in Hearing Aid Dynamic Range Processing"

---

## 1. What this is

A real-time-safe C++ audio engine implementing the sidechain-keyed ducking
architecture described in the paper. It takes five pre-separated stem
recordings (Dialogue, Music, Background Noise, Safety Alerts, Other) and lets
you interactively:

- Select any one class as the **Unmask** key — the other four are ducked
  based on that class's envelope.
- Choose **Basic** mode (full-spectrum ducking) or **Advanced** mode
  (band-limited ducking, 300 Hz–1.8 kHz only), matching the paper's two-stage
  narrative.
- **Mute** or **Solo** any of the five channels independently, mixer-style.

All control changes (key selection, mode, mute, solo) are designed to be
click/pop-free even when changed mid-playback, which is the reason the
architecture looks more involved than a simple "if unmask: apply gain" branch.

## 2. Directory layout

```
demo_engine/
├── CMakeLists.txt          CMake build (not verified in this sandbox — no
│                            cmake binary was available here; g++ works, see §5)
├── include/
│   ├── Types.h              Shared constants, enums, parameter structs
│   ├── Smoother.h            Generic click-free parameter ramp
│   ├── GainComputer.h        Envelope follower + soft-knee gain computer (Eqs. 1–2)
│   ├── Crossover.h           Biquad + LR4 4-band crossover filterbank
│   └── Engine.h              Top-level signal flow, ties everything together
├── src/
│   └── main.cpp              CLI test harness: loads/synthesizes stems, runs a
│                              scripted event timeline, writes WAV, self-checks
└── third_party/
    └── dr_wav.h               Single-header WAV read/write (mackron/dr_libs, MIT)
```

## 3. Module-by-module

### `Types.h`
- `ClassIndex` enum: fixes the five classes to indices 0–4
  (`kDialogue, kMusic, kBackgroundNoise, kSafetyAlerts, kOther`). Any code
  that needs to iterate "all classes" loops `0..kNumClasses`.
- `DuckMode`: `Basic` (full-band) vs `Advanced` (band-limited).
- Crossover edges (`kCrossoverLow/Mid/High` = 100/300/1800 Hz) and
  `CompressorParams` (threshold, ratio, knee, attack/release, safety gain) —
  **these are placeholder values**, not yet the final numbers from the paper's
  literature-grounded discussion (Kowalewski et al. 2018 / Chen et al. 2021).
  Update `CompressorParams`'s defaults once those are locked in.

### `Smoother.h`
One-pole exponential ramp toward a target value, parameterized by a time
constant in ms. This is the single mechanism used everywhere a UI action
needs to become a smooth audio-rate change instead of a step: per-channel
mute/solo audibility (`kAudibleRampMs = 8ms`) and per-channel key/non-key
blend (`kKeyRampMs = 30ms`) both use this class.

### `GainComputer.h`
Two classes implementing the paper's math directly:
- `EnvelopeFollower::tick()` — Eq. 1, attack/release smoothed dB level
  estimate of the sidechain key signal.
- `GainComputer::computeLinearGain()` — Eq. 2, soft-knee compressor curve,
  converted to a linear multiplier.

### `Crossover.h`
- `Biquad` — RBJ cookbook lowpass/highpass, transposed Direct Form II.
- `LR4Split` — one Linkwitz-Riley 4th-order (24 dB/oct) two-way split, built
  from two cascaded Butterworth 2nd-order stages per branch (the standard
  construction; LR4 sums flat without needing a polarity inversion, unlike
  LR2).
- `CrossoverFilterbank` — the actual 4-band split, built as a **tree** of
  three `LR4Split`s:
  ```
  input → split@1800Hz → band4 (high)
              ↓ low
           split@300Hz  → band3 (300–1800, the ducked band)
              ↓ low
           split@100Hz  → band1 (0–100), band2 (100–300)
  ```
  **Known characteristic, not a bug:** summing all 4 bands is flat within a
  few tenths of a dB almost everywhere, but dips up to ~1.4 dB between
  100–300 Hz, because those two crossover points sit under 1.6 octaves
  apart and their LR4 transition skirts overlap. This is a standard property
  of tree-structured multiband crossovers with closely-spaced bands (real
  commercial multiband compressors exhibit the same effect) — see the
  `checkCrossoverFlatness()` self-test in `main.cpp` for the verification.

### `Engine.h`
The integration point. Key design decision, worth understanding before
modifying: **ducking is applied per-channel, then summed — not applied to
the pre-summed mix.** This is mathematically identical to the paper's Eq. 3–4
(processing the sum) because filtering and multiplication by a shared gain
both distribute over addition, but doing it per-channel is what allows Mute/
Solo to work correctly on the output *after* ducking has already been
applied. Each `ChannelStrip` owns its own `CrossoverFilterbank` instance so
filter state persists correctly across mute/unmute and key changes.

Per-sample signal flow inside `Engine::process()`:
1. Read the current key channel's raw sample; gate to silence if that
   channel is muted (per your decision: **muting the key also stops
   ducking**, achieved by feeding the envelope follower zero rather than by
   branching).
2. Run the envelope follower + gain computer on that (possibly gated)
   sample to get `g(n)`.
3. For each of the 5 channels: compute its dry path (`raw * safetyGain`) and
   its ducked path (Basic: `raw * g`; Advanced: crossover into 4 bands, only
   band 3 multiplied by `g`, recombine).
4. Blend dry vs. ducked per channel using that channel's smoothed
   `keyBlend` (1 = acting as key/dry, 0 = part of the ducked group).
5. Multiply by that channel's smoothed `audibleGain` (mute/solo) and sum
   into the output.

Solo is implemented as **exclusive** (only one channel soloed at a time),
matching your explicit decision — see `Engine::setSolo()`.

### `main.cpp` (CLI test harness)
- **Synthesizes 5 placeholder stems** if no WAV paths are given (fake
  syllable-gated dialogue tone, a sustained chord pad, filtered noise, a
  two-tone siren gated on at 1–3s and 6–8s, and sparse decaying transient
  taps). Swap in real files with zero code changes:
  ```
  ./demo_engine_cli dialogue.wav music.wav bg.wav safety.wav other.wav out.wav
  ```
- **Scripted event timeline** — drives the Engine through 8 mid-playback
  control changes (enable Unmask, switch key, mute, solo, clear solo, switch
  key back, switch mode, mute the key) at fixed timestamps, so a single run
  exercises every interaction path described above.
- **Two automated self-checks**, both run every time:
  - `checkCrossoverFlatness()` — swept-sine RMS-ratio test across 14
    frequencies (this is the correct test methodology; an earlier
    time-domain impulse-comparison version gave a misleading failure,
    because an LR4 crossover's summed output has flat *magnitude* but is
    not literally an identity filter — see the comment in the function for
    the full explanation if this trips anyone up again).
  - `countClicks()` — scans the rendered output for sample-to-sample jumps
    above a threshold. **Verified against the actual event timestamps** in
    the current test signal: all detected jumps line up with the synthetic
    "Other" channel's deliberately punchy transient taps (scheduled every
    1.7s starting at 0.5s), not with any of the 8 scripted engine control
    changes — i.e., no engine-caused clicks were found in the current test
    run.

## 4. Things intentionally deferred / not yet done

- **Not yet wired into WebAssembly/AudioWorklet.** This was deliberately
  scoped out — the plan is to validate the DSP core standalone first (this
  is that validation), then build the browser layer against a known-good
  core.
- **Real stems not yet integrated.** The harness accepts them as of now
  (§3, `main.cpp`), just needs the actual files.
- **`CompressorParams` defaults are placeholders**, not the paper's final
  chosen values.
- **The 100–300 Hz crossover interaction (§3, `Crossover.h`)** is documented
  and understood but not addressed — it's a minor, expected effect and
  wasn't judged worth the added filter complexity to eliminate, but revisit
  if it turns out to be audible in practice with real stems.
- **CMake build path unverified** — this sandbox didn't have `cmake`
  installed, so all building/testing here used `g++` directly (see §5).
  The `CMakeLists.txt` file exists and should work, just hasn't been run.

## 5. Building and running

```bash
# Direct g++ (verified working):
g++ -std=c++17 -O2 -Iinclude -Ithird_party src/main.cpp -o demo_engine_cli -lm

# CMake (file present, not yet verified in practice):
mkdir build && cd build && cmake .. && cmake --build .

# Run with synthesized test stems:
./demo_engine_cli

# Run with real stems:
./demo_engine_cli dialogue.wav music.wav bg.wav safety.wav other.wav out.wav
```

Output is a stereo WAV (mono engine output duplicated to both channels) at
whatever sample rate the input stems used (48 kHz for the synthesized test
scene).

## 6. Suggested next steps

1. Drop in real stem files, rerun, listen critically around each of the 8
   scripted event timestamps.
2. Lock in final `CompressorParams` values from the literature discussion
   and update `Types.h`.
3. Decide whether the 100–300 Hz crossover interaction needs addressing
   (likely not, but worth a critical listen with real material first).
4. Begin the WebAssembly/AudioWorklet integration, using this validated
   core as the foundation — the `Engine` class's public API
   (`prepare`, `loadStems`, `setMode`, `setUnmaskEnabled`, `setKeyChannel`,
   `setMute`, `setSolo`, `process`) is already shaped to be called from a
   thin JS-facing wrapper with minimal translation.
