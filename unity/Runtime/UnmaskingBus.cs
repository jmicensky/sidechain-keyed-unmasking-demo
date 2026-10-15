// The group-level half of the processing: every mixer-group detector
// (sidechain key detector, per-group Enhancement detectors) lives here and
// runs once per DSP block on the summed signal of each class.
//
// WHY THIS SHAPE: Unity gives managed code no way to insert DSP into an
// AudioMixerGroup. What it does give is OnAudioFilterRead on each
// AudioSource (pre-mixer) and on the AudioListener (post-mixer, after every
// group has been mixed). So:
//   1. Each source's StemTap adds its block into its class's accumulator
//      here (Accumulate) - that sum IS the mixer group's signal, since a
//      group is just the sum of the sources routed to it.
//   2. The listener's callback runs after every source for the block has
//      been pulled, so it acts as an end-of-block barrier (EndBlock): the
//      detectors run sample-by-sample over the summed block and write
//      per-sample gain curves.
//   3. Each StemTap applies its class's gain curve to its own samples.
//      Every gain here is applied as a per-sample (or per-sample-per-band)
//      scalar and the band splits are linear, so applying the group's gain
//      to every member source and then summing is identical to compressing
//      the summed group - the same argument Types.h's AdvancedDuckingMode::
//      SummedBus comment makes.
// The cost is one DSP block of detector delay. AlignDetector = true
// removes it by having every StemTap output its PREVIOUS block (adds one
// block of output latency instead, and becomes sample-exact against the
// C++ engine - verified by unity/tests/ParityTest.cs).
//
// Threading: all audio-thread work (Accumulate/EndBlock/ProcessOutput and
// SourceTapProcessor) happens on Unity's audio thread. The main thread only
// calls SetSettings (an atomic reference swap, applied at the next block
// boundary) and TakeTelemetry.
using System;
using System.Threading;
using SidechainUnmasking.Dsp;

namespace SidechainUnmasking
{
    public sealed class UnmaskingBus
    {
        public const int NumClasses = 5;
        const int NumBands = DspTables.WdrcNumBands;
        const double KeyRampMs = 30.0;   // Engine.h kKeyRampMs
        const double StageRampMs = 25.0; // Engine.h kEnhancementBypassRampMs / WDRC kBypassRampMs
        const double EngagedGainLinear = 0.999; // telemetry: "duck engaged" = more than ~0.01 dB of cut

        static UnmaskingBus shared;
        /// <summary>The bus every StemTap reports to. Set by UnmaskingListener.</summary>
        public static UnmaskingBus Shared
        {
            get => Volatile.Read(ref shared);
            set => Volatile.Write(ref shared, value);
        }

        public double SampleRate { get; }
        public int MaxFrames { get; }
        public bool AlignDetector { get; }

        readonly object sync = new object();
        ProcessingSettings pending;

        /// <summary>Settings in effect for the most recently completed block.</summary>
        public ProcessingSettings Active { get; private set; }

        // --- Accumulators for the block currently being filled by StemTaps.
        readonly double[][] accMono = Jagged(NumClasses, 0);
        readonly double[][][] accMonoBands = new double[NumClasses][][];
        long accumulatingBlock;

        // --- Detectors.
        readonly EnvelopeFollower keyDetector = new EnvelopeFollower();
        readonly GainComputer keyGainComputer = new GainComputer();
        readonly Smoother unmaskAmount = new Smoother();
        readonly Smoother[] keyBlendSmoothers = new Smoother[NumClasses];
        readonly EnvelopeFollower[] enhDetector = new EnvelopeFollower[NumClasses];
        readonly GainComputer[] enhGainComputer = new GainComputer[NumClasses];
        readonly EnvelopeFollower[][] enhBandDetector = new EnvelopeFollower[NumClasses][];
        readonly GainComputer[] enhBandGainComputer = new GainComputer[NumClasses];
        readonly double[] enhMakeupLinear = new double[NumClasses];
        readonly Smoother enhBlendSmoother = new Smoother();
        readonly WdrcCompressor wdrc = new WdrcCompressor();
        double duckFloorLinear = 1.0;

