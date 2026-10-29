using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;

namespace CG4002.Visualizer
{
    /// <summary>States presented to the debug UI and future visualization layer.</summary>
    public enum VisualizerConnectionState
    {
        Connecting,
        Live,
        Stale,
        Disconnected,
        ConfigurationError
    }

    /// <summary>
    /// Maintains the pinned WSS connection and exposes only validated, newer
    /// exercise updates. Socket/TLS work runs asynchronously; Unity state and
    /// callbacks are changed on the main thread through mainThreadActions.
    /// </summary>
    public sealed class Ultra96VisualizerClient : MonoBehaviour
    {
        public const string DefaultUrl = "wss://192.168.0.2:9000";
        private const string CertificateResourcePath = "Certificates/server-cert";
        private const string CertificateAssetLocation =
            "Assets/Resources/Certificates/server-cert.bytes";
        private const float StaleAfterSeconds = 0.5f;

        // Unity APIs aren't generally safe on worker threads. Network continuations
        // enqueue small actions which Update() executes on Unity's main thread.
        private readonly ConcurrentQueue<Action> mainThreadActions = new();
        // Backoff is bounded at five seconds so a long outage doesn't produce a
        // tight reconnect loop, while demo recovery remains quick.
        private readonly int[] reconnectDelaysSeconds = { 1, 2, 4, 5 };
        private CancellationTokenSource lifetime;
        private byte[] pinnedCertificateSha256;
        private volatile string lastTlsValidationDetail = "TLS callback not reached";
        private float lastMessageRealtime;
        private bool hasMessage;

        public string Url { get; private set; } = DefaultUrl;
        public VisualizerConnectionState State { get; private set; } =
            VisualizerConnectionState.Disconnected;
        public ExerciseUpdateMessage Latest { get; private set; }
        public string StatusDetail { get; private set; } = "Not started";

        public event Action<ExerciseUpdateMessage> UpdateReceived;
        public event Action<VisualizerConnectionState, string> StateChanged;

        /// <summary>Keep one networking client alive when Unity changes scenes.</summary>
        private void Awake()
        {
            DontDestroyOnLoad(gameObject);
        }

        /// <summary>
        /// Load the trusted public certificate, calculate its pin, and start the
        /// background connection loop. Configuration errors stop startup safely.
        /// </summary>
        private void Start()
        {
            // The certificate is public and bundled in Resources. Its private key
            // remains exclusively on the Ultra96.
            TextAsset certificateAsset = Resources.Load<TextAsset>(CertificateResourcePath);
            if (certificateAsset == null)
            {
                SetState(
                    VisualizerConnectionState.ConfigurationError,
                    $"Missing {CertificateAssetLocation}");
                return;
            }

            try
            {
                using var certificate = LoadCertificate(certificateAsset.bytes);
                pinnedCertificateSha256 = Fingerprint(certificate);
            }
            catch (Exception exception)
            {
                SetState(
                    VisualizerConnectionState.ConfigurationError,
                    $"Invalid pinned certificate: {exception.Message}");
                return;
            }

            lifetime = new CancellationTokenSource();
            _ = RunConnectionLoopAsync(lifetime.Token);
        }

        /// <summary>
        /// Apply queued network results on Unity's main thread and mark an
        /// otherwise healthy connection stale when updates stop arriving.
        /// </summary>
        private void Update()
        {
            // Drain cross-thread work before evaluating freshness so a newly
            // received packet cannot be marked stale in the same frame.
            while (mainThreadActions.TryDequeue(out Action action))
                action();

            if (State == VisualizerConnectionState.Live && hasMessage &&
                Time.realtimeSinceStartup - lastMessageRealtime > StaleAfterSeconds)
            {
                SetState(VisualizerConnectionState.Stale, "No newer update for 500 ms");
            }
        }

        /// <summary>Cancel background socket work when Unity destroys this client.</summary>
        private void OnDestroy()
        {
            lifetime?.Cancel();
            lifetime?.Dispose();
            lifetime = null;
        }

        /// <summary>
        /// Maintain one WSS connection at a time and reconnect with bounded
        /// backoff until the MonoBehaviour's lifetime token is cancelled.
        /// </summary>
        private async Task RunConnectionLoopAsync(CancellationToken cancellationToken)
        {
            // This loop owns one connection at a time. Any transport failure is
            // reported, delayed with bounded backoff, and retried until destruction.
            int attempt = 0;
            while (!cancellationToken.IsCancellationRequested)
            {
                EnqueueState(VisualizerConnectionState.Connecting, $"Connecting to {Url}");
                try
                {
                    using var socket = new PinnedWebSocketConnection(
                        new Uri(Url),
                        ValidateServerCertificate);
                    await socket.ConnectAsync(cancellationToken);
                    attempt = 0;
                    await ReceiveLoopAsync(socket, cancellationToken);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    return;
                }
                catch (Exception exception)
                {
                    string diagnostic =
                        $"{exception.GetType().Name}: {exception.Message}; " +
                        $"{lastTlsValidationDetail}\n{exception}";
                    EnqueueState(VisualizerConnectionState.Disconnected, diagnostic);
                }

                int delay = reconnectDelaysSeconds[Math.Min(attempt, reconnectDelaysSeconds.Length - 1)];
                attempt++;
                try
                {
                    await Task.Delay(TimeSpan.FromSeconds(delay), cancellationToken);
                }
                catch (OperationCanceledException)
                {
                    return;
                }
            }
        }

        /// <summary>Receive complete text messages until cancellation or transport failure.</summary>
        private async Task ReceiveLoopAsync(
            PinnedWebSocketConnection socket,
            CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                ProcessMessage(await socket.ReceiveTextAsync(cancellationToken));
            }
        }

