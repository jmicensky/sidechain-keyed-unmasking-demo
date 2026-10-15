# Unity Integration Handoff: Group Compression + Sidechain Unmasking

**Purpose:** port the demo engine's dynamic range processing (sidechain-keyed
unmasking + per-group compression + output WDRC) into an existing Unity VR
project, where **AudioMixerGroups play the role of the source-separated
stems** and each **processing scene** is one listening condition.
**Target project:** `UIC/THESIS RESEARCH/Maybe VR` (Unity 6000.4.10f1, Steam
Audio spatializer, `Main Mixer.mixer`, Listening Lab testing framework).
**Reference implementation:** `unity/Runtime/` (C#, compiles against Unity
6000.4), parity-tested against the C++ engine. See §10.
**Formal version:** `unity/unity_signal_flow.tex`.

---

## 1. The four processing scenes

| Scene | Group compression (Enhancement) | Sidechain unmasking | What it tests |
|---|---|---|---|
| **S0 Unprocessed** | off | off | baseline mix |
| **S1 Unmasked** | off | on: key = Dialogue (or Safety), Advanced mode | "make room for one priority group" |
| **S2 Compressed** | on: each group compressed on its own level | off | "make quiet content audible everywhere" |
| **S3 Combined** | on | on | both, in series |

The output-bus WDRC stage is an independent toggle on any scene. In the C++
engine, Enhancement and Unmasking are mutually exclusive tabs, so **S3 is new**.
Its order is defined in §3.

---

## 2. The Unity constraint that drives the architecture

Unity gives C# **no way to insert DSP into an AudioMixerGroup**. Mixer effects
are either built-in (Compressor has no ratio or knee; Duck Volume is
full-band only) or native C++ plugins. The two managed hooks that exist are:

- `OnAudioFilterRead` on an **AudioSource's** GameObject. It runs per source,
  before the mixer.
- `OnAudioFilterRead` on the **AudioListener's** GameObject. It runs once per DSP
  block, **after every source and group has been mixed**.

So the processing is split in two:

1. **`StemTap`** (on every AudioSource) adds the source's samples into its
   class's accumulator. The **sum of all sources routed to a group *is* that
   group's signal**.
2. **`UnmaskingListener`** (on the XR camera) uses the listener callback as an
   **end-of-block barrier**. It runs every group detector (the sidechain key
   detector and the per-group compressor detectors) sample by sample over the
   summed block, then writes per-sample gain curves.
3. Each `StemTap` multiplies its own samples by its group's gain curve.

**Why this equals processing the group bus:** every gain here is a per-sample
scalar (or per-sample, per-band scalar), and the band splits are linear
filters. So `Σ_sources g(n)·x_i(n) = g(n)·Σ_sources x_i(n)`. Compressing each
member with the group's gain is identical to compressing the group sum. This
is the same argument behind the C++ engine's `AdvancedDuckingMode::SummedBus`.
The equivalence is verified numerically in §10 (−150 dBFS difference).

The mixer is still used for routing, fader balance, sends/reverb, and
**snapshots** (§7). It just isn't where the dynamics run.

---

## 3. Signal flow

```
 per AudioSource (StemTap, OnAudioFilterRead)            per block (UnmaskingListener)
 ───────────────────────────────────────────            ──────────────────────────────
 x  ──► capture block ──► mono (L+R)/2 ─────────────────► Σ into group c accumulator
   │                  └─► [Multiband] 6-band LR4 split ─► Σ per band into group c
   │                                                        │
   │                                                        ▼  (after all sources)
   │                                       ┌── Enhancement detector per group c ──► g_enh[c][n]
   │                                       └── Key detector on group k (pre/post enh) ──► g_duck[n]
   │                                                           (clamped by Max Reduction)
   ▼                                                        │
 render:  y = Enhancement(x, g_enh[c])  ◄───────────────────┘
          d = Duck(y, g_duck)           Basic: g·y   Advanced: below + above + g·in-range
          out = κ_c · (y · keyGain) + (1 − κ_c) · d     κ_c = 1 for the key group (30 ms ramp)
          ──► Steam Audio / mixer group ──► Master ──► Listener: output WDRC (6-band)
```

**Order in Combined (S3):** each group is first compressed on its own level,
then the non-key groups are ducked. The key group is never ducked; it receives
its own compression plus `keyGain`.