        // --- Per-sample results of the last completed block, read by StemTaps
        //     while they render the next callback.
        internal long CompletedBlock = -1;
        internal int CompletedFrames;
        internal bool CompletedMultiband;
        internal bool CompletedDuckActive;
        internal DuckMode CompletedDuckMode;
        internal double KeyGainLinear = 1.0;
        internal int RangeVersion;
        internal double RangeLowHz, RangeHighHz;
        internal readonly double[][] EnhGain = Jagged(NumClasses, 0);   // single-band: effective scalar incl. bypass blend + makeup
        internal readonly double[][][] EnhBandGain = new double[NumClasses][][]; // multiband: g_b * makeup
        internal readonly double[] EnhBlend;
        internal readonly double[] DuckGain;
        internal readonly double[][] KeyBlend = Jagged(NumClasses, 0);

        int lastKeyClass = -1;

        // --- Telemetry (read on the main thread via TakeTelemetry).
        Telemetry telemetry = Telemetry.Empty();

        public UnmaskingBus(double sampleRate, int maxFrames, bool alignDetector, ProcessingSettings initial)
        {
            SampleRate = sampleRate;
            MaxFrames = maxFrames;
            AlignDetector = alignDetector;

            for (int c = 0; c < NumClasses; c++)
            {
                accMono[c] = new double[maxFrames];
                accMonoBands[c] = Jagged(NumBands, maxFrames);
                EnhGain[c] = new double[maxFrames];
                EnhBandGain[c] = Jagged(NumBands, maxFrames);
                KeyBlend[c] = new double[maxFrames];

                keyBlendSmoothers[c] = new Smoother();
                keyBlendSmoothers[c].Prepare(sampleRate, KeyRampMs);
                keyBlendSmoothers[c].Reset(0.0); // as Engine::prepare - the key ramps in over 30 ms

                enhDetector[c] = new EnvelopeFollower();
                enhGainComputer[c] = new GainComputer();
                enhBandGainComputer[c] = new GainComputer();
                enhBandDetector[c] = new EnvelopeFollower[NumBands];
                for (int b = 0; b < NumBands; b++) enhBandDetector[c][b] = new EnvelopeFollower();
            }
            EnhBlend = new double[maxFrames];
            DuckGain = new double[maxFrames];

            var s = (initial ?? new ProcessingSettings()).Clone();
            keyDetector.Prepare(sampleRate, s.attackMs, s.releaseMs);
            for (int c = 0; c < NumClasses; c++)
            {
                var p = s.enhancement[c].ToParams();
                enhDetector[c].Prepare(sampleRate, p.attackMs, p.releaseMs);
                for (int b = 0; b < NumBands; b++) enhBandDetector[c][b].Prepare(sampleRate, 5.0, 80.0);
            }
            unmaskAmount.Prepare(sampleRate, StageRampMs);
            enhBlendSmoother.Prepare(sampleRate, StageRampMs);
            wdrc.Prepare(sampleRate);

            ApplySettings(s);
            // Stage on/off states start settled (no fade-in at scene load), matching the
            // C++ engine, where Unmask on/off is instantaneous and the Enhancement bypass
            // blend is reset to its target in prepare(). WDRC still fades in, as in C++.
            unmaskAmount.Reset(unmaskAmount.Target);
            enhBlendSmoother.Reset(enhBlendSmoother.Target);
        }

        /// <summary>Main thread: queue new settings; they take effect at the next block boundary.</summary>
        public void SetSettings(ProcessingSettings settings)
        {
            Volatile.Write(ref pending, settings.Clone());
        }

        // ------------------------------------------------------------------
        // Audio thread
        // ------------------------------------------------------------------

        /// <summary>
        /// StemTap: add this source's mono (and, in Multiband mode, mono per-band) block
        /// into its class's group sum. Returns the index of the block it contributed to.
        /// </summary>
        internal long Accumulate(int classIndex, double[] mono, double[][] monoBands, int frames)
        {
            lock (sync)
            {
                var a = accMono[classIndex];
                for (int n = 0; n < frames; n++) a[n] += mono[n];
                if (monoBands != null)
                {
                    for (int b = 0; b < NumBands; b++)
                    {
                        var ab = accMonoBands[classIndex][b];
                        var mb = monoBands[b];
                        for (int n = 0; n < frames; n++) ab[n] += mb[n];
                    }
                }
                return accumulatingBlock;
            }
        }

