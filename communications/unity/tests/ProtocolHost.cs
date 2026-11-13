// Managed validation without launching a Unity Editor or running Unity APIs.
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using System.Threading.Tasks;
using CG4002.Visualizer;
using NUnit.Framework;

internal static class ProtocolHost
{
    private static async Task<RepFeedbackResponse> Receive(PinnedWebSocketConnection socket, string type, CancellationToken token)
    {
        while (true)
        {
            string text = await socket.ReceiveTextAsync(token);
            if (RepFeedbackProtocol.TryParse(text, out var reply) && reply.Type == type) return reply;
        }
    }

    public static async Task<int> Main(string[] args)
    {
        try
        {
            if (args.Length > 0 && args[0] == "wss")
            {
                string pem = File.ReadAllText(args[2]).Replace("-----BEGIN CERTIFICATE-----", "")
                    .Replace("-----END CERTIFICATE-----", "");
                string expected = Convert.ToBase64String(Convert.FromBase64String(pem));
                using var token = new CancellationTokenSource(TimeSpan.FromSeconds(12));
                using var socket = new PinnedWebSocketConnection(new Uri(args[1]),
                    (_, certificate, chain, errors) => certificate != null &&
                        Convert.ToBase64String(certificate.GetRawCertData()) == expected);
                await socket.ConnectAsync(token.Token);
                var welcome = await Receive(socket, "rep_welcome", token.Token);
                string session = Guid.NewGuid().ToString("N");
                await socket.SendTextAsync(RepFeedbackProtocol.Build("rep_session_start", welcome.RunId, 1, session, 0, "squat"), token.Token);
                if ((await Receive(socket, "rep_result", token.Token)).Status != "session_started") throw new Exception("session start failed");
                await socket.SendTextAsync(RepFeedbackProtocol.Build("rep_event", welcome.RunId, 2, session, 1, "squat"), token.Token);
                var reply = await Receive(socket, "rep_result", token.Token);
                if (reply.Status != "accepted") throw new Exception("rep rejected");
                if (reply.Nodes.Any(node => node.Value<string>("status") == "pending"))
                    reply = await Receive(socket, "rep_delivery", token.Token);
                if (reply.Nodes.Count != 1 || reply.Nodes[0].Value<int>("node_id") != 1 || reply.Nodes.Any(node => node.Value<string>("status") != "applied"))
                    throw new Exception("Hip must apply rep feedback");
                await socket.SendTextAsync(RepFeedbackProtocol.Build("rep_event", welcome.RunId, 3, session, 1, "squat"), token.Token);
                if ((await Receive(socket, "rep_result", token.Token)).Status != "duplicate") throw new Exception("rep duplicate not suppressed");
                Console.WriteLine("Pinned C# WSS session/rep/duplicate end-to-end: PASS");
                return 0;
            }
            int count = 0;
            foreach (var type in Assembly.GetExecutingAssembly().GetTypes().Where(t => t.Namespace == "CG4002.Visualizer.Tests"))
            {
                object instance = Activator.CreateInstance(type);
                foreach (var method in type.GetMethods())
                {
                    foreach (TestCaseAttribute testcase in method.GetCustomAttributes(typeof(TestCaseAttribute), true))
                    {
                        method.Invoke(instance, testcase.Arguments); count++;
                    }
                    if (method.GetCustomAttributes(typeof(TestAttribute), true).Length > 0)
                    {
                        method.Invoke(instance, Array.Empty<object>()); count++;
                    }
                }
            }
            if (count < 10) throw new Exception("Protocol tests were not discovered");
            Console.WriteLine($"Managed protocol cases: {count} PASS (Unity source compiled; Editor not launched)");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error); return 1;
        }
    }
}
