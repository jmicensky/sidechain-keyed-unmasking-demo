// Main-thread glue: decode the whole-blend reference + 5 stem WAVs, draw
// waveform overviews for all six, ship the stems to the AudioWorklet, and
// wire up the control UI (unmask, key select, mode, mute/solo per channel,
// play/pause for both the reference and the decomposed mix — mutually
// exclusive so only one is ever audible at a time).

const CLASS_NAMES = ['Dialogue', 'Music', 'Background Noise', 'Safety Alerts', 'Other'];

// Mirrors Types.h's kUnmaskFrequencyRanges (same order as CLASS_NAMES).
// Advanced duck mode only ducks other channels within whichever range
// matches the currently-selected key channel - this table drives the
// "Advanced-mode ducked band" badge and the gain-viz notch position, kept
// in sync with the C++ side by hand since it's a fixed, rarely-changed table.
const UNMASK_CHANNEL_RANGES = [
  { lowHz: 400, highHz: 7000 },   // Dialogue
  { lowHz: 75, highHz: 12000 },   // Music
  { lowHz: 60, highHz: 2000 },    // Background Noise
  { lowHz: 300, highHz: 2000 },   // Safety Alerts
  { lowHz: 700, highHz: 12000 },  // Other
];

// Each scene's stems live in their own folder with their own file-naming
// convention (the two scenes on disk don't match each other), so every
// scene lists its own filenames explicitly rather than assuming a pattern.
const SCENES = {
  construction: {
    label: 'Construction',
    dir: '../Construction Scene/',
    stems: ['Dialogue.wav', 'MUSIC.wav', 'BKG.wav', 'SAFETY.wav', 'OTHER.wav'],
    blend: 'WHOLE BLEND.wav',
  },
  publicTransit: {
    label: 'Public Transit',
    dir: '../PublicTransit Scene/',
    stems: [
      'Dialogue_PublicTransit_01.wav',
      'MUSIC_publictransit_01.wav',
      'BKG_PublicTransit_01.wav',
      'SAFETY_publictransit_01.wav',
      'OTHER_publictransit_01.wav',
    ],
    blend: 'WholeBlend_publictransit.wav',
  },
};

let audioCtx = null;
let engineNode = null;
let workletReady = null; // Promise, resolves once the AudioWorklet's WASM module has loaded
let numFrames = 0;
let sampleRate = 48000;
let decomposedRunning = false;
let blendBlobUrl = null; // revoked and replaced on every scene load

const $ = (id) => document.getElementById(id);
const statusEl = $('status');
const timeEl = $('time');
const refTimeEl = $('refTime');
const refAudio = $('refAudio');

function fmtTime(seconds) {
  const m = Math.floor(seconds / 60);
  const s = (seconds - m * 60).toFixed(1);
  return `${m}:${s.padStart(4, '0')}`;
}

function downmixToMono(audioBuffer) {
  const n = audioBuffer.length;
  const out = new Float32Array(n);
  const chans = audioBuffer.numberOfChannels;
  for (let c = 0; c < chans; c++) {
    const data = audioBuffer.getChannelData(c);
    for (let i = 0; i < n; i++) out[i] += data[i] / chans;
  }
  return out;
}

// True stereo pair for the engine's own processing (mono sources get
// duplicated to both channels, matching the CLI harness's behavior).
function extractStereo(audioBuffer) {
  const left = audioBuffer.getChannelData(0).slice();
  const right = audioBuffer.numberOfChannels > 1 ? audioBuffer.getChannelData(1).slice() : left.slice();
  return { left, right };
}

async function decodeStem(ctx, dir, filename) {
  const resp = await fetch(dir + encodeURIComponent(filename));
  if (!resp.ok) throw new Error(`Failed to fetch ${filename}: ${resp.status}`);
  const arrayBuf = await resp.arrayBuffer();
  const audioBuf = await ctx.decodeAudioData(arrayBuf);
  return { mono: downmixToMono(audioBuf), ...extractStereo(audioBuf) };
}

