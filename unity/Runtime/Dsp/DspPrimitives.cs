// 1:1 C# ports of the C++ demo engine's DSP building blocks
// (include/GainComputer.h, Smoother.h, Crossover.h). Kept in double
// precision and kept free of any UnityEngine dependency, so the exact same
// file compiles into the Unity project AND into the offline parity test
// (unity/tests/) that checks this port against the C++ engine sample-for-
// sample. If you change the math here, change it in the C++ headers too
// (or re-run the parity test and document the divergence).
using System;

namespace SidechainUnmasking.Dsp
{
    /// <summary>Threshold/ratio/knee/attack/release - Types.h's CompressorParams.</summary>
    [Serializable]
    public struct CompressorParams
    {
        public double thresholdDb;
        public double ratio;
        public double kneeDb;
        public double attackMs;
        public double releaseMs;

        public static CompressorParams Default => new CompressorParams
        {
            thresholdDb = -30.0, ratio = 4.0, kneeDb = 6.0, attackMs = 5.0, releaseMs = 120.0,
        };
    }

    /// <summary>Eq. 1: attack/release smoothed level estimate in dB (GainComputer.h).</summary>
    public sealed class EnvelopeFollower
    {
        const double Epsilon = 1e-9;
        const double FloorDb = -120.0;

        double sampleRate = 48000.0;
        double alphaAttack, alphaRelease;
        double levelDb = FloorDb;

        public void Prepare(double sampleRate, double attackMs, double releaseMs)
        {
            this.sampleRate = sampleRate;
            SetTimes(attackMs, releaseMs);
            levelDb = FloorDb;
        }

        public void SetTimes(double attackMs, double releaseMs)
        {
            alphaAttack = Math.Exp(-1.0 / (0.001 * attackMs * sampleRate));
            alphaRelease = Math.Exp(-1.0 / (0.001 * releaseMs * sampleRate));
        }

        public double Tick(double sample)
        {
            double instDb = 20.0 * Math.Log10(Math.Abs(sample) + Epsilon);
            double alpha = instDb >= levelDb ? alphaAttack : alphaRelease;
            levelDb = alpha * levelDb + (1.0 - alpha) * instDb;
            levelDb = Math.Max(levelDb, FloorDb);
            return levelDb;
        }

        public double LevelDb => levelDb;
    }

    /// <summary>Eq. 2: soft-knee gain computer, level (dB) -> linear gain in (0, 1].</summary>
    public sealed class GainComputer
    {
        const double MinKneeDb = 1e-6;
        CompressorParams p = CompressorParams.Default;

        public void SetParams(CompressorParams parameters) { p = parameters; }

        public double ComputeLinearGain(double levelDb)
        {
            double T = p.thresholdDb, R = p.ratio, W = p.kneeDb;
            double gainDb;
            if (W < MinKneeDb)
                gainDb = levelDb <= T ? 0.0 : (1.0 / R - 1.0) * (levelDb - T);
            else if (levelDb < T - W / 2.0)
                gainDb = 0.0;
            else if (levelDb > T + W / 2.0)
                gainDb = (1.0 / R - 1.0) * (levelDb - T);
            else
            {
                double x = levelDb - T + W / 2.0;
                gainDb = (1.0 / R - 1.0) * (x * x) / (2.0 * W);
            }
            return Math.Pow(10.0, gainDb / 20.0);
        }
    }

    /// <summary>One-pole exponential ramp toward a target (Smoother.h).</summary>
    public sealed class Smoother
    {
        double sampleRate = 48000.0;
        double coeff;
        double current, target;

        public void Prepare(double sampleRate, double timeMs)
        {
            this.sampleRate = sampleRate;
            coeff = Math.Exp(-1.0 / (0.001 * timeMs * sampleRate));
        }

        public void Reset(double value) { current = value; target = value; }
        public void SetTarget(double value) { target = value; }

        public double Tick()
        {
            current = target + coeff * (current - target);
            return current;
        }

        public double Current => current;
        public double Target => target;
    }

    /// <summary>RBJ biquad, transposed direct form II (Crossover.h).</summary>
    public sealed class Biquad
    {
        double b0 = 1.0, b1, b2, a1, a2;
        double z1, z2;

