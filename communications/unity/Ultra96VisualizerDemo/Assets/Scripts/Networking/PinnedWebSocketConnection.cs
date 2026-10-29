using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace CG4002.Visualizer
{
    /// <summary>
    /// Minimal receive-oriented WSS client used because Unity's ClientWebSocket
    /// rejects a self-signed chain before invoking its validation callback.
    /// </summary>
    internal sealed class PinnedWebSocketConnection : IDisposable
    {
        private const string WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        private const int MaximumHeaderBytes = 16 * 1024;
        private const int MaximumMessageBytes = 16 * 1024;

        private readonly Uri uri;
        private readonly RemoteCertificateValidationCallback certificateValidator;
        private TcpClient tcpClient;
        private SslStream stream;

        /// <summary>
        /// Create a receive-oriented connection for a WSS endpoint and caller-
        /// supplied certificate validator.
        /// </summary>
        public PinnedWebSocketConnection(
            Uri uri,
            RemoteCertificateValidationCallback certificateValidator)
        {
            if (uri == null || !string.Equals(uri.Scheme, "wss", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("A wss:// URI is required", nameof(uri));
            this.uri = uri;
            this.certificateValidator = certificateValidator ??
                throw new ArgumentNullException(nameof(certificateValidator));
        }

        /// <summary>
        /// Open TCP, authenticate pinned TLS, and complete the RFC 6455 WebSocket
        /// upgrade before any application message is accepted.
        /// </summary>
        public async Task ConnectAsync(CancellationToken cancellationToken)
        {
            // Cancellation disposes the underlying socket/stream, which also
            // releases an in-progress Unity/Mono connect or TLS handshake.
            tcpClient = new TcpClient();
            using (cancellationToken.Register(() => tcpClient.Dispose()))
                await tcpClient.ConnectAsync(uri.Host, uri.Port);

            stream = new SslStream(
                tcpClient.GetStream(),
                false,
                certificateValidator);
            using (cancellationToken.Register(() => stream.Dispose()))
                await stream.AuthenticateAsClientAsync(uri.Host);

            // Perform the RFC 6455 HTTP Upgrade only after pinned TLS succeeds.
            // The random key prevents a cached/non-WebSocket response being accepted.
            string key = Convert.ToBase64String(RandomBytes(16));
            string path = string.IsNullOrEmpty(uri.PathAndQuery) ? "/" : uri.PathAndQuery;
            string request =
                $"GET {path} HTTP/1.1\r\n" +
                $"Host: {uri.Host}:{uri.Port}\r\n" +
                "Upgrade: websocket\r\n" +
                "Connection: Upgrade\r\n" +
                $"Sec-WebSocket-Key: {key}\r\n" +
                "Sec-WebSocket-Version: 13\r\n\r\n";
            byte[] requestBytes = Encoding.ASCII.GetBytes(request);
            await stream.WriteAsync(requestBytes, 0, requestBytes.Length, cancellationToken);
            await stream.FlushAsync(cancellationToken);

            string response = await ReadHttpHeadersAsync(cancellationToken);
            ValidateUpgradeResponse(response, key);
        }

        /// <summary>
        /// Receive and reassemble one UTF-8 text message while handling ping,
        /// pong, close, and continuation frames within strict size limits.
        /// </summary>
        public async Task<string> ReceiveTextAsync(CancellationToken cancellationToken)
        {
            // One logical text message may span several WebSocket frames. Reassemble
            // continuation frames up to a strict bound before decoding UTF-8.
            using var message = new MemoryStream();
            bool started = false;

            while (true)
            {
                Frame frame = await ReadFrameAsync(cancellationToken);
                switch (frame.Opcode)
                {
                    case 0x8:
                        throw new IOException("Gateway closed the WebSocket connection");
                    case 0x9:
                        // Reply to server ping frames so normal keepalive operation
                        // doesn't disconnect an otherwise idle phone.
                        await SendControlFrameAsync(0xA, frame.Payload, cancellationToken);
                        continue;
                    case 0xA:
                        continue;
                    case 0x1:
                        if (started)
                            throw new InvalidDataException("Unexpected text frame during fragmentation");
                        started = true;
                        break;
                    case 0x0:
                        if (!started)
                            throw new InvalidDataException("Unexpected continuation frame");
                        break;
                    default:
                        throw new InvalidDataException($"Unsupported WebSocket opcode {frame.Opcode}");
                }

                message.Write(frame.Payload, 0, frame.Payload.Length);
                if (message.Length > MaximumMessageBytes)
                    throw new InvalidDataException("Visualizer message exceeds 16 KiB");
                if (frame.Final)
                    return new UTF8Encoding(false, true).GetString(message.ToArray());
            }
        }

        /// <summary>Release TLS and TCP resources; it is safe to call repeatedly.</summary>
        public void Dispose()
        {
            stream?.Dispose();
            tcpClient?.Dispose();
            stream = null;
            tcpClient = null;
        }

        /// <summary>Read the bounded HTTP upgrade response through its blank line.</summary>
        private async Task<string> ReadHttpHeadersAsync(CancellationToken cancellationToken)
        {
            using var buffer = new MemoryStream();
            byte[] one = new byte[1];
            int matched = 0;
            byte[] terminator = { 13, 10, 13, 10 };

            while (buffer.Length < MaximumHeaderBytes)
            {
                await ReadExactlyAsync(one, 0, 1, cancellationToken);
                buffer.WriteByte(one[0]);
                matched = one[0] == terminator[matched] ? matched + 1 :
                    (one[0] == terminator[0] ? 1 : 0);
                if (matched == terminator.Length)
                    return Encoding.ASCII.GetString(buffer.ToArray());
            }
            throw new InvalidDataException("WebSocket upgrade response headers are too large");
        }

        /// <summary>
        /// Require a 101 response and verify Sec-WebSocket-Accept against the
        /// random client key so an ordinary HTTP response cannot be mistaken for WSS.
        /// </summary>
        private static void ValidateUpgradeResponse(string response, string key)
        {
            string[] lines = response.Split(new[] { "\r\n" }, StringSplitOptions.None);
            if (lines.Length == 0 || !lines[0].StartsWith("HTTP/1.1 101", StringComparison.Ordinal))
                throw new InvalidDataException($"WebSocket upgrade failed: {lines[0]}");

            var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (int index = 1; index < lines.Length; index++)
            {
                int separator = lines[index].IndexOf(':');
                if (separator > 0)
                    headers[lines[index].Substring(0, separator).Trim()] =
                        lines[index].Substring(separator + 1).Trim();
            }

            // SHA-1 here is mandated by the WebSocket handshake and isn't used for
            // certificate security. TLS identity uses SHA-256 pinning elsewhere.
            using SHA1 sha1 = SHA1.Create();
            string expectedAccept = Convert.ToBase64String(
                sha1.ComputeHash(Encoding.ASCII.GetBytes(key + WebSocketGuid)));
            if (!headers.TryGetValue("Sec-WebSocket-Accept", out string receivedAccept) ||
                !string.Equals(receivedAccept, expectedAccept, StringComparison.Ordinal))
                throw new InvalidDataException("WebSocket upgrade returned an invalid accept key");
        }

        /// <summary>Read and validate one unmasked server-to-client WebSocket frame.</summary>
        private async Task<Frame> ReadFrameAsync(CancellationToken cancellationToken)
        {
            // TCP is a byte stream: every header and payload read uses ReadExactly
            // rather than assuming one ReadAsync call returns the requested bytes.
            byte[] header = new byte[2];
            await ReadExactlyAsync(header, 0, 2, cancellationToken);
            bool final = (header[0] & 0x80) != 0;
            int opcode = header[0] & 0x0F;
            bool masked = (header[1] & 0x80) != 0;
            if (masked)
                throw new InvalidDataException("Server WebSocket frames must not be masked");

            ulong length = (ulong)(header[1] & 0x7F);
            if (length == 126)
            {
                byte[] extended = new byte[2];
                await ReadExactlyAsync(extended, 0, 2, cancellationToken);
                length = (ulong)((extended[0] << 8) | extended[1]);
            }
            else if (length == 127)
            {
                byte[] extended = new byte[8];
                await ReadExactlyAsync(extended, 0, 8, cancellationToken);
                length = 0;
                foreach (byte value in extended)
                    length = (length << 8) | value;
            }

            if (length > MaximumMessageBytes)
                throw new InvalidDataException("WebSocket frame exceeds 16 KiB");
            byte[] payload = new byte[(int)length];
            await ReadExactlyAsync(payload, 0, payload.Length, cancellationToken);
            return new Frame(final, opcode, payload);
        }

        /// <summary>Send a correctly masked client control frame, currently used for pong.</summary>
        private async Task SendControlFrameAsync(
            int opcode,
            byte[] payload,
            CancellationToken cancellationToken)
        {
            if (payload.Length > 125)
                throw new InvalidDataException("Invalid WebSocket control frame");
            // RFC 6455 requires every client-to-server frame, including pong, to
            // be masked. Server-to-client frames handled above must be unmasked.
            byte[] mask = RandomBytes(4);
            byte[] frame = new byte[2 + mask.Length + payload.Length];
            frame[0] = (byte)(0x80 | opcode);
            frame[1] = (byte)(0x80 | payload.Length);
            Buffer.BlockCopy(mask, 0, frame, 2, mask.Length);
            for (int index = 0; index < payload.Length; index++)
                frame[6 + index] = (byte)(payload[index] ^ mask[index % 4]);
            await stream.WriteAsync(frame, 0, frame.Length, cancellationToken);
            await stream.FlushAsync(cancellationToken);
        }

        /// <summary>
        /// Fill exactly the requested buffer region because TCP reads may return
        /// fewer bytes than requested even when the connection remains healthy.
        /// </summary>
        private async Task ReadExactlyAsync(
            byte[] buffer,
            int offset,
            int count,
            CancellationToken cancellationToken)
        {
            while (count > 0)
            {
                int received = await stream.ReadAsync(buffer, offset, count, cancellationToken);
                if (received == 0)
                    throw new EndOfStreamException("Connection closed unexpectedly");
                offset += received;
                count -= received;
            }
        }

        /// <summary>Generate cryptographically strong WebSocket keys and masks.</summary>
        private static byte[] RandomBytes(int count)
        {
            byte[] bytes = new byte[count];
            using RandomNumberGenerator random = RandomNumberGenerator.Create();
            random.GetBytes(bytes);
            return bytes;
        }

        private readonly struct Frame
        {
            /// <summary>True when this is the final frame of a logical message.</summary>
            public bool Final { get; }
            /// <summary>RFC 6455 frame type, such as text, continuation, or ping.</summary>
            public int Opcode { get; }
            /// <summary>Unmasked server payload bytes.</summary>
            public byte[] Payload { get; }

            /// <summary>Store a decoded frame for message-level processing.</summary>
            public Frame(bool final, int opcode, byte[] payload)
            {
                Final = final;
                Opcode = opcode;
                Payload = payload;
            }
        }
    }
}
