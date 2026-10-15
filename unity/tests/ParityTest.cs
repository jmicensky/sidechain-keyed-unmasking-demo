// Offline parity test for the Unity C# port. Drives UnmaskingBus +
// SourceTapProcessor exactly the way Unity does (every source's
// OnAudioFilterRead for a block, then the AudioListener's), on the real
// Construction Scene stems, and compares against the C++ Engine's output
// rendered by render_reference.cpp. See run_parity_test.sh.
//
//   mono ParityTest.exe <dataDir> [renderDir]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using SidechainUnmasking;

static class ParityTest
{
    const int SampleRate = 48000;
    static string dataDir;
    static float[][] stems;
    static int failures;

    static int Main(string[] args)
    {
        dataDir = args[0];
        string renderDir = args.Length > 1 ? args[1] : null;
        stems = new float[UnmaskingBus.NumClasses][];
        for (int c = 0; c < stems.Length; c++) stems[c] = ReadF32(Path.Combine(dataDir, $"stem{c}.f32"));
        Console.WriteLine($"Loaded 5 stems, {stems[0].Length / 2} frames ({stems[0].Length / 2.0 / SampleRate:0.0} s)\n");

        Console.WriteLine("== 1. Parity vs C++ Engine (one source per group, AlignDetector on, 1024-frame blocks) ==");
        Check("unprocessed", Unprocessed(), 0.0);
        Check("basic_dialogue", Unmask(DuckMode.Basic, SoundClass.Dialogue), 0.0);
        var adv = Unmask(DuckMode.Advanced, SoundClass.Dialogue); adv.maxReductionDb = 9f;
        Check("advanced_dialogue", adv, 0.0);
        var safety = Unmask(DuckMode.Advanced, SoundClass.SafetyAlerts); safety.thresholdDb = -36f; safety.keyGainDb = 3f;
        Check("advanced_safety_keygain", safety, 0.0);
        Check("enh_single", Enhancement(EnhancementBandMode.SingleBand), 0.0);
        // C++'s per-channel WdrcCompressor instances fade in over 25 ms at startup
        // (internal bypass ramp from 0); the port doesn't model that startup ramp.
        Check("enh_multi", Enhancement(EnhancementBandMode.Multiband), 0.25);
        var wdrc = Unmask(DuckMode.Advanced, SoundClass.Dialogue);
        wdrc.wdrcEnabled = true; wdrc.wdrcThresholdDb = -30f; wdrc.wdrcRatio = 3f; wdrc.wdrcMakeupGainDb = 4f;
        wdrc.wdrcAttackMs = 5f; wdrc.wdrcReleaseMs = 150f;
        // With AlignDetector on, the listener's first block is silence (taps output their
        // previous block), so the WDRC's 25 ms startup fade-in and its band detectors run
        // one block out of phase with C++ until they settle. Startup-only: excluded here.
        Check("advanced_wdrc", wdrc, 0.5);

        Console.WriteLine("\n== 2. Same block size, AlignDetector off (one block of detector lag) ==");
        foreach (int block in new[] { 256, 512, 1024 })
            Lag("advanced_dialogue", adv, block);

        Console.WriteLine("\n== 3. Group equivalence: N sources in a group == one source carrying their sum ==");
        GroupEquivalence("Unmasked (Advanced)", adv);
        GroupEquivalence("Compressed (single-band)", Enhancement(EnhancementBandMode.SingleBand));
        GroupEquivalence("Compressed (multiband)", Enhancement(EnhancementBandMode.Multiband));
        GroupEquivalence("Combined (multiband + Advanced, post-enh key)", Combined(EnhancementBandMode.Multiband, KeyDetectorTap.PostEnhancement));

        Console.WriteLine("\n== 4. Combined scene sanity ==");
        CombinedSanity();

        Console.WriteLine("\n== 5. Throughput (Mono JIT on this Mac, not Unity's runtime; indicative only) ==");
        Throughput();

        if (renderDir != null)
        {
            Directory.CreateDirectory(renderDir);
            Console.WriteLine($"\n== Renders for listening -> {renderDir} ==");
            WriteWav(Path.Combine(renderDir, "S0_unprocessed.wav"), Run(Unprocessed(), OneSourcePerClass(), true, 1024));
            WriteWav(Path.Combine(renderDir, "S1_unmasked_advanced_dialogue.wav"), Run(adv, OneSourcePerClass(), true, 1024));
            WriteWav(Path.Combine(renderDir, "S2_compressed_singleband.wav"), Run(Enhancement(EnhancementBandMode.SingleBand), OneSourcePerClass(), true, 1024));
            WriteWav(Path.Combine(renderDir, "S3_combined.wav"), Run(Combined(EnhancementBandMode.SingleBand, KeyDetectorTap.PreEnhancement), OneSourcePerClass(), true, 1024));
            Console.WriteLine("  wrote S0..S3");
        }

        Console.WriteLine(failures == 0 ? "\nALL CHECKS PASSED" : $"\n{failures} CHECK(S) FAILED");
        return failures == 0 ? 0 : 1;
    }

