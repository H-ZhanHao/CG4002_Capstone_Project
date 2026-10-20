using NUnit.Framework;

namespace CG4002.Visualizer.Tests
{
    /// <summary>
    /// Protects the JSON boundary shared with visualizer_protocol.py on Ultra96.
    /// These tests intentionally exercise wire text rather than constructing the
    /// C# model directly, so parsing and validation are checked together.
    /// </summary>
    public sealed class ExerciseUpdateProtocolTests
    {
        /// <summary>Confirms a complete version-1 message maps to the C# model.</summary>
        [Test]
        public void RequiredFieldsAreAccepted()
        {
            const string json =
                "{\"v\":1,\"type\":\"exercise_update\",\"run_id\":312," +
                "\"seq\":481,\"ts_us\":9841200,\"exercise\":\"squat\"}";

            Assert.That(ExerciseUpdateProtocol.TryParse(json, out var message, out var error),
                Is.True, error);
            Assert.That(message.RunId, Is.EqualTo(312));
            Assert.That(message.Sequence, Is.EqualTo(481));
        }

        /// <summary>
        /// Confirms malformed JSON, incompatible metadata, and invalid ranges
        /// are rejected.
        /// </summary>
        [TestCase("{bad json}")]
        [TestCase("{\"v\":2,\"type\":\"exercise_update\",\"run_id\":0,\"seq\":0,\"ts_us\":0,\"exercise\":\"unknown\"}")]
        [TestCase("{\"v\":1,\"type\":\"feedback\",\"run_id\":0,\"seq\":0,\"ts_us\":0,\"exercise\":\"unknown\"}")]
        [TestCase("{\"v\":1,\"type\":\"exercise_update\",\"run_id\":-1,\"seq\":0,\"ts_us\":0,\"exercise\":\"unknown\"}")]
        public void InvalidMessagesAreRejected(string json)
        {
            Assert.That(ExerciseUpdateProtocol.TryParse(json, out _, out _), Is.False);
        }
    }
}
