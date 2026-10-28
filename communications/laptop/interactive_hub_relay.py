"""Protocol-aware individual-demo relay with interactive diagnostics.

This is deliberately separate from tcp_relay.py, whose transparent behavior
remains unchanged. It supports TEST_REQUEST, benchmark control, and bounded
streaming file-transfer injection over the established TLS session.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import hashlib
import random
import re
import socket
import ssl
import struct
import sys
import time
import zlib
from pathlib import Path
from dataclasses import dataclass

LOGGER = logging.getLogger("interactive_hub_relay")
HEADER_FORMAT = "!HBBBBHIIQ"
IMU_FORMAT = "!hhhhhhH"
PHYSICAL_IMU_FORMAT = "!ffffffH"
ACTIVITY_TRIGGER_FORMAT = "!IIQ"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
IMU_SIZE = struct.calcsize(IMU_FORMAT)
PHYSICAL_IMU_SIZE = struct.calcsize(PHYSICAL_IMU_FORMAT)
ACTIVITY_TRIGGER_SIZE = struct.calcsize(ACTIVITY_TRIGGER_FORMAT)
MAGIC = 0xA55A
VERSION = 1
TEST_REQUEST = 0x20
TEST_RESPONSE = 0x21
ACTIVITY_TRIGGER = 0x05
ARM = 0x0A
DATA = 0x10
PHYSICAL_DATA = 0x12
BENCHMARK_CONTROL = 0x22
BENCHMARK_REPORT = 0x23
FILE_BEGIN = 0x30
FILE_CHUNK = 0x31
FILE_END = 0x32
FILE_RESULT = 0x33
HELLO = 0x01
HELLO_ACK = 0x02
ERROR = 0x7F
STATUS_MOCK_DATA = 0x8000
STATUS_TEST_REQUEST_PROCESSED = 0x2000
STATUS_BENCHMARK = 0x1000
STATUS_SOURCE_UNAVAILABLE = 0x4000
STATUS_SENSOR_VALID = 0x0001
STATUS_HISTORICAL = 0x0002
STATUS_HISTORY_OVERWRITE = 0x0004
MAX_PAYLOAD_SIZE = 1024
STATS_INTERVAL_SECONDS = 5.0
FILE_CHUNK_DATA_SIZE = 900

ERROR_NAMES = {
    4: "INVALID_SESSION",
    5: "UNAUTHORIZED_NODE",
    6: "INVALID_FRAME",
    7: "UNEXPECTED_MESSAGE",
    8: "SEQUENCE_REUSE",
}

MESSAGE_TYPE_NAMES = {
    HELLO: "HELLO",
    HELLO_ACK: "HELLO_ACK",
    ACTIVITY_TRIGGER: "ACTIVITY_TRIGGER",
    ARM: "ARM",
    DATA: "DATA",
    PHYSICAL_DATA: "PHYSICAL_DATA",
    TEST_REQUEST: "TEST_REQUEST",
    TEST_RESPONSE: "TEST_RESPONSE",
    BENCHMARK_CONTROL: "BENCHMARK_CONTROL",
    BENCHMARK_REPORT: "BENCHMARK_REPORT",
    FILE_BEGIN: "FILE_BEGIN",
    FILE_CHUNK: "FILE_CHUNK",
    FILE_END: "FILE_END",
    FILE_RESULT: "FILE_RESULT",
    ERROR: "ERROR",
}


class HubColorFormatter(logging.Formatter):
    """Color each complete single-node log line with a stable node color."""

    COLORS = {1: "\033[38;5;208m", 2: "\033[34m"}  # orange, blue
    RESET = "\033[0m"

    def __init__(self, fmt: str, use_color: bool,
                 datefmt: str | None = None) -> None:
        """Configure ordinary log formatting and optional terminal colors."""
        super().__init__(fmt, datefmt=datefmt)
        self.use_color = use_color

    def format(self, record: logging.LogRecord) -> str:
        """Color a line only when it refers unambiguously to Node 1 or Node 2."""
        rendered = super().format(record)
        if not self.use_color:
            return rendered
        matches = {
            int(value)
            for value in re.findall(r"(?:\bnode=|\bNode )(1|2)\b", rendered)
        }
        if len(matches) == 1:
            rendered = f"{self.COLORS[matches.pop()]}{rendered}{self.RESET}"
        return rendered

# Editable complete IMU payloads: ax, ay, az, gx, gy, gz, status.
DUMMY_PAYLOADS = (
    ( 1001, -2001,  100,  10, -20,  30, STATUS_MOCK_DATA),
    (-2001,  3001,  400, -20,  30, -40, STATUS_MOCK_DATA),
    ( 3001, -4001,  500,  30, -40,  50, STATUS_MOCK_DATA),
    (-4001,  5001,  600, -50,  60,  70, STATUS_MOCK_DATA),
    # Boundary case: verifies that FireBeetle transformation uses saturation.
    (32760, -32760, 0, 32700, -32700, 1, STATUS_MOCK_DATA),
)


def describe_status(status: int) -> str:
    """Translate known IMU status bits into compact names for terminal output."""
    names = []
    if status & STATUS_MOCK_DATA:
        names.append("mock_data")
    if status & STATUS_TEST_REQUEST_PROCESSED:
        names.append("test_request_processed")
    if status & STATUS_BENCHMARK:
        names.append("benchmark")
    if status & STATUS_SOURCE_UNAVAILABLE:
        names.append("source_unavailable")
    if status & STATUS_SENSOR_VALID:
        names.append("sensor_valid")
    if status & STATUS_HISTORICAL:
        names.append("historical")
    if status & STATUS_HISTORY_OVERWRITE:
        names.append("history_overwrite")
    return "|".join(names) if names else "none"


@dataclass
class ControllerState:
    """Track protocol state owned by one active hub Session.

    The cached frame supports deliberate replay tests, while ``pending`` maps
    TEST_REQUEST sequences to their send time and payload for latency reporting.
    """

    session_id: int = 0
    hub_node_id: int = 0
    next_sequence: int = 1
    pending: dict[int, tuple[float, tuple[int, ...]]] | None = None
    last_sequence: int = 0
    last_payload: tuple[int, ...] | None = None
    last_frame: bytes | None = None
    benchmark_active: bool = False

    def __post_init__(self) -> None:
        """Give each hub its own pending-request dictionary."""
        if self.pending is None:
            self.pending = {}


@dataclass
class StreamStats:
    """Accumulate DATA or PHYSICAL_DATA health and throughput counters."""

    started_at: float | None = None
    window_started_at: float | None = None
    total_frames: int = 0
    total_bytes: int = 0
    window_frames: int = 0
    window_bytes: int = 0
    sequence_gaps: int = 0
    duplicates: int = 0
    out_of_order: int = 0
    timestamp_regressions: int = 0
    last_sequence: int | None = None
    last_timestamp_us: int | None = None
    latest_payload: tuple[int, ...] | None = None
    latest_message_type: int | None = None

    def record(self, header: tuple[int, ...], frame: bytes,
               now: float | None = None) -> None:
        """Record one DATA frame using wrap-safe 32-bit sequence arithmetic."""
        now = time.monotonic() if now is None else now
        if self.started_at is None:
            self.started_at = now
            self.window_started_at = now

        sequence = header[7]
        timestamp_us = header[8]
        if self.last_sequence is not None:
            forward = (sequence - self.last_sequence) & 0xFFFFFFFF
            if forward == 0:
                self.duplicates += 1
            elif forward < 0x80000000:
                self.sequence_gaps += forward - 1
            else:
                self.out_of_order += 1
        if self.last_timestamp_us is not None and timestamp_us <= self.last_timestamp_us:
            self.timestamp_regressions += 1

        self.last_sequence = sequence
        self.last_timestamp_us = timestamp_us
        message_type = header[2]
        payload_format = (
            PHYSICAL_IMU_FORMAT if message_type == PHYSICAL_DATA else IMU_FORMAT
        )
        self.latest_payload = struct.unpack(payload_format, frame[HEADER_SIZE:])
        self.latest_message_type = message_type
        self.total_frames += 1
        self.total_bytes += len(frame)
        self.window_frames += 1
        self.window_bytes += len(frame)

    def report(self, now: float | None = None) -> dict[str, float | int]:
        """Return lifetime totals and current-window rates, then reset the window."""
        now = time.monotonic() if now is None else now
        started_at = self.started_at if self.started_at is not None else now
        window_started = (
            self.window_started_at if self.window_started_at is not None else now
        )
        duration = max(now - started_at, 0.000001)
        window_duration = max(now - window_started, 0.000001)
        result = {
            "duration": duration,
            "current_kbps": self.window_bytes * 8 / window_duration / 1000,
            "average_kbps": self.total_bytes * 8 / duration / 1000,
            "current_pps": self.window_frames / window_duration,
            "total_frames": self.total_frames,
            "total_bytes": self.total_bytes,
            "sequence_gaps": self.sequence_gaps,
            "duplicates": self.duplicates,
            "out_of_order": self.out_of_order,
            "timestamp_regressions": self.timestamp_regressions,
        }
        self.window_started_at = now
        self.window_frames = 0
        self.window_bytes = 0
        return result


@dataclass
class HubConnection:
    """Group one ready hub's Session state, writer lock, and stream counters."""

    state: ControllerState
    writer: asyncio.StreamWriter
    write_lock: asyncio.Lock
    stream_stats: StreamStats
    benchmark_stats: StreamStats
    last_stream_report: dict[str, float | int] | None = None