        /// <summary>
        /// UnmaskingListener: every source has contributed this block. Run all group
        /// detectors over the summed block and publish per-sample gains.
        /// </summary>
        public void EndBlock(int frames)
        {
            lock (sync)
            {
                frames = Math.Min(frames, MaxFrames);
                var next = Interlocked.Exchange(ref pending, null);
                if (next != null) ApplySettings(next);
                var s = Active;

                bool multiband = s.enhancementBandMode == EnhancementBandMode.Multiband;
                int key = (int)s.keyClass;
                bool postEnhancementKey = s.keyDetectorTap == KeyDetectorTap.PostEnhancement;
                long engaged = 0;
                double sumDuckDb = 0.0, minDuckDb = 0.0;

                for (int n = 0; n < frames; n++)
                {
                    // 1. Group compression (Enhancement) detectors, one per class. They run
                    //    every block whether or not Enhancement is on, so their envelope
                    //    state is never cold when a scene switches it in.
                    double beta = enhBlendSmoother.Tick();
                    EnhBlend[n] = beta;
                    for (int c = 0; c < NumClasses; c++)
                    {
                        if (!multiband)
                        {
                            double g = enhGainComputer[c].ComputeLinearGain(enhDetector[c].Tick(accMono[c][n]));
                            EnhGain[c][n] = (1.0 - beta) + beta * g * enhMakeupLinear[c];
                            if (s.enhancementEnabled) telemetry.TrackEnhancement(c, g);
                        }
                        else
                        {
                            EnhGain[c][n] = 1.0;
                            double minG = 1.0;
                            for (int b = 0; b < NumBands; b++)
                            {
                                double level = enhBandDetector[c][b].Tick(accMonoBands[c][b][n]) + DspTables.WdrcLevelCompensationDb;
                                double g = enhBandGainComputer[c].ComputeLinearGain(level);
                                EnhBandGain[c][b][n] = g * enhMakeupLinear[c];
                                minG = Math.Min(minG, g);
                            }
                            if (s.enhancementEnabled) telemetry.TrackEnhancement(c, minG);
                        }
                    }

                    // 2. Sidechain key detector: the key group's mono sum, optionally after
                    //    its own Enhancement gain (Combined scenes only).
                    double keyIn = accMono[key][n];
                    if (postEnhancementKey)
                    {
                        if (!multiband) keyIn *= EnhGain[key][n];
                        else
                        {
                            double sum = 0.0;
                            for (int b = 0; b < NumBands; b++) sum += EnhBandGain[key][b][n] * accMonoBands[key][b][n];
                            keyIn = (1.0 - beta) * keyIn + beta * sum;
                        }
                    }
                    double gDuck = keyGainComputer.ComputeLinearGain(keyDetector.Tick(keyIn));
                    gDuck = Math.Max(gDuck, duckFloorLinear); // Engine::clampGainLinear
                    double u = unmaskAmount.Tick();
                    gDuck = 1.0 + u * (gDuck - 1.0);
                    DuckGain[n] = gDuck;

                    for (int c = 0; c < NumClasses; c++) KeyBlend[c][n] = keyBlendSmoothers[c].Tick();

                    if (gDuck < EngagedGainLinear) engaged++;
                    double gDb = 20.0 * Math.Log10(Math.Max(gDuck, 1e-6));
                    sumDuckDb += gDb;
                    minDuckDb = Math.Min(minDuckDb, gDb);
                }

                for (int c = 0; c < NumClasses; c++)
                {
                    Array.Clear(accMono[c], 0, frames);
                    for (int b = 0; b < NumBands; b++) Array.Clear(accMonoBands[c][b], 0, frames);
                }

                bool anyKeyBlend = false;
                for (int c = 0; c < NumClasses; c++)
                    anyKeyBlend |= keyBlendSmoothers[c].Current > 1e-6 || keyBlendSmoothers[c].Target > 0.0;
                CompletedDuckActive = s.unmaskEnabled || unmaskAmount.Current > 1e-6 || anyKeyBlend;
                CompletedMultiband = multiband;
                CompletedDuckMode = s.duckMode;
                CompletedFrames = frames;
                CompletedBlock = accumulatingBlock;
                accumulatingBlock++;

                telemetry.samples += frames;
                if (s.unmaskEnabled) telemetry.unmaskSamples += frames;
                telemetry.engagedSamples += engaged;
                telemetry.sumDuckGainDb += sumDuckDb;
                telemetry.minDuckGainDb = Math.Min(telemetry.minDuckGainDb, minDuckDb);
            }
        }

        /// <summary>UnmaskingListener: output-bus WDRC on the final mix, after EndBlock.</summary>
        public void ProcessOutput(float[] data, int channels)
        {
            int frames = data.Length / channels;
            for (int n = 0; n < frames; n++)
            {
                int i = n * channels;
                double inL = data[i];
                double inR = channels > 1 ? data[i + 1] : inL;
                wdrc.Tick(inL, inR, out double outL, out double outR);
                data[i] = (float)outL;
                if (channels > 1) data[i + 1] = (float)outR;
                // Channels beyond the first two (non-stereo speaker modes) pass through.
            }
            telemetry.minWdrcGainDb = Math.Min(telemetry.minWdrcGainDb, wdrc.LastGainReductionDb);
        }

