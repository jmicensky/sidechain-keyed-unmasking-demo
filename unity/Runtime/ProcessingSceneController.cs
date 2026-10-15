using System;
using System.Collections.Generic;
using UnityEngine;

namespace SidechainUnmasking
{
    /// <summary>
    /// Switches between processing conditions. Apply() hands the scene's settings to the
    /// audio thread (they take effect at the next DSP block) and transitions its mixer
    /// snapshot. Switch between trials, during silence: stage on/off changes crossfade
    /// over 25-30 ms, but a duck-mode or band-mode change mid-sound restarts filter state.
    /// </summary>
    public class ProcessingSceneController : MonoBehaviour
    {
        [SerializeField] UnmaskingListener listener;
        [SerializeField] ProcessingScene initialScene;
        [Tooltip("Every condition used in this experiment, so they can be looked up by sceneId.")]
        [SerializeField] List<ProcessingScene> scenes = new List<ProcessingScene>();

        public ProcessingScene Current { get; private set; }
        public IReadOnlyList<ProcessingScene> Scenes => scenes;
        public event Action<ProcessingScene> SceneChanged;

        UnmaskingBus Bus => listener ? listener.Bus : null;

        void OnEnable() => ProcessingScene.Edited += OnSceneEdited;
        void OnDisable() => ProcessingScene.Edited -= OnSceneEdited;

        void Start()
        {
            if (!listener) listener = FindAnyObjectByType<UnmaskingListener>();
            if (!listener) Debug.LogError("ProcessingSceneController: no UnmaskingListener in the scene.", this);
            if (initialScene) Apply(initialScene);
        }

        public void Apply(ProcessingScene scene) => Apply(scene, scene.snapshotTransitionSeconds);

        public void Apply(ProcessingScene scene, float snapshotTransitionSeconds)
        {
            if (!scene || Bus == null) return;
            Current = scene;
            Bus.SetSettings(scene.settings);
            if (scene.mixerSnapshot) scene.mixerSnapshot.TransitionTo(snapshotTransitionSeconds);
            SceneChanged?.Invoke(scene);
        }

        public bool Apply(string sceneId)
        {
            var scene = scenes.Find(s => s && s.sceneId == sceneId);
            if (!scene)
            {
                Debug.LogWarning($"ProcessingSceneController: no scene with id '{sceneId}'.", this);
                return false;
            }
            Apply(scene);
            return true;
        }

        /// <summary>Counters since the last call: engaged fraction, mean/min duck, per-group cuts.</summary>
        public UnmaskingBus.Telemetry TakeTelemetry() => Bus?.TakeTelemetry() ?? default;

        void OnSceneEdited(ProcessingScene scene)
        {
            if (Application.isPlaying && scene == Current && Bus != null) Bus.SetSettings(scene.settings);
        }
    }
}