        public void SetLowpass(double sampleRate, double fc, double q)
        {
            double w0 = 2.0 * Math.PI * fc / sampleRate;
            double cosw0 = Math.Cos(w0);
            double alpha = Math.Sin(w0) / (2.0 * q);
            double a0 = 1.0 + alpha;
            b0 = ((1.0 - cosw0) / 2.0) / a0;
            b1 = (1.0 - cosw0) / a0;
            b2 = b0;
            a1 = (-2.0 * cosw0) / a0;
            a2 = (1.0 - alpha) / a0;
            Reset();
        }

        public void SetHighpass(double sampleRate, double fc, double q)
        {
            double w0 = 2.0 * Math.PI * fc / sampleRate;
            double cosw0 = Math.Cos(w0);
            double alpha = Math.Sin(w0) / (2.0 * q);
            double a0 = 1.0 + alpha;
            b0 = ((1.0 + cosw0) / 2.0) / a0;
            b1 = (-(1.0 + cosw0)) / a0;
            b2 = b0;
            a1 = (-2.0 * cosw0) / a0;
            a2 = (1.0 - alpha) / a0;
            Reset();
        }

        public void Reset() { z1 = 0.0; z2 = 0.0; }

        public double Tick(double x)
        {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    }

    /// <summary>One Linkwitz-Riley 4th-order two-way split (Crossover.h's LR4Split).</summary>
    public sealed class LR4Split
    {
        const double ButterworthQ = 0.70710678;
        readonly Biquad lowA = new Biquad(), lowB = new Biquad(), highA = new Biquad(), highB = new Biquad();

        public void Prepare(double sampleRate, double fc)
        {
            lowA.SetLowpass(sampleRate, fc, ButterworthQ);
            lowB.SetLowpass(sampleRate, fc, ButterworthQ);
            highA.SetHighpass(sampleRate, fc, ButterworthQ);
            highB.SetHighpass(sampleRate, fc, ButterworthQ);
        }

        public void Tick(double x, out double low, out double high)
        {
            low = lowB.Tick(lowA.Tick(x));
            high = highB.Tick(highA.Tick(x));
        }
    }

    /// <summary>
    /// N-band tree of LR4 splits, peeling the top band off the low remainder from the
    /// highest crossover down (Crossover.h's CrossoverFilterbank). band[0] = lowest.
    /// </summary>
    public sealed class CrossoverFilterbank
    {
        readonly LR4Split[] stages;
        public int NumBands { get; }

        public CrossoverFilterbank(int numBands)
        {
            if (numBands < 2) throw new ArgumentException("CrossoverFilterbank needs at least 2 bands");
            NumBands = numBands;
            stages = new LR4Split[numBands - 1];
            for (int i = 0; i < stages.Length; i++) stages[i] = new LR4Split();
        }

        public void Prepare(double sampleRate, double[] crossoverHz)
        {
            for (int i = 0; i < stages.Length; i++) stages[i].Prepare(sampleRate, crossoverHz[i]);
        }

        public void Tick(double x, double[] band)
        {
            double current = x;
            for (int i = stages.Length - 1; i >= 1; --i)
            {
                stages[i].Tick(current, out double lo, out double hi);
                band[i + 1] = hi;
                current = lo;
            }
            stages[0].Tick(current, out band[0], out band[1]);
        }
    }

    /// <summary>Fixed tables from Types.h.</summary>
    public static class DspTables
    {
        /// <summary>WDRC / multiband Enhancement band edges (kWdrcCrossoverHz): 6 bands.</summary>
        public static readonly double[] WdrcCrossoverHz = { 100.0, 299.1, 894.4, 2675.0, 8000.0 };
        public const int WdrcNumBands = 6;

        /// <summary>
        /// Per-band detectors read quieter than a full-band detector for the same signal;
        /// +10 dB is the empirical match WdrcCompressor.h uses (kLevelCompensationDb).
        /// </summary>
        public const double WdrcLevelCompensationDb = 10.0;

        /// <summary>Advanced duck mode's ducked range per key class (kUnmaskFrequencyRanges).</summary>
        public static readonly (double lowHz, double highHz)[] UnmaskRanges =
        {
            (175.0, 7000.0),  // Dialogue
            (75.0, 12000.0),  // Music
            (60.0, 2000.0),   // Background Noise
            (300.0, 2000.0),  // Safety Alerts
            (700.0, 12000.0), // Other
        };
    }
}