    // ------------------------------------------------------------------ scenarios

    static ProcessingSettings Unprocessed() => new ProcessingSettings();

    static ProcessingSettings Unmask(DuckMode mode, SoundClass key) => new ProcessingSettings
    {
        unmaskEnabled = true, duckMode = mode, keyClass = key,
        thresholdDb = -40.5f, ratio = 5.5f, kneeDb = 22.5f, attackMs = 16.5f, releaseMs = 347f,
    };

    static ProcessingSettings Enhancement(EnhancementBandMode bandMode)
    {
        var s = new ProcessingSettings { enhancementEnabled = true, enhancementBandMode = bandMode };
        float[] thr = { -30, -36, -42, -24, -33 }, ratio = { 3, 4, 6, 2.5f, 5 }, knee = { 6, 12, 0, 8, 4 };
        float[] atk = { 5, 10, 2, 20, 8 }, rel = { 120, 250, 60, 400, 90 }, makeup = { 3, 0, -2, 6, 1.5f };
        for (int c = 0; c < 5; c++)
        {
            var g = s.enhancement[c];
            g.thresholdDb = thr[c]; g.ratio = ratio[c]; g.kneeDb = knee[c];
            g.attackMs = atk[c]; g.releaseMs = rel[c]; g.makeupGainDb = makeup[c];
        }
        return s;
    }

    static ProcessingSettings Combined(EnhancementBandMode bandMode, KeyDetectorTap tap)
    {
        var s = Enhancement(bandMode);
        var u = Unmask(DuckMode.Advanced, SoundClass.Dialogue);
        s.unmaskEnabled = true; s.duckMode = u.duckMode; s.keyClass = u.keyClass;
        s.thresholdDb = u.thresholdDb; s.ratio = u.ratio; s.kneeDb = u.kneeDb;
        s.attackMs = u.attackMs; s.releaseMs = u.releaseMs; s.keyDetectorTap = tap;
        return s;
    }

    // ------------------------------------------------------------------ engine driver

    static List<(SoundClass cls, float[] data)> OneSourcePerClass()
    {
        var list = new List<(SoundClass, float[])>();
        for (int c = 0; c < 5; c++) list.Add(((SoundClass)c, stems[c]));
        return list;
    }

    /// <summary>Simulates Unity's audio thread: per block, every source's filter, then the listener's.</summary>
    static float[] Run(ProcessingSettings s, List<(SoundClass cls, float[] data)> sources, bool align, int block)
    {
        int frames = sources[0].data.Length / 2;
        var bus = new UnmaskingBus(SampleRate, 4096, align, s);
        var taps = new SourceTapProcessor[sources.Count];
        for (int i = 0; i < taps.Length; i++) taps[i] = new SourceTapProcessor { SoundClass = sources[i].cls };

        var output = new float[frames * 2];
        var buf = new float[block * 2];
        var mix = new float[block * 2];
        for (int start = 0; start < frames; start += block)
        {
            int n = Math.Min(block, frames - start);
            if (buf.Length != n * 2) { buf = new float[n * 2]; mix = new float[n * 2]; }
            Array.Clear(mix, 0, mix.Length);
            for (int i = 0; i < taps.Length; i++)
            {
                Array.Copy(sources[i].data, start * 2, buf, 0, n * 2);
                taps[i].Process(bus, buf, 2);
                for (int j = 0; j < n * 2; j++) mix[j] += buf[j]; // mixer group + master sum
            }
            bus.EndBlock(n);
            bus.ProcessOutput(mix, 2);
            Array.Copy(mix, 0, output, start * 2, n * 2);
        }
        return output;
    }

    // ------------------------------------------------------------------ checks

