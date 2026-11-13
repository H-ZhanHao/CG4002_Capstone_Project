using System;
using System.Linq;
using System.Collections.Generic;
using System.Globalization;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace CG4002.Visualizer
{
    public sealed class FallEventMessage
    {
        public uint BootId { get; internal set; }
        public ulong EventId { get; internal set; }
        public int NodeId => 1;
    }

    public static class FallProtocol
    {
        public static bool TryParse(string json, out FallEventMessage message)
        {
            message = null;
            try
            {
                if (json == null || json.Length > 1024) return false;
                var root = JObject.Parse(json, new JsonLoadSettings {
                    DuplicatePropertyNameHandling = DuplicatePropertyNameHandling.Error });
                string[] fields = { "v", "type", "node_id", "boot_id", "event_id" };
                if (root.Count != fields.Length || fields.Any(field => root[field] == null)) return false;
                if (root["v"].Type != JTokenType.Integer || root.Value<long>("v") != 1 ||
                    root["type"].Type != JTokenType.String || root.Value<string>("type") != "fall_event" ||
                    root["node_id"].Type != JTokenType.Integer || root.Value<long>("node_id") != 1 ||
                    root["boot_id"].Type != JTokenType.Integer || root["event_id"].Type != JTokenType.String)
                    return false;
                long boot = root.Value<long>("boot_id");
                string value = root.Value<string>("event_id");
                if (boot <= 0 || boot > uint.MaxValue ||
                    !ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out ulong id) ||
                    id == 0 || value != id.ToString(CultureInfo.InvariantCulture)) return false;
                message = new FallEventMessage { BootId = (uint)boot, EventId = id };
                return true;
            }
            catch (Exception error) when (error is JsonException || error is OverflowException ||
                                          error is ArgumentException || error is InvalidOperationException)
            { return false; }
        }

        public static string BuildAck(FallEventMessage message)
        {
            return new JObject { ["v"] = 1, ["type"] = "fall_ack", ["node_id"] = 1,
                ["boot_id"] = message.BootId,
                ["event_id"] = message.EventId.ToString(CultureInfo.InvariantCulture)
            }.ToString(Formatting.None);
        }
    }

    public sealed class FallDetectionResponse
    {
        public long RunId { get; internal set; }
        public long Sequence { get; internal set; }
        public string Status { get; internal set; }
        public string Reason { get; internal set; }
        public bool RequestedEnabled { get; internal set; }
        public bool? Enabled { get; internal set; }
    }

    public static class FallDetectionProtocol
    {
        public static string Build(long runId, long sequence, bool enabled)
        {
            if (runId <= 0 || runId > RepFeedbackProtocol.MaxSafeInteger ||
                sequence < 0 || sequence > RepFeedbackProtocol.MaxSafeInteger)
                throw new ArgumentException("Invalid fall-control run or sequence");
            return new JObject { ["v"] = 1, ["type"] = "fall_detection_set",
                ["run_id"] = runId, ["seq"] = sequence, ["enabled"] = enabled
            }.ToString(Formatting.None);
        }

        public static bool TryParse(string json, out FallDetectionResponse response)
        {
            response = null;
            try
            {
                if (json == null || json.Length > 1024) return false;
                var root = JObject.Parse(json, new JsonLoadSettings {
                    DuplicatePropertyNameHandling = DuplicatePropertyNameHandling.Error });
                string[] fields = { "v", "type", "run_id", "seq", "status", "reason", "requested_enabled", "enabled" };
                if (root.Count != fields.Length || fields.Any(field => root[field] == null) ||
                    root["v"].Type != JTokenType.Integer || root.Value<long>("v") != 1 ||
                    root["type"].Type != JTokenType.String || root.Value<string>("type") != "fall_detection_result" ||
                    root["run_id"].Type != JTokenType.Integer || root["seq"].Type != JTokenType.Integer ||
                    root["status"].Type != JTokenType.String || root["reason"].Type != JTokenType.String ||
                    root["requested_enabled"].Type != JTokenType.Boolean ||
                    (root["enabled"].Type != JTokenType.Boolean && root["enabled"].Type != JTokenType.Null))
                    return false;
                long run = root.Value<long>("run_id"), sequence = root.Value<long>("seq");
                string status = root.Value<string>("status");
                bool requested = root.Value<bool>("requested_enabled");
                bool? enabled = root.Value<bool?>("enabled");
                if (run <= 0 || run > RepFeedbackProtocol.MaxSafeInteger || sequence < -1 ||
                    sequence > RepFeedbackProtocol.MaxSafeInteger ||
                    !new[] { "pending", "applied", "rejected", "timeout", "unavailable" }.Contains(status) ||
                    (status == "applied" ? enabled != requested : enabled != null)) return false;
                response = new FallDetectionResponse { RunId = run, Sequence = sequence,
                    Status = status, Reason = root.Value<string>("reason"),
                    RequestedEnabled = requested, Enabled = enabled };
                return true;
            }
            catch (Exception error) when (error is JsonException || error is OverflowException ||
                                          error is ArgumentException || error is InvalidOperationException)
            { return false; }
        }
    }

    /// <summary>Process-lifetime deduplication survives socket reconnects.</summary>
    public sealed class FallReceiptTracker
    {
        private readonly Dictionary<uint, ulong> highWater = new();
        public bool Accept(FallEventMessage message, out bool isNew)
        {
            bool known = highWater.TryGetValue(message.BootId, out ulong previous);
            isNew = false;
            if (!known && highWater.Count >= 64) return false;
            if (!known || message.EventId > previous)
            {
                highWater[message.BootId] = message.EventId;
                isNew = true;
            }
            return true;
        }
    }
}
