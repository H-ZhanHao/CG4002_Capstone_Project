"""Loopback-only bridge from validated exercise events to one Unity client.

Local producers submit newline-delimited version-1 JSON on port 9100. The
gateway validates it, retains only the newest update, and sends WSS on port
9000. Both listeners stay on loopback so the existing SSH tunnel and opaque
laptop relay remain the only route to the phone.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
from collections import deque
import json
import logging
import os
import ssl
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import websockets
from websockets.exceptions import ConnectionClosed

from visualizer_protocol import (
    VisualizerProtocolError,
    build_message,
    parse_message,
    parse_rep_message,
    serialize_message,
)


LOGGER = logging.getLogger("visualizer_gateway")
LOOPBACK_HOST = "127.0.0.1"
MAX_PUBLISHER_LINE_BYTES = 16 * 1024
VISUALIZER_SENT_COLOR = "\033[38;5;135m"  # purple
COLOR_RESET = "\033[0m"


class VisualizerColorFormatter(logging.Formatter):
    """Highlight successful Unity deliveries when writing to a real terminal."""

    def __init__(self, *args: Any, use_color: bool, **kwargs: Any) -> None:
        """Remember whether the output stream supports interactive ANSI colour."""
        super().__init__(*args, **kwargs)
        self.use_color = use_color

    def format(self, record: logging.LogRecord) -> str:
        """Colour only the complete ``VISUALIZER sent`` line purple."""
        formatted = super().format(record)
        if self.use_color and record.getMessage().startswith("VISUALIZER sent "):
            return f"{VISUALIZER_SENT_COLOR}{formatted}{COLOR_RESET}"
        return formatted


@dataclass(frozen=True)
class GatewayConfig:
    """Immutable listener, rate-limit, TLS, and optional demo-source settings."""

    websocket_port: int = 9000
    publisher_port: int = 9100
    max_rate_hz: float = 20.0
    cert_file: str = ""
    key_file: str = ""
    demo_source: bool = False
    control_port: int = 9101


class LatestValue:
    """Single-slot delivery that replaces an obsolete unsent update.

    ``latest`` survives visualizer disconnects, allowing a reconnecting phone
    to receive the newest known state without accumulating history.
    """

    def __init__(self) -> None:
        """Create the single pending-update slot and retained latest value."""
        self._queue: asyncio.Queue[str] = asyncio.Queue(maxsize=1)
        self.latest: str | None = None

    def publish(self, value: str) -> None:
        """Replace any unsent update because only current exercise state matters."""
        # Keep a copy outside the queue so a future subscriber can immediately
        # receive the newest state even if nobody was connected when it arrived.
        self.latest = value
        if self._queue.full():
            # Exercise updates describe current state; an unsent older value is
            # obsolete once a newer value exists.
            self._queue.get_nowait()
        self._queue.put_nowait(value)

    def subscribe(self) -> asyncio.Queue[str]:
        """Create the sole subscriber queue, seeded with the latest known state."""
        # Version 1 permits one visualizer, so replacing this queue cannot orphan
        # another legitimate subscriber.
        queue: asyncio.Queue[str] = asyncio.Queue(maxsize=1)
        if self.latest is not None:
            queue.put_nowait(self.latest)
        self._queue = queue
        return queue


def create_server_ssl_context(cert_file: str, key_file: str) -> ssl.SSLContext:
    """Build WSS TLS, failing closed on missing or invalid key material.

    TLS continues end to end through the SSH tunnel and opaque laptop relay, so
    it remains mandatory even though this listener itself is loopback-only.
    """
    if not cert_file or not key_file:
        raise ValueError("WSS requires both --tls-cert and --tls-key")
    cert_path = Path(cert_file)
    key_path = Path(key_file)
    if not cert_path.is_file() or not key_path.is_file():
        raise ValueError("TLS certificate or private-key file does not exist")

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    try:
        context.load_cert_chain(certfile=cert_path, keyfile=key_path)
    except (OSError, ssl.SSLError) as error:
        raise ValueError(f"invalid TLS certificate/private key: {error}") from error
    return context


class VisualizerGateway:
    """Bridge local exercise events to one rate-limited, TLS-protected Unity client.

    The sensor server and gateway remain separate processes: ``server.py`` can
    publish short newline-delimited JSON messages locally, while this component
    owns WebSocket/TLS lifecycle, latest-only buffering, and phone reconnects.
    """

    def __init__(self, config: GatewayConfig) -> None:
        """Validate configuration and initialize listener/client bookkeeping."""
        if config.max_rate_hz <= 0:
            raise ValueError("maximum update rate must be greater than zero")
        self.config = config
        self.updates = LatestValue()
        self._websocket_server: Any = None
        self._publisher_server: asyncio.AbstractServer | None = None
        self._active_client: Any = None
        self._client_lock = asyncio.Lock()
        self._demo_task: asyncio.Task[None] | None = None

    @property
    def websocket_port(self) -> int:
        """Return the actual WSS port, including an OS-assigned test port."""
        return self._websocket_server.sockets[0].getsockname()[1]

    @property
    def publisher_port(self) -> int:
        """Return the actual local publisher port after the gateway starts."""
        assert self._publisher_server is not None
        return self._publisher_server.sockets[0].getsockname()[1]

    async def start(self) -> None:
        """Start the loopback WSS and publisher listeners plus optional demo task."""
        # TLS terminates here on the Ultra96. The private key is never copied to
        # the laptop relay or exposed by the loopback publisher interface.
        ssl_context = create_server_ssl_context(
            self.config.cert_file, self.config.key_file
        )
        self._websocket_server = await websockets.serve(
            self._handle_visualizer,
            LOOPBACK_HOST,
            self.config.websocket_port,
            ssl=ssl_context,
            max_size=1024,
            max_queue=8,
            ping_interval=20,
            ping_timeout=20,
        )
        # Publisher JSON is plaintext only on Ultra96 loopback. The reader limit
        # bounds memory before an input line reaches JSON parsing.
        self._publisher_server = await asyncio.start_server(
            self._handle_publisher,
            LOOPBACK_HOST,
            self.config.publisher_port,
            limit=MAX_PUBLISHER_LINE_BYTES + 1,
        )
        if self.config.demo_source:
            self._demo_task = asyncio.create_task(
                self._run_demo_source(), name="visualizer-demo-source"
            )
        LOGGER.info(
            "WSS listening on %s:%d; publisher listening on %s:%d",
            LOOPBACK_HOST,
            self.websocket_port,
            LOOPBACK_HOST,
            self.publisher_port,
        )

    async def close(self) -> None:
        """Stop child tasks and listeners without leaving pending asyncio work."""
        if self._demo_task is not None:
            self._demo_task.cancel()
            await asyncio.gather(self._demo_task, return_exceptions=True)
            self._demo_task = None
        if self._active_client is not None:
            # A clean close lets Unity reconnect immediately instead of waiting
            # for its keepalive timeout.
            await self._active_client.close(code=1001, reason="gateway stopping")
        if self._websocket_server is not None:
            self._websocket_server.close()
            await self._websocket_server.wait_closed()
            self._websocket_server = None
        if self._publisher_server is not None:
            self._publisher_server.close()
            await self._publisher_server.wait_closed()
            self._publisher_server = None

    def publish(self, message: dict[str, Any]) -> None:
        """Validate, serialize, and expose one update to the active subscriber."""
        serialized = serialize_message(message)
        self.updates.publish(serialized)
        LOGGER.debug(
            "Accepted update run_id=%d seq=%d", message["run_id"], message["seq"]
        )

    async def _handle_visualizer(self, websocket: Any, _path: str = "") -> None:
        """Serve one Unity client with latest-only, rate-limited delivery."""
        peer = websocket.remote_address
        async with self._client_lock:
            # A single phone avoids competing consumers and ambiguous reconnect
            # state in this version of the gateway.
            if self._active_client is not None:
                LOGGER.warning("Rejecting additional Unity visualizer from %s", peer)
                await websocket.close(code=1013, reason="one visualizer is already active")
                return
            self._active_client = websocket

        LOGGER.info("Unity visualizer connected from %s", peer)
        queue = self.updates.subscribe()
        responses = asyncio.Queue(maxsize=32)
        control_writer = None
        request_times = deque()

        async def respond(message):
            try:
                responses.put_nowait(json.dumps(message, separators=(",", ":")))
            except asyncio.QueueFull:
                await websocket.close(code=1013, reason="rep response queue full; reconnect and sync")

        async def relay_control():
            nonlocal control_writer
            while not websocket.closed:
                writer = None
                connected = False
                try:
                    reader, writer = await asyncio.wait_for(asyncio.open_connection(
                        LOOPBACK_HOST, self.config.control_port, limit=4096), 1)
                    line = await asyncio.wait_for(reader.readline(), 1)
                    welcome = json.loads(line)
                    if welcome.get("type") != "rep_welcome" or welcome.get("v") != 1:
                        raise ValueError("invalid rep control welcome")
                    control_writer = writer
                    connected = True
                    await respond(welcome)
                    while True:
                        line = await reader.readline()
                        if not line or len(line) > 4096:
                            raise ConnectionError("rep control disconnected")
                        message = json.loads(line)
                        if message.get("type") not in ("rep_result", "rep_delivery") or message.get("v") != 1:
                            raise ValueError("invalid rep control response")
                        await respond(message)
                except (OSError, ValueError, asyncio.TimeoutError) as error:
                    LOGGER.debug("Rep control unavailable: %s", error)
                    if connected:
                        await websocket.close(code=1012, reason="rep server changed; reconnect and sync")
                        return
                    await asyncio.sleep(1)
                finally:
                    control_writer = None
                    if writer is not None:
                        writer.close()
                        with contextlib.suppress(OSError):
                            await writer.wait_closed()

        async def receive_phone():
            async for payload in websocket:
                try:
                    now = time.monotonic()
                    while request_times and now - request_times[0] >= 1:
                        request_times.popleft()
                    if len(request_times) >= 20:
                        await websocket.close(code=1008, reason="phone message rate_limit")
                        return
                    request_times.append(now)
                    message = parse_rep_message(payload)
                    if control_writer is None:
                        raise VisualizerProtocolError("rep control unavailable")
                    # No replay queue: forward only on this live phone/control link.
                    control_writer.write((json.dumps(message, separators=(",", ":")) + "\n").encode())
                    await asyncio.wait_for(control_writer.drain(), 0.5)
                except VisualizerProtocolError as error:
                    await respond({"v": 1, "type": "rep_result", "run_id": 0,
                        "seq": -1, "rep_session_id": "", "rep_index": -1,
                        "status": "rejected", "reason": str(error),
                        "feedback_event_id": 0, "nodes": []})
                except (OSError, asyncio.TimeoutError):
                    await websocket.close(code=1012, reason="rep control unavailable; reconnect and sync")
                    return

        async def send_messages():
            # Exactly one WSS writer; receipts never enter the replaceable
            # exercise slot. Outstanding response memory is bounded to 32 items.
            update = asyncio.create_task(queue.get())
            receipt = asyncio.create_task(responses.get())
            last_send = 0.0
            try:
                while True:
                    await asyncio.wait((update, receipt), return_when=asyncio.FIRST_COMPLETED)
                    if receipt.done():
                        await websocket.send(receipt.result())
                        receipt = asyncio.create_task(responses.get())
                    if update.done():
                        payload = update.result()
                        delay = 1.0 / self.config.max_rate_hz - (time.monotonic() - last_send)
                        # Exercise throttling must not delay rep receipts.
                        deadline = time.monotonic() + max(0, delay)
                        while time.monotonic() < deadline:
                            ready, _ = await asyncio.wait((receipt,),
                                timeout=max(0, deadline - time.monotonic()))
                            if receipt in ready:
                                await websocket.send(receipt.result())
                                receipt = asyncio.create_task(responses.get())
                        while not queue.empty():
                            payload = queue.get_nowait()
                        message = parse_message(payload)
                        await websocket.send(payload)
                        last_send = time.monotonic()
                        LOGGER.info("VISUALIZER sent run_id=%d seq=%d exercise=%s",
                                    message["run_id"], message["seq"], message["exercise"])
                        update = asyncio.create_task(queue.get())
            finally:
                update.cancel()
                receipt.cancel()
                await asyncio.gather(update, receipt, return_exceptions=True)

        tasks = [asyncio.create_task(function()) for function in
                 (relay_control, receive_phone, send_messages, websocket.wait_closed)]
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                if not task.cancelled():
                    error = task.exception()
                    if error and not isinstance(error, ConnectionClosed):
                        LOGGER.warning("Visualizer connection error: %s", error)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await websocket.close()
            async with self._client_lock:
                if self._active_client is websocket:
                    self._active_client = None
            LOGGER.info("Unity visualizer disconnected from %s", peer)

    async def _handle_publisher(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Accept bounded newline-delimited JSON from one local producer."""
        peer = writer.get_extra_info("peername")
        # Uses one short connection per event, so lifecycle messages
        # are DEBUG-only; accepted sends and rejection warnings stay visible.
        LOGGER.debug("Local publisher connected from %s", peer)
        try:
            while True:
                try:
                    # readline handles fragmented TCP input and multiple events
                    # coalesced into one read while preserving message boundaries.
                    line = await reader.readline()
                except ValueError:
                    LOGGER.warning("Rejected oversized publisher input from %s", peer)
                    return
                if not line:
                    return
                if len(line) > MAX_PUBLISHER_LINE_BYTES or not line.endswith(b"\n"):
                    LOGGER.warning("Rejected oversized publisher input from %s", peer)
                    return
                try:
                    message = parse_message(line)
                    self.publish(message)
                except VisualizerProtocolError as error:
                    # Reject only this event; a long-lived producer may continue
                    # with another valid line on the same connection.
                    LOGGER.warning("Rejected publisher update from %s: %s", peer, error)
        finally:
            writer.close()
            with contextlib.suppress(ConnectionError, OSError):
                await writer.wait_closed()
            LOGGER.debug("Local publisher disconnected from %s", peer)

    async def _run_demo_source(self) -> None:
        """Publish deterministic dummy exercises for isolated gateway/UI testing."""
        # Deterministic blocks remain available for isolated phone/gateway tests;
        # the sensor server uses the publisher interface instead.
        choices = (
            "unknown",
            "squat",
            "push_up",
            "sit_up",
            "lunge",
            "leg_raise",
        )
        run_id = 1
        seq = 0
        start_ns = time.monotonic_ns()
        while True:
            # Fifty events per label creates predictable five-second blocks at
            # 10 Hz for UI, stale-state, and reconnection testing.
            exercise = choices[(seq // 50) % len(choices)]
            self.publish(
                build_message(
                    run_id=run_id,
                    seq=seq,
                    ts_us=(time.monotonic_ns() - start_ns) // 1000,
                    exercise=exercise,
                )
            )
            seq += 1
            await asyncio.sleep(0.1)


def _port(value: str) -> int:
    """Validate ports while allowing zero for ephemeral integration-test ports."""
    port = int(value)
    if not 0 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 0 and 65535")
    return port


def _positive_rate(value: str) -> float:
    """Reject disabled or negative output rates during argument parsing."""
    rate = float(value)
    if rate <= 0:
        raise argparse.ArgumentTypeError("rate must be greater than zero")
    return rate


def parse_args() -> argparse.Namespace:
    """Read TLS paths, listener ports, rate limit, demo mode, and log level."""
    parser = argparse.ArgumentParser(description="Ultra96 WSS visualizer gateway")
    parser.add_argument("--tls-cert", required=True, help="server certificate PEM")
    parser.add_argument("--tls-key", required=True, help="server private-key PEM")
    parser.add_argument("--websocket-port", type=_port, default=9000)
    parser.add_argument("--publisher-port", type=_port, default=9100)
    parser.add_argument("--max-rate-hz", type=_positive_rate, default=20.0)
    parser.add_argument("--demo-source", action="store_true")
    parser.add_argument("--control-port", type=_port, default=9101)
    parser.add_argument(
        "--log-level", choices=("DEBUG", "INFO", "WARNING", "ERROR"), default="INFO"
    )
    return parser.parse_args()


async def run(config: GatewayConfig) -> None:
    """Run until cancelled and always close listeners and child tasks."""
    gateway = VisualizerGateway(config)
    await gateway.start()
    try:
        await asyncio.Future()
    finally:
        await gateway.close()


def main() -> None:
    """Build gateway configuration, configure concise logs, and run the service."""
    args = parse_args()
    console = logging.StreamHandler()
    console.setFormatter(VisualizerColorFormatter(
        "%(asctime)s %(levelname)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        use_color=console.stream.isatty(),
    ))
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        # This executable has one operator-facing component, so the formatter
        # omits its logger name and highlights only successful Unity deliveries.
        handlers=[console],
    )
    # The gateway emits clearer application-level lifecycle messages. Keep the
    # library's warnings/errors, but hide its duplicate open/closed INFO lines.
    logging.getLogger("websockets.server").setLevel(logging.WARNING)
    config = GatewayConfig(
        websocket_port=args.websocket_port,
        publisher_port=args.publisher_port,
        max_rate_hz=args.max_rate_hz,
        cert_file=os.path.abspath(args.tls_cert),
        key_file=os.path.abspath(args.tls_key),
        demo_source=args.demo_source,
        control_port=args.control_port,
    )
    try:
        asyncio.run(run(config))
    except (ValueError, OSError) as error:
        raise SystemExit(f"Gateway startup failed: {error}") from error
    except KeyboardInterrupt:
        LOGGER.info("Gateway stopped")


if __name__ == "__main__":
    main()
