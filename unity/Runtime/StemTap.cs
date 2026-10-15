using UnityEngine;

namespace SidechainUnmasking
{
    /// <summary>
    /// Puts one AudioSource into the unmasking/compression signal flow as a member of its
    /// class's group. Must sit on the same GameObject as the AudioSource. Usually added
    /// automatically by StemTapInstaller from the source's output mixer group.
    ///
    /// Where this runs relative to the spatializer is set per source by
    /// AudioSource.spatializePostEffects: false (default) = spatializer first, so the tap
    /// sees and processes the binaural, distance-attenuated signal (what reaches the ears);
    /// true = tap first, on the dry source signal. See UNITY_HANDOFF.md section 5.
    /// </summary>
    [RequireComponent(typeof(AudioSource))]
    [DisallowMultipleComponent]
    public class StemTap : MonoBehaviour
    {
        [Tooltip("Which source-separation group this source belongs to.")]
        public SoundClass soundClass;

        readonly SourceTapProcessor processor = new SourceTapProcessor();

        void OnAudioFilterRead(float[] data, int channels)
        {
            processor.SoundClass = soundClass;
            processor.Process(UnmaskingBus.Shared, data, channels);
        }
    }
}