        /// <summary>
        /// Validate one JSON update, then queue ordering and UI notification work
        /// for Unity's main thread.
        /// </summary>
        private void ProcessMessage(string json)
        {
            // Malformed/unsupported packets are isolated from the connection; one
            // bad application message must not crash or disconnect the visualizer.
            if (!ExerciseUpdateProtocol.TryParse(json, out ExerciseUpdateMessage message, out string error))
            {
                Debug.LogWarning($"Rejected visualizer message: {error}");
                return;
            }

            mainThreadActions.Enqueue(() =>
            {
                // Sequence numbers restart only when run_id changes. Duplicates or
                // delayed packets from the current run must not roll the UI backward.
                if (Latest != null && message.RunId == Latest.RunId &&
                    message.Sequence <= Latest.Sequence)
                {
                    Debug.LogWarning(
                        $"Ignored non-newer update run_id={message.RunId} seq={message.Sequence}");
                    return;
                }

                Latest = message;
                hasMessage = true;
                lastMessageRealtime = Time.realtimeSinceStartup;
                SetState(
                    VisualizerConnectionState.Live,
                    $"Run {message.RunId}, sequence {message.Sequence}");
                UpdateReceived?.Invoke(message);
            });
        }

        /// <summary>
        /// Authenticate the Ultra96 by exact SHA-256 certificate pin and reject
        /// a matching certificate when it is outside its validity period.
        /// </summary>
        private bool ValidateServerCertificate(
            object sender,
            X509Certificate certificate,
            X509Chain chain,
            SslPolicyErrors errors)
        {
            if (certificate == null || pinnedCertificateSha256 == null)
            {
                lastTlsValidationDetail =
                    "TLS rejected: the server or pinned certificate is missing";
                return false;
            }

            // Exact DER-certificate pinning authenticates the Ultra96 even though
            // the demo uses a project self-signed certificate. Validity dates are
            // checked separately because accepting an expired exact pin is unsafe.
            using var presented = new X509Certificate2(certificate);
            DateTime now = DateTime.UtcNow;
            bool timeValid = now >= presented.NotBefore.ToUniversalTime() &&
                             now <= presented.NotAfter.ToUniversalTime();
            bool pinMatches = FixedTimeEquals(
                pinnedCertificateSha256,
                Fingerprint(presented));

            if (!pinMatches || !timeValid)
            {
                lastTlsValidationDetail =
                    "TLS rejected: " +
                    $"pinMatches={pinMatches}, timeValid={timeValid}, policyErrors={errors}";
                return false;
            }

            // The endpoint is fixed to wss://LAPTOP_IP:9000 and provisioning
            // verifies LAPTOP_IP in the pinned certificate SAN. Some Unity
            // TLS backends report a name mismatch for IP-address SANs, so runtime
            // identity is enforced by the exact SHA-256 certificate pin instead.
            lastTlsValidationDetail =
                $"TLS accepted pinned certificate; policyErrors={errors}";
            return true;
        }

        /// <summary>Load the bundled public certificate from PEM or binary DER bytes.</summary>
        private static X509Certificate2 LoadCertificate(byte[] data)
        {
            // Unity imports the .bytes resource unchanged. Support both PEM text
            // and binary DER so certificate storage can change without client logic.
            string text = Encoding.ASCII.GetString(data);
            const string begin = "-----BEGIN CERTIFICATE-----";
            const string end = "-----END CERTIFICATE-----";
            int beginIndex = text.IndexOf(begin, StringComparison.Ordinal);
            if (beginIndex < 0)
                return new X509Certificate2(data);

            beginIndex += begin.Length;
            int endIndex = text.IndexOf(end, beginIndex, StringComparison.Ordinal);
            if (endIndex < 0)
                throw new InvalidDataException("PEM certificate has no END CERTIFICATE marker");
            string base64 = text.Substring(beginIndex, endIndex - beginIndex)
                .Replace("\r", string.Empty)
                .Replace("\n", string.Empty)
                .Trim();
            return new X509Certificate2(Convert.FromBase64String(base64));
        }

        /// <summary>Calculate the SHA-256 pin from a certificate's DER representation.</summary>
        private static byte[] Fingerprint(X509Certificate2 certificate)
        {
            using SHA256 sha256 = SHA256.Create();
            return sha256.ComputeHash(certificate.RawData);
        }

        /// <summary>Compare certificate pins without early-exit timing differences.</summary>
        private static bool FixedTimeEquals(byte[] expected, byte[] actual)
        {
            // Compare every byte even after a difference is found. This avoids
            // leaking the matching prefix length through comparison timing.
            if (expected == null || actual == null || expected.Length != actual.Length)
                return false;
            int difference = 0;
            for (int index = 0; index < expected.Length; index++)
                difference |= expected[index] ^ actual[index];
            return difference == 0;
        }

        /// <summary>Queue a connection-state change for Unity's main thread.</summary>
        private void EnqueueState(VisualizerConnectionState state, string detail)
        {
            mainThreadActions.Enqueue(() => SetState(state, detail));
        }

        /// <summary>Store, log, and publish a changed connection state.</summary>
        private void SetState(VisualizerConnectionState state, string detail)
        {
            if (State == state && StatusDetail == detail)
                return;
            State = state;
            StatusDetail = detail;
            string line = $"Visualizer state={state}: {detail}";
            if (state == VisualizerConnectionState.Disconnected ||
                state == VisualizerConnectionState.ConfigurationError)
                Debug.LogError(line);
            else
                Debug.Log(line);
            StateChanged?.Invoke(state, detail);
        }
    }
}