class FrameObserver:
    """Reassemble copied bytes into frames for inspection without changing them.

    TCP reads may split one frame or combine several frames. The observer keeps
    incomplete bytes until a full validated protocol frame can be delivered.
    """

    def __init__(self, callback) -> None:
        """Create an empty receive buffer and register a completed-frame callback."""
        self.buffer = bytearray()
        self.callback = callback

    def feed(self, data: bytes) -> None:
        """Append a TCP fragment and emit every complete frame now available."""
        self.buffer.extend(data)
        while len(self.buffer) >= HEADER_SIZE:
            header = struct.unpack_from(HEADER_FORMAT, self.buffer)
            payload_length = header[5]
            if header[0] != MAGIC or header[1] != VERSION:
                raise ValueError("invalid protocol header")
            if payload_length > MAX_PAYLOAD_SIZE:
                raise ValueError("payload exceeds protocol limit")
            frame_length = HEADER_SIZE + payload_length
            if len(self.buffer) < frame_length:
                return
            frame = bytes(self.buffer[:frame_length])
            del self.buffer[:frame_length]
            self.callback(header, frame)


def build_test_request(session_id: int, sequence: int, payload: tuple[int, ...]) -> bytes:
    """Build one TEST_REQUEST carrying the same binary IMU shape as DATA."""
    timestamp_us = time.monotonic_ns() // 1000
    return struct.pack(
        HEADER_FORMAT,
        MAGIC, VERSION, TEST_REQUEST, 0, 0, IMU_SIZE,
        session_id, sequence, timestamp_us,
    ) + struct.pack(IMU_FORMAT, *payload)


