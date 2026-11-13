using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace CG4002.Visualizer
{
    public sealed class RepFeedbackResponse
    {
        public string Type { get; internal set; }
        public long RunId { get; internal set; }
        public long Sequence { get; internal set; }
        public string SessionId { get; internal set; }
        public int RepIndex { get; internal set; }
        public string Status { get; internal set; }
        public string Reason { get; internal set; }
        public long FeedbackEventId { get; internal set; }
        public JArray Nodes { get; internal set; }
    }

    /// <summary>Version 1 phone-owned rep contract. No rep counting algorithm.</summary>
    public static class RepFeedbackProtocol
    {
        public const long MaxSafeInteger = 9007199254740991;
        private static readonly HashSet<string> Exercises = new()
            { "squat", "push_up", "sit_up", "lunge", "leg_raise", "jumping_jack" };

        public static string Build(string type, long runId, long sequence,
            string sessionId, int repIndex, string exercise)
        {
            if (type != "rep_session_start" && type != "rep_sync" && type != "rep_event")
                throw new ArgumentException("Unsupported rep command");
            if (runId <= 0 || runId > MaxSafeInteger || sequence < 0 || sequence > MaxSafeInteger ||
                repIndex < 0 || repIndex > 1000000000 || sessionId == null ||
                !Regex.IsMatch(sessionId, @"\A[A-Za-z0-9_-]{1,64}\z") || !Exercises.Contains(exercise))
                throw new ArgumentException("Invalid rep session, count, exercise, or identity");
            return new JObject {
                ["v"] = 1, ["type"] = type, ["run_id"] = runId, ["seq"] = sequence,
                ["rep_session_id"] = sessionId, ["rep_index"] = repIndex, ["exercise"] = exercise
            }.ToString(Formatting.None);
        }

        public static bool TryParse(string json, out RepFeedbackResponse response)
        {
            response = null;
            try
            {
                JObject root = JObject.Parse(json);
                if (root["v"]?.Type != JTokenType.Integer || root.Value<int>("v") != 1 ||
                    root["type"]?.Type != JTokenType.String || root["run_id"]?.Type != JTokenType.Integer)
                    return false;
                string type = root.Value<string>("type");
                long run = root.Value<long>("run_id");
                if (run < 0 || run > MaxSafeInteger) return false;
                if (type == "rep_welcome")
                {
                    if (run == 0 || root["requires_sync"]?.Type != JTokenType.Boolean ||
                        !root.Value<bool>("requires_sync")) return false;
                    response = new RepFeedbackResponse { Type = type, RunId = run };
                    return true;
                }
                if (type != "rep_result" && type != "rep_delivery") return false;
                foreach (string field in new[] { "seq", "rep_index", "feedback_event_id" })
                    if (root[field]?.Type != JTokenType.Integer) return false;
                foreach (string field in new[] { "rep_session_id", "status", "reason" })
                    if (root[field]?.Type != JTokenType.String) return false;
                if (root["nodes"] is not JArray nodes || nodes.Count > 2) return false;
                long sequence = root.Value<long>("seq");
                int index = root.Value<int>("rep_index");
                long eventId = root.Value<long>("feedback_event_id");
                if (sequence < -1 || sequence > MaxSafeInteger || index < -1 ||
                    index > 1000000000 || eventId < 0) return false;
                string status = root.Value<string>("status");
                if (status != "accepted" && status != "duplicate" && status != "rejected" &&
                    status != "synced" && status != "session_started" && status != "complete") return false;
                var seen = new HashSet<int>();
                foreach (JToken node in nodes)
                {
                    if (node["node_id"]?.Type != JTokenType.Integer || node["status"]?.Type != JTokenType.String ||
                        node["result"]?.Type != JTokenType.Integer) return false;
                    int id = node.Value<int>("node_id");
                    if ((id != 1 && id != 2) || !seen.Add(id)) return false;
                }
                response = new RepFeedbackResponse { Type = type, RunId = run, Sequence = sequence,
                    SessionId = root.Value<string>("rep_session_id"), RepIndex = index,
                    Status = status, Reason = root.Value<string>("reason"), FeedbackEventId = eventId, Nodes = nodes };
                return true;
            }
            catch (Exception error) when (error is JsonException || error is OverflowException ||
                                          error is ArgumentException || error is InvalidOperationException)
            {
                return false;
            }
        }
    }
}