    static void Check(string name, ProcessingSettings s, double skipSeconds)
    {
        var reference = ReadF32(Path.Combine(dataDir, $"ref_{name}.f32"));
        var output = Run(s, OneSourcePerClass(), true, 1024);
        var (maxErr, refPeak) = Compare(reference, output, 1024, skipSeconds);
        double errDbfs = 20 * Math.Log10(maxErr + 1e-30);
        bool pass = errDbfs < -100.0;
        if (!pass) failures++;
        Console.WriteLine($"  {(pass ? "PASS" : "FAIL")}  {name,-26} max |C# - C++| = {maxErr:0.00e+00} ({errDbfs:0.0} dBFS), ref peak {refPeak:0.000}" +
                          (skipSeconds > 0 ? $"  [first {skipSeconds} s excluded]" : ""));
    }

    static void Lag(string name, ProcessingSettings s, int block)
    {
        var reference = ReadF32(Path.Combine(dataDir, $"ref_{name}.f32"));
        var output = Run(s, OneSourcePerClass(), false, block);
        var (maxErr, _) = Compare(reference, output, 0, 0.0);
        double rmsErr = RmsDiff(reference, output);
        Console.WriteLine($"  lagged, {block,4}-frame blocks ({1000.0 * block / SampleRate:0.0} ms lag): max |err| {20 * Math.Log10(maxErr):0.0} dBFS, " +
                          $"RMS err {20 * Math.Log10(rmsErr):0.0} dBFS  (vs. 0 latency added)");
    }

    static void GroupEquivalence(string label, ProcessingSettings s)
    {
        // A: Background Noise group holds two real sources (BKG stem + OTHER stem),
        //    Dialogue group holds the dialogue stem split across two sources (60/40).
        // B: the same groups, each fed one source carrying the pre-summed signal.
        int len = stems[0].Length;
        var dlgA = new float[len]; var dlgB = new float[len]; var bgSum = new float[len];
        for (int i = 0; i < len; i++)
        {
            dlgA[i] = 0.6f * stems[0][i];
            dlgB[i] = stems[0][i] - dlgA[i];
            bgSum[i] = stems[2][i] + stems[4][i];
        }
        var multi = new List<(SoundClass, float[])>
        {
            (SoundClass.Dialogue, dlgA), (SoundClass.Dialogue, dlgB),
            (SoundClass.Music, stems[1]), (SoundClass.SafetyAlerts, stems[3]),
            (SoundClass.BackgroundNoise, stems[2]), (SoundClass.BackgroundNoise, stems[4]),
        };
        var single = new List<(SoundClass, float[])>
        {
            (SoundClass.Dialogue, stems[0]), (SoundClass.Music, stems[1]),
            (SoundClass.SafetyAlerts, stems[3]), (SoundClass.BackgroundNoise, bgSum),
        };
        var a = Run(s, multi, true, 1024);
        var b = Run(s, single, true, 1024);
        var (maxErr, peak) = Compare(b, a, 0, 0.0);
        double errDbfs = 20 * Math.Log10(maxErr + 1e-30);
        bool pass = errDbfs < -100.0;
        if (!pass) failures++;
        Console.WriteLine($"  {(pass ? "PASS" : "FAIL")}  {label,-46} max diff {errDbfs:0.0} dBFS (peak {peak:0.000})");
    }

    static void CombinedSanity()
    {
        // Combined with Enhancement switched off must equal the Unmasked scene; with
        // Unmask switched off it must equal the Compressed scene.
        var adv = Unmask(DuckMode.Advanced, SoundClass.Dialogue);
        var combinedNoEnh = Combined(EnhancementBandMode.SingleBand, KeyDetectorTap.PreEnhancement);
        combinedNoEnh.enhancementEnabled = false;
        Report("Combined minus Enhancement == Unmasked", Run(combinedNoEnh, OneSourcePerClass(), true, 1024), Run(adv, OneSourcePerClass(), true, 1024));

        var combinedNoUnmask = Combined(EnhancementBandMode.Multiband, KeyDetectorTap.PreEnhancement);
        combinedNoUnmask.unmaskEnabled = false;
        Report("Combined minus Unmask == Compressed", Run(combinedNoUnmask, OneSourcePerClass(), true, 1024),
               Run(Enhancement(EnhancementBandMode.Multiband), OneSourcePerClass(), true, 1024));

        // Pre vs post key tap should differ (Dialogue's makeup is +3 dB, so the
        // post-enhancement key reads hotter and ducks more).
        var t = new Dictionary<KeyDetectorTap, double>();
        foreach (var tap in new[] { KeyDetectorTap.PreEnhancement, KeyDetectorTap.PostEnhancement })
        {
            var s = Combined(EnhancementBandMode.SingleBand, tap);
            var bus = new UnmaskingBus(SampleRate, 4096, true, s);
            var taps = new SourceTapProcessor[5];
            for (int c = 0; c < 5; c++) taps[c] = new SourceTapProcessor { SoundClass = (SoundClass)c };
            var buf = new float[2048];
            for (int start = 0; start + 1024 <= stems[0].Length / 2; start += 1024)
            {
                for (int c = 0; c < 5; c++) { Array.Copy(stems[c], start * 2, buf, 0, 2048); taps[c].Process(bus, buf, 2); }
                bus.EndBlock(1024);
            }
            var tel = bus.TakeTelemetry();
            t[tap] = tel.MeanDuckGainDb;
            Console.WriteLine($"  key tap {tap,-15}: mean duck {tel.MeanDuckGainDb:0.00} dB, deepest {tel.minDuckGainDb:0.00} dB, engaged {100 * tel.EngagedFraction:0.0}% of samples");
        }
        bool pass = t[KeyDetectorTap.PostEnhancement] < t[KeyDetectorTap.PreEnhancement];
        if (!pass) failures++;
        Console.WriteLine($"  {(pass ? "PASS" : "FAIL")}  post-enhancement key (Dialogue +3 dB makeup) ducks harder than pre");
    }

