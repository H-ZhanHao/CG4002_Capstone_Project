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
            // Reuse a scene-provided connection, but always attach its display.
            Ultra96VisualizerClient client = Object.FindAnyObjectByType<Ultra96VisualizerClient>();
            if (client == null)
            {
                var root = new GameObject("Ultra96 Visualizer");
                client = root.AddComponent<Ultra96VisualizerClient>();
            }

            ExerciseDebugPanel panel = client.GetComponent<ExerciseDebugPanel>();
            if (panel == null)
                panel = client.gameObject.AddComponent<ExerciseDebugPanel>();
            panel.Initialize(client);
            panel.enabled = true;
        }
    }
}
