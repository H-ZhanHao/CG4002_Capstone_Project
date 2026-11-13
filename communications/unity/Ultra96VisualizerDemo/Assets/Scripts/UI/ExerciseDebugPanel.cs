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
        private GUIStyle alertHeadingStyle;
        private GUIStyle alertBodyStyle;
        private GUIStyle alertButtonStyle;

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

            bool alertVisible = client.FallAlertVisible && client.LatestFall != null;
            bool initialEnabled = GUI.enabled;
            GUI.enabled = initialEnabled && !alertVisible;
            float width = Mathf.Min(Screen.width - 32f, 620f);
            GUILayout.BeginArea(new Rect(16f, 16f, width, 410f), GUI.skin.box);
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
            GUILayout.Space(8f);
            GUILayout.Label("Fall detection: " + client.FallDetectionDetail, bodyStyle);
            bool previousEnabled = GUI.enabled;
            GUI.enabled = previousEnabled && client.CanControlFallDetection;
            if (client.FallDetectionEnabled.HasValue)
            {
                if (GUILayout.Button(client.FallDetectionEnabled.Value ? "Disable fall detection" : "Enable fall detection"))
                    _ = client.SetFallDetectionEnabled(!client.FallDetectionEnabled.Value);
            }
            else
            {
                // Unknown can mean either setting after a lost ACK: allow an explicit choice.
                GUILayout.BeginHorizontal();
                if (GUILayout.Button("Set ON")) _ = client.SetFallDetectionEnabled(true);
                if (GUILayout.Button("Set OFF")) _ = client.SetFallDetectionEnabled(false);
                GUILayout.EndHorizontal();
            }
            GUI.enabled = previousEnabled;
            GUILayout.EndArea();
            GUI.enabled = initialEnabled;
            if (alertVisible)
                DrawFallAlert();
        }

        private void DrawFallAlert()
        {
            // Keep the alert inside the Game/phone viewport, independently of
            // the debug panel height or a long connection-status message.
            float margin = Mathf.Min(16f, Screen.width * 0.04f, Screen.height * 0.04f);
            float width = Mathf.Max(1f, Mathf.Min(620f, Screen.width - 2f * margin));
            float height = Mathf.Max(1f, Mathf.Min(170f, Screen.height - 2f * margin));
            var area = new Rect((Screen.width - width) / 2f,
                (Screen.height - height) / 2f, width, height);
            alertHeadingStyle ??= new GUIStyle(GUI.skin.label)
                { fontStyle = FontStyle.Bold, alignment = TextAnchor.MiddleCenter };
            alertHeadingStyle.fontSize = Mathf.Clamp((int)(width / 16f), 16, 32);
            alertBodyStyle ??= new GUIStyle(GUI.skin.label)
                { fontSize = 18, alignment = TextAnchor.MiddleCenter, wordWrap = true };
            alertButtonStyle ??= new GUIStyle(GUI.skin.button)
                { fontSize = 18, fontStyle = FontStyle.Bold };

            int previousDepth = GUI.depth;
            Color previousColor = GUI.color;
            bool previousEnabled = GUI.enabled;
            GUI.depth = -1000;
            GUI.enabled = true;
            GUI.color = new Color(0.55f, 0.03f, 0.03f, 1f);
            GUI.DrawTexture(area, Texture2D.whiteTexture);
            GUI.color = Color.white;
            GUILayout.BeginArea(area, GUI.skin.box);
            GUILayout.Space(8f);
            GUILayout.Label("FALL DETECTED", alertHeadingStyle);
            GUILayout.Label("Hip sensor reported a fall.", alertBodyStyle);
            GUILayout.FlexibleSpace();
            if (GUILayout.Button("Dismiss alert", alertButtonStyle, GUILayout.Height(38f)))
                client.DismissFallAlert();
            GUILayout.Space(8f);
            GUILayout.EndArea();
            GUI.depth = previousDepth;
            GUI.color = previousColor;
            GUI.enabled = previousEnabled;
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