def build_benchmark_control(session_id: int, sequence: int, action: int,
                            batch_frames: int = 16,
                            duration_ms: int = 10000) -> bytes:
    """Build a BENCHMARK_CONTROL frame that starts or stops hub traffic."""
    payload = struct.pack("!BBHI", action, 0, batch_frames, duration_ms)
    return struct.pack(
        HEADER_FORMAT, MAGIC, VERSION, BENCHMARK_CONTROL, 0, 0, len(payload),
        session_id, sequence, time.monotonic_ns() // 1000,
    ) + payload


def build_file_frames(session_id: int, sequence: int, data: bytes,
                      file_id: int | None = None, chunk_size: int = FILE_CHUNK_DATA_SIZE,
                      corrupt_crc: bool = False,
                      wrong_offset: bool = False) -> tuple[int, bytes, list[bytes], bytes, bytes]:
    """Split a bounded file into BEGIN, CRC-protected CHUNK, and END frames.

    ``corrupt_crc`` and ``wrong_offset`` deliberately damage the first chunk so
    the professor demo can show that the receiver rejects malformed transfers.
    """
    if not data or not 0 < chunk_size <= MAX_PAYLOAD_SIZE - 16:
        raise ValueError("file data and chunk size must be valid")
    file_id = file_id or random.randint(1, 0xFFFFFFFF)
    digest = hashlib.sha256(data).digest()
    begin_payload = struct.pack("!IIHH32s", file_id, len(data), chunk_size, 0, digest)
    begin = build_header(FILE_BEGIN, session_id, sequence, begin_payload)
    chunks = []
    for index, offset in enumerate(range(0, len(data), chunk_size)):
        chunk = data[offset:offset + chunk_size]
        wire_offset = offset + (1 if wrong_offset and index == 0 else 0)
        crc = zlib.crc32(chunk) & 0xFFFFFFFF
        if corrupt_crc and index == 0:
            crc ^= 1
        payload = struct.pack("!IIHHI", file_id, wire_offset, len(chunk), 0, crc) + chunk
        chunks.append(build_header(FILE_CHUNK, session_id, sequence + index + 1, payload))
    end_payload = struct.pack("!II32s", file_id, len(data), digest)
    end = build_header(FILE_END, session_id, sequence + len(chunks) + 1, end_payload)
    return file_id, begin, chunks, end, digest


def build_header(message_type: int, session_id: int, sequence: int,
                 payload: bytes) -> bytes:
    """Wrap a payload in the protocol's common network-byte-order header."""
    return struct.pack(
        HEADER_FORMAT, MAGIC, VERSION, message_type, 0, 0, len(payload),
        session_id, sequence, time.monotonic_ns() // 1000,
    ) + payload


