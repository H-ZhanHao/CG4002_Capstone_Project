using System;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace CG4002.Visualizer
{
    /// <summary>
    /// Owns JSON parsing and version-1 validation. Networking code never creates
    /// ExerciseUpdateMessage directly from untrusted text; it comes through here.
    /// </summary>
    public static class ExerciseUpdateProtocol
    {
        /// <summary>Only protocol version currently accepted by this Unity client.</summary>
        public const int CurrentVersion = 1;
        /// <summary>Discriminator required so unrelated JSON is never treated as an update.</summary>
        public const string MessageType = "exercise_update";

        /// <summary>
        /// Parse untrusted WebSocket text, require every version-1 field and
        /// type, and construct the immutable application model only on success.
        /// </summary>
        public static bool TryParse(
            string json,
            out ExerciseUpdateMessage message,
            out string error)
        {
            message = null;
            error = null;

            // JObject is used instead of JsonUtility so missing fields and
            // incorrect JSON types can be distinguished reliably.
            JObject root;
            try
            {
                root = JObject.Parse(json);
            }
            catch (JsonException exception)
            {
                error = $"Malformed JSON: {exception.Message}";
                return false;
            }

            if (!TryRequiredInteger(root, "v", out long version) || version != CurrentVersion)
                return Fail("v must be integer 1", out error);
            if (!TryRequiredString(root, "type", out string type) || type != MessageType)
                return Fail("type must be exercise_update", out error);
            if (!TryRequiredInteger(root, "run_id", out long runId) || runId < 0)
                return Fail("run_id must be a non-negative integer", out error);
            if (!TryRequiredInteger(root, "seq", out long sequence) || sequence < 0)
                return Fail("seq must be a non-negative integer", out error);
            if (!TryRequiredInteger(root, "ts_us", out long timestampUs) || timestampUs < 0)
                return Fail("ts_us must be a non-negative integer", out error);
            if (!TryRequiredString(root, "exercise", out string exercise) ||
                string.IsNullOrWhiteSpace(exercise))
                return Fail("exercise must be a non-empty string", out error);

            // Unknown extra fields are ignored for forward compatibility.
            message = new ExerciseUpdateMessage(
                (int)version,
                type,
                runId,
                sequence,
                timestampUs,
                exercise);
            return true;
        }

        /// <summary>Read a required JSON integer without accepting another JSON type.</summary>
        private static bool TryRequiredInteger(JObject root, string name, out long value)
        {
            JToken token = root[name];
            if (token == null || token.Type != JTokenType.Integer)
            {
                value = 0;
                return false;
            }
            value = token.Value<long>();
            return true;
        }

        /// <summary>Read a required JSON string without coercing numbers or booleans.</summary>
        private static bool TryRequiredString(JObject root, string name, out string value)
        {
            JToken token = root[name];
            if (token == null || token.Type != JTokenType.String)
            {
                value = null;
                return false;
            }
            value = token.Value<string>();
            return true;
        }

        /// <summary>Return a consistent failed-parse result and its readable reason.</summary>
        private static bool Fail(string reason, out string error)
        {
            error = reason;
            return false;
        }
    }
}
