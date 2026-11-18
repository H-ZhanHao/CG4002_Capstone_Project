using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Security;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using NUnit.Framework;

namespace CG4002.Visualizer.Tests
{
    public sealed class RepSessionSendTests
    {
        // Exercise production request/gate logic without a Unity scene or real TLS.
        private sealed class RecordingTlsStream : SslStream
        {
            public readonly ConcurrentQueue<string> Messages = new();
            public RecordingTlsStream() : base(new MemoryStream()) { }
            public override Task WriteAsync(byte[] bytes, int offset, int count, CancellationToken token)
            {
                token.ThrowIfCancellationRequested();
                int size = bytes[offset + 1] & 127;
                int header = 2;
                if (size == 126) { size = bytes[offset + 2] * 256 + bytes[offset + 3]; header = 4; }
                byte[] plain = new byte[size];
                for (int i = 0; i < size; i++)
                    plain[i] = (byte)(bytes[offset + header + 4 + i] ^ bytes[offset + header + i % 4]);
                Messages.Enqueue(Encoding.UTF8.GetString(plain));
                return Task.CompletedTask;
            }
            public override Task FlushAsync(CancellationToken token) => Task.CompletedTask;
        }

        private static void Set(object target, string name, object value) =>
            target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        private static object Socket(RecordingTlsStream stream)
        {
            Type type = typeof(Ultra96VisualizerClient).Assembly.GetType("CG4002.Visualizer.PinnedWebSocketConnection");
            var socket = Activator.CreateInstance(type, new object[] {
                new Uri("wss://127.0.0.1:9000"),
                new RemoteCertificateValidationCallback((a, b, c, d) => true) });
            Set(socket, "stream", stream);
            return socket;
        }

        private static Ultra96VisualizerClient Client(RecordingTlsStream stream, SemaphoreSlim gate)
        {
            var client = (Ultra96VisualizerClient)FormatterServices.GetUninitializedObject(typeof(Ultra96VisualizerClient));
            Set(client, "repSendGate", gate);
            Set(client, "activeSocket", Socket(stream));
            Set(client, "<RepRunId>k__BackingField", 123L);
            Set(client, "mainThreadActions", new ConcurrentQueue<Action>());
            return client;
        }

        private static void Run(Func<Task> test)
        {
            Task task = Task.Run(test);
            Assert.IsTrue(task.Wait(TimeSpan.FromSeconds(5)), "Send did not complete");
            task.GetAwaiter().GetResult();
        }

        [Test]
        public void StartAndSyncWaitForBusyWriteButRepEventsDoNotQueue() => Run(async () =>
        {
            using var stream = new RecordingTlsStream();
            using var gate = new SemaphoreSlim(0, 1);
            var client = Client(stream, gate);
            Task<bool> start = client.BeginRepSession("squat");
            Assert.IsFalse(start.IsCompleted);
            gate.Release();
            Assert.IsTrue(await start);
            Assert.IsFalse(client.RepSessionReady); // Sending isn't server confirmation.
            Assert.IsTrue(stream.Messages.TryDequeue(out string first));
            Assert.AreEqual("rep_session_start", JObject.Parse(first).Value<string>("type"));
            await gate.WaitAsync();
            Task<bool> sync = client.SyncRepSession(2);
            Assert.IsFalse(sync.IsCompleted);
            gate.Release();
            Assert.IsTrue(await sync);
            Assert.IsTrue(stream.Messages.TryDequeue(out string second));
            Assert.AreEqual(2, JObject.Parse(second).Value<int>("rep_index"));
            Set(client, "<RepSessionReady>k__BackingField", true);
            await gate.WaitAsync();
            Assert.IsFalse(await client.SendRepEvent(3));
            gate.Release();
            Assert.IsTrue(stream.Messages.IsEmpty);
        });

        [Test]
        public void DoubleStartOrExerciseChangeSendsOnlyNewestWaitingSession() => Run(async () =>
        {
            using var stream = new RecordingTlsStream();
            using var gate = new SemaphoreSlim(0, 1);
            var client = Client(stream, gate);
            Task<bool> old = client.BeginRepSession("squat");
            Task<bool> current = client.BeginRepSession("push_up");
            gate.Release();
            Assert.IsFalse(await old);
            Assert.IsTrue(await current);
            Assert.AreEqual(1, stream.Messages.Count);
            stream.Messages.TryDequeue(out string json);
            Assert.AreEqual("push_up", JObject.Parse(json).Value<string>("exercise"));
            Assert.AreEqual(client.RepSessionId, JObject.Parse(json).Value<string>("rep_session_id"));
        });

        [Test]
        public void ReconnectOrNewServerRunInvalidatesWaitingRequest() => Run(async () =>
        {
            foreach (bool changeSocket in new[] { true, false })
            {
                using var stream = new RecordingTlsStream();
                using var replacement = new RecordingTlsStream();
                using var gate = new SemaphoreSlim(0, 1);
                var client = Client(stream, gate);
                Task<bool> pending = client.BeginRepSession("squat");
                if (changeSocket) Set(client, "activeSocket", Socket(replacement));
                else Set(client, "<RepRunId>k__BackingField", 456L);
                gate.Release();
                Assert.IsFalse(await pending);
                Assert.IsTrue(stream.Messages.IsEmpty);
                Assert.IsTrue(replacement.Messages.IsEmpty);
                Assert.AreEqual(1, gate.CurrentCount);
            }
        });

        [Test]
        public void OldSessionConfirmationCannotMakeNewSessionReady() => Run(async () =>
        {
            using var stream = new RecordingTlsStream();
            using var gate = new SemaphoreSlim(1, 1);
            var client = Client(stream, gate);
            Assert.IsTrue(await client.BeginRepSession("squat"));
            string oldId = client.RepSessionId;
            Assert.IsTrue(await client.BeginRepSession("push_up"));
            var flags = BindingFlags.Instance | BindingFlags.NonPublic;
            object socket = typeof(Ultra96VisualizerClient).GetField("activeSocket", flags).GetValue(client);
            var actions = (ConcurrentQueue<Action>)typeof(Ultra96VisualizerClient)
                .GetField("mainThreadActions", flags).GetValue(client);
            foreach (string sessionId in new[] { oldId, client.RepSessionId })
            {
                string json = new JObject { ["v"] = 1, ["type"] = "rep_result", ["run_id"] = 123,
                    ["seq"] = 1, ["rep_session_id"] = sessionId, ["rep_index"] = 0,
                    ["status"] = "session_started", ["reason"] = "baseline_only",
                    ["feedback_event_id"] = 0, ["nodes"] = new JArray() }.ToString();
                typeof(Ultra96VisualizerClient).GetMethod("ProcessMessage", flags)
                    .Invoke(client, new[] { (object)json, socket });
                while (actions.TryDequeue(out Action action)) action();
                Assert.AreEqual(sessionId == client.RepSessionId, client.RepSessionReady);
            }
        });

        [Test]
        public void CancelledWaitDoesNotReleaseAnotherRequestsGate() => Run(async () =>
        {
            using var stream = new RecordingTlsStream();
            using var gate = new SemaphoreSlim(0, 1);
            using var cancellation = new CancellationTokenSource();
            var client = Client(stream, gate);
            Set(client, "connectionToken", cancellation.Token);
            Task<bool> pending = client.BeginRepSession("squat");
            cancellation.Cancel();
            Assert.IsFalse(await pending);
            Assert.AreEqual(0, gate.CurrentCount);
        });
    }
}