// ---------------------------------------------------------------------
// Waveform rendering: min/max peak-per-pixel overview, drawn once per
// loaded file. Cheap enough for ~4M-sample buffers (single pass).
// ---------------------------------------------------------------------
function drawWaveform(canvas, data, color) {
  const dpr = window.devicePixelRatio || 1;
  const cssWidth = canvas.clientWidth;
  const cssHeight = canvas.clientHeight;
  canvas.width = Math.max(1, Math.round(cssWidth * dpr));
  canvas.height = Math.max(1, Math.round(cssHeight * dpr));
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssWidth, cssHeight);

  const w = cssWidth;
  const h = cssHeight;
  const mid = h / 2;
  const samplesPerPixel = Math.max(1, Math.floor(data.length / w));
  ctx.fillStyle = color;
  for (let x = 0; x < w; x++) {
    const start = x * samplesPerPixel;
    const end = Math.min(data.length, start + samplesPerPixel);
    let min = 1, max = -1;
    for (let i = start; i < end; i++) {
      const v = data[i];
      if (v < min) min = v;
      if (v > max) max = v;
    }
    if (min > max) { min = 0; max = 0; }
    const y1 = mid - max * mid;
    const y2 = mid - min * mid;
    ctx.fillRect(x, y1, 1, Math.max(1, y2 - y1));
  }
}

function setPlayheadFraction(divId, fraction) {
  const el = $(divId);
  if (el) el.style.left = `${Math.min(1, Math.max(0, fraction)) * 100}%`;
}

// ---------------------------------------------------------------------
// Gain-reduction visualization: an EQ-style view of the sidechain gain
// currently being applied.
//   Mode 0 (Basic)     - ducks the whole spectrum: one horizontal line at
//                        the current dB level.
//   Mode 1 (Advanced)  - ducks only within the active key channel's own
//                        frequency range (see UNMASK_CHANNEL_RANGES above,
//                        mirrored from Types.h kUnmaskFrequencyRanges): flat
//                        at 0 dB outside that band, dips only within it. The
//                        band moves when the Key Channel selection changes.
//   Mode 2 (Resonance) - ducks several independently-moving frequencies
//                        (the key's loudest, mutually-separated spectral
//                        peaks): flat baseline with one notch dip per peak
//                        that sweeps to wherever the engine detected it.
// ---------------------------------------------------------------------
const GAIN_DB_MIN = -30;
const GAIN_FREQ_MIN = 20;
const GAIN_FREQ_MAX = 20000;
const GAIN_VIZ_PAD_TOP = 6; // keeps the 0 dB resting line visible below the canvas edge

// The Advanced-mode ducked band tracks whichever channel is the current key
// (see UNMASK_CHANNEL_RANGES), not a fixed pair of edges.
function currentUnmaskRange() {
  const c = parseInt($('keyChannel').value, 10);
  return UNMASK_CHANNEL_RANGES[c] || UNMASK_CHANNEL_RANGES[0];
}

let currentGainDb = 0;
let currentResonance = { peaks: [] }; // [{freq, gainLinear}, ...], filled in once the engine reports real data

function gainLinearToDb(linear) {
  return 20 * Math.log10(Math.max(linear, 1e-5));
}