**Key-detector tap (S3 only):** `PreEnhancement` (default) keys the duck off
the key group *before* its own compression. That keeps thresholds calibrated
in the thesis sweeps valid, since those were measured on raw stems.
`PostEnhancement` keys off what the listener actually hears of the key, so
the key's makeup gain then deepens the duck. This is a research decision: pick
one and log it.

---

## 4. Latency: one DSP block, and how to spend it

The detectors can only run once every source for the block has been summed,
so the gain for block *k* exists at the end of block *k*. There are two options
(`UnmaskingListener.alignDetector`):

| Mode | Behavior | Cost |
|---|---|---|
| **Align on** (recommended) | every tapped source outputs its *previous* block, so gain and audio line up exactly | +1 block output latency on processed sources |
| **Align off** | current audio gets the previous block's gain | sidechain reacts 1 block late, so onsets overshoot |

Measured on the Construction Scene stems (Advanced, Dialogue key), with Align
off versus the sample-accurate C++ engine:

| DSP buffer | lag | peak error | RMS error |
|---|---|---|---|
| 256 | 5.3 ms | −27.4 dBFS | −69.7 dBFS |
| 512 | 10.7 ms | −27.4 dBFS | −65.0 dBFS |
| 1024 | 21.3 ms | −25.2 dBFS | −59.5 dBFS |

**Action:** Maybe VR currently has `DSP Buffer Size = 1024`
(`ProjectSettings/AudioManager.asset`). Set **Project Settings → Audio → DSP
Buffer Size = Best latency (256)** and keep **Align on**. That adds 5.3 ms of
audio latency, which is acceptable for VR, and makes the processing exact.

---

## 5. Spatialization (Steam Audio)

Each source is HRTF-spatialized by Steam Audio *before* the mixer. Whether
the tap runs before or after the spatializer is set per source by
`AudioSource.spatializePostEffects`:

- **`false` (Unity default): spatializer → tap.** The detector hears the
  binaural, distance-attenuated signal, i.e. what reaches the ears. This is
  the closer analog to a hearing aid separating what its microphones pick up.
  A distant talker ducks less than a near one. **Recommended.**
- **`true`: tap → spatializer.** The detector hears the dry source, so
  thresholds are independent of listener position.

Pick one project-wide and record it. Either way the gain is a slowly varying
scalar, so applying it before or after the (linear) HRTF gives the same
result to within the attack time. Verify once in-editor: log
`UnmaskingBus.TakeTelemetry()` while walking toward and away from a talker,
and confirm the duck depth follows distance as expected.

---

## 6. Mapping Maybe VR's mixer to the five classes

`Main Mixer.mixer` today:

```
Master
├── REVERB              (Receive ← sends; Steam Audio Reverb)
├── Indoor - SUM        (Send → REVERB)
│   ├── Dialogue
│   ├── Music           (Send → REVERB)
│   ├── Background Noise
│   └── Randomized FX
└── OutdoorSurrounds
```

`StemTapInstaller` mapping (map **leaf** groups that sources output to directly):

| Mixer group | SoundClass | Notes |
|---|---|---|
| Dialogue | Dialogue | `TestZone.dialogueSources` |
| Music | Music | |
| Background Noise | BackgroundNoise | |
| OutdoorSurrounds | BackgroundNoise | several groups may share a class |
| Randomized FX | SafetyAlerts (or Other) | the embedded cues participants must detect |
| REVERB | *(unmapped)* | see caveat below |

Caveats:
- **Reverb returns are not processed.** Sends carry the *processed* signal,
  because taps run pre-mixer, so a ducked group's reverb input is ducked too.
  But the REVERB group's tail is a shared return that no tap can reach.
  If reverb masking matters, give each class its own reverb, or accept and
  document it.
- **The key choice is the experimental lever.** The Listening Lab measures
  both dialogue comprehension *and* cue detection. Keying on Dialogue helps
  the first and ducks the cues; keying on SafetyAlerts does the opposite.
  Scenes S1/S3 should probably exist in both key variants.

---

## 7. Scenes = `ProcessingScene` assets (+ optional mixer snapshot)

A "mixer scene" can't be a plain AudioMixerSnapshot, because snapshots only
interpolate mixer parameters and the dynamics live in C#. Each condition is
therefore a **`ProcessingScene` ScriptableObject** (*Create → Listening Lab →
Processing Scene*) holding:

- `sceneId`: written to the trial log, so keep it stable once data collection starts.
- `settings`: every parameter in §9.
- `mixerSnapshot` (optional): the static fader balance for that condition,
  e.g. **loudness matching**. Processed scenes come out quieter (the reference
  renders measured RMS S0 −29.4, S1 −30.4, S2 −30.2, S3 −30.9 dBFS). Match
  integrated loudness across conditions with a snapshot, or the comparison
  is confounded by level.

