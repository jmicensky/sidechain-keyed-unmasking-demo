// The per-source half of the processing (see UnmaskingBus for the overall
// design). One instance per AudioSource, driven from StemTap's
// OnAudioFilterRead. Each callback it:
//   1. captures this source's block (and, in Multiband mode, splits it into
//      the 6 WDRC bands) and adds its mono contribution to its class's
//      group sum on the bus;
//   2. renders a block using the gain curves the bus computed at the last
//      barrier - the PREVIOUS block when AlignDetector is on (exact), or
//      the current block when it's off (one block of detector lag).
// Unity-free so the offline parity test can drive it directly.
using System;
using SidechainUnmasking.Dsp;

namespace SidechainUnmasking
{
    public sealed class SourceTapProcessor
    {
        const int NumBands = DspTables.WdrcNumBands;

        public SoundClass SoundClass;

        int channels, capacity;
        double sampleRate;

        // Two capture slots (current + previous block), for AlignDetector.
        readonly double[][][] raw = new double[2][][];        // [slot][ch][n]
        readonly double[][][][] bands = new double[2][][][];  // [slot][ch][band][n]
        readonly long[] slotBlock = { -1, -1 };
        readonly bool[] slotHasBands = new bool[2];
        readonly int[] slotFrames = new int[2];
        int writeSlot;

        CrossoverFilterbank[] filterbanks;        // per channel, Multiband Enhancement
        LR4Split[] highSplits, lowSplits;         // per channel, Advanced ducking
        int rangeVersion = -1;
        bool duckSplitsLive;

        double[] monoScratch;
        double[][] monoBandScratch;
        readonly double[] bandTmp = new double[NumBands];

        /// <summary>Audio thread. data is Unity's interleaved filter buffer, processed in place.</summary>
        public void Process(UnmaskingBus bus, float[] data, int numChannels)
        {
            if (bus == null || numChannels <= 0) return; // no listener yet: pass through
            int frames = data.Length / numChannels;
            if (frames > bus.MaxFrames) return;
            EnsureCapacity(numChannels, frames, bus.SampleRate);

            var settings = bus.Active;
            bool needBands = settings.enhancementBandMode == EnhancementBandMode.Multiband;
            double invCh = 1.0 / numChannels;

            // 1. Capture + analyze.
            int slot = writeSlot;
            var r = raw[slot];
            for (int n = 0; n < frames; n++)
            {
                double sum = 0.0;
                for (int k = 0; k < numChannels; k++)
                {
                    double x = data[n * numChannels + k];
                    r[k][n] = x;
                    sum += x;
                }
                monoScratch[n] = sum * invCh;
            }
            if (needBands)
            {
                for (int b = 0; b < NumBands; b++) Array.Clear(monoBandScratch[b], 0, frames);
                for (int k = 0; k < numChannels; k++)
                {
                    var fb = filterbanks[k];
                    var bk = bands[slot][k];
                    for (int n = 0; n < frames; n++)
                    {
                        fb.Tick(r[k][n], bandTmp);
                        for (int b = 0; b < NumBands; b++)
                        {
                            bk[b][n] = bandTmp[b];
                            monoBandScratch[b][n] += bandTmp[b];
                        }
                    }
                }
                for (int b = 0; b < NumBands; b++)
                    for (int n = 0; n < frames; n++) monoBandScratch[b][n] *= invCh;
            }
            slotBlock[slot] = bus.Accumulate((int)SoundClass, monoScratch, needBands ? monoBandScratch : null, frames);
            slotHasBands[slot] = needBands;
            slotFrames[slot] = frames;

            // 2. Render.
            int renderSlot = bus.AlignDetector ? 1 - slot : slot;
            writeSlot = bus.AlignDetector ? 1 - slot : slot;
            bool gainsValid = bus.CompletedBlock >= 0 &&
                              (!bus.AlignDetector || slotBlock[renderSlot] == bus.CompletedBlock);
            if (!gainsValid)
            {
                // Aligned: nothing captured for the block the bus just finished (first
                // callback, or the source was paused) - emit silence for one block.
                // Lagged: no gains computed yet - pass the input through untouched.
                if (bus.AlignDetector) Array.Clear(data, 0, data.Length);
                return;
            }
            // Unity's block size is fixed, so these three are equal in practice; the min
            // only guards a block-size change between consecutive callbacks.
            Render(bus, data, numChannels, renderSlot,
                   Math.Min(frames, Math.Min(slotFrames[renderSlot], bus.CompletedFrames)));
        }

