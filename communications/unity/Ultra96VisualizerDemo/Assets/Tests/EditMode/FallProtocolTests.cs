using NUnit.Framework;

namespace CG4002.Visualizer.Tests
{
    public sealed class FallProtocolTests
    {
        private const string Valid = "{\"v\":1,\"type\":\"fall_event\",\"node_id\":1,\"boot_id\":4294967295,\"event_id\":\"18446744073709551615\"}";

        [Test]
        public void PreservesFullIdentityAndBuildsAck()
        {
            Assert.IsTrue(FallProtocol.TryParse(Valid, out var message));
            Assert.AreEqual(ulong.MaxValue, message.EventId);
            Assert.AreEqual(uint.MaxValue, message.BootId);
            Assert.AreEqual(Valid.Replace("fall_event", "fall_ack"), FallProtocol.BuildAck(message));
        }

        [Test]
        public void DuplicatesAreAcknowledgedWithoutAnotherAlertAndNewBootResetsCount()
        {
            var tracker = new FallReceiptTracker();
            FallProtocol.TryParse(Valid, out var fall);
            Assert.IsTrue(tracker.Accept(fall, out bool fresh));
            Assert.IsTrue(fresh);
            Assert.IsTrue(tracker.Accept(fall, out fresh));
            Assert.IsFalse(fresh);
            FallProtocol.TryParse(Valid.Replace("4294967295", "42").Replace("18446744073709551615", "1"), out fall);
            Assert.IsTrue(tracker.Accept(fall, out fresh));
            Assert.IsTrue(fresh);
        }

        [Test]
        public void RejectsMalformedOrUnsupportedMessages()
        {
            foreach (string json in new[] { "{}", "[]", "bad", Valid.Replace("4294967295", "0"),
                Valid.Replace("18446744073709551615", "18446744073709551616"),
                Valid.Replace("18446744073709551615", "01"),
                Valid.Replace("\"node_id\":1", "\"node_id\":2"),
                Valid.Replace("\"v\":1", "\"v\":true"),
                Valid.Replace("\"v\":1", "\"v\":1,\"v\":1"),
                Valid.Replace("\"v\":1", "\"v\":1,\"extra\":0") })
                Assert.IsFalse(FallProtocol.TryParse(json, out _), json);
        }
    }
}