        // ------------------------------------------------------------------
        // Telemetry
        // ------------------------------------------------------------------

        public struct Telemetry
        {
            public long samples;          // samples processed since the last TakeTelemetry
            public long unmaskSamples;    // ...of which Unmask was enabled
            public long engagedSamples;   // ...of which the duck gain was below -0.01 dB
            public double sumDuckGainDb;  // for mean duck gain = sumDuckGainDb / samples
            public double minDuckGainDb;  // deepest duck, dB (<= 0)
            public double[] minEnhancementGainDb; // deepest group-compression cut per class, dB
            public double minWdrcGainDb;

            public double EngagedFraction => unmaskSamples > 0 ? (double)engagedSamples / unmaskSamples : 0.0;
            public double MeanDuckGainDb => samples > 0 ? sumDuckGainDb / samples : 0.0;

            internal static Telemetry Empty() => new Telemetry { minEnhancementGainDb = new double[NumClasses] };

            internal void TrackEnhancement(int c, double gLinear)
            {
                double db = 20.0 * Math.Log10(Math.Max(gLinear, 1e-6));
                if (db < minEnhancementGainDb[c]) minEnhancementGainDb[c] = db;
            }
        }

        /// <summary>Main thread: copy and reset the counters (e.g. once per trial).</summary>
        public Telemetry TakeTelemetry()
        {
            lock (sync)
            {
                var t = telemetry;
                telemetry = Telemetry.Empty();
                return t;
            }
        }

        // ------------------------------------------------------------------

        void ApplySettings(ProcessingSettings s)
        {
            Active = s;

            var sc = s.SidechainParams;
            keyGainComputer.SetParams(sc);
            keyDetector.SetTimes(sc.attackMs, sc.releaseMs);
            duckFloorLinear = Math.Pow(10.0, -Math.Min(Math.Max(s.maxReductionDb, 0.0), 40.0) / 20.0);
            KeyGainLinear = Math.Pow(10.0, s.keyGainDb / 20.0);
            unmaskAmount.SetTarget(s.unmaskEnabled ? 1.0 : 0.0);
            for (int c = 0; c < NumClasses; c++)
                keyBlendSmoothers[c].SetTarget(s.unmaskEnabled && c == (int)s.keyClass ? 1.0 : 0.0);

            if ((int)s.keyClass != lastKeyClass)
            {
                lastKeyClass = (int)s.keyClass;
                (RangeLowHz, RangeHighHz) = DspTables.UnmaskRanges[lastKeyClass];
                RangeVersion++;
            }

            for (int c = 0; c < NumClasses; c++)
            {
                var gs = s.enhancement[c];
                var p = gs.ToParams();
                p.ratio = Math.Min(Math.Max(p.ratio, 1.0), 10.0); // Engine::setEnhancementRatio
                enhDetector[c].SetTimes(p.attackMs, p.releaseMs);
                enhGainComputer[c].SetParams(p);
                // Multiband reuses WdrcCompressor in C++, which clamps its own times.
                double atk = Math.Min(Math.Max(p.attackMs, 1.0), 50.0);
                double rel = Math.Min(Math.Max(p.releaseMs, 30.0), 3000.0);
                for (int b = 0; b < NumBands; b++) enhBandDetector[c][b].SetTimes(atk, rel);
                enhBandGainComputer[c].SetParams(p);
                enhMakeupLinear[c] = Math.Pow(10.0, gs.makeupGainDb / 20.0);
            }
            enhBlendSmoother.SetTarget(s.enhancementEnabled ? 1.0 : 0.0);

            wdrc.SetThresholdDb(s.wdrcThresholdDb);
            wdrc.SetRatio(s.wdrcRatio);
            wdrc.SetKneeDb(s.wdrcKneeDb);
            wdrc.SetAttackMs(s.wdrcAttackMs);
            wdrc.SetReleaseMs(s.wdrcReleaseMs);
            wdrc.SetMakeupGainDb(s.wdrcMakeupGainDb);
            wdrc.SetBypassed(!s.wdrcEnabled);
        }

        static double[][] Jagged(int rows, int cols)
        {
            var a = new double[rows][];
            for (int i = 0; i < rows; i++) a[i] = new double[cols];
            return a;
        }
    }
}
