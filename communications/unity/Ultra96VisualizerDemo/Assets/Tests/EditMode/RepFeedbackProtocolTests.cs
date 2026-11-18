using System;
using NUnit.Framework;

namespace CG4002.Visualizer.Tests
{
    public sealed class RepFeedbackProtocolTests
    {
        [Test]
        public void BuildsExplicitVersionedCommands()
        {
            Assert.AreEqual(
                "{\"v\":1,\"type\":\"rep_event\",\"run_id\":123,\"seq\":4,\"rep_session_id\":\"setA\",\"rep_index\":7,\"exercise\":\"squat\"}",
                RepFeedbackProtocol.Build("rep_event", 123, 4, "setA", 7, "squat"));
            Assert.DoesNotThrow(() => RepFeedbackProtocol.Build("rep_session_start", 123, 0, "newSet", 0, "push_up"));
            Assert.DoesNotThrow(() => RepFeedbackProtocol.Build("rep_sync", 123, 8, "setA", 10, "squat"));
        }

        [Test]
        public void RejectsInvalidCommandsAndIdentities()
        {
            Assert.Throws<ArgumentException>(() => RepFeedbackProtocol.Build("STOP", 123, 0, "a", 1, "squat"));
            Assert.Throws<ArgumentException>(() => RepFeedbackProtocol.Build("rep_event", 0, 0, "a", 1, "squat"));
            Assert.Throws<ArgumentException>(() => RepFeedbackProtocol.Build("rep_event", 123, -1, "a", 1, "squat"));
            Assert.Throws<ArgumentException>(() => RepFeedbackProtocol.Build("rep_event", 123, 0, "bad space", 1, "squat"));
            Assert.Throws<ArgumentException>(() => RepFeedbackProtocol.Build("rep_event", 123, 0, "a", -1, "squat"));
            Assert.Throws<ArgumentException>(() => RepFeedbackProtocol.Build("rep_event", 123, 0, "a", 1, "unknown"));
        }

        [Test]
        public void ParsesWelcomeAndPerNodeDelivery()
        {
            Assert.IsTrue(RepFeedbackProtocol.TryParse(
                "{\"v\":1,\"type\":\"rep_welcome\",\"run_id\":123,\"requires_sync\":true}", out var welcome));
            Assert.AreEqual(123, welcome.RunId);
            Assert.IsTrue(RepFeedbackProtocol.TryParse(
                "{\"v\":1,\"type\":\"rep_delivery\",\"run_id\":123,\"seq\":4,\"rep_session_id\":\"setA\",\"rep_index\":7,\"status\":\"complete\",\"reason\":\"new_rep\",\"feedback_event_id\":9,\"nodes\":[{\"node_id\":1,\"status\":\"applied\",\"result\":0},{\"node_id\":2,\"status\":\"timeout\",\"result\":-1}]}", out var delivery));
            Assert.AreEqual("complete", delivery.Status);
            Assert.AreEqual(9, delivery.FeedbackEventId);
        }

        [TestCase("{}")]
        [TestCase("{\"v\":2,\"type\":\"rep_welcome\",\"run_id\":123,\"requires_sync\":true}")]
        [TestCase("{\"v\":1,\"type\":\"rep_welcome\",\"run_id\":true,\"requires_sync\":true}")]
        [TestCase("{\"v\":1,\"type\":\"rep_welcome\",\"run_id\":123,\"requires_sync\":false}")]
        [TestCase("{\"v\":1,\"type\":\"STOP\",\"run_id\":123}")]
        public void RejectsInvalidResponses(string json)
        {
            Assert.IsFalse(RepFeedbackProtocol.TryParse(json, out _));
        }
    }
}
