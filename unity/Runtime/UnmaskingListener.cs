using UnityEngine;

namespace SidechainUnmasking
{
    /// <summary>
    /// Owns the UnmaskingBus and closes each DSP block. Goes on the AudioListener's
    /// GameObject (the XR camera): Unity calls a listener's OnAudioFilterRead after every
    /// source and mixer group has been mixed for the block, which makes it the
    /// end-of-block barrier where the group detectors run, and the place where the
    /// output-bus WDRC stage processes the final mix.
    /// </summary>
    [RequireComponent(typeof(AudioListener))]
    [DisallowMultipleComponent]
    [DefaultExecutionOrder(-1000)] // Bus must exist before anything calls SetSettings
    public class UnmaskingListener : MonoBehaviour
    {
        [Tooltip("On: every tapped source is delayed by one DSP block so detector and gain are " +
                 "sample-aligned (matches the C++ engine exactly). Off: no added latency, but " +
                 "the sidechain reacts one DSP block late. See UNITY_HANDOFF.md section 4.")]
        [SerializeField] bool alignDetector = true;

        [Tooltip("Largest DSP block the bus will accept. Must be >= the project's DSP buffer size.")]
        [SerializeField] int maxBlockFrames = 4096;

        public UnmaskingBus Bus { get; private set; }

        void Awake()
        {
            CreateBus(null);
            AudioSettings.OnAudioConfigurationChanged += OnAudioConfigurationChanged;
        }

        void OnDestroy()
        {
            AudioSettings.OnAudioConfigurationChanged -= OnAudioConfigurationChanged;
            if (UnmaskingBus.Shared == Bus) UnmaskingBus.Shared = null;
        }

        // Sample rate or buffer size may have changed (e.g. headset reconnect):
        // rebuild with the same settings.
        void OnAudioConfigurationChanged(bool deviceWasChanged) => CreateBus(Bus?.Active);

        void CreateBus(ProcessingSettings settings)
        {
            AudioSettings.GetDSPBufferSize(out int bufferLength, out _);
            Bus = new UnmaskingBus(AudioSettings.outputSampleRate, Mathf.Max(maxBlockFrames, bufferLength),
                                   alignDetector, settings ?? new ProcessingSettings());
            UnmaskingBus.Shared = Bus;
        }

        void OnAudioFilterRead(float[] data, int channels)
        {
            var bus = Bus;
            if (bus == null) return;
            bus.EndBlock(data.Length / channels);
            bus.ProcessOutput(data, channels);
        }
    }
}
