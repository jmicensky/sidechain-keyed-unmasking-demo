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

## Decisions (resolved with the author, 2026-11-04)

1. **SII tables: the sii.to reference code.** Use the MATLAB implementation by
   Hannes Muesch (`SII.M` in http://sii.to/siito/assets/siimatlab.zip). Its
   one-third-octave tables (band centres, internal noise, standard speech
   spectrum, band importance, constants) agree value for value with Pavlovic's C
   kit (`SOURCES/SII.C` in http://sii.to/siito/assets/DevelopmentKit.zip).
   Verified: a port of `SII.M` gives 0.4454 on the kit's `TO.TST` (expected 0.445)
   and 0.4382 on `TO_1.TST` (expected 0.438). The expected values are in the kit's
   `readme.doc`. Use those two files as the unit test. Terms (sii.to/programs.html):
   free to use except in commercially distributed software. The thesis use is fine,
   but credit Muesch and Pavlovic in the module header. Port the values; don't
   vendor their files.
2. **Calibration: 0 dBFS = 100 dB SPL**, the same as `compute_partial_loudness.py`.
3. **Extended SII as the main value**, with static SII alongside. Source:
   Rhebergen & Versfeld (2005), JASA 117(4) 2181–2192, doi:10.1121/1.1861713. The
   PDF is `Documents/2181_1_online.pdf` (not in git). The method, pp. 2182–2183:
   - speech and noise are filtered into bands separately, with a Hilbert envelope
     per band;
   - rectangular windows that all end at the same instant;
   - an instantaneous SII every 9.4 ms, averaged over frames.

   The paper used 21 critical bands but calls one-third-octave "equally valid", so
   we use the verified one-third-octave core. It gives window lengths only at the
   ends: 35 ms at the lowest band and 9.4 ms at 8 kHz (Moore 1997 gap detection
   × 2.5). The bands in between are not tabulated. Author's decisions:
   - **Windows:** linear in log-frequency between 35 ms (160 Hz band) and 9.4 ms
     (8 kHz band). This is our assumption, and the thesis methods must say so. The
     paper's single-window alternative is 12 ms.
   - **Band importance: average speech** (Table 3, `SII.M` column 1), not the
     paper's SPIN.
   - **Silence gate:** average only over frames where Dialogue's in-band level is
     within 40 dB of its peak, the same rule as `compute_separated_margin`. The
     paper sidestepped silences by using stationary speech-shaped noise as speech;
     our Dialogue has real pauses.
4. **Partial loudness is dropped**; SII replaces it. `run_lhs_sweep.py` imports
   mosqito at the top level, and `/usr/bin/python3` (the Python with
   numpy/scipy/soundfile) doesn't have mosqito installed, so remove that import.
5. **SII for hearing-impaired listeners: deferred.**
6. **Sweep bounds:** the old LHS ranges, plus **max reduction as a sixth sampled
   parameter, 3–12 dB**, in the LHS sweeps only. The fixed-threshold sweep and the
   Resonance sweep keep their current parameters and don't sample it.
7. **LHS point counts: 100 per scene** (Construction and Public Transit). The author cut this from 150 to save run time.

## Remaining work, in order

### 1. Wire the separated margin into the sweep scripts

**Status (2026-11-04): done.** Run every script with `/usr/bin/python3`; pystoi
0.4.1 was installed into its user site-packages this session.

- `split_metrics.py`: a shared `render_split` (CLI `--split-out`) and
  `score_split`. It returns speech margin, worst margin, ESTOI, Extended SII and
  static SII. `gain_columns` writes each metric both absolute and as `_gain`
  over unprocessed.
- `run_lhs_sweep.py`: rewritten.
  - Samples 6 dimensions (max reduction 3–12 dB).
  - With `--fixed-threshold`, max reduction stays at 6 dB.
  - Writes `lhs_speech_<scene>_dialogue[_thr35].csv`.
  - Mosqito and partial loudness are gone.
- `run_resonance_sweep.py`: rewritten in the same way. Writes
  `resonance_speech_<scene>_dialogue.csv`. ESTOI trims the 2048-sample delay
  from the mix (`RESONANCE_LATENCY_SAMPLES`). Margin and SII use the aligned
  split files.
- `rerender_folder_exports.py`: re-renders the 9 `UnMasking Exports`
  settings (max reduction 24 dB, from `settings.txt`) as Basic and Advanced.
  Writes `folder_export_speech.csv`.
- `sbr_speech_crossover.py`: replaces `sbr_margin_crossover.py` (kept) for the
  new metrics.
  - Step 1 measures SBR for the new LHS Basic points.
  - Step 2 scores the SBR-matched settings (default
    `sbr_matched_settings.tsv`; pass `--settings` for the 2-scene best rows).
  - Run it after the LHS sweeps.
- `volume_only.cpp --split-dir <dir>`: writes per-condition Dialogue/background
  WAVs at real levels. `score_volume_only_speech.py` turns them into
  `volume_only_speech.csv`. With the flag, `volume_only_*.csv` is unchanged
  (checked with git).

Original plan:

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

**Status (2026-11-04): implemented** in `analysis/sii.py`. Test it with
`/usr/bin/python3 analysis/test_sii.py`. The test covers the C kit's TO/TO_1 cases,
checks that Extended and static SII agree for stationary signals, and checks that
Extended SII beats static SII for interrupted noise. Use
`compute_sii(key, nonkey, sr)` on the split renders; it returns `esii` and
`static_sii`. It takes about 6 s per pair of 81 s stereo stems.

Finding: on real Dialogue, Extended SII is much lower than static SII.
Unprocessed Construction gives 0.185 vs 0.379. Basic at −30 dB, 4:1, 6 dB max
reduction gives 0.215 vs 0.412, so the changes are similar (+0.030 vs +0.033).
The gap remains at any gate depth (a 15 dB gate gives 0.296 vs 0.445), so the
gate isn't the cause. It comes from averaging per-frame dB levels of
fluctuating speech, against the power mean that static SII uses. Rhebergen &
Versfeld's 0.35 threshold criterion was fitted with stationary speech-shaped
"speech", so absolute Extended SII values here are not comparable to it. Report
changes, not absolute values.

Original plan:

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

**Status (2026-11-04): all runs done** (outputs in `analysis/`, not yet committed):
- `lhs_speech_construction_dialogue.csv`, `lhs_speech_publictransit_dialogue.csv`:
  100 points each, Basic and Advanced. Both use seed 42, so the two scenes share
  the same 100 settings: paired by design.
- `lhs_speech_construction_dialogue_thr35.csv`: fixed threshold −35 dB, max
  reduction 6 dB.
- `resonance_speech_construction_dialogue.csv`: 100 points.
- `folder_export_speech.csv`: the 9 re-rendered folder exports.
- `volume_only_speech.csv`: joins to `volume_only_results.csv` on scene,
  condition and target.
- `sbr_speech_crossover{,_summary}.*` and `sbr_speech_winners.tsv`: 3-scene
  settings. `sbr_speech_winners_2scene.tsv` and `sbr_speech_crossover_2scene_summary.txt`:
  2-scene settings.

Open questions for the author:
- **SII at rendered vs loudness-matched levels.** Group compression and Combined
  lower Dialogue by 7–10 dB overall, since there is no makeup gain.
- **Which SBR-matched settings set to report.** The 3-scene and 2-scene sets
  reach the same SBR (within 0.45 dB). They give the same margin and SII for
  basic_HI, comp_HI and comp_NH, but differ a lot for basic_NH and Combined.

Original plan:

| Run | Old output | Points |
|---|---|---|
| LHS sweep, Construction, Basic + Advanced | `lhs_sweep_results.csv` | 100 (was 150) |
| LHS sweep, Public Transit | `lhs_sweep_results_publictransit_dialogue.csv` | 100 (was 50) |
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

---

## Restructure (decided with the author, 2026-11-05)

The paper is reorganised around two families of processing on the separated
stems, with Dialogue as the example priority channel. Resonance is dropped
(too hard to justify). Safety/situational-awareness trade-offs move to a
"suggested implementation" section of two-piece priority designs, which is
prose and signal-flow proposals only: no sweeps.

Processes to measure (Construction + Public Transit only):

| Family | Process | Engine | Notes |
|---|---|---|---|
| Group compression | Single-band | Enhancement, Single-band (`comp` in sbr_matched_search) | main table |
| Group compression | Multiband | Enhancement, Multiband (6-band WdrcCompressor per stem) | small side table; per-stem threshold/ratio shared across all 6 bands, shared knee/attack/release (same dims as single-band) |
| Sidechain | Summed-bus, full band | Basic | main table |
| Sidechain | Individual compressors | NEW full-band per-channel: Basic with per-stem threshold/ratio, shared detector and knee/attack/release/max reduction (search tool only; the engine's Per-channel is band-limited) | main table |
| Preview | Group + sidechain | Combined | measured, reported only in the suggested-implementation section |

Advanced Summed-bus (frequency-specific, 150 Hz-8 kHz) is not measured
(author, 2026-11-05).

Data per process:
- 100-point LHS per scene: reachable SBR range and metric distributions.
- Johnson & Healy matched settings (HI +5.1, NH +12.2 dB), searched on the
  two scenes.
- Volume-only baseline at the matched SBRs (kept). Fixed-threshold sweep and
  the 9 folder exports are not rerun.

Metrics next to SBR: Extended SII (+ static SII), ESTOI, background (non-
dialogue bus) level change. Speech-band margin moves out of the main tables
(r = 0.99 with SBR gain). All scoring on output loudness-matched to the
unprocessed mix (group compression has no makeup gain).

Phasing (author, 2026-11-05): run now = Group single-band, Basic, full-band
Per-channel. Later = Group multiband, Combined preview. The existing 2-scene
matched settings for basic and comp (sbr_matched_2scene_*_best.tsv) are reused,
since loudness matching does not change SBR; only Per-channel needs a new
matched search.
LHS point counts for this phase (author, 2026-11-05): Basic 50 per scene;
Group single-band and full-band Per-channel 75 per scene (more parameters).

### Status (2026-11-05)
- `sbr_matched_search.cpp`: new `perchannel` mode (full-band, per-stem
  threshold/ratio on the shared Dialogue detector; knee/attack/release/max
  reduction shared), via a `DuckGains` helper used by render() and
  renderSplit(). Check: with all four stems set to a Basic law it reproduces
  Basic sample-for-sample (max diff 0.0) in both scenes; `--validate 1` still
  matches Engine for basic and comp (diff 0.00000 dB).
- `sbr_matched_2scene_perchannel{.csv,_best.tsv}`: matched search, `--lhs 1500`.
  HI: Construction +5.25, Public Transit +4.96 dB. NH: +12.18 / +12.21 dB.
- `process_metrics.py`: loudness match (BS.1770-4) to the unprocessed mix, then
  background/Dialogue level change, Extended/static SII, ESTOI, margin.
  ~19 s and ~2.6 GB per render.
- `run_process_lhs.py`: LHS per process (basic 50, perchannel 75, comp 75),
  seed 42, same points in both scenes; writes `process_lhs_<process>.csv`.
- `score_matched_processes.py`: matched settings plus volume-only conditions
  (static gain on the unprocessed background sum, same gated SBR) in one table,
  `process_matched.csv`. Replaces volume_only.cpp for this phase: that tool
  matches J&H pooled SBR and has no per-channel mode.
- Runs complete (12:11-13:01): `process_lhs_{basic,perchannel,comp}.csv`,
  `process_matched.csv`, `process_lhs_summary.txt` (summarize_process_lhs.py).
  Caveat for the "potential SBR change" claim: LHS ceilings depend on the
  sampled ranges (sidechain max reduction 3-12 dB; comp ranges from the search
  tool), so they compare processes under those ranges, not absolute limits.
- Group multiband (2026-11-06): `compmb` mode in sbr_matched_search (a
  `GroupComp` helper: one WdrcCompressor per group, the group's one law in all
  6 bands, set up as Engine::syncEnhancementChannel() does). `--validate 1`
  matches Engine's Enhancement Multiband output exactly (diff 0.00000 dB, 3
  settings x 2 scenes). Same dims and LHS points as `comp` (paired). Note the
  search's Dialogue-compression constraint (1.5-6 dB mean GR) reads the
  deepest band's GR for multiband. Outputs: `sbr_matched_2scene_compmb*`,
  `process_lhs_compmb.csv`; `process_matched.csv` and
  `process_lhs_summary.txt` regenerated with it. Combined is not being run.

## WDRC chain test (decided with the author, 2026-11-08)

Question: with the hearing aid's WDRC fitting held fixed (it is fitted to a
dynamic range, not an SBR), what mix reaches the ear with class-based
processing before it vs WDRC alone? Chains: WDRC alone; Basic, Per-channel,
group single-band and multiband (existing matched settings, not re-tuned);
static gain (same SBR into the WDRC as each process, and at the J&H targets).
Fittings (generic stand-ins for a prescription): -50 dBFS (~50 dB SPL), 2.5:1,
6 dB knee, attack 5 ms, release 80 ms (fast) and 1000 ms (slow). Unity already
has a 1:1 port of the WDRC (`Dsp/WdrcCompressor.cs`), so the fitting carries
over to the VR test; Unity export deferred.

- `sbr_matched_search --eval --wdrc thr,ratio,knee,attack,release`: SplitWdrc
  applies the WDRC's per-band gains (from the real mix) to Dialogue and the
  background separately; adds sbr_out_db, WDRC mean gain reduction (all /
  Dialogue-active samples), output range (99th-30th pct of 125 ms mix levels)
  and Dialogue range at the output. `--validate-wdrc 1`: split output vs
  WdrcCompressor on the mix, max difference ~148 dB below peak, all 8 cases.
- New `static` mode (one dim, bg_gain_db) for the volume-only chains.
- `score_wdrc_chains.py` -> `wdrc_chains.csv`.
- Validation already shows the expected WDRC effect: fast fit takes
  unprocessed Construction from +1.31 to -2.18 dB SBR at the ear, Basic HI from
  +7.85 to +2.60 dB; slow fit loses less (-1.20, +4.06). Mean WDRC gain
  reduction is ~10-14 dB and output range ~4-6 dB: the generic fit is strong.
- Run complete (2026-11-08, 19:12-19:18): `wdrc_chains.csv`, 76 conditions.
  Every chain loses SBR through the WDRC, more at higher input SBR and with the
  fast fit (NH chains: about -5.4 to -6.8 dB fast, -3.1 to -4.6 dB slow; WDRC
  alone on Public Transit, input -4.5 dB, loses ~0). Group compression loses
  the least and leaves the WDRC the least gain reduction. Sidechain keeps its
  Extended SII and background-level advantage over a static gain after the
  WDRC. Output dynamic range is not identical across chains under the same
  fitting (about 3.4-9.6 dB): report it next to the results.
- Fitting grid (2026-11-08): `score_wdrc_grid.py` -> `wdrc_grid.csv`,
  `wdrc_grid_summary.txt`. Threshold -60/-50/-40/-30 x ratio 1.5/2.5/3.5 x
  fast/slow, chains WDRC alone, Basic, comp and their static matches;
  renderer-only measures (no SII/ESTOI). `--eval --wdrc` now also reports
  in_range_db (mix range into the WDRC). SBR lost through the WDRC follows
  dynamic-range reduction (r 0.90-1.00 in every chain except WDRC alone on
  Public Transit, which loses ~0) and not mean gain reduction (r 0.2-0.5
  overall). Threshold moves mean GR by ~15 dB but SBR loss by < 1 dB; ratio and
  speed drive both loss and range reduction. Group compression loses less SBR
  than its static match in 96/96 fittings (mean 1.0 dB, 1.3 dB at 3.5:1);
  Basic only slightly (mean 0.2 dB, 62/96).