function drawGainReduction(canvas, gainDb, mode, resonance) {
  const dpr = window.devicePixelRatio || 1;
  const cssWidth = canvas.clientWidth;
  const cssHeight = canvas.clientHeight;
  canvas.width = Math.max(1, Math.round(cssWidth * dpr));
  canvas.height = Math.max(1, Math.round(cssHeight * dpr));
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssWidth, cssHeight);

  const freqToX = (f) => cssWidth * (Math.log10(f / GAIN_FREQ_MIN) / Math.log10(GAIN_FREQ_MAX / GAIN_FREQ_MIN));
  const dbToY = (db) => GAIN_VIZ_PAD_TOP + (cssHeight - GAIN_VIZ_PAD_TOP) * ((0 - db) / (0 - GAIN_DB_MIN));

  ctx.font = '10px -apple-system, sans-serif';

  // dB gridlines
  [0, -6, -12, -18, -24].forEach((db) => {
    const y = dbToY(db);
    ctx.strokeStyle = '#e8e8e8';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, y + 0.5);
    ctx.lineTo(cssWidth, y + 0.5);
    ctx.stroke();
    ctx.fillStyle = '#999';
    ctx.fillText(`${db}`, 3, y < 10 ? y + 11 : y - 3);
  });

  // Frequency gridlines. In Advanced mode the ducked band's own edges are
  // highlighted (and move with the Key Channel selection); other modes just
  // show fixed context lines.
  const unmaskRange = currentUnmaskRange();
  const gridFreqs = mode === 1
    ? [100, 1000, 10000, unmaskRange.lowHz, unmaskRange.highHz]
    : [100, 1000, 10000];
  gridFreqs.forEach((f) => {
    const x = freqToX(f);
    const isBandEdge = mode === 1 && (f === unmaskRange.lowHz || f === unmaskRange.highHz);
    ctx.strokeStyle = isBandEdge ? '#c4c4c4' : '#eee';
    ctx.beginPath();
    ctx.moveTo(x + 0.5, 0);
    ctx.lineTo(x + 0.5, cssHeight);
    ctx.stroke();
    ctx.fillStyle = '#999';
    const label = f >= 1000 ? `${f / 1000}k` : `${Math.round(f)}`;
    ctx.fillText(label, Math.min(cssWidth - 22, x + 3), cssHeight - 3);
  });

  const y0 = dbToY(0);
  const yG = dbToY(gainDb);
  ctx.strokeStyle = '#e03131';
  ctx.lineWidth = 2;

  if (mode === 0) {
    ctx.beginPath();
    ctx.moveTo(0, yG);
    ctx.lineTo(cssWidth, yG);
    ctx.stroke();
  } else if (mode === 1) {
    const xLow = freqToX(unmaskRange.lowHz);
    const xHigh = freqToX(unmaskRange.highHz);
    ctx.beginPath();
    ctx.moveTo(0, y0);
    ctx.lineTo(xLow, y0);
    ctx.lineTo(xLow, yG);
    ctx.lineTo(xHigh, yG);
    ctx.lineTo(xHigh, y0);
    ctx.lineTo(cssWidth, y0);
    ctx.stroke();
  } else {
    ctx.beginPath();
    ctx.moveTo(0, y0);
    ctx.lineTo(cssWidth, y0);
    ctx.stroke();
    if (resonance && resonance.peaks) {
      // Half of the live Bandwidth slider value - keeps the drawn width an
      // honest reflection of the filter Q actually being applied, so
      // experimenting with the slider is visually meaningful.
      const halfWidthOctaves = parseFloat($('resonanceBandwidth').value) / 2;
      resonance.peaks.forEach((peak, idx) => {
        drawNotchDip(ctx, freqToX, dbToY, cssWidth, y0, peak.freq, gainLinearToDb(peak.gainLinear), idx, halfWidthOctaves);
      });
    }
  }
}

// One triangular dip for Resonance mode, centered on a dynamically detected
// frequency. labelRow staggers each peak's label vertically so
// closely-spaced notches don't render overlapping text.
function drawNotchDip(ctx, freqToX, dbToY, cssWidth, y0, freqHz, gainDb, labelRow, halfWidthOctaves) {
  const xCenter = freqToX(freqHz);
  const xLow = Math.max(0, freqToX(freqHz / Math.pow(2, halfWidthOctaves)));
  const xHigh = Math.min(cssWidth, freqToX(freqHz * Math.pow(2, halfWidthOctaves)));
  const yG = dbToY(gainDb);
  ctx.beginPath();
  ctx.moveTo(xLow, y0);
  ctx.lineTo(xCenter, yG);
  ctx.lineTo(xHigh, y0);
  ctx.stroke();
  if (gainDb < -0.5) {
    ctx.fillStyle = '#e03131';
    const label = `${Math.round(freqHz)}Hz ${gainDb.toFixed(1)}dB`;
    const rowOffset = (labelRow % 2) * 11;
    ctx.fillText(label, Math.min(cssWidth - 68, Math.max(0, xCenter - 30)), Math.max(10, yG - 5 - rowOffset));
  }
}

// ---------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------

// Creates the AudioContext + AudioWorkletNode exactly once, however many
// times loadScene() is called afterward. Resolves once the worklet's WASM
// module has actually finished loading (not just once the node exists), so
// loadScene() can safely postMessage('loadStems') right after awaiting this
// on every call, not just the first.
function ensureAudioGraph() {
  if (workletReady) return workletReady;

  workletReady = (async () => {
    audioCtx = new AudioContext();
    sampleRate = audioCtx.sampleRate;
    await audioCtx.suspend(); // stay silent until the user presses Play
    await audioCtx.audioWorklet.addModule('engine-worklet.js');

    engineNode = new AudioWorkletNode(audioCtx, 'engine-processor', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
    });
    engineNode.connect(audioCtx.destination);

    const readyPromise = new Promise((resolve) => {
      engineNode.port.onmessage = (event) => {
        const msg = event.data;
        if (msg.type === 'ready') {
          resolve();
        } else if (msg.type === 'stemsLoaded') {
          const durSec = numFrames / sampleRate;
          setStatus(`Ready — ${fmtTime(durSec)} loaded`);
          timeEl.textContent = `0:00.0 / ${fmtTime(durSec)}`;
          refTimeEl.textContent = `0:00.0 / ${fmtTime(durSec)}`;
          setPlayheadFraction('playhead-blend', 0);
          for (let c = 0; c < 5; c++) setPlayheadFraction(`playhead${c}`, 0);
          $('playBtn').disabled = false;
          $('restartBtn').disabled = false;
          $('refPlayBtn').disabled = false;
        } else if (msg.type === 'playhead') {
          const sec = msg.frame / sampleRate;
          timeEl.textContent = `${fmtTime(sec)} / ${fmtTime(numFrames / sampleRate)}`;
          for (let c = 0; c < 5; c++) setPlayheadFraction(`playhead${c}`, msg.frame / numFrames);

          currentGainDb = gainLinearToDb(msg.gainLinear);
          currentResonance = msg.resonance || currentResonance;
          updateGainVisualization();
          updateWdrcMeter(msg.wdrcGainReductionDb || 0);
          if (msg.channelGains) updateChannelGainMeters(msg.channelGains);
        }
      };
    });
    await readyPromise;
  })();

  return workletReady;
}

