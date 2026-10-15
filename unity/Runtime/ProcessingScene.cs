using System;
using UnityEngine;
using UnityEngine.Audio;

namespace SidechainUnmasking
{
    /// <summary>
    /// One processing condition ("mixer scene"): which stages are on and how they're set,
    /// plus an optional AudioMixerSnapshot for the static group fader balance that goes
    /// with it. Make one asset per condition: Unprocessed, Unmasked, Compressed, Combined.
    /// </summary>
    [CreateAssetMenu(menuName = "Listening Lab/Processing Scene", fileName = "PS_New")]
    public class ProcessingScene : ScriptableObject
    {
        [Tooltip("Written to the trial log, so keep it stable once data collection starts.")]
        public string sceneId = "Unprocessed";

        [Tooltip("Optional. Snapshot holding this condition's group fader levels / loudness match.")]
        public AudioMixerSnapshot mixerSnapshot;
        [Min(0f)] public float snapshotTransitionSeconds = 0.5f;

        public ProcessingSettings settings = new ProcessingSettings();

        /// <summary>Raised when the asset is edited in the inspector, so Play-mode tuning is live.</summary>
        public static event Action<ProcessingScene> Edited;

        void OnValidate() => Edited?.Invoke(this);
    }
}
