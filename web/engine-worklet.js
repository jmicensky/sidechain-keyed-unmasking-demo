// AudioWorkletProcessor driving the WASM Engine one render quantum (128
// frames) at a time. Loaded via audioContext.audioWorklet.addModule(), which
// the Web Audio spec always treats as an ES module, so the `import` below
// works with no bundler. engine.mjs has the wasm binary inlined (SINGLE_FILE
// build), so no second fetch happens inside worklet scope.
import createEngineModule from './engine.mjs';

const kNumClasses = 5;
const kRenderQuantum = 128;

class EngineProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._ready = false;
    this._enginePtr = 0;
    this._outPtrL = 0;
    this._outPtrR = 0;
    this._module = null;
    this._pendingStems = null;
    this._quantumCount = 0;

    this.port.onmessage = (event) => this._handleMessage(event.data);

    createEngineModule().then((module) => {
      this._module = module;
      this._enginePtr = module._engine_create();
      module._engine_prepare(this._enginePtr, sampleRate);
      this._outPtrL = module._engine_alloc(kRenderQuantum);
      this._outPtrR = module._engine_alloc(kRenderQuantum);
      this._ready = true;
      this.port.postMessage({ type: 'ready' });
      if (this._pendingStems) {
        this._loadStems(this._pendingStems);
        this._pendingStems = null;
      }
    });
  }

  _handleMessage(msg) {
    if (msg.type === 'loadStems') {
      if (!this._ready) {
        this._pendingStems = msg;
      } else {
        this._loadStems(msg);
      }
      return;
    }
    if (!this._ready) return;
    const m = this._module;
    const e = this._enginePtr;
    switch (msg.type) {
      case 'setMode':
        m._engine_set_mode(e, msg.mode); break;
      case 'setUnmaskEnabled':
        m._engine_set_unmask_enabled(e, msg.enabled ? 1 : 0); break;
      case 'setKeyChannel':
        m._engine_set_key_channel(e, msg.channel); break;
      case 'setMute':
        m._engine_set_mute(e, msg.channel, msg.muted ? 1 : 0); break;
      case 'setSolo':
        m._engine_set_solo(e, msg.channel, msg.soloed ? 1 : 0); break;
      case 'setThresholdDb':
        m._engine_set_threshold_db(e, msg.thresholdDb); break;
      case 'setRatio':
        m._engine_set_ratio(e, msg.ratio); break;
      case 'setKneeDb':
        m._engine_set_knee_db(e, msg.kneeDb); break;
      case 'setAttackMs':
        m._engine_set_attack_ms(e, msg.attackMs); break;
      case 'setReleaseMs':
        m._engine_set_release_ms(e, msg.releaseMs); break;
      case 'setResonanceNumPeaks':
        m._engine_set_resonance_num_peaks(e, msg.count); break;
      case 'setResonanceBandwidthOctaves':
        m._engine_set_resonance_bandwidth_octaves(e, msg.bandwidthOctaves); break;
      case 'setResonanceMaxReductionDb':
        m._engine_set_resonance_max_reduction_db(e, msg.maxReductionDb); break;
      case 'setWdrcBypassed':
        m._engine_set_wdrc_bypassed(e, msg.bypassed ? 1 : 0); break;
      case 'setWdrcThresholdDb':
        m._engine_set_wdrc_threshold_db(e, msg.thresholdDb); break;
      case 'setWdrcRatio':
        m._engine_set_wdrc_ratio(e, msg.ratio); break;
      case 'setWdrcMakeupGainDb':
        m._engine_set_wdrc_makeup_gain_db(e, msg.makeupGainDb); break;
      case 'setWdrcAttackMs':
        m._engine_set_wdrc_attack_ms(e, msg.attackMs); break;
      case 'setWdrcReleaseMs':
        m._engine_set_wdrc_release_ms(e, msg.releaseMs); break;
      case 'seek':
        m._engine_reset_playhead(e); break;
      default:
        break;
    }
  }

  // channelsL/channelsR are each 5 mono Float32Arrays (one per class,
  // Dialogue..Other), preserving the true stereo image of each stem instead
  // of collapsing to mono.
  _loadStems({ channelsL, channelsR, numFrames }) {
    const m = this._module;
    const stage = (chan) => {
      const ptr = m._engine_alloc(numFrames);
      m.HEAPF32.set(chan, ptr >> 2);
      return ptr;
    };
    const ptrsL = channelsL.map(stage);
    const ptrsR = channelsR.map(stage);
    m._engine_load_stems(
      this._enginePtr,
      ptrsL[0], ptrsR[0],
      ptrsL[1], ptrsR[1],
      ptrsL[2], ptrsR[2],
      ptrsL[3], ptrsR[3],
      ptrsL[4], ptrsR[4],
      numFrames
    );
    ptrsL.forEach((p) => m._engine_free(p));
    ptrsR.forEach((p) => m._engine_free(p));
    m._engine_reset_playhead(this._enginePtr);
    this.port.postMessage({ type: 'stemsLoaded', numFrames });
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length < 2) return true;
    const frames = out[0].length;

    if (!this._ready || !this._module) {
      for (const ch of out) ch.fill(0);
      return true;
    }

    const m = this._module;
    m._engine_process(this._enginePtr, this._outPtrL, this._outPtrR, frames);
    const renderedL = m.HEAPF32.subarray(this._outPtrL >> 2, (this._outPtrL >> 2) + frames);
    const renderedR = m.HEAPF32.subarray(this._outPtrR >> 2, (this._outPtrR >> 2) + frames);

    out[0].set(renderedL);
    out[1].set(renderedR);

    // Throttle: posting every 128-frame quantum (~2.7ms @ 48kHz) would flood
    // the main thread. Every 8th quantum is ~21ms (~47Hz), smooth enough for
    // the gain-reduction meter without spamming postMessage.
    this._quantumCount++;
    if (this._quantumCount % 8 === 0) {
      const e = this._enginePtr;
      const playhead = m._engine_playhead(e);
      const gainLinear = m._engine_last_gain_linear(e);
      const wdrcGainReductionDb = m._engine_wdrc_gain_reduction_db(e);
      // Queried live (not cached) since the peak count can change mid-playback.
      const numPeaks = m._engine_resonance_num_peaks(e);
      const peaks = [];
      for (let p = 0; p < numPeaks; p++) {
        peaks.push({
          freq: m._engine_resonance_freq(e, p),
          gainLinear: m._engine_resonance_gain_linear(e, p),
        });
      }
      this.port.postMessage({ type: 'playhead', frame: playhead, gainLinear, wdrcGainReductionDb, resonance: { peaks } });
    }

    return true;
  }
}

registerProcessor('engine-processor', EngineProcessor);
