using UnityEngine;

namespace CG4002.Visualizer
{
    /// <summary>
    /// Minimal assessment/debug display. It deliberately reads the client's
    /// public state and contains no networking or protocol logic.
    /// </summary>
    public sealed class ExerciseDebugPanel : MonoBehaviour
    {
        private Ultra96VisualizerClient client;
        private GUIStyle headingStyle;
        private GUIStyle bodyStyle;

        /// <summary>Attach the single client whose public state this panel displays.</summary>
        public void Initialize(Ultra96VisualizerClient visualizerClient)
        {
            client = visualizerClient;
        }

        /// <summary>
        /// Render current connection state and the newest validated exercise
        /// update using Unity's immediate-mode debug UI.
        /// </summary>
        private void OnGUI()
        {
            if (client == null)
                return;

            headingStyle ??= new GUIStyle(GUI.skin.label)
            {
                fontSize = 30,
                fontStyle = FontStyle.Bold
            };
            bodyStyle ??= new GUIStyle(GUI.skin.label) { fontSize = 23 };

            float width = Mathf.Min(Screen.width - 32f, 620f);
            GUILayout.BeginArea(new Rect(16f, 16f, width, 310f), GUI.skin.box);
            // State colour makes loss/staleness visible without requiring the
            // evaluator to inspect terminal output.
            Color previousColor = GUI.color;
            GUI.color = StateColor(client.State);
            GUILayout.Label(client.State.ToString().ToUpperInvariant(), headingStyle);
            GUI.color = previousColor;

            if (client.Latest == null)
            {
                GUILayout.Label("Exercise: —", bodyStyle);
                GUILayout.Label("Waiting for first valid update", bodyStyle);
            }
            else
            {
                ExerciseUpdateMessage message = client.Latest;
                GUILayout.Label($"Exercise: {message.Exercise}", bodyStyle);
                GUILayout.Label($"Run ID: {message.RunId}", bodyStyle);
                GUILayout.Label($"Sequence: {message.Sequence}", bodyStyle);
                GUILayout.Label($"Ultra96 timestamp: {message.TimestampUs} µs", bodyStyle);
            }

            GUILayout.Space(8f);
            GUILayout.Label(client.StatusDetail, bodyStyle);
            GUILayout.EndArea();
        }

        /// <summary>Map connection health to the assessment panel's status colour.</summary>
        private static Color StateColor(VisualizerConnectionState state)
        {
            return state switch
            {
                VisualizerConnectionState.Live => Color.green,
                VisualizerConnectionState.Connecting => Color.yellow,
                VisualizerConnectionState.Stale => new Color(1f, 0.55f, 0f),
                _ => Color.red
            };
        }
    }
}