// Fetches one scene's stems + reference blend, draws their waveforms, and
// hands the stems to the (already-running) engine. Safe to call repeatedly
// to switch scenes - pauses whatever's currently playing first.
async function loadScene(sceneKey) {
  const scene = SCENES[sceneKey];
  if (!scene) throw new Error(`Unknown scene: ${sceneKey}`);

  if (decomposedRunning && audioCtx) {
    await audioCtx.suspend();
    $('playBtn').textContent = 'Play';
    decomposedRunning = false;
  }
  if (!refAudio.paused) {
    refAudio.pause();
    $('refPlayBtn').textContent = 'Play';
  }
  $('playBtn').disabled = true;
  $('restartBtn').disabled = true;
  $('refPlayBtn').disabled = true;

  setStatus(`Loading ${scene.label} scene...`);
  await ensureAudioGraph();

  const blendResp = await fetch(scene.dir + encodeURIComponent(scene.blend));
  if (!blendResp.ok) throw new Error(`Failed to fetch ${scene.blend}: ${blendResp.status}`);
  const blendArrayBuf = await blendResp.arrayBuffer();
  const newBlendBlobUrl = URL.createObjectURL(new Blob([blendArrayBuf], { type: 'audio/wav' }));
  const blendAudioBuf = await audioCtx.decodeAudioData(blendArrayBuf.slice(0));
  const blendMono = downmixToMono(blendAudioBuf);

  const stems = await Promise.all(scene.stems.map((f) => decodeStem(audioCtx, scene.dir, f)));
  numFrames = Math.min(...stems.map((s) => s.mono.length), blendMono.length);
  const monoForWaveform = stems.map((s) => s.mono.subarray(0, numFrames));
  const channelsL = stems.map((s) => s.left.subarray(0, numFrames));
  const channelsR = stems.map((s) => s.right.subarray(0, numFrames));

  drawWaveform($('wave-blend'), blendMono.subarray(0, numFrames), '#555');
  for (let c = 0; c < 5; c++) {
    drawWaveform($(`wave${c}`), monoForWaveform[c], '#4a7fd6');
  }

  if (blendBlobUrl) URL.revokeObjectURL(blendBlobUrl);
  blendBlobUrl = newBlendBlobUrl;
  refAudio.src = blendBlobUrl;

  engineNode.port.postMessage(
    { type: 'loadStems', channelsL, channelsR, numFrames },
    [...channelsL.map((c) => c.buffer), ...channelsR.map((c) => c.buffer)]
  );

  applyAllControls();
}

function setStatus(msg) {
  statusEl.textContent = msg;
}

// Resonance mode ducks several independent frequencies with independent
// depths, so a single dB number doesn't tell the whole story the way it
// does for Basic/Advanced - show every notch's actual reduction there instead.
function updateGainVisualization() {
  const mode = parseInt($('duckMode').value, 10);
  if (mode === 2 && currentResonance.peaks.length > 0) {
    $('gainDbReadout').textContent = currentResonance.peaks
      .map((p, i) => `f${i + 1} ${gainLinearToDb(p.gainLinear).toFixed(1)} dB`)
      .join('   ');
  } else {
    $('gainDbReadout').textContent = `${currentGainDb.toFixed(1)} dB`;
  }
  drawGainReduction($('gainViz'), currentGainDb, mode, currentResonance);
}