`ProcessingSceneController.Apply(scene)` hands the settings to the audio
thread at the next block boundary and transitions the snapshot. **Switch scenes
between trials, in silence.** Stage on/off changes crossfade over 25–30 ms,
but changing duck mode, band mode or key class mid-sound restarts filter
state. Inspector edits to the active scene apply live in Play mode, which is
useful for tuning.

Suggested assets: `PS_S0_Unprocessed`, `PS_S1_Unmasked_Dialogue`,
`PS_S1_Unmasked_Safety`, `PS_S2_Compressed`, `PS_S3_Combined_Dialogue`,
`PS_S3_Combined_Safety`.

---

## 8. Integration steps (Maybe VR)

1. Copy `unity/Runtime/` into `Assets/Scripts/SidechainUnmasking/`. Do **not**
   copy `unity/tests/`; its attribute stubs collide with UnityEngine.
2. **Audio settings:** DSP Buffer Size → Best latency (§4). Leave sample rate
   at system default; the bus reads `AudioSettings.outputSampleRate` and
   rebuilds itself on audio configuration changes.
3. **XR camera** (the GameObject with the AudioListener): add
   `UnmaskingListener` with Align Detector on.
4. **Scene bootstrap object:** add `StemTapInstaller` with the §6 mapping, and
   `ProcessingSceneController` with the listener reference, the scene list,
   and the initial scene `PS_S0_Unprocessed`. The installer adds a `StemTap` to
   every AudioSource routed to a mapped group, including inactive ones like the
   `TestZone` dialogue/cue sources. Call `InstallAll()` again for sources
   spawned later.
5. **Decide `spatializePostEffects`** (§5) and set it consistently on tapped
   sources. `QuadSpeakerRig` and `TestZone` sources currently use the default.
6. **Testing framework hook-up** (small edits to existing scripts):
   - `TestingSequence`: add `public ProcessingScene processingScene;` (or
     assign conditions per participant for counterbalancing in
     `ParticipantSession`).
   - `TestingSequenceController.RunSequence`: before scheduling the
     conversation, call `sceneController.Apply(condition)` and
     `sceneController.TakeTelemetry()` (which resets the counters). After the
     conversation, call `TakeTelemetry()` again and store it on the trial.
   - `TrialRecord`: add `processingSceneId`, `duckEngagedFraction`,
     `meanDuckGainDb`, `minDuckGainDb`, and `minEnhancementGainDb[5]`;
     `TrialLogger` writes them.
7. **Calibrate** (§9) and set the loudness-match snapshots (§7).
8. Smoke test: with S0 active, output must be bit-identical to having no taps
   at all, apart from the one-block delay when aligned. Then step through
   S1–S3, watching the telemetry.

---

## 9. Parameters and calibration

All thresholds are **absolute dBFS at the detector**. VR playback levels
(source volume, distance rolloff, HRTF) will not match the stem levels the
thesis sweeps used, so **re-calibrate thresholds in-scene** and do not copy them.
The sweep found threshold dominates everything else (r = −0.929 with margin
gain; ratio/knee/attack/release |r| < 0.19). The useful calibration target is
therefore the **engaged fraction**, the share of time the duck is active:

| Sweep tier (Advanced, Construction) | Thr | Ratio | Knee | Atk | Rel | Engaged |
|---|---|---|---|---|---|---|
| Aggressive | −58.6 | 7.4 | 5.9 | 3.2 | 215 | 68.8 % |
| Balanced | −40.3 | 5.7 | 22.9 | 16.9 | 347 | 29.9 % |
| Conservative | −28.8 | 7.7 | 9.4 | 8.5 | 384 | 1.9 % |

**Procedure:** play a representative trial in S1, read
`Telemetry.EngagedFraction`, and move the threshold until it hits the
intended tier's engagement. Keep ratio/knee/times from the tier.

Parameter defaults (`ProcessingSettings`, matching the C++ engine):

