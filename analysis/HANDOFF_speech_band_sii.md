# Handoff: speech-band margin, SII, and re-running the margin sweeps

Started 2026-11-04. For the session that re-runs the sweeps. Read this whole
file before changing anything. The thesis is `Thesis-Draft/Thesis_reformat.tex`
(a separate git repo, synced with Overleaf); this repo holds the engine and
all analysis.

## Why this work exists

The paper's "mean margin gain" was measured on a narrow band, padded 0.85
octaves inward from Dialogue's Advanced-mode range (721–3883 Hz while that range
was 400 Hz–7 kHz). The padding hid an artifact where the analysis Butterworth
bandpass interacted with Advanced mode's LR4 crossover. The margin also used a
power-subtraction estimate of the masker (mix minus dry Dialogue). That estimate
reads about 1 dB high against separated renders, and is invalid when Dialogue
itself is processed (group compression, Combined).

The author decided:

1. **Measure margin over the speech band, 150 Hz–8 kHz**, fixed and independent
   of the processing's own bands.
2. **Fix the crossover artifact by method, not by padding**: separated renders
   plus FFT-bin band power.
3. **Make Advanced mode duck the same 150 Hz–8 kHz band** when Dialogue is the key.
4. **Add the Speech Intelligibility Index (SII, ANSI S3.5)**.
5. **Re-run the margin sweeps.** The old sweeps kept no renders, so nothing can be
   recomputed from disk.

## Ground rules from the author

- **Don't write results or interpretation prose in the thesis.** The author
  writes it. Generate tables and leave `\todo{...}` pointers listing what the
  numbers show.
- **Mark any thesis text you draft** as `\todo{AI draft --- rewrite: ...}`
  (`\protect\todo` inside captions). The Acknowledgments disclose AI use on
  that basis.
- **Restaurant is excluded** from most results. It stands in for a separator
  misclassifying background speech as Dialogue. Run sweeps on Construction and
  Public Transit only.
- **Ask before committing or pushing** in either repo.
- **SBR targets** (Johnson & Healy 2024, Ear & Hearing 45(6)): HI +5.1 dB,
  NH +12.2 dB.

## Done in the 2026-11-04 session (uncommitted)

### Dialogue's Advanced band is now 150 Hz–8 kHz

The tables were 175 Hz–7 kHz before (400 Hz–7 kHz in the paper). The mirrors are
kept in sync by hand:

- `include/Types.h` `kUnmaskFrequencyRanges`
- `web/app.js` `UNMASK_CHANNEL_RANGES`, plus the default readout in `web/index.html`
- `unity/Runtime/Dsp/DspPrimitives.cs` `UnmaskRanges`
- `analysis/compute_metrics.py` `CLASS_RANGES`
- The doc tables in `HANDOFF.md` §1/§3 and `unity/UNITY_HANDOFF.md`

The CLI (`./demo_engine_cli`) and `web/engine.mjs` (`wasm/build.sh`) were rebuilt
with the new band. None of the C++ SBR tools use Advanced mode
(`sbr_sweep.cpp`, `sbr_matched_search.cpp`, `volume_only.cpp`), so **no SBR result
is affected.**

### `demo_engine_cli --static ... --split-out <prefix>`