// Keeps the "Advanced-mode ducked band" badge in sync with the Key Channel
// select - reads straight from UNMASK_CHANNEL_RANGES (not the engine), so it
// updates instantly on dropdown change even before a scene is loaded or Play
// is ever pressed.
function updateUnmaskRangeBadge() {
  const c = parseInt($('keyChannel').value, 10);
  const range = UNMASK_CHANNEL_RANGES[c] || UNMASK_CHANNEL_RANGES[0];
  const fmtHz = (hz) => (hz >= 1000 ? `${hz / 1000}kHz` : `${hz}Hz`);
  $('unmaskRangeReadout').textContent = `${fmtHz(range.lowHz)}–${fmtHz(range.highHz)}`;
  $('unmaskRangeChannelName').textContent = CLASS_NAMES[c];
}

// Keeps each channel row's Threshold/Ratio fieldset in sync with the
// Advanced-ducking-mode toggle and the Key Channel selection:
//  - enabled only in Per-channel mode, and only for non-key channels (the
//    key channel's own audio always takes the dry path in Advanced mode, so
//    its knobs would have no audible effect - see the "Key channel row"
//    design decision)
//  - while in Summed-bus mode, every knob's displayed value live-tracks the
//    shared Sidechain Compressor panel's Threshold/Ratio, so it's visually
//    obvious they're one shared compressor until Per-channel is engaged
function updateChannelKnobsState() {
  const mode = parseInt($('advancedDuckingMode').value, 10);
  const duckMode = parseInt($('duckMode').value, 10);
  const keyIdx = parseInt($('keyChannel').value, 10);
  const sharedThreshold = parseFloat($('thresholdDb').value);
  const sharedRatio = parseFloat($('ratio').value);
  // Per-channel knobs (and their gain-reduction meters) are only live when
  // Per-channel mode is selected *and* Advanced duck mode is actually
  // active - selecting Per-channel while in Basic/Resonance mode has no
  // audible effect yet, so leave the row inert until Advanced is chosen too.
  const perChannelActive = mode === 1 && duckMode === 1;
  for (let c = 0; c < 5; c++) {
    const disabled = !perChannelActive || (c === keyIdx);
    $(`channelKnobs${c}`).disabled = disabled;
    if (mode === 0) {
      $(`chThresholdDb${c}`).value = sharedThreshold;
      $(`chThresholdReadout${c}`).textContent = `${sharedThreshold} dB`;
      $(`chRatio${c}`).value = sharedRatio;
      $(`chRatioReadout${c}`).textContent = `${sharedRatio.toFixed(1)}:1`;
    }
    if (disabled) {
      $(`chGrFill${c}`).style.width = '0%';
      $(`chGrReadout${c}`).textContent = '0.0 dB';
    }
  }
}

// Updates the 5 per-channel gain-reduction meters from the engine's live
// per-channel gain report - only meaningful while Per-channel Advanced
// ducking is actually active (see updateChannelKnobsState()), so inert rows
// are skipped and just keep showing the 0.0 dB reset from there.
function updateChannelGainMeters(channelGains) {
  const mode = parseInt($('advancedDuckingMode').value, 10);
  const duckMode = parseInt($('duckMode').value, 10);
  const keyIdx = parseInt($('keyChannel').value, 10);
  if (mode !== 1 || duckMode !== 1) return;
  for (let c = 0; c < 5; c++) {
    if (c === keyIdx) continue;
    const db = gainLinearToDb(channelGains[c]);
    const pct = Math.min(100, (Math.abs(db) / WDRC_METER_MAX_DB) * 100);
    $(`chGrFill${c}`).style.width = `${pct}%`;
    $(`chGrReadout${c}`).textContent = `${db.toFixed(1)} dB`;
  }
}

// Small/subtle meter next to the WDRC header - just a fill bar and a
// number, not a full EQ-style graph like the sidechain/resonance meter
// above (this stage has no frequency shape to show, just one overall
// amount of reduction).
const WDRC_METER_MAX_DB = 24;
function updateWdrcMeter(reductionDb) {
  const pct = Math.min(100, (Math.abs(reductionDb) / WDRC_METER_MAX_DB) * 100);
  $('wdrcGrFill').style.width = `${pct}%`;
  $('wdrcGrReadout').textContent = `${reductionDb.toFixed(1)} dB`;
}

