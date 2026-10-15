// The full parameter set for one "processing scene" (Unprocessed /
// Unmasked / Compressed / Combined). Plain serializable data: the
// ProcessingScene ScriptableObject holds one of these for the inspector,
// and ProcessingSceneController hands a Clone() of it to the audio thread
// (UnmaskingBus.SetSettings) so inspector edits never race the DSP.
using System;
using SidechainUnmasking.Dsp;
using UnityEngine;

namespace SidechainUnmasking
{
    /// <summary>The five source-separation classes (Types.h's ClassIndex), one per mixer group.</summary>
    public enum SoundClass { Dialogue = 0, Music = 1, BackgroundNoise = 2, SafetyAlerts = 3, Other = 4 }

    /// <summary>
    /// Basic = full-band ducking. Advanced = ducking only inside the key class's own
    /// frequency range (DspTables.UnmaskRanges). Resonance (STFT) is not ported yet -
    /// see UNITY_HANDOFF.md section 8.
    /// </summary>
    public enum DuckMode { Basic, Advanced }

    public enum EnhancementBandMode { SingleBand, Multiband }

    /// <summary>
    /// Combined scenes only: whether the sidechain detector hears the key group before or
    /// after its own Enhancement compression. Pre keeps the thresholds calibrated in the
    /// thesis sweeps (which were measured on raw stems) valid.
    /// </summary>
    public enum KeyDetectorTap { PreEnhancement, PostEnhancement }

    /// <summary>One class's independent Enhancement (group) compressor.</summary>
    [Serializable]
    public class GroupCompressorSettings
    {
        [Range(-80f, 0f)] public float thresholdDb = -30f;
        [Range(1f, 10f)] public float ratio = 4f;
        [Range(0f, 24f)] public float kneeDb = 6f;
        [Range(1f, 50f)] public float attackMs = 5f;
        [Range(20f, 3000f)] public float releaseMs = 120f;
        [Range(-12f, 24f)] public float makeupGainDb = 0f;

        public CompressorParams ToParams() => new CompressorParams
        {
            thresholdDb = thresholdDb, ratio = ratio, kneeDb = kneeDb, attackMs = attackMs, releaseMs = releaseMs,
        };

        public GroupCompressorSettings Clone() => (GroupCompressorSettings)MemberwiseClone();
    }

    [Serializable]
    public class ProcessingSettings
    {
        [Header("Sidechain unmasking")]
        public bool unmaskEnabled;
        public SoundClass keyClass = SoundClass.Dialogue;
        public DuckMode duckMode = DuckMode.Advanced;
        [Range(-80f, 0f)] public float thresholdDb = -30f;
        [Range(1f, 20f)] public float ratio = 4f;
        [Range(0f, 24f)] public float kneeDb = 6f;
        [Range(1f, 50f)] public float attackMs = 5f;
        [Range(20f, 3000f)] public float releaseMs = 120f;
        [Tooltip("Ceiling on duck depth in dB (magnitude). Engine.h default: 6.")]
        [Range(0f, 40f)] public float maxReductionDb = 6f;
        [Tooltip("Static gain on the key group's own (dry) path - CompressorParams.safetyGainDb.")]
        [Range(-12f, 12f)] public float keyGainDb = 0f;
        public KeyDetectorTap keyDetectorTap = KeyDetectorTap.PreEnhancement;

        [Header("Group compression (Enhancement)")]
        public bool enhancementEnabled;
        public EnhancementBandMode enhancementBandMode = EnhancementBandMode.SingleBand;
        [Tooltip("Indexed by SoundClass.")]
        public GroupCompressorSettings[] enhancement = NewEnhancementArray();

        [Header("Output-bus WDRC (applied to the final mix)")]
        public bool wdrcEnabled;
        [Range(-80f, 0f)] public float wdrcThresholdDb = -24f;
        [Range(1f, 10f)] public float wdrcRatio = 2f;
        [Range(0f, 24f)] public float wdrcKneeDb = 6f;
        [Range(1f, 50f)] public float wdrcAttackMs = 5f;
        [Range(30f, 3000f)] public float wdrcReleaseMs = 80f;
        [Range(-12f, 24f)] public float wdrcMakeupGainDb = 0f;

        public CompressorParams SidechainParams => new CompressorParams
        {
            thresholdDb = thresholdDb, ratio = ratio, kneeDb = kneeDb, attackMs = attackMs, releaseMs = releaseMs,
        };

        public static GroupCompressorSettings[] NewEnhancementArray()
        {
            var a = new GroupCompressorSettings[UnmaskingBus.NumClasses];
            for (int c = 0; c < a.Length; c++) a[c] = new GroupCompressorSettings();
            return a;
        }

        public ProcessingSettings Clone()
        {
            var copy = (ProcessingSettings)MemberwiseClone();
            copy.enhancement = NewEnhancementArray();
            for (int c = 0; c < copy.enhancement.Length && enhancement != null && c < enhancement.Length; c++)
                copy.enhancement[c] = enhancement[c].Clone();
            return copy;
        }
    }
}
