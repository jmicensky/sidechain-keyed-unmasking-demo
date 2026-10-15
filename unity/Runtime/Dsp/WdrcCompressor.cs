// C# port of include/WdrcCompressor.h - the 6-band output-bus WDRC stage,
// applied once to the final mix (on the AudioListener, see
// UnmaskingListener). Same math, same clamps, same +10 dB per-band level
// compensation, same 25 ms bypass crossfade.
using System;

namespace SidechainUnmasking.Dsp
{
    public sealed class WdrcCompressor
    {
        const double BypassRampMs = 25.0;
        const int NumBands = DspTables.WdrcNumBands;

        readonly CrossoverFilterbank filterbankL = new CrossoverFilterbank(NumBands);
        readonly CrossoverFilterbank filterbankR = new CrossoverFilterbank(NumBands);
        readonly EnvelopeFollower[] bandDetectors = new EnvelopeFollower[NumBands];
        readonly GainComputer gainComputer = new GainComputer();
        readonly Smoother bypassBlend = new Smoother();
        readonly double[] bandsL = new double[NumBands], bandsR = new double[NumBands];

        CompressorParams p = CompressorParams.Default;
        double makeupGainLinear = 1.0;
        double attackMs = 5.0, releaseMs = 80.0;

        public double LastGainReductionDb { get; private set; }

        public WdrcCompressor()
        {
            for (int b = 0; b < NumBands; b++) bandDetectors[b] = new EnvelopeFollower();
        }

        public void Prepare(double sampleRate)
        {
            filterbankL.Prepare(sampleRate, DspTables.WdrcCrossoverHz);
            filterbankR.Prepare(sampleRate, DspTables.WdrcCrossoverHz);
            foreach (var ef in bandDetectors) ef.Prepare(sampleRate, attackMs, releaseMs);
            gainComputer.SetParams(p);
            bypassBlend.Prepare(sampleRate, BypassRampMs);
            bypassBlend.Reset(0.0); // starts bypassed, as in C++
        }

        public void SetBypassed(bool bypassed) { bypassBlend.SetTarget(bypassed ? 0.0 : 1.0); }

        // Clamped to the WDRC literature range, exactly as the C++ setters do.
        public void SetAttackMs(double ms)
        {
            attackMs = Math.Min(Math.Max(ms, 1.0), 50.0);
            foreach (var ef in bandDetectors) ef.SetTimes(attackMs, releaseMs);
        }

        public void SetReleaseMs(double ms)
        {
            releaseMs = Math.Min(Math.Max(ms, 30.0), 3000.0);
            foreach (var ef in bandDetectors) ef.SetTimes(attackMs, releaseMs);
        }

        public void SetThresholdDb(double db) { p.thresholdDb = db; gainComputer.SetParams(p); }
        public void SetRatio(double ratio) { p.ratio = ratio; gainComputer.SetParams(p); }
        public void SetKneeDb(double db) { p.kneeDb = db; gainComputer.SetParams(p); }
        public void SetMakeupGainDb(double db) { makeupGainLinear = Math.Pow(10.0, db / 20.0); }

        public void Tick(double inL, double inR, out double outL, out double outR)
        {
            double blend = bypassBlend.Tick();
            if (blend < 1e-6)
            {
                outL = inL;
                outR = inR;
                LastGainReductionDb = 0.0;
                return;
            }

            filterbankL.Tick(inL, bandsL);
            filterbankR.Tick(inR, bandsR);

            double compressedL = 0.0, compressedR = 0.0, minGainDb = 0.0;
            for (int b = 0; b < NumBands; b++)
            {
                double mono = 0.5 * (bandsL[b] + bandsR[b]);
                double levelDb = bandDetectors[b].Tick(mono) + DspTables.WdrcLevelCompensationDb;
                double g = gainComputer.ComputeLinearGain(levelDb);
                minGainDb = Math.Min(minGainDb, 20.0 * Math.Log10(Math.Max(g, 1e-6)));
                compressedL += bandsL[b] * g;
                compressedR += bandsR[b] * g;
            }
            LastGainReductionDb = minGainDb;
            compressedL *= makeupGainLinear;
            compressedR *= makeupGainLinear;

            outL = (1.0 - blend) * inL + blend * compressedL;
            outR = (1.0 - blend) * inR + blend * compressedR;
        }
    }
}