function applyAllControls() {
  if (!engineNode) return;
  const port = engineNode.port;
  port.postMessage({ type: 'setUnmaskEnabled', enabled: $('unmaskEnabled').checked });
  port.postMessage({ type: 'setKeyChannel', channel: parseInt($('keyChannel').value, 10) });
  port.postMessage({ type: 'setMode', mode: parseInt($('duckMode').value, 10) });
  port.postMessage({ type: 'setThresholdDb', thresholdDb: parseFloat($('thresholdDb').value) });
  port.postMessage({ type: 'setKneeDb', kneeDb: parseFloat($('kneeDb').value) });
  port.postMessage({ type: 'setMaxReductionDb', maxReductionDb: parseFloat($('maxReductionDb').value) });
  port.postMessage({ type: 'setRatio', ratio: parseFloat($('ratio').value) });
  port.postMessage({ type: 'setAttackMs', attackMs: parseFloat($('attackMs').value) });
  port.postMessage({ type: 'setReleaseMs', releaseMs: parseFloat($('releaseMs').value) });
  port.postMessage({ type: 'setAdvancedDuckingMode', mode: parseInt($('advancedDuckingMode').value, 10) });
  for (let c = 0; c < 5; c++) {
    port.postMessage({ type: 'setChannelThresholdDb', channel: c, thresholdDb: parseFloat($(`chThresholdDb${c}`).value) });
    port.postMessage({ type: 'setChannelRatio', channel: c, ratio: parseFloat($(`chRatio${c}`).value) });
  }
  port.postMessage({ type: 'setResonanceNumPeaks', count: parseInt($('resonanceNumPeaks').value, 10) });
  port.postMessage({ type: 'setResonanceBandwidthOctaves', bandwidthOctaves: parseFloat($('resonanceBandwidth').value) });
  port.postMessage({ type: 'setResonanceMaxReductionDb', maxReductionDb: parseFloat($('resonanceMaxReduction').value) });
  port.postMessage({ type: 'setWdrcBypassed', bypassed: $('wdrcBypassed').checked });
  port.postMessage({ type: 'setWdrcThresholdDb', thresholdDb: parseFloat($('wdrcThresholdDb').value) });
  port.postMessage({ type: 'setWdrcRatio', ratio: parseFloat($('wdrcRatio').value) });
  port.postMessage({ type: 'setWdrcMakeupGainDb', makeupGainDb: parseFloat($('wdrcMakeupGainDb').value) });
  port.postMessage({ type: 'setWdrcAttackMs', attackMs: parseFloat($('wdrcAttackMs').value) });
  port.postMessage({ type: 'setWdrcReleaseMs', releaseMs: parseFloat($('wdrcReleaseMs').value) });
  for (let c = 0; c < 5; c++) {
    port.postMessage({ type: 'setMute', channel: c, muted: $(`mute${c}`).checked });
    port.postMessage({ type: 'setSolo', channel: c, soloed: $(`solo${c}`).checked });
  }
}

// ---------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------
function buildChannelRows() {
  const container = $('channels');
  CLASS_NAMES.forEach((name, c) => {
    const row = document.createElement('div');
    row.className = 'channel-row';
    row.innerHTML = `
      <div class="channel-header">
        <span class="channel-name">${name}</span>
        <label><input type="checkbox" id="mute${c}"> Mute</label>
        <label><input type="checkbox" id="solo${c}"> Solo</label>
      </div>
      <fieldset class="channel-knobs" id="channelKnobs${c}" disabled>
        <label>Threshold:
          <input type="range" id="chThresholdDb${c}" min="-60" max="0" step="1" value="-30">
          <span id="chThresholdReadout${c}" class="readout">-30 dB</span>
        </label>
        <label>Ratio:
          <input type="range" id="chRatio${c}" min="1" max="10" step="0.1" value="4">
          <span id="chRatioReadout${c}" class="readout">4.0:1</span>
        </label>
        <span class="gr-meter">
          <span class="gr-meter-track"><span id="chGrFill${c}" class="gr-meter-fill"></span></span>
          <span id="chGrReadout${c}" class="gr-meter-label">0.0 dB</span>
        </span>
      </fieldset>
      <div class="waveform-container">
        <canvas id="wave${c}" class="waveform"></canvas>
        <div id="playhead${c}" class="playhead"></div>
      </div>
    `;
    container.appendChild(row);
  });

  const keySelect = $('keyChannel');
  CLASS_NAMES.forEach((name, c) => {
    const opt = document.createElement('option');
    opt.value = c;
    opt.textContent = name;
    if (c === 0) opt.selected = true; // Dialogue, matches Engine's default
    keySelect.appendChild(opt);
  });
}