class InteractiveHubRelay:
    """Terminate hub TLS while relaying and injecting protocol-aware demo frames.

    Hub traffic still flows transparently to the Ultra96. Frame observers learn
    each hub's Session and collect diagnostics, while keyboard actions write
    additional requests through the same serialized hub connection.
    """

    def __init__(self, listen_host: str, listen_port: int,
                 upstream_host: str, upstream_port: int,
                 certificate: str | None = None,
                 private_key: str | None = None,
                 file_path: str | None = None) -> None:
        """Store endpoints and initialize the active-hub and task registries."""
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.upstream_host = upstream_host
        self.upstream_port = upstream_port
        self.certificate = certificate
        self.private_key = private_key
        self.file_path = Path(file_path) if file_path else None
        self.server: asyncio.AbstractServer | None = None
        self.stop_event = asyncio.Event()
        self.hubs: dict[int, HubConnection] = {}
        self.selected_hub: int | None = None
        self.last_request_hub: int | None = None
        self.file_tasks: dict[int, asyncio.Task] = {}

    async def run(self) -> None:
        """Create the optional TLS listener and coordinate keyboard-driven shutdown."""

        # Terminate authenticated hub TLS at the laptop relay.
        tls_context = None
        if self.certificate or self.private_key:
            if not self.certificate or not self.private_key:
                raise ValueError("both --certificate and --private-key are required")
            tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)

            # Reject TLS versions older than TLS 1.2.
            tls_context.minimum_version = ssl.TLSVersion.TLSv1_2

            # Load the laptop certificate and its private key.
            tls_context.load_cert_chain(self.certificate, self.private_key)

        self.server = await asyncio.start_server(
            self.handle_connection, self.listen_host, self.listen_port,
            ssl=tls_context, ssl_handshake_timeout=5.0)
        
        LOGGER.info("Listening with %s on %s:%d; forwarding to %s:%d",
                    "TLS" if tls_context else "plain TCP",
                    self.listen_host, self.listen_port,
                    self.upstream_host, self.upstream_port)
        LOGGER.info("Keyboard controls (the selected node stays selected until changed):")
        LOGGER.info("  Node target: [1] select Node 1  [2] select Node 2  [a] choose a random node")
        LOGGER.info("  Request:     [any other key] send test request   [d] resend duplicate   [c] send conflicting replay")
        LOGGER.info("  File test:   [f] valid transfer   [r] bad CRC   [o] bad offset   [t] timeout")
        LOGGER.info("  Benchmark:   [b] start selected   [x] stop selected   [m] start all   [n] stop all")
        LOGGER.info("  Controller:  [q] quit")
        async with self.server:
            await self.server.start_serving()
            keyboard_task = asyncio.create_task(self.keyboard())
            try:
                await self.stop_event.wait()
            finally:
                keyboard_task.cancel()
                await asyncio.gather(keyboard_task, return_exceptions=True)
                for task in self.file_tasks.values():
                    task.cancel()
                await asyncio.gather(*self.file_tasks.values(),
                                     return_exceptions=True)
                self.file_tasks.clear()
                for hub in list(self.hubs.values()):
                    hub.writer.close()

    def choose_hub(self) -> HubConnection | None:
        """Return the manually selected hub, or a random ready hub in auto mode."""
        ready = list(self.hubs.values())
        if not ready:
            return None
        if self.selected_hub is not None:
            return self.hubs.get(self.selected_hub)
        return random.choice(ready)

    async def send_benchmark(self, hub: HubConnection, action: int) -> None:
        """Send benchmark start/stop control and initialize local measurements."""
        state = hub.state
        sequence = state.next_sequence
        state.next_sequence = (sequence + 1) & 0xFFFFFFFF
        if action:
            hub.benchmark_stats = StreamStats()
            state.benchmark_active = True
        frame = build_benchmark_control(state.session_id, sequence, action)

        # Serialize keyboard and file-transfer frames with frames concurrently forwarded 
        # from Ultra96 to this FireBeetle.
        async with hub.write_lock:
            hub.writer.write(frame)
            await hub.writer.drain()

        LOGGER.info("BENCHMARK_%s node=%d sequence=%d",
                    "START" if action else "STOP", state.hub_node_id, sequence)

    async def send_file(self, hub: HubConnection, key: str) -> None:
        """Send a valid or deliberately faulty file-transfer test to one hub."""
        if self.file_path is None:
            LOGGER.warning("No --file was configured")
            return
        state = hub.state
        LOGGER.info("FILE_PREPARING node=%d path=%s",
                    state.hub_node_id, self.file_path.resolve())
        try:
            data = await asyncio.to_thread(self.file_path.read_bytes)
        except OSError as error:
            LOGGER.error("FILE_OPEN_FAILED node=%d path=%s: %s",
                         state.hub_node_id, self.file_path.resolve(), error)
            return
        sequence = state.next_sequence
        file_id, begin, chunks, end, digest = build_file_frames(
            state.session_id, sequence, data,
            corrupt_crc=key == "r", wrong_offset=key == "o")
        state.next_sequence = (sequence + len(chunks) + 2) & 0xFFFFFFFF
        LOGGER.info("FILE_BEGIN node=%d id=%d bytes=%d chunks=%d sha256=%s",
                    state.hub_node_id, file_id, len(data), len(chunks), digest.hex())
        try:
            async with hub.write_lock:
                hub.writer.write(begin)
                await hub.writer.drain()
                sent_chunks = chunks if key == "f" else [] if key == "t" else chunks[:1]
                for index, frame in enumerate(sent_chunks):
                    hub.writer.write(frame)
                    await hub.writer.drain()
                    await asyncio.sleep(0.02)
                    if (index + 1) % 32 == 0:
                        LOGGER.info("FILE_PROGRESS node=%d id=%d chunks=%d/%d",
                                    state.hub_node_id, file_id, index + 1,
                                    len(sent_chunks))
                if key == "f":
                    hub.writer.write(end)
                    await hub.writer.drain()
            if key == "t":
                LOGGER.info("FILE_TIMEOUT_TEST node=%d id=%d: FILE_BEGIN only",
                            state.hub_node_id, file_id)
        except (ConnectionError, OSError) as error:
            LOGGER.warning("FILE_SEND_ABORTED node=%d id=%d connection lost: %s",
                           state.hub_node_id, file_id, error)

    def start_file(self, hub: HubConnection, key: str) -> None:
        """Start at most one background file operation for the selected hub."""
        node_id = hub.state.hub_node_id
        if node_id in self.file_tasks:
            LOGGER.warning("Node %d already has a file operation in progress",
                           node_id)
            return

        task = asyncio.create_task(self.send_file(hub, key))
        self.file_tasks[node_id] = task

        def file_finished(completed: asyncio.Task) -> None:
            """Remove completed file-task bookkeeping and surface failures."""
            if self.file_tasks.get(node_id) is completed:
                del self.file_tasks[node_id]
            if completed.cancelled():
                return
            error = completed.exception()
            if error is not None:
                LOGGER.error("File operation failed for Node %d: %s",
                             node_id, error)

        task.add_done_callback(file_finished)

    async def send_test_request(self, hub: HubConnection, key: str) -> None:
        """Send a new, exactly duplicated, or conflicting TEST_REQUEST frame."""
        state = hub.state
        if key in ("d", "c") and state.last_frame is None:
            LOGGER.warning(
                "No TEST_REQUEST is cached for Node %d in the current Session",
                state.hub_node_id)
            return
        if key == "d" and state.last_frame is not None:
            payload = state.last_payload
            sequence = state.last_sequence
            frame = state.last_frame
            LOGGER.info("Sending exact duplicate: node=%d sequence=%d",
                        state.hub_node_id, sequence)
        elif key == "c" and state.last_payload is not None:
            payload = next(item for item in DUMMY_PAYLOADS
                           if item != state.last_payload)
            sequence = state.last_sequence
            frame = build_test_request(state.session_id, sequence, payload)
            LOGGER.info("Sending conflicting payload: node=%d sequence=%d",
                        state.hub_node_id, sequence)
        else:
            payload = random.choice(DUMMY_PAYLOADS)
            sequence = state.next_sequence
            state.next_sequence = (sequence + 1) & 0xFFFFFFFF
            frame = build_test_request(state.session_id, sequence, payload)
            state.last_sequence = sequence
            state.last_payload = payload
            state.last_frame = frame
            self.last_request_hub = state.hub_node_id
        state.pending[sequence] = (time.monotonic(), payload)
        async with hub.write_lock:
            hub.writer.write(frame)
            await hub.writer.drain()
        LOGGER.info(
            "TEST_REQUEST node=%d session=%d sequence=%d payload_index=%d "
            "accel=%s gyro=%s status=%s",
            state.hub_node_id, state.session_id, sequence,
            DUMMY_PAYLOADS.index(payload), payload[:3], payload[3:6],
            describe_status(payload[6]))

    async def keyboard(self) -> None:
        """Convert one-key commands into hub selection and protocol test actions."""
        while True:
            key = await asyncio.to_thread(read_one_key)
            if key.lower() == "q" or key == "\x03":
                LOGGER.info("Shutdown requested")
                self.stop_event.set()
                return
            if key in ("1", "2"):
                node_id = int(key)
                if node_id not in self.hubs:
                    LOGGER.warning("Node %d is not ready", node_id)
                else:
                    self.selected_hub = node_id
                    LOGGER.info(
                        "NODE_SELECTION mode=manual selected_node=%d", node_id)
                continue
            if key.lower() == "a":
                self.selected_hub = None
                LOGGER.info(
                    "NODE_SELECTION mode=automatic ready_nodes=%s",
                    sorted(self.hubs))
                continue
            if key.lower() in ("m", "n"):
                if not self.hubs:
                    LOGGER.warning("No ready nodes")
                    continue
                if self.file_tasks:
                    LOGGER.warning(
                        "Finish active file operations before controlling all benchmarks")
                    continue
                action = 1 if key.lower() == "m" else 0
                await asyncio.gather(*(self.send_benchmark(hub, action)
                                       for hub in list(self.hubs.values())))
                continue
            if key.lower() in ("d", "c") and self.last_request_hub is None:
                LOGGER.warning(
                    "No TEST_REQUEST is cached in any current Session")
                continue
            hub = (self.hubs.get(self.last_request_hub)
                   if key.lower() in ("d", "c")
                   else self.choose_hub())
            if hub is None:
                LOGGER.warning("No ready node for current selection")
                continue
            if key.lower() in ("f", "r", "o", "t"):
                self.start_file(hub, key.lower())
            elif hub.state.hub_node_id in self.file_tasks:
                LOGGER.warning(
                    "Node %d file operation is active; selection and quit remain available",
                    hub.state.hub_node_id)
            elif key.lower() in ("b", "x"):
                await self.send_benchmark(hub, 1 if key.lower() == "b" else 0)
            else:
                await self.send_test_request(hub, key.lower())

    async def handle_connection(self, hub_reader, hub_writer) -> None:
        """Relay one hub to Ultra96 and observe both directions for demo state."""
        upstream_reader, upstream_writer = await asyncio.open_connection(
            self.upstream_host, self.upstream_port)

        # Each hub owns a separate lock so forwarded frames and injected
        # commands cannot interleave on that hub's TLS stream.
        write_lock = asyncio.Lock()

        state = ControllerState()
        hub = HubConnection(state, hub_writer, write_lock,
                            StreamStats(), StreamStats())

        def observe_hub(header, frame) -> None:
            """Process complete hub-originated frames for diagnostics and results."""
            message_type, node_id, sequence = header[2], header[3], header[7]
            if message_type == HELLO:
                state.hub_node_id = node_id
            elif message_type == ACTIVITY_TRIGGER:
                if header[5] != ACTIVITY_TRIGGER_SIZE:
                    LOGGER.warning(
                        "ACTIVITY_TRIGGER_INVALID node=%d session=%d "
                        "sequence=%d payload_length=%d",
                        node_id, header[6], sequence, header[5])
                else:
                    boot_id, arm_generation, peak_timestamp_us = struct.unpack(
                        ACTIVITY_TRIGGER_FORMAT, frame[HEADER_SIZE:])
                    LOGGER.info(
                        "ACTIVITY_TRIGGER_RECEIVED node=%d session=%d "
                        "sequence=%d boot_id=%d arm_generation=%d "
                        "peak_timestamp_us=%d detected_timestamp_us=%d",
                        node_id, header[6], sequence, boot_id, arm_generation,
                        peak_timestamp_us, header[8])
            elif message_type in (DATA, PHYSICAL_DATA):
                status_index = 6
                payload_format = (
                    PHYSICAL_IMU_FORMAT
                    if message_type == PHYSICAL_DATA else IMU_FORMAT
                )
                values = struct.unpack(payload_format, frame[HEADER_SIZE:])
                (hub.benchmark_stats if values[status_index] & STATUS_BENCHMARK
                 else hub.stream_stats).record(header, frame)
            elif message_type == TEST_RESPONSE:
                values = struct.unpack(IMU_FORMAT, frame[HEADER_SIZE:])
                pending = state.pending.pop(sequence, None)
                latency_ms = (time.monotonic() - pending[0]) * 1000 if pending else -1
                LOGGER.info(
                    "TEST_RESPONSE node=%d session=%d sequence=%d latency=%.1fms "
                    "accel=%s gyro=%s status=%s",
                    node_id, header[6], sequence, latency_ms,
                    values[:3], values[3:6], describe_status(values[6]))
            elif message_type == ERROR:
                code, offending, _reserved = struct.unpack("!HBB", frame[HEADER_SIZE:])
                error_name = ERROR_NAMES.get(code, "UNKNOWN_ERROR")
                type_name = MESSAGE_TYPE_NAMES.get(offending, "UNKNOWN_TYPE")
                LOGGER.error(
                    "NODE_ERROR node=%d error=%s(%d) "
                    "offending_type=%s(0x%02X) sequence=%d",
                    state.hub_node_id, error_name, code,
                    type_name, offending, sequence)
            elif message_type == BENCHMARK_REPORT:
                values = struct.unpack("!IIIIII", frame[HEADER_SIZE:])
                state.benchmark_active = False
                received = hub.benchmark_stats.report()
                LOGGER.info(
                    "BENCHMARK_REPORT node=%d control_sequence=%d elapsed=%dms "
                    "frames=%d bytes=%d queue_drops=%d send_failures=%d",
                    state.hub_node_id, *values)
                LOGGER.info(
                    "BENCHMARK_RECEIVED node=%d frames=%d bytes=%d average=%.2fkbps "
                    "gaps=%d duplicates=%d out_of_order=%d esp_frame_difference=%d",
                    state.hub_node_id, received["total_frames"], received["total_bytes"],
                    received["average_kbps"], received["sequence_gaps"],
                    received["duplicates"], received["out_of_order"],
                    values[2] - received["total_frames"])
            elif message_type == FILE_RESULT:
                file_id, result, reserved, received_bytes, digest = struct.unpack(
                    "!IHHI32s", frame[HEADER_SIZE:])
                LOGGER.info("FILE_RESULT node=%d id=%d result=%d bytes=%d sha256=%s reserved=%d",
                            state.hub_node_id, file_id, result, received_bytes,
                            digest.hex(), reserved)

        def observe_upstream(header, _frame) -> None:
            """Learn the assigned Session from Ultra96 and mark the hub ready."""
            if header[2] == HELLO_ACK and header[3] == 0 and header[6] != 0:
                state.session_id = header[6]
                previous = self.hubs.get(state.hub_node_id)
                if previous is not None and previous.writer is not hub_writer:
                    previous.writer.close()
                self.hubs[state.hub_node_id] = hub
                LOGGER.info("Node %d ready; session=%d; ready_nodes=%s",
                            state.hub_node_id, state.session_id,
                            sorted(self.hubs))

        hub_observer = FrameObserver(observe_hub)
        upstream_observer = FrameObserver(observe_upstream)

        async def copy(reader, writer, observer, lock=None) -> None:
            """Forward bytes unchanged while feeding a copy to a frame observer."""
            while data := await reader.read(4096):
                observer.feed(data)
                if lock:
                    async with lock:
                        writer.write(data)
                        await writer.drain()
                else:
                    writer.write(data)
                    await writer.drain()

        async def report_stream_stats() -> None:
            """Periodically print normal, combined, and benchmark stream health."""
            while True:
                await asyncio.sleep(STATS_INTERVAL_SECONDS)
                if hub.stream_stats.total_frames:
                    report = hub.stream_stats.report()
                    hub.last_stream_report = report
                    payload = hub.stream_stats.latest_payload
                    physical = (
                        hub.stream_stats.latest_message_type == PHYSICAL_DATA
                    )
                    LOGGER.info(
                        "%s node=%d stream_age=%.1fs current=%.2fkbps/%.1fpps "
                        "average=%.2fkbps frames=%d bytes=%d gaps=%d duplicates=%d "
                        "out_of_order=%d timestamp_regressions=%d",
                        "PHYSICAL_STREAM" if physical else "STREAM",
                        state.hub_node_id, report["duration"],
                        report["current_kbps"], report["current_pps"],
                        report["average_kbps"], report["total_frames"],
                        report["total_bytes"], report["sequence_gaps"],
                        report["duplicates"], report["out_of_order"],
                        report["timestamp_regressions"])
                    if payload is not None:
                        if physical:
                            LOGGER.info(
                                "LATEST_PHYSICAL_DATA node=%d accel_mps2=%s "
                                "gyro_rad_s=%s status=%s",
                                state.hub_node_id, payload[:3], payload[3:6],
                                describe_status(payload[6]))
                        else:
                            LOGGER.info(
                                "LATEST_DATA node=%d accel=%s gyro=%s status=%s",
                                state.hub_node_id, payload[:3], payload[3:6],
                                describe_status(payload[6]))
                    ready_reports = [item.last_stream_report
                                     for item in self.hubs.values()
                                     if item.last_stream_report is not None]
                    if len(ready_reports) >= 2:
                        LOGGER.info(
                            "COMBINED_STREAM nodes=%s current=%.2fkbps/%.1fpps "
                            "frames=%d bytes=%d gaps=%d duplicates=%d out_of_order=%d",
                            sorted(self.hubs),
                            sum(item["current_kbps"] for item in ready_reports),
                            sum(item["current_pps"] for item in ready_reports),
                            sum(item["total_frames"] for item in ready_reports),
                            sum(item["total_bytes"] for item in ready_reports),
                            sum(item["sequence_gaps"] for item in ready_reports),
                            sum(item["duplicates"] for item in ready_reports),
                            sum(item["out_of_order"] for item in ready_reports))
                if state.benchmark_active and hub.benchmark_stats.total_frames:
                    report = hub.benchmark_stats.report()
                    LOGGER.info(
                        "BENCHMARK_LIVE node=%d duration=%.1fs current=%.2fkbps/%.1fpps "
                        "average=%.2fkbps frames=%d bytes=%d gaps=%d "
                        "duplicates=%d out_of_order=%d",
                        state.hub_node_id, report["duration"], report["current_kbps"],
                        report["current_pps"], report["average_kbps"],
                        report["total_frames"], report["total_bytes"],
                        report["sequence_gaps"], report["duplicates"],
                        report["out_of_order"])

        # Run both relay directions and periodic statistics concurrently for this hub connection.
        tasks = {
            # FireBeetle to Ultra96
            asyncio.create_task(copy(hub_reader, upstream_writer, hub_observer)),

            # Ultra96 to FireBeetle. Writes share the per-hub lock
            asyncio.create_task(copy(upstream_reader, hub_writer, upstream_observer, write_lock)),

            asyncio.create_task(report_stream_stats()),
        }
        try:
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            # More than one relay task may finish during TLS shutdown. Collect
            # every result before propagating the first real failure so asyncio
            # never reports a secondary "Task exception was never retrieved".
            results = await asyncio.gather(*done, return_exceptions=True)
            failure = next(
                (result for result in results
                 if isinstance(result, BaseException)
                 and not isinstance(result, asyncio.CancelledError)),
                None,
            )
            if failure is not None:
                raise failure
        except (ConnectionError, OSError, ssl.SSLError) as error:
            if not self.stop_event.is_set():
                current = self.hubs.get(state.hub_node_id)
                if current is not None and current is not hub:
                    LOGGER.info(
                        "Superseded Node %d connection finished closing: %s",
                        state.hub_node_id, error)
                elif state.hub_node_id:
                    LOGGER.info("Node %d connection closed: %s",
                                state.hub_node_id, error)
                else:
                    LOGGER.info("Unidentified node connection closed: %s", error)
        finally:
            # asyncio.run() cancels active connection handlers when the main
            # coroutine exits. Always collect their child relay tasks here as
            # well as in the normal completion path; otherwise a simultaneous
            # TLS close can leave an SSLError reported as "never retrieved".
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if self.hubs.get(state.hub_node_id) is hub:
                del self.hubs[state.hub_node_id]
                LOGGER.info("Node %d disconnected; ready_nodes=%s",
                            state.hub_node_id, sorted(self.hubs))
                if self.selected_hub == state.hub_node_id:
                    self.selected_hub = None
                if self.last_request_hub == state.hub_node_id:
                    self.last_request_hub = None
            hub_writer.close()
            upstream_writer.close()
            # Windows Proactor TLS shutdown can surface a late WinError 64
            # after a physical peer reset even when the awaited error is
            # suppressed. Closing is sufficient there; other event loops can
            # wait for orderly shutdown without producing callback noise.
            if sys.platform != "win32":
                with contextlib.suppress(BaseException):
                    await hub_writer.wait_closed()
                with contextlib.suppress(BaseException):
                    await upstream_writer.wait_closed()


