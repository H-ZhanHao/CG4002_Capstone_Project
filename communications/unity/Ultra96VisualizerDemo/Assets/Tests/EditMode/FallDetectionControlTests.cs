using System;
using NUnit.Framework;

namespace CG4002.Visualizer.Tests
{
    public sealed class FallDetectionControlTests
    {
        [Test]
        public void SettingUsesExplicitBooleanAndRequiresConfirmation()
        {
            Assert.AreEqual("{\"v\":1,\"type\":\"fall_detection_set\",\"run_id\":123,\"seq\":1,\"enabled\":false}",
                FallDetectionProtocol.Build(123, 1, false));
            const string pending = "{\"v\":1,\"type\":\"fall_detection_result\",\"run_id\":123,\"seq\":1,\"status\":\"pending\",\"reason\":\"awaiting_esp\",\"requested_enabled\":true,\"enabled\":null}";
            Assert.IsTrue(FallDetectionProtocol.TryParse(pending, out var response));
            Assert.IsNull(response.Enabled);
            string applied = pending.Replace("\"pending\"", "\"applied\"").Replace("\"enabled\":null", "\"enabled\":true");
            Assert.IsTrue(FallDetectionProtocol.TryParse(applied, out response));
            Assert.IsTrue(response.Enabled.Value);
            Assert.IsFalse(FallDetectionProtocol.TryParse(applied.Replace("\"enabled\":true", "\"enabled\":false"), out _));
            Assert.IsFalse(FallDetectionProtocol.TryParse(pending.Replace("\"enabled\":null", "\"enabled\":true"), out _));
            Assert.IsFalse(FallDetectionProtocol.TryParse(pending.Replace("\"requested_enabled\":true", "\"requested_enabled\":1"), out _));
            Assert.IsFalse(FallDetectionProtocol.TryParse(pending.Replace("\"v\":1", "\"v\":1,\"v\":1"), out _));
        }

        [Test]
        public void InvalidRunAndSequenceCannotBeSent()
        {
            Assert.Throws<ArgumentException>(() => FallDetectionProtocol.Build(0, 1, true));
            Assert.Throws<ArgumentException>(() => FallDetectionProtocol.Build(1, -1, false));
            Assert.Throws<ArgumentException>(() => FallDetectionProtocol.Build(1, 9007199254740992, true));
        }
    }
}
