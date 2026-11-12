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