def read_one_key() -> str:
    """Read one key without Enter on Windows, or one input character elsewhere."""
    if sys.platform == "win32":
        import msvcrt
        return msvcrt.getwch()
    return sys.stdin.read(1)


def parse_args() -> argparse.Namespace:
    """Read relay endpoints, optional TLS/file paths, and terminal log level."""
    parser = argparse.ArgumentParser(
        description="Interactive protocol-aware two-node relay")
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--listen-port", type=int, default=15000)
    parser.add_argument("--upstream-host", default="127.0.0.1")
    parser.add_argument("--upstream-port", type=int, default=19000)
    parser.add_argument("--certificate", help="TLS server certificate PEM")
    parser.add_argument("--private-key", help="TLS server private-key PEM")
    parser.add_argument("--file", help="File sent by f (r corrupts CRC, o offset)")
    parser.add_argument("--log-level", default="INFO")
    return parser.parse_args()


def main() -> None:
    """Configure readable terminal logging and run the interactive relay."""
    args = parse_args()
    handler = logging.StreamHandler()
    handler.setFormatter(HubColorFormatter(
        "%(asctime)s %(levelname)s: %(message)s",
        use_color=handler.stream.isatty(),
        datefmt="%Y-%m-%d %H:%M:%S"))
    logging.basicConfig(level=getattr(logging, args.log_level.upper()),
                        handlers=[handler])
    relay = InteractiveHubRelay(args.listen_host, args.listen_port,
                                args.upstream_host, args.upstream_port,
                                args.certificate, args.private_key, args.file)
    try:
        asyncio.run(relay.run())
    except KeyboardInterrupt:
        LOGGER.info("Controller stopped")


if __name__ == "__main__":
    main()

