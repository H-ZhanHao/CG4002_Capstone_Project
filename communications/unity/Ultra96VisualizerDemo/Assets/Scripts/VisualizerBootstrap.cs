using UnityEngine;

namespace CG4002.Visualizer
{
    /// <summary>
    /// Creates the communications demo after any scene loads. This keeps the
    /// networking milestone independent from later AR scene organization.
    /// </summary>
    public static class VisualizerBootstrap
    {
        /// <summary>
        /// Create exactly one persistent client and its debug panel after the
        /// first scene loads, unless a scene has explicitly provided a client.
        /// </summary>
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void CreateVisualizer()
        {
            // Avoid a second connection if a future scene supplies its own client.
            if (Object.FindFirstObjectByType<Ultra96VisualizerClient>() != null)
                return;

            var root = new GameObject("Ultra96 Visualizer");
            Ultra96VisualizerClient client = root.AddComponent<Ultra96VisualizerClient>();
            ExerciseDebugPanel panel = root.AddComponent<ExerciseDebugPanel>();
            panel.Initialize(client);
        }
    }
}