    static void Report(string label, float[] a, float[] b)
    {
        var (maxErr, _) = Compare(b, a, 0, 0.0);
        double errDbfs = 20 * Math.Log10(maxErr + 1e-30);
        bool pass = errDbfs < -100.0;
        if (!pass) failures++;
        Console.WriteLine($"  {(pass ? "PASS" : "FAIL")}  {label,-40} max diff {errDbfs:0.0} dBFS");
    }

    static void Throughput()
    {
        var cases = new (string, ProcessingSettings)[]
        {
            ("Unprocessed", Unprocessed()),
            ("Unmasked (Advanced)", Unmask(DuckMode.Advanced, SoundClass.Dialogue)),
            ("Compressed (multiband)", Enhancement(EnhancementBandMode.Multiband)),
            ("Combined (multiband + Advanced)", Combined(EnhancementBandMode.Multiband, KeyDetectorTap.PreEnhancement)),
        };
        double audioSeconds = stems[0].Length / 2.0 / SampleRate;
        foreach (var (name, s) in cases)
        {
            var sw = Stopwatch.StartNew();
            Run(s, OneSourcePerClass(), true, 1024);
            sw.Stop();
            Console.WriteLine($"  {name,-32} 5 stereo sources: {audioSeconds / sw.Elapsed.TotalSeconds,6:0.0}x realtime");
        }
    }

    // ------------------------------------------------------------------ io / math

    /// <summary>Max |out[n + delay] - ref[n]| over the overlap, skipping the first skipSeconds.</summary>
    static (double maxErr, double refPeak) Compare(float[] reference, float[] output, int delayFrames, double skipSeconds)
    {
        int frames = reference.Length / 2;
        int start = (int)(skipSeconds * SampleRate);
        double maxErr = 0, peak = 0;
        for (int n = start; n + delayFrames < frames; n++)
            for (int k = 0; k < 2; k++)
            {
                double r = reference[2 * n + k];
                double o = output[2 * (n + delayFrames) + k];
                maxErr = Math.Max(maxErr, Math.Abs(o - r));
                peak = Math.Max(peak, Math.Abs(r));
            }
        return (maxErr, peak);
    }

    static double RmsDiff(float[] a, float[] b)
    {
        double sum = 0;
        for (int i = 0; i < a.Length; i++) { double d = a[i] - b[i]; sum += d * d; }
        return Math.Sqrt(sum / a.Length);
    }

    static float[] ReadF32(string path)
    {
        var bytes = File.ReadAllBytes(path);
        var f = new float[bytes.Length / 4];
        Buffer.BlockCopy(bytes, 0, f, 0, bytes.Length);
        return f;
    }

    static void WriteWav(string path, float[] interleavedStereo)
    {
        using var w = new BinaryWriter(File.Create(path));
        int dataBytes = interleavedStereo.Length * 4;
        w.Write("RIFF".ToCharArray()); w.Write(36 + dataBytes); w.Write("WAVE".ToCharArray());
        w.Write("fmt ".ToCharArray()); w.Write(16); w.Write((short)3); w.Write((short)2);
        w.Write(SampleRate); w.Write(SampleRate * 8); w.Write((short)8); w.Write((short)32);
        w.Write("data".ToCharArray()); w.Write(dataBytes);
        foreach (var x in interleavedStereo) w.Write(x);
    }
}