| Stage | Parameter | Default | Range |
|---|---|---|---|
| Unmask | threshold / ratio / knee | −30 dB / 4:1 / 6 dB | −80–0 / 1–10 / 0–24 |
| | attack / release | 5 / 120 ms | 1–50 / 20–3000 |
| | max reduction | 6 dB | 0–40 |
| | key gain (`safetyGainDb`) | 0 dB | ±12 |
| | duck mode | Advanced | Basic / Advanced |
| | Advanced range by key | Dialogue 175–7000 Hz, Music 75–12k, BG 60–2000, Safety 300–2000, Other 700–12k | `DspTables.UnmaskRanges` |
| Enhancement (per group) | thr / ratio / knee / atk / rel / makeup | −30 / 4 / 6 / 5 / 120 / 0 | multiband clamps atk 1–50, rel 30–3000 |
| | band mode | single-band | single / 6-band (100, 299, 894, 2675, 8000 Hz) |
| Output WDRC | thr / ratio / knee / atk / rel / makeup | −24 / 2 / 6 / 5 / 80 / 0 | |

The per-band detectors (multiband Enhancement, WDRC) add +10 dB level
compensation, exactly as in C++. The C++ `CompressorParams` defaults are
still placeholders, not final thesis values.

---

## 10. Reference implementation and verification

`unity/Runtime/`:

| File | Role |
|---|---|
| `Dsp/DspPrimitives.cs` | 1:1 ports of EnvelopeFollower, GainComputer, Smoother, Biquad, LR4Split, CrossoverFilterbank, tables |
| `Dsp/WdrcCompressor.cs` | 1:1 port of the 6-band WDRC |
| `ProcessingSettings.cs` | enums + all scene parameters |
| `UnmaskingBus.cs` | group accumulators, block barrier, detectors, telemetry, output WDRC |
| `SourceTapProcessor.cs` | per-source capture / band split / render |
| `StemTap.cs`, `UnmaskingListener.cs` | the two `OnAudioFilterRead` MonoBehaviours |
| `ProcessingScene.cs`, `ProcessingSceneController.cs`, `StemTapInstaller.cs` | scene assets, switching, group→class mapping |

The DSP core has no UnityEngine dependency, so it can be tested outside Unity.
`unity/tests/run_parity_test.sh` renders the real Construction Scene stems
(30 s) through the C++ `Engine` and through the C# port driven exactly as
Unity drives it. Results:

- Basic, Advanced, Advanced + key gain, single-band Enhancement, Unprocessed:
  max difference **≤ −144 dBFS** (float rounding). Multiband Enhancement:
  −113.5 dBFS (the residue of C++'s internal 25 ms startup fade). Output WDRC:
  −142.6 dBFS after 0.5 s (startup fade runs one block out of phase when
  aligned).
- **Group equivalence:** N sources per group vs. one pre-summed source,
  ≤ −147 dBFS in every scene.
- S3 with Enhancement off equals S1; S3 with Unmask off equals S2 (exact).
- Throughput: 7–60× realtime for 5 stereo sources under Mono on a Mac. This is
  indicative only; profile on the headset.

Compiled against Unity 6000.4's managed DLLs with no warnings except the usual
CS0649 for serialized fields. **Not yet run inside the Unity editor**, so step
8's smoke test is the first in-engine check.

---

## 11. Not ported / known limits

- **Resonance duck mode** (STFT, 2048-point, 43 ms latency) is not ported. In
  this architecture each tapped source would need its own STFT, with the mask
  built at the barrier from the key group's spectrum. That's feasible, but
  CPU-heavy in managed code on Quest. Port it last, or move it to a native
  plugin.
- **Advanced per-channel thresholds** (`AdvancedDuckingMode::PerChannel`) are not
  ported; adding them is a small change (one GainComputer per class at the barrier).
- **Mute/solo** are left to the mixer. If you mute the key group at the mixer,
  its taps still feed the detector, so also switch to a scene with unmasking off.
- **Unmapped audio** (reverb returns, UI sounds) bypasses processing.
- **Alternative architecture:** a Unity Native Audio Plugin wrapping
  `include/*.h` directly as mixer effects, with the key group routed via
  Send → sidechain target. That gives true group-level DSP with no block lag,
  at the cost of per-platform native builds (Quest arm64 `.so`, Windows `.dll`,
  macOS `.bundle`). Worth it only if managed CPU cost or Resonance mode
  requires it.

## 12. Open decisions for the experimenter

1. Key class(es) per scene: Dialogue, Safety, or both variants (§6).
2. `PreEnhancement` vs `PostEnhancement` key tap in S3 (§3).
3. `spatializePostEffects`: ear-level vs source-level detection (§5).
4. Calibration tier, set via the engaged fraction (§9).
5. Loudness-matching method across scenes (§7).
6. Whether output WDRC is part of any condition, or held constant.