This also writes `<prefix>_key.wav` (the key class's own output) and
`<prefix>_nonkey.wav` (everything else). The key output comes from a second
render with every non-key stem zeroed; the non-key output is the mix minus that.

This is exact for every mode. The key always passes through unducked, the
Basic/Advanced duck gain is driven by the key alone, and Resonance's per-bin mask
is built from the key's spectrum only (`ResonanceSuppressor.h::processHop`).

Checked on Construction: the key output equals the raw Dialogue stem.
- Basic: error 0.
- Advanced: max error 7e-7.
- Resonance: delayed 2048 samples by the STFT, then max error 5e-16.

The separated files carry that delay with them, so no lag search is needed.

Cost is about 0.7 s per point instead of about 0.4 s.

### `compute_metrics.py`: new functions

The old `compute_masking_margin` is kept, unchanged, so old numbers stay
reproducible.

- `SPEECH_BAND_HZ = (150.0, 8000.0)`
- `band_power_frames(signal, sr, low, high)`: 20 ms Hann frames at 50% overlap,
  power summed over the FFT bins in the band. There is no analysis filter, so
  nothing interacts with the crossover.
- `compute_separated_margin(priority_out, masker_out, sr)` returns the mean and
  worst margin. Frames count when the priority output's in-band level is within
  40 dB of its peak.

### Artifact check

Construction, Dialogue key, threshold −30 dB, ratio 4:1, max reduction 12 dB.
Advanced minus Basic background level, by band:

| Band | Advanced − Basic |
|---|---|
| 300 Hz–7 kHz (core band) | +0.02 dB |
| 150–175 Hz | +0.64 dB |
| 175–300 Hz | +0.22 dB |
| 7–8 kHz | +0.48 dB |
| 150 Hz–8 kHz overall | +0.13 dB |

The core band matches, as it should: both modes apply identical gain there.
Advanced ducks less only at the edges, where the LR4 crossover is in transition
(about half-power at the crossover frequency). That is real behaviour of the
processing, not a measurement artifact.

Margin gain over 150 Hz–8 kHz is +1.36 dB for Basic and +1.20 dB for Advanced;
the old artifact was about 0.9 dB. Resonance at its defaults gave +0.09 dB.

The scratch renders for this check were in the previous session's scratchpad
and are gone. Re-create them with the commands in `--static` usage if needed.

## Decisions still open

Ask the author. The recommended defaults are in bold.

1. **SII tables.** The ANSI S3.5-1997 (R2017) band-importance function, the
   standard speech spectrum, the reference internal noise and the constants must
   come from the standard itself or from a reference implementation. **Do not type
   them from memory.** Ask the author for the PDF (UIC library access). Verify the
   implementation against the standard's worked example before using it.
2. **Calibration** (SII needs dB SPL). Either **0 dBFS = 100 dB SPL**, already used
   by `compute_partial_loudness.py`, or setting each scene's unprocessed mix to
   65 dBA as Johnson & Healy did.
3. **Static or Extended SII.** Recommended: **Extended SII** (Rhebergen & Versfeld
   2005; short windows, for fluctuating maskers) as the main value, with static
   SII alongside for comparison with Johnson & Healy. Static SII on long-term
   spectra blurs exactly what ducking does.
4. **Partial-loudness metric.** Keep it in the sweeps, or **let SII replace it**?
   It is the slowest part of each point, and SII covers band masking with
   upward spread, which the partial-loudness proxy lacks.
5. **SII for hearing-impaired listeners.** **Defer.** Later options: Bisgaard
   N-series audiograms, or Johnson & Healy's mean listener (34 dB HL PTA).

## Remaining work, in order

### 1. Wire the separated margin into the sweep scripts

All three render through the CLI, score one temp WAV, and call
`compute_masking_margin(dry, mix, sr, *CLASS_RANGES[key], ...)`:

- `run_lhs_sweep.py`
  - `render()` (around line 108): add `--split-out`.
  - `evaluate_condition()` (around line 123): use the `_key`/`_nonkey` files.
  - The unprocessed baseline needs `--split-out` too (`--mode unprocessed`
    supports it).
- `run_resonance_sweep.py`: the same changes. Its comment at line ~153 about the
  2048-sample latency no longer applies to margin, because separated renders are
  already aligned. ESTOI still needs its own alignment.
- `compute_folder_export_metrics.py`: reads the 9 mixes in `UnMasking Exports/`
  (browser/DAW exports, mix only). Separated margin needs those settings
  **re-rendered through the CLI**; parse the settings from the folder names or
  `settings` text the script already reads (`parse_settings`).

Write new column names (for example `speech_margin_gain_db`) rather than reusing
`mean_margin_gain_db`, so old and new CSVs can't be confused. Write to new CSV
files; don't overwrite the old ones.

Compression modes: `sbr_matched_search --eval ... --split-dir` already writes
separated Dialogue/background WAVs for basic/comp/combined. Point
`sbr_margin_crossover.py::true_margin` at `compute_separated_margin`.

### 2. Implement SII

Once decision 1 is resolved:

- New module, for example `analysis/sii.py`.
- One-third-octave procedure, 18 bands, 160 Hz–8 kHz.
- Inputs per band:
  - speech spectrum level, from `_key.wav`;
  - noise spectrum level, from `_nonkey.wav`;
  - hearing threshold, 0 dB HL for now.
- Include self-masking, upward spread of masking, internal noise, the level
  distortion factor and band importance.
- Extended SII: the same calculation per short window (frame length per
  Rhebergen & Versfeld), averaged over windows where speech is present.
- Add it to the sweeps and to `volume_only.cpp`'s comparison. For C++ tools,
  write split WAVs and score them in Python, or port the module.
- Add a unit test against the standard's worked example.

### 3. Time one sweep point end to end

Render plus ESTOI plus SII, plus partial loudness if kept. Then estimate the full
run and tell the author before starting it.

### 4. Re-run (Construction and Public Transit only)

| Run | Old output | Points |
|---|---|---|
| LHS sweep, Construction, Basic + Advanced | `lhs_sweep_results.csv` | 150 |
| LHS sweep, Public Transit | `lhs_sweep_results_publictransit_dialogue.csv` | 50 (consider 150 to match) |
| Fixed-threshold sweep | `lhs_sweep_fixed_thr35.csv` | 100 |
| Resonance sweep | `resonance_sweep_results_construction_dialogue.csv` | 100 |
| 9-run parameter sweep (re-rendered) | `folder_export_results.csv` | 9 |
| Margin crossover / compression modes | `sbr_margin_crossover` outputs | cheap |
| Volume-only comparison: add margin + SII columns | `volume_only_*.csv` | cheap |

Not re-run:
- the Restaurant LHS sweep (excluded);
- every SBR sweep and SBR-matched search (no margin, and no Advanced mode).

The old sweeps capped max reduction at 6 dB. Margin rises monotonically with
processing, so a "best margin" setting always sits at that cap. Check
whether the author wants the same bounds or bounds matching
`sbr_matched_search`'s.

### 5. Regenerate the thesis tables

These `analysis/*.tex` fragments carry the old margin and were written by hand
(there are no generator scripts). Add `write_*_tex.py` generators, following
`write_scene_spread_tex.py`, and copy the outputs into `Thesis-Draft/`.

`parameter_sweep_table_basic`, `parameter_sweep_table`, `partial_loudness_table`,
`recommended_settings_table`, `fixed_threshold_table`, `scene_generalization_table`,
`resonance_sweep_table`, `lhs_sweep_master_table`, `fixed_threshold_master_table`,
and the `listening_examples_*` tables (if the chosen settings change).

### 6. Paper passages for the author

Point the author to these; don't rewrite them. Line numbers are as of 2026-11-04.

- **l.155**: analogy table, "400 Hz–7 kHz for Dialogue".
- **l.199**: margin definition: band, padding and estimator all change.
- **l.252 area**: a new SII subsection is needed under Performance Metrics.
- **l.686**: the author's rationale for choosing 400 Hz–7 kHz ("choppy character
  ... most noticeable at low-mid frequencies"). It now contradicts the new
  band, and the author needs to restate why the band is 150 Hz–8 kHz.
- **l.735**: "400 Hz–7 kHz band (for Dialogue)".
- **Abstract and Results**: every quoted margin number, including "1–8.5 dB".

### 7. Housekeeping

- `HANDOFF.md`'s margin description (§ near line 428) still describes the padded
  estimator. Update it once the new margin is in the sweeps.
- The live web demo and `paper-audio/` listening examples were rendered with the
  old band. Re-export the Advanced examples if the paper keeps linking them.
