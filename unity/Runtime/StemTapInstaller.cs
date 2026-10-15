using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Audio;

namespace SidechainUnmasking
{
    /// <summary>
    /// Maps mixer groups to source-separation classes and adds a StemTap to every
    /// AudioSource routed to a mapped group, so the mixer's routing stays the single
    /// source of truth for "which group is this sound in". Several groups may map to the
    /// same class (e.g. Background Noise and OutdoorSurrounds both -> BackgroundNoise).
    /// Sources routed to an unmapped group (e.g. a reverb return) are left unprocessed.
    /// </summary>
    [DefaultExecutionOrder(-900)]
    public class StemTapInstaller : MonoBehaviour
    {
        [Serializable]
        public class GroupMapping
        {
            public AudioMixerGroup group;
            public SoundClass soundClass;
        }

        [Tooltip("Map each group that sources route to directly (leaf groups), not their parents.")]
        [SerializeField] List<GroupMapping> mappings = new List<GroupMapping>();

        void Awake() => InstallAll();

        /// <summary>Call again after spawning new AudioSources at runtime.</summary>
        public void InstallAll()
        {
            foreach (var source in FindObjectsByType<AudioSource>(FindObjectsInactive.Include))
                Install(source);
        }

        public StemTap Install(AudioSource source)
        {
            if (!source || !TryGetClass(source.outputAudioMixerGroup, out var soundClass)) return null;
            var tap = source.GetComponent<StemTap>();
            if (!tap) tap = source.gameObject.AddComponent<StemTap>();
            tap.soundClass = soundClass;
            return tap;
        }

        public bool TryGetClass(AudioMixerGroup group, out SoundClass soundClass)
        {
            soundClass = default;
            if (!group) return false;
            foreach (var m in mappings)
            {
                if (m.group == group) { soundClass = m.soundClass; return true; }
            }
            return false;
        }
    }
}