function wireControls() {
  $('unmaskEnabled').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setUnmaskEnabled', enabled: $('unmaskEnabled').checked });
  });
  $('keyChannel').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setKeyChannel', channel: parseInt($('keyChannel').value, 10) });
    updateUnmaskRangeBadge();
    updateGainVisualization(); // redraw so the ducked-band box tracks the new key channel's range
    updateChannelKnobsState(); // the newly-keyed channel's own knobs go inert
  });
  $('advancedDuckingMode').addEventListener('change', () => {
    const mode = parseInt($('advancedDuckingMode').value, 10);
    if (mode === 1) {
      // Entering Per-channel: show each knob starting at the shared
      // "summed bus" values - the engine seeds its own independent
      // per-channel state from those same shared values at this exact
      // instant (see Engine::setAdvancedDuckingMode()), so this display
      // update and the engine's internal state agree without needing to
      // separately post setChannelThresholdDb/setChannelRatio for all 5.
      const sharedThreshold = parseFloat($('thresholdDb').value);
      const sharedRatio = parseFloat($('ratio').value);
      for (let c = 0; c < 5; c++) {
        $(`chThresholdDb${c}`).value = sharedThreshold;
        $(`chThresholdReadout${c}`).textContent = `${sharedThreshold} dB`;
        $(`chRatio${c}`).value = sharedRatio;
        $(`chRatioReadout${c}`).textContent = `${sharedRatio.toFixed(1)}:1`;
      }
    }
    engineNode?.port.postMessage({ type: 'setAdvancedDuckingMode', mode });
    updateChannelKnobsState();
  });
  for (let c = 0; c < 5; c++) {
    $(`chThresholdDb${c}`).addEventListener('input', () => {
      const db = parseFloat($(`chThresholdDb${c}`).value);
      $(`chThresholdReadout${c}`).textContent = `${db} dB`;
      engineNode?.port.postMessage({ type: 'setChannelThresholdDb', channel: c, thresholdDb: db });
    });
    $(`chRatio${c}`).addEventListener('input', () => {
      const r = parseFloat($(`chRatio${c}`).value);
      $(`chRatioReadout${c}`).textContent = `${r.toFixed(1)}:1`;
      engineNode?.port.postMessage({ type: 'setChannelRatio', channel: c, ratio: r });
    });
  }
  $('duckMode').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setMode', mode: parseInt($('duckMode').value, 10) });
    updateGainVisualization();
    updateChannelKnobsState(); // per-channel knobs only go live in Advanced mode
  });
  $('thresholdDb').addEventListener('input', () => {
    const db = parseFloat($('thresholdDb').value);
    $('thresholdReadout').textContent = `${db} dB`;
    engineNode?.port.postMessage({ type: 'setThresholdDb', thresholdDb: db });
    updateChannelKnobsState(); // Summed-bus mode: per-channel knobs live-track this
  });
  $('kneeDb').addEventListener('input', () => {
    const db = parseFloat($('kneeDb').value);
    $('kneeReadout').textContent = db === 0 ? 'Hard knee' : `${db} dB soft`;
    engineNode?.port.postMessage({ type: 'setKneeDb', kneeDb: db });
  });
  $('maxReductionDb').addEventListener('input', () => {
    const db = parseFloat($('maxReductionDb').value);
    $('maxReductionReadout').textContent = `${db} dB`;
    engineNode?.port.postMessage({ type: 'setMaxReductionDb', maxReductionDb: db });
  });
  $('ratio').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setRatio', ratio: parseFloat($('ratio').value) });
    updateChannelKnobsState(); // Summed-bus mode: per-channel knobs live-track this
  });
  $('attackMs').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setAttackMs', attackMs: parseFloat($('attackMs').value) });
  });
  $('releaseMs').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setReleaseMs', releaseMs: parseFloat($('releaseMs').value) });
  });
  $('resonanceNumPeaks').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setResonanceNumPeaks', count: parseInt($('resonanceNumPeaks').value, 10) });
  });
  $('resonanceBandwidth').addEventListener('input', () => {
    const oct = parseFloat($('resonanceBandwidth').value);
    $('resonanceBandwidthReadout').textContent = `${oct.toFixed(2)} oct`;
    engineNode?.port.postMessage({ type: 'setResonanceBandwidthOctaves', bandwidthOctaves: oct });
    updateGainVisualization(); // redraw now so the notch-width preview tracks the slider even while paused
  });
  $('resonanceMaxReduction').addEventListener('input', () => {
    const db = parseFloat($('resonanceMaxReduction').value);
    $('resonanceMaxReductionReadout').textContent = `${db} dB`;
    engineNode?.port.postMessage({ type: 'setResonanceMaxReductionDb', maxReductionDb: db });
  });
  $('wdrcBypassed').addEventListener('change', () => {
    engineNode?.port.postMessage({ type: 'setWdrcBypassed', bypassed: $('wdrcBypassed').checked });
  });
  $('wdrcThresholdDb').addEventListener('input', () => {
    const db = parseFloat($('wdrcThresholdDb').value);
    $('wdrcThresholdReadout').textContent = `${db} dB`;
    engineNode?.port.postMessage({ type: 'setWdrcThresholdDb', thresholdDb: db });
  });
  $('wdrcRatio').addEventListener('input', () => {
    const r = parseFloat($('wdrcRatio').value);
    $('wdrcRatioReadout').textContent = `${r.toFixed(1)}:1`;
    engineNode?.port.postMessage({ type: 'setWdrcRatio', ratio: r });
  });
  $('wdrcMakeupGainDb').addEventListener('input', () => {
    const db = parseFloat($('wdrcMakeupGainDb').value);
    $('wdrcMakeupGainReadout').textContent = `${db} dB`;
    engineNode?.port.postMessage({ type: 'setWdrcMakeupGainDb', makeupGainDb: db });
  });
  $('wdrcAttackMs').addEventListener('input', () => {
    const ms = parseFloat($('wdrcAttackMs').value);
    $('wdrcAttackReadout').textContent = `${ms} ms`;
    engineNode?.port.postMessage({ type: 'setWdrcAttackMs', attackMs: ms });
  });
  $('wdrcReleaseMs').addEventListener('input', () => {
    const ms = parseFloat($('wdrcReleaseMs').value);
    $('wdrcReleaseReadout').textContent = `${ms} ms`;
    engineNode?.port.postMessage({ type: 'setWdrcReleaseMs', releaseMs: ms });
  });
  for (let c = 0; c < 5; c++) {
    $(`mute${c}`).addEventListener('change', () => {
      engineNode?.port.postMessage({ type: 'setMute', channel: c, muted: $(`mute${c}`).checked });
    });
    $(`solo${c}`).addEventListener('change', () => {
      engineNode?.port.postMessage({ type: 'setSolo', channel: c, soloed: $(`solo${c}`).checked });
    });
  }

  $('loadBtn').addEventListener('click', async () => {
    $('loadBtn').disabled = true;
    try {
      await loadScene($('sceneSelect').value);
    } catch (err) {
      setStatus(`Error: ${err.message}`);
      console.error(err);
    } finally {
      $('loadBtn').disabled = false;
    }
  });

  // Decomposed mix Play/Pause — starting it pauses the reference so only
  // one of the two is ever audible.
  $('playBtn').addEventListener('click', async () => {
    if (!audioCtx) return;
    if (decomposedRunning) {
      await audioCtx.suspend();
      $('playBtn').textContent = 'Play';
      decomposedRunning = false;
    } else {
      if (!refAudio.paused) {
        refAudio.pause();
        $('refPlayBtn').textContent = 'Play';
      }
      await audioCtx.resume();
      $('playBtn').textContent = 'Pause';
      decomposedRunning = true;
    }
  });

  $('restartBtn').addEventListener('click', () => {
    engineNode?.port.postMessage({ type: 'seek' });
  });

  // Reference "Whole Blend" Play/Pause — untouched A/B reference, mutually
  // exclusive with the decomposed mix.
  $('refPlayBtn').addEventListener('click', async () => {
    if (refAudio.paused) {
      if (decomposedRunning && audioCtx) {
        await audioCtx.suspend();
        $('playBtn').textContent = 'Play';
        decomposedRunning = false;
      }
      refAudio.play();
      $('refPlayBtn').textContent = 'Pause';
    } else {
      refAudio.pause();
      $('refPlayBtn').textContent = 'Play';
    }
  });
}

refAudio.addEventListener('timeupdate', () => {
  refTimeEl.textContent = `${fmtTime(refAudio.currentTime)} / ${fmtTime(refAudio.duration || numFrames / sampleRate)}`;
  setPlayheadFraction('playhead-blend', refAudio.currentTime / (refAudio.duration || 1));
});
refAudio.addEventListener('ended', () => {
  $('refPlayBtn').textContent = 'Play';
});

buildChannelRows();
wireControls();
updateUnmaskRangeBadge();
updateChannelKnobsState();
updateGainVisualization();
updateWdrcMeter(0);