        void Render(UnmaskingBus bus, float[] data, int numChannels, int slot, int frames)
        {
            int c = (int)SoundClass;
            bool multiband = slotHasBands[slot] && bus.CompletedMultiband;
            bool duckActive = bus.CompletedDuckActive;
            bool advanced = duckActive && bus.CompletedDuckMode == DuckMode.Advanced;

            if (advanced && (rangeVersion != bus.RangeVersion || !duckSplitsLive))
            {
                for (int k = 0; k < numChannels; k++)
                {
                    highSplits[k].Prepare(sampleRate, bus.RangeHighHz);
                    lowSplits[k].Prepare(sampleRate, bus.RangeLowHz);
                }
                rangeVersion = bus.RangeVersion;
            }
            duckSplitsLive = advanced;

            var r = raw[slot];
            var enhGain = bus.EnhGain[c];
            var enhBandGain = bus.EnhBandGain[c];
            var keyBlend = bus.KeyBlend[c];
            double keyGain = bus.KeyGainLinear;

            for (int n = 0; n < frames; n++)
            {
                double beta = bus.EnhBlend[n];
                double g = bus.DuckGain[n];
                double kb = keyBlend[n];
                for (int k = 0; k < numChannels; k++)
                {
                    double x = r[k][n];

                    // Group compression (Enhancement).
                    double y;
                    if (multiband)
                    {
                        var bk = bands[slot][k];
                        double sum = 0.0;
                        for (int b = 0; b < NumBands; b++) sum += enhBandGain[b][n] * bk[b][n];
                        y = (1.0 - beta) * x + beta * sum;
                    }
                    else
                    {
                        y = enhGain[n] * x;
                    }

                    // Sidechain ducking, crossfaded against the key group's dry path.
                    double outSample = y;
                    if (duckActive)
                    {
                        double ducked;
                        if (advanced)
                        {
                            highSplits[k].Tick(y, out double lowRemainder, out double above);
                            lowSplits[k].Tick(lowRemainder, out double below, out double inRange);
                            ducked = below + above + g * inRange;
                        }
                        else
                        {
                            ducked = g * y;
                        }
                        outSample = kb * (y * keyGain) + (1.0 - kb) * ducked;
                    }
                    data[n * numChannels + k] = (float)outSample;
                }
            }
        }

        void EnsureCapacity(int numChannels, int frames, double rate)
        {
            if (numChannels == channels && frames <= capacity && rate == sampleRate) return;
            // Runs once (first callback) unless the audio configuration changes.
            channels = numChannels;
            capacity = Math.Max(frames, capacity);
            sampleRate = rate;
            for (int s = 0; s < 2; s++)
            {
                raw[s] = new double[channels][];
                bands[s] = new double[channels][][];
                for (int k = 0; k < channels; k++)
                {
                    raw[s][k] = new double[capacity];
                    bands[s][k] = new double[NumBands][];
                    for (int b = 0; b < NumBands; b++) bands[s][k][b] = new double[capacity];
                }
                slotBlock[s] = -1;
            }
            filterbanks = new CrossoverFilterbank[channels];
            highSplits = new LR4Split[channels];
            lowSplits = new LR4Split[channels];
            for (int k = 0; k < channels; k++)
            {
                filterbanks[k] = new CrossoverFilterbank(NumBands);
                filterbanks[k].Prepare(sampleRate, DspTables.WdrcCrossoverHz);
                highSplits[k] = new LR4Split();
                lowSplits[k] = new LR4Split();
            }
            rangeVersion = -1;
            monoScratch = new double[capacity];
            monoBandScratch = new double[NumBands][];
            for (int b = 0; b < NumBands; b++) monoBandScratch[b] = new double[capacity];
        }
    }
}
