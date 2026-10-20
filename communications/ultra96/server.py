"""Concurrent Ultra96 sensor server and visualizer-event adapter.

Each accepted node connection owns its protocol Session, stream state, file
verifier, and TEST_RESPONSE replay tracker. Node worker threads validate binary
frames but never perform visualizer network I/O. A shared bounded publisher
assigns one run-wide event sequence and sends validated JSON to the existing
loopback gateway on a background thread, isolating sensor reception from phone
and gateway failures.
"""

import argparse
from dataclasses import dataclass
import hashlib
import logging
from logging.handlers import RotatingFileHandler
import math
import os
from pathlib import Path
import queue
import random
import secrets
import socket
import struct
import sys
import tempfile
import threading
import time
import zlib

try:
    from visualizer_protocol import build_message, serialize_message
except ImportError:
    from .visualizer_protocol import build_message, serialize_message

try:
    from window_assembler import DecodedImuSample, WindowAssembler, WindowConfig
except ImportError:
    from .window_assembler import DecodedImuSample, WindowAssembler, WindowConfig


HOST = "0.0.0.0"
PORT = 5000

HEADER_SIZE = 24
HELLO_PAYLOAD_SIZE = 44
HELLO_ACK_PAYLOAD_SIZE = 0
DATA_PAYLOAD_SIZE = 16
PHYSICAL_DATA_PAYLOAD_SIZE = 26
ERROR_PAYLOAD_SIZE = 4
MAX_PAYLOAD_SIZE = 1024
SYNC_REQ_PAYLOAD_SIZE = 8
SYNC_RESP_PAYLOAD_SIZE = 24
ACTIVITY_TRIGGER_PAYLOAD_SIZE = 16
START_FLUSH_PAYLOAD_SIZE = 28
START_PAYLOAD_SIZE = 16
STOP_PAYLOAD_SIZE = 0
CONTROL_ACK_PAYLOAD_SIZE = 16

MAGIC = 0xA55A
PROTOCOL_VERSION = 1
MESSAGE_TYPE_HELLO = 0x01
MESSAGE_TYPE_HELLO_ACK = 0x02
MESSAGE_TYPE_SYNC_REQ = 0x03
MESSAGE_TYPE_SYNC_RESP = 0x04
MESSAGE_TYPE_ACTIVITY_TRIGGER = 0x05
MESSAGE_TYPE_START_FLUSH = 0x06
MESSAGE_TYPE_START = 0x07
MESSAGE_TYPE_STOP = 0x08
MESSAGE_TYPE_CONTROL_ACK = 0x09
MESSAGE_TYPE_DATA = 0x10
MESSAGE_TYPE_HEARTBEAT = 0x11
MESSAGE_TYPE_PHYSICAL_DATA = 0x12
MESSAGE_TYPE_TEST_RESPONSE = 0x21
MESSAGE_TYPE_BENCHMARK_REPORT = 0x23
MESSAGE_TYPE_FILE_BEGIN = 0x30
MESSAGE_TYPE_FILE_CHUNK = 0x31
MESSAGE_TYPE_FILE_END = 0x32
MESSAGE_TYPE_FILE_RESULT = 0x33
MESSAGE_TYPE_ERROR = 0x7F

SAMPLE_RATE_50_HZ = 0x0001
SAMPLE_RATE_100_HZ = 0x0002
STATUS_MOCK_DATA = 0x8000
STATUS_SOURCE_UNAVAILABLE = 0x4000
STATUS_TEST_REQUEST_PROCESSED = 0x2000
STATUS_BENCHMARK = 0x1000
STATUS_SENSOR_VALID = 0x0001
STATUS_HISTORICAL = 0x0002
STATUS_HISTORY_OVERWRITE = 0x0004
KNOWN_STATUS_MASK = (
    STATUS_MOCK_DATA
    | STATUS_SOURCE_UNAVAILABLE
    | STATUS_TEST_REQUEST_PROCESSED
    | STATUS_BENCHMARK
    | STATUS_SENSOR_VALID
    | STATUS_HISTORICAL
    | STATUS_HISTORY_OVERWRITE
)
DATA_LOG_PERIOD_SECONDS = 1
SAMPLE_INTERVAL_TOLERANCE = 0.10
HELLO_TIMEOUT_SECONDS = 5.0
CLIENT_INACTIVITY_TIMEOUT_SECONDS = 3.0
CLIENT_POLL_INTERVAL_SECONDS = 0.25
CLOCK_SYNC_ROUNDS = 5
CLOCK_SYNC_PERIOD_SECONDS = 60.0
CLOCK_SYNC_RESPONSE_TIMEOUT_SECONDS = 1.0
CLOCK_SYNC_MAX_DELAY_US = 500_000
CLOCK_SYNC_MAX_ATTEMPTS = 10
CLOCK_SYNC_INITIAL_RETRY_DELAY_SECONDS = 1.0
# Conservative defaults for the current Wi-Fi/TLS/relay path. Revisit these
# during real-IMU integration using physical multi-node timing evidence.
CLOCK_SYNC_SELECTED_ROUNDS = 3
CLOCK_SYNC_QUALITY_MAX_DELAY_US = 250_000
CLOCK_SYNC_MAX_OFFSET_SPREAD_US = 20_000
CLOCK_SYNC_DIRECT_APPLY_US = 5_000
CLOCK_SYNC_CONFIRMATION_AGREEMENT_US = 5_000
CLOCK_SYNC_SLEW_US_PER_SECOND = 1_000
CONTROL_ACK_TIMEOUT_SECONDS = 3.0
# Leave one second after the ACK deadline to deliver STOP before an armed start.
SCHEDULED_START_LEAD_US = 4_000_000
CONTROL_RESULT_APPLIED = 0
CONTROL_RESULT_ALREADY_STOPPED = 1
CONTROL_RESULT_INVALID_STATE = 2
CONTROL_RESULT_DUPLICATE = 3
CONTROL_RESULT_INSUFFICIENT_HISTORY = 4
CONTROL_RESULT_STALE_IDENTITY = 5
CONTROL_RESULT_BUSY = 6
PHYSICAL_HISTORY_DURATION_US = 5_000_000
PHYSICAL_MAX_ACCEL_MPS2 = 160.0
PHYSICAL_MAX_GYRO_RAD_S = 35.0
CLOCK_MAPPING_MAX_AGE_US = int(CLOCK_SYNC_PERIOD_SECONDS * 2 * 1_000_000)
VISUALIZER_HOST = "127.0.0.1"
VISUALIZER_PUBLISHER_PORT = 9100
VISUALIZER_QUEUE_SIZE = 64
RUN_ID_MIN = 10_000_000
RUN_ID_RANGE = 90_000_000
TEST_RESPONSE_REPLAY_WINDOW = 256
# Temporary labels used until the real inference adapter supplies its result.
# Snake_case keeps multi-word values consistent with the existing ``push_up``.
SUPPORTED_EXERCISES = (
    "unknown",
    "squat",
    "push_up",
    "sit_up",
    "lunge",
    "leg_raise",
    "jumping_jack"
)
HUB_COLORS = {1: "\033[38;5;208m", 2: "\033[34m"}  # orange, blue
COLOR_RESET = "\033[0m"
DEFAULT_LOG_FILE = "/tmp/comms/logs/server.log"
DEFAULT_LOG_MAX_BYTES = 10 * 1024 * 1024
DEFAULT_LOG_BACKUP_COUNT = 5
LOGGER = logging.getLogger("server")

ERROR_EXPECTED_HELLO = 1
ERROR_INVALID_HELLO = 2
ERROR_AUTHENTICATION_FAILED = 3
ERROR_INVALID_SESSION = 4
ERROR_UNAUTHORIZED_NODE = 5
ERROR_INVALID_FRAME = 6
ERROR_UNEXPECTED_MESSAGE = 7
ERROR_INVALID_SEQUENCE = 8

HEADER_FORMAT = "!HBBBBHIIQ"
HELLO_PAYLOAD_FORMAT = "!BBBBIHBB32s"
DATA_PAYLOAD_FORMAT = "!hhhhhhHH"
HEARTBEAT_PAYLOAD_FORMAT = "!HHI"
ERROR_PAYLOAD_FORMAT = "!HBB"


def colorize_hub_line(hub_node_id, text, use_color=None):
    """Color a complete Node 1/2 line when output is an interactive terminal."""
    if use_color is None:
        use_color = sys.stdout.isatty()
    color = HUB_COLORS.get(hub_node_id)
    if not use_color or color is None:
        return text
    return f"{color}{text}{COLOR_RESET}"


class HubColorFormatter(logging.Formatter):
    """Apply Node 1/2 colours only to an interactive console handler."""

    def __init__(self, *args, use_color=True, **kwargs):
        """Configure normal log formatting and optional terminal colour output."""
        super().__init__(*args, **kwargs)
        self.use_color = use_color

    def format(self, record):
        """Render a record, then colour the complete line for its hub if known."""
        rendered = super().format(record)
        hub_node_id = getattr(record, "hub_node_id", None)
        return colorize_hub_line(hub_node_id, rendered, self.use_color)


def log_hub(level, hub_node_id, message):
    """Emit one complete hub-specific record for atomic threaded logging."""
    LOGGER.log(level, message, extra={"hub_node_id": hub_node_id})


def configure_logging(log_file, max_bytes, backup_count):
    """Configure coloured console output and a plain rotating log file.

    Python logging serializes handler writes, so records from concurrent Node 1
    and Node 2 workers cannot interleave. Only the console formatter adds ANSI
    colour; the persistent file remains searchable plain text.
    """
    console = logging.StreamHandler()
    console.setFormatter(HubColorFormatter(
        "%(asctime)s %(levelname)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        use_color=console.stream.isatty(),
    ))

    path = Path(log_file)
    path.parent.mkdir(parents=True, exist_ok=True)
    file_handler = RotatingFileHandler(
        path,
        maxBytes=max_bytes,
        backupCount=backup_count,
        encoding="utf-8",
    )
    file_handler.setFormatter(logging.Formatter(
        "%(asctime)s %(levelname)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    ))

    old_handlers = list(LOGGER.handlers)
    LOGGER.handlers.clear()
    for handler in old_handlers:
        handler.close()
    LOGGER.setLevel(logging.INFO)
    LOGGER.propagate = False
    LOGGER.addHandler(console)
    LOGGER.addHandler(file_handler)


def describe_status(status):
    """Return readable status-bit names without hiding unknown bits."""
    names = []
    if status & STATUS_MOCK_DATA:
        names.append("mock_data")
    if status & STATUS_SOURCE_UNAVAILABLE:
        names.append("source_unavailable")
    if status & STATUS_TEST_REQUEST_PROCESSED:
        names.append("test_request_processed")
    if status & STATUS_BENCHMARK:
        names.append("benchmark")
    if status & STATUS_SENSOR_VALID:
        names.append("sensor_valid")
    if status & STATUS_HISTORICAL:
        names.append("historical")
    if status & STATUS_HISTORY_OVERWRITE:
        names.append("history_overwrite")
    unknown = status & ~KNOWN_STATUS_MASK
    if unknown:
        names.append(f"unknown_0x{unknown:04X}")
    return "|".join(names) if names else "none"


class TestResponseTracker:
    """Bounded exactly-once state for one active Hub Session.

    The connection handler creates one tracker after HELLO and discards it when
    that connection ends, so Sequences cannot cross Session boundaries. Recent
    payloads distinguish exact duplicates from conflicting reuse. Once an entry
    is evicted, the latest-Sequence check still rejects it as stale instead of
    accidentally publishing it again.
    """

    def __init__(self, capacity=TEST_RESPONSE_REPLAY_WINDOW):
        """Create bounded replay history for one connection's active Session."""
        self.capacity = capacity
        self._payloads = {}
        self._order = []
        self._latest_sequence = None

    def accept(self, sequence, payload):
        """Accept a new response once, ignore an exact duplicate, or reject reuse.

        Returns ``True`` only when the caller should publish a new visualizer
        event. Sequence comparison follows unsigned 32-bit wraparound rules.
        """
        previous = self._payloads.get(sequence)
        if previous is not None:
            if previous == payload:
                return False
            raise ProtocolError(
                ERROR_INVALID_SEQUENCE, MESSAGE_TYPE_TEST_RESPONSE,
                f"Conflicting TEST_RESPONSE Sequence {sequence}",
            )
        if self._latest_sequence is not None:
            # Interpret the uint32 difference using serial-number arithmetic so
            # a legitimate wrap from 0xffffffff to 0 remains forward progress.
            forward = (sequence - self._latest_sequence) & 0xFFFFFFFF
            if forward == 0 or forward >= 0x80000000:
                raise ProtocolError(
                    ERROR_INVALID_SEQUENCE, MESSAGE_TYPE_TEST_RESPONSE,
                    f"Stale TEST_RESPONSE Sequence {sequence}",
                )
        self._payloads[sequence] = payload
        self._order.append(sequence)
        self._latest_sequence = sequence
        if len(self._order) > self.capacity:
            expired = self._order.pop(0)
            del self._payloads[expired]
        return True


class VisualizerEventPublisher:
    """Shared non-blocking adapter to the loopback JSON publisher input.

    One instance is shared by every hub worker. The lock makes run-wide event
    sequencing and exercise selection atomic across Hub 3 and Hub 6. Workers
    enqueue without waiting; the dedicated thread owns socket connection and
    writes. A full queue or unavailable gateway drops the event with one compact
    log line rather than delaying DATA reception or terminating a Hub Session.

    ``exercise_choice`` and ``sender`` are injectable so tests—and a future real
    AI integration—can replace random selection or transport without changing
    response validation and exactly-once handling.
    """

    def __init__(self, sender=None, exercise_choice=None, run_id=None,
                 queue_size=VISUALIZER_QUEUE_SIZE):
        """Start one run-wide sequencer and its bounded background sender.

        Optional sender, exercise selector, and Run ID dependencies keep the
        production path deterministic and isolated during automated tests.
        """
        # One publisher lifetime is one visualizer run. A new random ID lets
        # seq restart at zero after a server restart without Unity mistaking
        # new events for delayed events from the previous run. Restricting the
        # value to eight digits keeps it easy to compare on screen and in logs.
        self.run_id = (
            secrets.randbelow(RUN_ID_RANGE) + RUN_ID_MIN
            if run_id is None
            else run_id
        )
        self._sender = sender or self._send_loopback
        self._exercise_choice = exercise_choice or random.choice
        # Bound memory even if the gateway remains unavailable while hubs keep
        # producing responses.
        self._queue = queue.Queue(maxsize=queue_size)

        # Protect the run-wide visualizer sequence shared by all hub workers.
        self._lock = threading.Lock()
        self._next_sequence = 0
        
        self._stopping = threading.Event()

        # Perform gateway network delivery outside the hub worker threads,
        # so a slow or unavailable gateway cannot block hub reception.
        self._worker = threading.Thread(
            target=self._run, daemon=True, name="visualizer-publisher")
        self._worker.start()

    def publish_for_response(self):
        """Build and enqueue one visualizer event without blocking a hub worker.

        The lock gives concurrent hubs one shared, monotonically increasing
        event sequence. The returned message supplies IDs for immediate logging
        even though loopback network delivery happens on the worker thread.
        """
        with self._lock:
            sequence = self._next_sequence
            self._next_sequence += 1
            message = build_message(
                run_id=self.run_id,
                seq=sequence,
                ts_us=time.monotonic_ns() // 1000,
                exercise=self._exercise_choice(SUPPORTED_EXERCISES),
            )
        try:
            self._queue.put_nowait(message)
        except queue.Full:
            LOGGER.warning(
                "VISUALIZER_DROP run_id=%d seq=%d reason=queue_full",
                self.run_id, sequence)
        return message

    def _run(self):
        """Drain queued events to the gateway while isolating delivery failures."""
        while not self._stopping.is_set():
            try:
                message = self._queue.get(timeout=0.1)
            except queue.Empty:
                continue
            try:
                self._sender(message)
            except (ConnectionError, OSError, ValueError) as error:
                LOGGER.warning(
                    "VISUALIZER_DROP run_id=%d seq=%d reason=%s",
                    message["run_id"], message["seq"], type(error).__name__)
            finally:
                self._queue.task_done()

    @staticmethod
    def _send_loopback(message):
        """Send one newline-delimited JSON event to the local gateway input."""
        # Use a short-lived loopback connection and strict timeout. Port 9100 is
        # owned by visualizer_gateway.py and is never exposed through the tunnel.
        payload = (serialize_message(message) + "\n").encode("utf-8")
        with socket.create_connection(
                (VISUALIZER_HOST, VISUALIZER_PUBLISHER_PORT), timeout=0.25) as publisher:
            publisher.settimeout(0.25)
            publisher.sendall(payload)

    def close(self):
        """Request publisher shutdown and briefly wait for its worker thread."""
        self._stopping.set()
        self._worker.join(timeout=1.0)


class FileVerifier:
    """One connection's disk-backed, strictly ordered file reconstruction."""

    def __init__(self):
        """Initialize empty transfer state with no temporary file allocated."""
        self.reset(remove=True)

    def __del__(self):
        """Best-effort cleanup of an incomplete transfer's temporary file."""
        self.reset(remove=self.completed is None)

    def reset(self, remove=True):
        """Clear transfer state, optionally deleting the temporary file."""
        old_path = getattr(self, "path", None)
        old_file = getattr(self, "file", None)
        if old_file is not None:
            old_file.close()
        if remove and old_path:
            try:
                os.unlink(old_path)
            except FileNotFoundError:
                pass
        self.file = None
        self.path = None
        self.file_id = 0
        self.total_size = 0
        self.chunk_size = 0
        self.offset = 0
        self.expected_sha256 = b""
        self.sha256 = None
        self.completed = None

    def process(self, frame, session_id, hub_node_id):
        """Validate and apply one forwarded FILE_BEGIN, FILE_CHUNK, or FILE_END.

        Chunks must arrive in order and pass CRC validation. FILE_END succeeds
        only when byte counts and both SHA-256 values agree; successful content
        remains on disk for comparison with the hub's later FILE_RESULT.
        """
        header = unpack_header(frame)
        message_type, node_id, flags = header[2], header[3], header[4]
        if header[6] != session_id or node_id != 0 or flags != 0:
            raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                "Invalid forwarded file header")
        payload = frame[HEADER_SIZE:]
        if message_type == MESSAGE_TYPE_FILE_BEGIN:
            if self.file is not None or len(payload) != 44:
                raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                    "Invalid or overlapping FILE_BEGIN")
            file_id, total_size, chunk_size, reserved, digest = struct.unpack(
                "!IIHH32s", payload)
            if not file_id or not total_size or total_size > 1024 * 1024 or \
                    not 0 < chunk_size <= 1008 or reserved:
                raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                    "Invalid FILE_BEGIN metadata")
            temporary = tempfile.NamedTemporaryFile(
                prefix=f"node{hub_node_id}_file_{file_id}_", delete=False)
            self.file, self.path = temporary, temporary.name
            self.file_id, self.total_size, self.chunk_size = file_id, total_size, chunk_size
            self.expected_sha256, self.sha256 = digest, hashlib.sha256()
            log_hub(
                logging.INFO,
                hub_node_id,
                f"FILE_BEGIN node={hub_node_id} id={file_id} size={total_size} "
                f"chunk={chunk_size} sha256={digest.hex()}")
            return
        if message_type == MESSAGE_TYPE_FILE_CHUNK:
            if self.file is None or len(payload) < 16:
                raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                    "FILE_CHUNK without active transfer")
            file_id, offset, length, reserved, crc = struct.unpack("!IIHHI", payload[:16])
            data = payload[16:]
            final_chunk = offset + length == self.total_size
            if file_id != self.file_id or offset != self.offset or length != len(data) or \
                    reserved or not length or offset + length > self.total_size or \
                    (length != self.chunk_size and not final_chunk) or \
                    zlib.crc32(data) & 0xFFFFFFFF != crc:
                raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                    "Invalid FILE_CHUNK metadata, offset, or CRC")
            self.file.write(data)
            self.sha256.update(data)
            self.offset += length
            log_hub(
                logging.INFO,
                hub_node_id,
                f"FILE_CHUNK node={hub_node_id} id={file_id} offset={offset} "
                f"length={length} crc=0x{crc:08X} crc_valid=true")
            return
        if message_type == MESSAGE_TYPE_FILE_END:
            if self.file is None or len(payload) != 40:
                raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                    "FILE_END without active transfer")
            file_id, total_size, digest = struct.unpack("!II32s", payload)
            calculated = self.sha256.digest()
            if file_id != self.file_id or total_size != self.total_size or \
                    self.offset != self.total_size or digest != self.expected_sha256 or \
                    calculated != self.expected_sha256:
                raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                    "FILE_END size or SHA-256 mismatch")
            self.file.close()
            self.file = None
            self.completed = (file_id, self.offset, calculated)
            log_hub(
                logging.INFO,
                hub_node_id,
                f"FILE_VERIFIED node={hub_node_id} id={file_id} bytes={self.offset} "
                f"sha256={calculated.hex()} path={self.path}")


def process_file_result(frame, session_id, hub_node_id, verifier):
    """Compare the hub's final file result with Ultra96 reconstruction state."""
    header = unpack_header(frame)
    if header[3] != hub_node_id or header[4] != 0 or header[6] != session_id or \
            len(frame) != HEADER_SIZE + 44:
        raise ProtocolError(ERROR_INVALID_FRAME, MESSAGE_TYPE_FILE_RESULT,
                            "Invalid FILE_RESULT header")
    file_id, result, reserved, received, digest = struct.unpack(
        "!IHHI32s", frame[HEADER_SIZE:])
    if reserved:
        raise ProtocolError(ERROR_INVALID_FRAME, MESSAGE_TYPE_FILE_RESULT,
                            "Invalid FILE_RESULT reserved field")
    if result == 0 and verifier.completed != (file_id, received, digest):
        raise ProtocolError(ERROR_INVALID_FRAME, MESSAGE_TYPE_FILE_RESULT,
                            "ESP and Ultra96 file results disagree")
    log_hub(
        logging.INFO,
        hub_node_id,
        f"FILE_RESULT node={hub_node_id} id={file_id} result={result} "
        f"bytes={received} sha256={digest.hex()}")
    if result != 0:
        # FILE_BEGIN was already forwarded before the ESP could discover a bad
        # chunk. The terminal failure result tells Ultra96 to discard that
        # incomplete reconstruction so the next FILE_BEGIN is independent.
        verifier.reset(remove=True)

# Demo-only shared device token. TLS protects it in transit, but this
# predictable value is not a production authentication secret.
DEVELOPMENT_DEVICE_TOKEN = bytes(range(32))

HUB_CONFIG = {
    1: {
        "assigned_node_mask": 0x02,
        "device_token": DEVELOPMENT_DEVICE_TOKEN,
    },
    2: {
        "assigned_node_mask": 0x04,
        "device_token": DEVELOPMENT_DEVICE_TOKEN,
    },
}


@dataclass(frozen=True)
class ClockMapping:
    """One Session-scoped estimate mapping coordinator time to node time."""

    node_minus_coordinator_us: int
    round_trip_us: int
    synchronized_at_us: int


def midpoint_offset(numerator):
    """Divide a signed offset numerator by two with integer nearest rounding."""
    if numerator >= 0:
        return (numerator + 1) // 2
    return -((-numerator + 1) // 2)


class NodeControlConnection:
    """Thread-owned socket plus clock synchronization state for one Session."""

    def __init__(self, client, node_id, session_id, coordinator, boot_id=0):
        self.client = client
        self.node_id = node_id
        self.session_id = session_id
        self.coordinator = coordinator
        self.boot_id = boot_id
        self.last_arm_generation = 0
        self.send_lock = threading.Lock()
        self.mapping_lock = threading.Lock()
        self.mapping = None
        self.target_mapping = None
        self.pending_confirmation = None
        self.mapping_adjusted_at_us = 0
        self.samples = []
        self.pending_sync = None
        self.next_sync_sequence = 0
        self.last_sync_completed_us = 0
        self.sync_attempts = 0
        self.next_sync_allowed_us = 0

    def send(self, frame):
        """Serialize writes shared by the worker and group coordinator."""
        with self.send_lock:
            self.client.sendall(frame)

    def begin_sync(self, now_us=None, preserve_confirmation=False):
        """Start a fresh multi-round exchange without reusing an older mapping."""
        self.samples = []
        self.pending_sync = None
        if not preserve_confirmation:
            self.pending_confirmation = None
        self.sync_attempts = 0
        self.next_sync_allowed_us = 0
        self._send_sync_request(now_us)

    def _send_sync_request(self, now_us=None):
        if now_us is None:
            now_us = time.monotonic_ns() // 1000
        sequence = self.next_sync_sequence
        self.next_sync_sequence = (sequence + 1) & 0xFFFFFFFF
        payload = struct.pack("!Q", now_us)
        frame = build_header(
            MESSAGE_TYPE_SYNC_REQ,
            node_id=0,
            payload_length=SYNC_REQ_PAYLOAD_SIZE,
            session_id=self.session_id,
            sequence=sequence,
            timestamp_us=now_us,
        ) + payload
        self.send(frame)
        self.pending_sync = (sequence, now_us, now_us)
        self.sync_attempts += 1

    def _pause_failed_initial_sync(self, now_us):
        """Pace another initial batch while retaining an established mapping."""
        self.last_sync_completed_us = now_us
        if self.mapping is None:
            self.next_sync_allowed_us = now_us + int(
                CLOCK_SYNC_INITIAL_RETRY_DELAY_SECONDS * 1_000_000)

    def _request_another_sync_round(self, now_us):
        """Continue a batch, or pause after too many unusable attempts."""
        if self.sync_attempts < CLOCK_SYNC_MAX_ATTEMPTS:
            self._send_sync_request(now_us)
            return None

        usable_count = sum(
            sample.round_trip_us <= CLOCK_SYNC_QUALITY_MAX_DELAY_US
            for sample in self.samples
        )
        log_hub(
            logging.WARNING,
            self.node_id,
            f"CLOCK_SYNC_REJECTED node={self.node_id} "
            f"session={self.session_id} reason=attempt_limit "
            f"attempts={self.sync_attempts} rounds={len(self.samples)} "
            f"usable={usable_count}",
        )
        self.pending_sync = None
        self.pending_confirmation = None
        self._pause_failed_initial_sync(now_us)
        return self.get_mapping(now_us)

    def _advance_mapping_locked(self, now_us):
        """Slew the effective offset toward a validated target without a jump."""
        if self.mapping is None or self.target_mapping is None:
            return self.mapping
        elapsed_us = max(0, now_us - self.mapping_adjusted_at_us)
        maximum_step_us = (
            elapsed_us * CLOCK_SYNC_SLEW_US_PER_SECOND // 1_000_000
        )
        if maximum_step_us <= 0:
            return self.mapping

        remaining_us = (
            self.target_mapping.node_minus_coordinator_us -
            self.mapping.node_minus_coordinator_us
        )
        step_us = max(-maximum_step_us, min(maximum_step_us, remaining_us))
        if step_us == 0:
            return self.mapping

        reached_target = step_us == remaining_us
        target = self.target_mapping
        self.mapping = ClockMapping(
            node_minus_coordinator_us=(
                self.mapping.node_minus_coordinator_us + step_us),
            round_trip_us=target.round_trip_us,
            synchronized_at_us=target.synchronized_at_us,
        )
        self.mapping_adjusted_at_us = now_us
        if reached_target:
            self.target_mapping = None
            log_hub(
                logging.INFO,
                self.node_id,
                f"CLOCK_SYNC_SLEW_COMPLETE node={self.node_id} "
                f"session={self.session_id} "
                f"offset_us={self.mapping.node_minus_coordinator_us}",
            )
        return self.mapping

    def get_mapping(self, now_us=None):
        """Return the current effective mapping after any due slew progress."""
        if now_us is None:
            now_us = time.monotonic_ns() // 1000
        with self.mapping_lock:
            return self._advance_mapping_locked(now_us)

    def _select_sync_candidate(self, synchronized_at_us):
        """Build a robust candidate from three agreeing low-delay rounds."""
        usable = [
            sample for sample in self.samples
            if sample.round_trip_us <= CLOCK_SYNC_QUALITY_MAX_DELAY_US
        ]
        if len(usable) < CLOCK_SYNC_SELECTED_ROUNDS:
            return None, (
                f"insufficient_quality_rounds usable={len(usable)} "
                f"required={CLOCK_SYNC_SELECTED_ROUNDS}"
            ), None

        selected = sorted(
            usable, key=lambda sample: sample.round_trip_us
        )[:CLOCK_SYNC_SELECTED_ROUNDS]
        offsets = sorted(
            sample.node_minus_coordinator_us for sample in selected
        )
        spread_us = offsets[-1] - offsets[0]
        if spread_us > CLOCK_SYNC_MAX_OFFSET_SPREAD_US:
            return None, (
                f"offset_spread spread_us={spread_us} "
                f"limit_us={CLOCK_SYNC_MAX_OFFSET_SPREAD_US}"
            ), spread_us

        candidate = ClockMapping(
            node_minus_coordinator_us=offsets[len(offsets) // 2],
            round_trip_us=min(sample.round_trip_us for sample in selected),
            synchronized_at_us=synchronized_at_us,
        )
        return candidate, None, spread_us

    def _apply_sync_candidate(self, candidate, spread_us, received_at_us):
        """Apply a small refresh or require confirmation before a large slew."""
        result = None
        confirmed = False
        with self.mapping_lock:
            current = self._advance_mapping_locked(received_at_us)
            if current is None:
                self.mapping = candidate
                self.target_mapping = None
                self.pending_confirmation = None
                self.mapping_adjusted_at_us = received_at_us
                result = "initial"
                change_us = 0
            else:
                confirmation = self.pending_confirmation
                if confirmation is not None:
                    first_candidate, first_spread_us = confirmation
                    candidate_difference_us = abs(
                        candidate.node_minus_coordinator_us -
                        first_candidate.node_minus_coordinator_us
                    )
                    self.pending_confirmation = None
                    if (candidate_difference_us >
                            CLOCK_SYNC_CONFIRMATION_AGREEMENT_US):
                        rejection = (
                            current,
                            first_candidate,
                            candidate_difference_us,
                        )
                        result = "confirmation_rejected"
                        change_us = 0
                    else:
                        confirmed_offset_us = midpoint_offset(
                            first_candidate.node_minus_coordinator_us +
                            candidate.node_minus_coordinator_us
                        )
                        candidate = ClockMapping(
                            node_minus_coordinator_us=confirmed_offset_us,
                            round_trip_us=min(
                                first_candidate.round_trip_us,
                                candidate.round_trip_us,
                            ),
                            synchronized_at_us=received_at_us,
                        )
                        spread_us = max(first_spread_us, spread_us)
                        change_us = (
                            candidate.node_minus_coordinator_us -
                            current.node_minus_coordinator_us
                        )
                        confirmed = True
                        result = "confirmed"
                else:
                    change_us = (
                        candidate.node_minus_coordinator_us -
                        current.node_minus_coordinator_us
                    )
                    if abs(change_us) > CLOCK_SYNC_DIRECT_APPLY_US:
                        self.pending_confirmation = (candidate, spread_us)
                        result = "confirmation_pending"

                if result == "confirmation_rejected":
                    effective = current
                elif result == "confirmation_pending":
                    effective = current
                elif abs(change_us) <= CLOCK_SYNC_DIRECT_APPLY_US:
                    self.mapping = candidate
                    self.target_mapping = None
                    self.mapping_adjusted_at_us = received_at_us
                    result = "applied"
                    effective = self.mapping
                else:
                    self.target_mapping = candidate
                    self.mapping_adjusted_at_us = received_at_us
                    result = "limited"
                    effective = self.mapping

            if current is None:
                effective = self.mapping

        if result == "confirmation_pending":
            log_hub(
                logging.INFO,
                self.node_id,
                f"CLOCK_SYNC_CONFIRMATION_PENDING node={self.node_id} "
                f"session={self.session_id} "
                f"effective_offset_us={effective.node_minus_coordinator_us} "
                f"candidate_offset_us={candidate.node_minus_coordinator_us} "
                f"change_us={change_us} delay_us={candidate.round_trip_us} "
                f"spread_us={spread_us} rounds={len(self.samples)} "
                f"selected={CLOCK_SYNC_SELECTED_ROUNDS} "
                f"agreement_limit_us={CLOCK_SYNC_CONFIRMATION_AGREEMENT_US}",
            )
            return effective, True, False

        if result == "confirmation_rejected":
            _, first_candidate, candidate_difference_us = rejection
            log_hub(
                logging.WARNING,
                self.node_id,
                f"CLOCK_SYNC_REJECTED node={self.node_id} "
                f"session={self.session_id} "
                f"reason=confirmation_disagreement "
                f"first_offset_us={first_candidate.node_minus_coordinator_us} "
                f"confirming_offset_us={candidate.node_minus_coordinator_us} "
                f"difference_us={candidate_difference_us} "
                f"limit_us={CLOCK_SYNC_CONFIRMATION_AGREEMENT_US}",
            )
            return effective, False, False

        if result == "limited":
            log_hub(
                logging.INFO,
                self.node_id,
                f"CLOCK_SYNC_LIMITED node={self.node_id} "
                f"session={self.session_id} "
                f"effective_offset_us={effective.node_minus_coordinator_us} "
                f"target_offset_us={candidate.node_minus_coordinator_us} "
                f"change_us={change_us} delay_us={candidate.round_trip_us} "
                f"spread_us={spread_us} rounds={len(self.samples)} "
                f"selected={CLOCK_SYNC_SELECTED_ROUNDS} "
                f"slew_us_per_second={CLOCK_SYNC_SLEW_US_PER_SECOND} "
                f"confirmed={str(confirmed).lower()}",
            )
        else:
            log_hub(
                logging.INFO,
                self.node_id,
                f"CLOCK_SYNC_APPLIED node={self.node_id} "
                f"session={self.session_id} result={result} "
                f"offset_us={effective.node_minus_coordinator_us} "
                f"change_us={change_us} delay_us={candidate.round_trip_us} "
                f"spread_us={spread_us} rounds={len(self.samples)} "
                f"selected={CLOCK_SYNC_SELECTED_ROUNDS} "
                f"confirmed={str(confirmed).lower()}",
            )
        return effective, False, True

    def process_sync_response(self, frame, received_at_us=None):
        """Validate one response and filter a stable multi-round mapping."""
        if received_at_us is None:
            received_at_us = time.monotonic_ns() // 1000
        header = unpack_header(frame)
        if (header[2] != MESSAGE_TYPE_SYNC_RESP or
                header[3] != self.node_id or header[4] != 0 or
                header[5] != SYNC_RESP_PAYLOAD_SIZE or
                header[6] != self.session_id):
            raise ProtocolError(
                ERROR_INVALID_FRAME, MESSAGE_TYPE_SYNC_RESP,
                "Invalid SYNC_RESP header",
            )
        t1, t2, t3 = struct.unpack("!QQQ", frame[HEADER_SIZE:])
        if t2 > t3:
            raise ProtocolError(
                ERROR_INVALID_FRAME, MESSAGE_TYPE_SYNC_RESP,
                "SYNC_RESP node receive time is after node send time",
            )

        if self.pending_sync is None:
            log_hub(
                logging.WARNING,
                self.node_id,
                f"CLOCK_SYNC_ROUND_DISCARDED node={self.node_id} "
                f"session={self.session_id} reason=no_pending_request "
                f"sequence={header[7]}",
            )
            return self.get_mapping(received_at_us)

        sequence, expected_t1, _sent_at = self.pending_sync
        if header[7] != sequence or t1 != expected_t1:
            log_hub(
                logging.WARNING,
                self.node_id,
                f"CLOCK_SYNC_ROUND_DISCARDED node={self.node_id} "
                f"session={self.session_id} reason=stale_response "
                f"sequence={header[7]} expected_sequence={sequence}",
            )
            return self.get_mapping(received_at_us)

        delay_us = (received_at_us - t1) - (t3 - t2)
        if delay_us < 0:
            raise ProtocolError(
                ERROR_INVALID_FRAME, MESSAGE_TYPE_SYNC_RESP,
                f"SYNC_RESP delay {delay_us}us is outside the accepted range",
            )
        if delay_us > CLOCK_SYNC_MAX_DELAY_US:
            self.pending_sync = None
            log_hub(
                logging.WARNING,
                self.node_id,
                f"CLOCK_SYNC_ROUND_DISCARDED node={self.node_id} "
                f"session={self.session_id} reason=excessive_delay "
                f"delay_us={delay_us} limit_us={CLOCK_SYNC_MAX_DELAY_US} "
                f"attempt={self.sync_attempts}",
            )
            return self._request_another_sync_round(received_at_us)

        offset_us = midpoint_offset((t2 - t1) + (t3 - received_at_us))
        self.samples.append(ClockMapping(
            node_minus_coordinator_us=offset_us,
            round_trip_us=delay_us,
            synchronized_at_us=received_at_us,
        ))
        self.pending_sync = None
        if len(self.samples) < CLOCK_SYNC_ROUNDS:
            return self._request_another_sync_round(received_at_us)

        self.last_sync_completed_us = received_at_us
        candidate, rejection_reason, spread_us = self._select_sync_candidate(
            received_at_us)
        if candidate is None:
            self.pending_confirmation = None
            log_hub(
                logging.WARNING,
                self.node_id,
                f"CLOCK_SYNC_REJECTED node={self.node_id} "
                f"session={self.session_id} reason={rejection_reason} "
                f"rounds={len(self.samples)}",
            )
            self._pause_failed_initial_sync(received_at_us)
            return self.get_mapping(received_at_us)

        mapping, confirmation_required, mapping_updated = self._apply_sync_candidate(
            candidate, spread_us, received_at_us)
        if confirmation_required:
            self.begin_sync(received_at_us, preserve_confirmation=True)
        elif mapping_updated:
            self.coordinator.mapping_updated(self)
        return mapping

    def tick(self, now_us=None):
        """Retry a lost exchange or start the periodic refresh when due."""
        if now_us is None:
            now_us = time.monotonic_ns() // 1000
        self.get_mapping(now_us)
        self.coordinator.tick(now_us)
        if self.pending_sync is not None:
            _sequence, _t1, sent_at = self.pending_sync
            if now_us - sent_at >= int(CLOCK_SYNC_RESPONSE_TIMEOUT_SECONDS * 1_000_000):
                self.pending_sync = None
                log_hub(
                    logging.WARNING,
                    self.node_id,
                    f"CLOCK_SYNC_ROUND_DISCARDED node={self.node_id} "
                    f"session={self.session_id} reason=response_timeout "
                    f"attempt={self.sync_attempts}",
                )
                self._request_another_sync_round(now_us)
            return
        if (self.mapping is None and now_us >= self.next_sync_allowed_us):
            self.begin_sync(now_us)
        elif (self.mapping is not None and
              now_us - self.last_sync_completed_us >=
                  int(CLOCK_SYNC_PERIOD_SECONDS * 1_000_000)):
            self.begin_sync(now_us)


class CaptureCoordinator:
    """Coordinate scheduled legacy capture or trigger-driven physical capture."""

    def __init__(self, required_nodes=(1, 2), auto_start=True,
                 window_assembler=None, physical_mode=False):
        self.required_nodes = frozenset(required_nodes)
        self._connections = {}
        self._lock = threading.RLock()
        self._control_sequence = 0
        self.run_active = False
        self.start_pending = False
        self._pending_start_acks = {}
        self._pending_stop_acks = {}
        self._start_ack_deadline_us = None
        self._cancelled_start_sequence = None
        self._pending_start_us = None
        self.desired_running = auto_start
        self.window_assembler = window_assembler
        self.physical_mode = physical_mode
        self._accepted_trigger = None

    def _next_sequence(self):
        sequence = self._control_sequence
        self._control_sequence = (sequence + 1) & 0xFFFFFFFF
        return sequence

    def register(self, connection):
        with self._lock:
            previous = self._connections.get(connection.node_id)
            if self.run_active or self.start_pending:
                self._stop_locked("session changed")
            self._connections[connection.node_id] = connection
        if previous is not None and previous is not connection:
            try:
                previous.client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def unregister(self, connection):
        with self._lock:
            if self._connections.get(connection.node_id) is not connection:
                return
            if self.run_active or self.start_pending:
                self._stop_locked(f"Node {connection.node_id} disconnected")
            del self._connections[connection.node_id]
            self._pending_start_acks.pop(connection.node_id, None)
            self._pending_stop_acks.pop(connection.node_id, None)

    def mapping_updated(self, connection):
        with self._lock:
            if self._connections.get(connection.node_id) is not connection:
                return
            if not self.physical_mode:
                self._maybe_start_locked()

    def activity_triggered(self, connection, header, trigger):
        """Map one authenticated peak and start one two-node historical flush."""
        with self._lock:
            if self._connections.get(connection.node_id) is not connection:
                return False
            if not self.desired_running:
                return False
            identity = (connection.node_id, connection.session_id, header[7])
            contents = (trigger[0], trigger[1], trigger[2], header[8])
            if self._accepted_trigger is not None:
                old_identity, old_contents = self._accepted_trigger
                if identity == old_identity:
                    if contents != old_contents:
                        raise ProtocolError(
                            ERROR_INVALID_SEQUENCE,
                            MESSAGE_TYPE_ACTIVITY_TRIGGER,
                            "ACTIVITY_TRIGGER Sequence reused with different contents",
                        )
                    return False
                if self.run_active or self.start_pending:
                    return False
            if (trigger[0] != connection.boot_id or trigger[1] == 0 or
                    trigger[1] <= connection.last_arm_generation):
                raise ProtocolError(
                    ERROR_INVALID_FRAME, MESSAGE_TYPE_ACTIVITY_TRIGGER,
                    "ACTIVITY_TRIGGER has stale boot or arm identity",
                )
            if set(self._connections) != self.required_nodes:
                return False
            now_us = time.monotonic_ns() // 1000
            mappings = {
                node_id: item.get_mapping(now_us)
                for node_id, item in self._connections.items()
            }
            if any(mapping is None or
                   now_us - mapping.synchronized_at_us > CLOCK_MAPPING_MAX_AGE_US
                   for mapping in mappings.values()):
                return False
            source_mapping = mappings[connection.node_id]
            coordinator_t0_us = (
                trigger[2] - source_mapping.node_minus_coordinator_us
            )
            sequence = self._next_sequence()
            self.start_pending = True
            self._pending_start_us = coordinator_t0_us - PHYSICAL_HISTORY_DURATION_US
            self._pending_start_acks = {
                node_id: sequence for node_id in self._connections
            }
            self._start_ack_deadline_us = now_us + int(
                CONTROL_ACK_TIMEOUT_SECONDS * 1_000_000)
            self._accepted_trigger = (identity, contents)
            connection.last_arm_generation = trigger[1]
            for node_id in sorted(self._connections):
                item = self._connections[node_id]
                mapping = mappings[node_id]
                payload = struct.pack(
                    "!QqIB3xI", coordinator_t0_us,
                    mapping.node_minus_coordinator_us,
                    PHYSICAL_HISTORY_DURATION_US,
                    connection.node_id, header[7],
                )
                frame = build_header(
                    MESSAGE_TYPE_START_FLUSH, node_id=0,
                    payload_length=START_FLUSH_PAYLOAD_SIZE,
                    session_id=item.session_id, sequence=sequence,
                ) + payload
                item.send(frame)
                log_hub(
                    logging.INFO, node_id,
                    f"START_FLUSH_SENT node={node_id} session={item.session_id} "
                    f"sequence={sequence} trigger_node={connection.node_id} "
                    f"trigger_sequence={header[7]} coordinator_t0_us={coordinator_t0_us}",
                )
            return True

    def _maybe_start_locked(self):
        if self.physical_mode:
            return False
        if (not self.desired_running or self.run_active or self.start_pending or
                set(self._connections) != self.required_nodes):
            return False
        now_us = time.monotonic_ns() // 1000
        mappings = {
            node_id: item.get_mapping(now_us)
            for node_id, item in self._connections.items()
        }
        if any(mapping is None for mapping in mappings.values()):
            return False
        start_us = now_us + SCHEDULED_START_LEAD_US
        sequence = self._next_sequence()
        self.start_pending = True
        self._pending_start_us = start_us
        self._pending_start_acks = {
            node_id: sequence for node_id in self._connections
        }
        self._start_ack_deadline_us = now_us + int(
            CONTROL_ACK_TIMEOUT_SECONDS * 1_000_000)
        self._cancelled_start_sequence = None
        for node_id in sorted(self._connections):
            item = self._connections[node_id]
            mapping = mappings[node_id]
            payload = struct.pack(
                "!Qq", start_us, mapping.node_minus_coordinator_us)
            frame = build_header(
                MESSAGE_TYPE_START,
                node_id=0,
                payload_length=START_PAYLOAD_SIZE,
                session_id=item.session_id,
                sequence=sequence,
            ) + payload
            item.send(frame)
            log_hub(logging.INFO, node_id,
                f"START_SENT node={node_id} session={item.session_id} "
                f"sequence={sequence} coordinator_start_us={start_us} "
                f"local_start_us={start_us + mapping.node_minus_coordinator_us}")
        return True

    def _stop_locked(self, reason):
        if self._pending_start_acks:
            self._cancelled_start_sequence = next(
                iter(self._pending_start_acks.values()))
        sequence = self._next_sequence()
        self._pending_stop_acks = {
            node_id: sequence for node_id in self._connections
        }
        for node_id, item in list(self._connections.items()):
            frame = build_header(
                MESSAGE_TYPE_STOP,
                node_id=0,
                payload_length=STOP_PAYLOAD_SIZE,
                session_id=item.session_id,
                sequence=sequence,
            )
            try:
                item.send(frame)
                log_hub(logging.INFO, node_id,
                    f"STOP_SENT node={node_id} session={item.session_id} "
                    f"sequence={sequence} reason={reason}")
            except OSError:
                pass
        self.run_active = False
        self.start_pending = False
        self._start_ack_deadline_us = None
        self._pending_start_acks.clear()
        self._pending_start_us = None
        self._accepted_trigger = None
        if self.window_assembler is not None:
            self.window_assembler.stop_run(reason)

    def tick(self, now_us=None):
        """Fail a group START that is still missing acknowledgements at its deadline."""
        if now_us is None:
            now_us = time.monotonic_ns() // 1000
        with self._lock:
            if (not self.start_pending or self._start_ack_deadline_us is None or
                    now_us < self._start_ack_deadline_us):
                return False
            missing_nodes = sorted(self._pending_start_acks)
            sequence = next(iter(self._pending_start_acks.values()))
            self.desired_running = False
            LOGGER.error(
                "START_ACK_TIMEOUT sequence=%d missing_nodes=%s timeout_seconds=%.1f; "
                "automatic restart disabled until manual START",
                sequence, missing_nodes, CONTROL_ACK_TIMEOUT_SECONDS)
            self._stop_locked("START acknowledgement timeout")
            return True

    def control_acknowledged(
        self, connection, command_type, command_sequence, result,
        effective_local_us,
    ):
        """Apply one authenticated ESP result to the pending group command."""
        with self._lock:
            if self._connections.get(connection.node_id) is not connection:
                return
            if command_type in (MESSAGE_TYPE_START, MESSAGE_TYPE_START_FLUSH):
                expected = self._pending_start_acks.get(connection.node_id)
                if expected != command_sequence:
                    if (expected is None and not self.start_pending and
                            command_sequence == self._cancelled_start_sequence):
                        log_hub(logging.WARNING, connection.node_id,
                            f"LATE_START_ACK_IGNORED node={connection.node_id} "
                            f"sequence={command_sequence}")
                        return
                    raise ProtocolError(
                        ERROR_INVALID_SEQUENCE, MESSAGE_TYPE_CONTROL_ACK,
                        "CONTROL_ACK does not match the pending START/START_FLUSH",
                    )
                if result != CONTROL_RESULT_APPLIED:
                    log_hub(logging.ERROR, connection.node_id,
                    f"START_REJECTED node={connection.node_id} "
                        f"sequence={command_sequence} result={result}")
                    self.desired_running = False
                    self._stop_locked("START rejected")
                    return
                del self._pending_start_acks[connection.node_id]
                log_hub(logging.INFO, connection.node_id,
                    f"START_ACK node={connection.node_id} "
                    f"sequence={command_sequence} local_start_us={effective_local_us}")
                if not self._pending_start_acks:
                    self.start_pending = False
                    self._start_ack_deadline_us = None
                    self.run_active = True
                    LOGGER.info("START_CONFIRMED nodes=%s sequence=%d",
                        sorted(self.required_nodes), command_sequence)
                    if self.window_assembler is not None:
                        self.window_assembler.start_run(
                            command_sequence, self._pending_start_us)
                    self._pending_start_us = None
                return

            if command_type == MESSAGE_TYPE_STOP:
                expected = self._pending_stop_acks.get(connection.node_id)
                if expected != command_sequence:
                    raise ProtocolError(
                        ERROR_INVALID_SEQUENCE, MESSAGE_TYPE_CONTROL_ACK,
                        "CONTROL_ACK does not match the pending STOP",
                    )
                if result not in (
                    CONTROL_RESULT_APPLIED,
                    CONTROL_RESULT_ALREADY_STOPPED,
                ):
                    raise ProtocolError(
                        ERROR_INVALID_FRAME, MESSAGE_TYPE_CONTROL_ACK,
                        f"STOP acknowledgement result {result} is invalid",
                    )
                del self._pending_stop_acks[connection.node_id]
                log_hub(logging.INFO, connection.node_id,
                    f"STOP_ACK node={connection.node_id} "
                    f"sequence={command_sequence} result={result} "
                    f"local_stop_us={effective_local_us}")
                return

            raise ProtocolError(
                ERROR_INVALID_FRAME, MESSAGE_TYPE_CONTROL_ACK,
                f"Unsupported acknowledged command 0x{command_type:02X}",
            )

    def stop_all(self, reason="operator shutdown"):
        with self._lock:
            self._stop_locked(reason)

    def request_start(self):
        """Remember the operator's intent and START now or when both nodes are ready."""
        with self._lock:
            self.desired_running = True
            started = False if self.physical_mode else self._maybe_start_locked()
        if not started:
            LOGGER.info("START_REQUESTED waiting_for_both_synchronized_nodes")
        return started

    def request_stop(self):
        """STOP both nodes and suppress automatic restart until START is requested."""
        with self._lock:
            self.desired_running = False
            self._stop_locked("manual STOP")


def run_control_console(coordinator):
    """Accept optional line-based START/STOP commands without extra services."""
    LOGGER.info("Control commands: start (or s), stop (or x)")
    for line in sys.stdin:
        command = line.strip().lower()
        if command in ("start", "s"):
            coordinator.request_start()
        elif command in ("stop", "x"):
            coordinator.request_stop()
        elif command:
            LOGGER.warning("Unknown control command %r; use start or stop", command)


class ManagedClient:
    """Close a socket and unregister its Session on every handler return path."""

    def __init__(self, client, cleanup):
        self.client = client
        self.cleanup = cleanup

    def __enter__(self):
        return self.client

    def __exit__(self, exc_type, exc_value, traceback):
        self.cleanup()
        self.client.close()
        return False


class ProtocolError(Exception):
    """Validated protocol failure that can be returned as an ERROR frame."""

    def __init__(self, code, offending_type, message):
        """Store the wire error code and message type alongside readable detail."""
        super().__init__(message)
        self.code = code
        self.offending_type = offending_type


def unpack_header(frame):
    """Decode the fixed 24-byte common header from a complete frame."""
    return struct.unpack(HEADER_FORMAT, frame[:HEADER_SIZE])


def build_header(
    message_type,
    node_id,
    payload_length,
    session_id,
    sequence=0,
    timestamp_us=None,
):
    """Build an Ultra96-originated header with the current monotonic timestamp."""
    return struct.pack(
        HEADER_FORMAT,
        MAGIC,
        PROTOCOL_VERSION,
        message_type,
        node_id,
        0,
        payload_length,
        session_id,
        sequence,
        time.monotonic_ns() // 1000 if timestamp_us is None else timestamp_us,
    )


def allocate_session_id():
    """Generate a nonzero random 32-bit Session ID for one hub connection."""
    session_id = 0
    while session_id == 0:
        session_id = secrets.randbits(32)
    return session_id


def send_hello_ack(client, session_id):
    """Confirm an authenticated HELLO and assign its active Session ID."""
    client.sendall(
        build_header(
            MESSAGE_TYPE_HELLO_ACK,
            node_id=0,
            payload_length=HELLO_ACK_PAYLOAD_SIZE,
            session_id=session_id,
        )
    )


def send_error(client, session_id, code, offending_type):
    """Best-effort delivery of one protocol ERROR before closing the client."""
    payload = struct.pack(
        ERROR_PAYLOAD_FORMAT,
        code,
        offending_type,
        0,
    )
    frame = build_header(
        MESSAGE_TYPE_ERROR,
        node_id=0,
        payload_length=ERROR_PAYLOAD_SIZE,
        session_id=session_id,
    ) + payload

    try:
        client.sendall(frame)
    except OSError:
        pass


def validate_hello(frame):
    """Authenticate and validate the first frame from a hub connection.

    Returns the hub ID, boot ID, firmware version, and authorized node mask used
    to constrain every subsequent frame on the connection.
    """
    (
        _magic,
        _version,
        message_type,
        header_node_id,
        flags,
        payload_length,
        session_id,
        sequence,
        _timestamp_us,
    ) = unpack_header(frame)

    if message_type != MESSAGE_TYPE_HELLO:
        raise ProtocolError(
            ERROR_EXPECTED_HELLO,
            message_type,
            "DATA/control received before HELLO",
        )

    if payload_length != HELLO_PAYLOAD_SIZE:
        raise ProtocolError(
            ERROR_INVALID_HELLO,
            message_type,
            f"HELLO payload must be {HELLO_PAYLOAD_SIZE} bytes",
        )

    if session_id != 0 or sequence != 0 or flags != 0:
        raise ProtocolError(
            ERROR_INVALID_HELLO,
            message_type,
            "HELLO must use zero session, sequence, and flags",
        )

    (
        payload_hub_node_id,
        firmware_major,
        firmware_minor,
        firmware_patch,
        boot_id,
        supported_sampling_rates,
        assigned_node_mask,
        reserved,
        device_token,
    ) = struct.unpack(HELLO_PAYLOAD_FORMAT, frame[HEADER_SIZE:])

    if (
        payload_hub_node_id != header_node_id
        or header_node_id not in HUB_CONFIG
        or boot_id == 0
        or reserved != 0
        or not (supported_sampling_rates & (SAMPLE_RATE_50_HZ | SAMPLE_RATE_100_HZ))
    ):
        raise ProtocolError(
            ERROR_INVALID_HELLO,
            message_type,
            "HELLO identity, boot ID, capability, or reserved field is invalid",
        )

    expected = HUB_CONFIG[header_node_id]

    if (
        assigned_node_mask != expected["assigned_node_mask"]
        or not secrets.compare_digest(
            device_token,
            expected["device_token"],
        )
    ):
        raise ProtocolError(
            ERROR_AUTHENTICATION_FAILED,
            message_type,
            "HELLO assigned-node mask or development token is invalid",
        )

    firmware = (
        firmware_major,
        firmware_minor,
        firmware_patch,
    )
    return header_node_id, boot_id, firmware, assigned_node_mask


def process_data(
    frame, stream_state, session_id, assigned_node_mask, clock_mapping=None,
    sample_rate_hz=100,
):
    """Validate one DATA frame and update per-stream sequence/timestamp health.

    Normal and benchmark DATA have independent stream keys. Periodic logging
    reports decoded sensor values while warnings expose gaps or timing faults.
    """
    (
        _magic,
        _version,
        message_type,
        node_id,
        flags,
        payload_length,
        received_session_id,
        sequence,
        timestamp_us,
    ) = unpack_header(frame)

    if received_session_id != session_id:
        raise ProtocolError(
            ERROR_INVALID_SESSION,
            message_type,
            f"Invalid Session ID {received_session_id}",
        )

    if message_type != MESSAGE_TYPE_DATA:
        raise ProtocolError(
            ERROR_UNEXPECTED_MESSAGE,
            message_type,
            f"Unsupported Stage 3 message type 0x{message_type:02X}",
        )

    if payload_length != DATA_PAYLOAD_SIZE:
        raise ProtocolError(
            ERROR_INVALID_FRAME,
            message_type,
            f"DATA payload must be {DATA_PAYLOAD_SIZE} bytes",
        )

    if node_id < 1 or node_id > 6 or not (
        assigned_node_mask & (1 << node_id)
    ):
        raise ProtocolError(
            ERROR_UNAUTHORIZED_NODE,
            message_type,
            f"Connection is not authorized to forward Node {node_id}",
        )

    (
        ax,
        ay,
        az,
        gx,
        gy,
        gz,
        battery_mv,
        status,
    ) = struct.unpack(DATA_PAYLOAD_FORMAT, frame[HEADER_SIZE:])
    is_benchmark = bool(status & STATUS_BENCHMARK)
    coordinator_timestamp_us = (
        timestamp_us - clock_mapping.node_minus_coordinator_us
        if clock_mapping is not None else None
    )

    stream_key = (
        session_id, node_id, message_type,
        bool(status & STATUS_BENCHMARK),
    )
    previous = stream_state.get(stream_key)
    # A scheduled START deliberately resets the source Sequence to zero even
    # when another node's reconnect leaves this Session alive.
    if sequence == 0:
        previous = None
    timestamp_delta_us = None

    if previous is not None:
        previous_sequence, previous_timestamp_us = previous
        expected_sequence = (previous_sequence + 1) & 0xFFFFFFFF

        if sequence != expected_sequence:
            log_hub(
                logging.WARNING,
                node_id,
                "Sequence discontinuity: "
                f"node={node_id} expected={expected_sequence} "
                f"received={sequence}")

        timestamp_delta_us = timestamp_us - previous_timestamp_us

        if timestamp_delta_us <= 0:
            log_hub(
                logging.WARNING,
                node_id,
                "Non-monotonic timestamp: "
                f"node={node_id} previous={previous_timestamp_us} "
                f"received={timestamp_us}")
        elif not is_benchmark:
            expected_interval_us = 1_000_000 / sample_rate_hz
            minimum_interval_us = expected_interval_us * (
                1 - SAMPLE_INTERVAL_TOLERANCE)
            maximum_interval_us = expected_interval_us * (
                1 + SAMPLE_INTERVAL_TOLERANCE)
            if not minimum_interval_us <= timestamp_delta_us <= maximum_interval_us:
                log_hub(
                    logging.WARNING,
                    node_id,
                    "Unusual sample interval: "
                    f"node={node_id} delta_us={timestamp_delta_us} "
                    f"expected_us={expected_interval_us:.0f} "
                    f"tolerance={SAMPLE_INTERVAL_TOLERANCE:.0%}")

    stream_state[stream_key] = (sequence, timestamp_us)

    log_interval = (
        5000 if is_benchmark
        else max(1, round(sample_rate_hz * DATA_LOG_PERIOD_SECONDS))
    )
    if sequence % log_interval == 0:
        interval_text = (
            "first"
            if timestamp_delta_us is None
            else f"{timestamp_delta_us}us"
        )
        log_hub(
            logging.INFO,
            node_id,
            f"DATA node={node_id} session={session_id} "
            f"sequence={sequence} timestamp_us={timestamp_us} "
            f"coordinator_timestamp_us={coordinator_timestamp_us} "
            f"interval={interval_text} flags=0x{flags:02X}")
        log_hub(
            logging.INFO,
            node_id,
            f"  accel=({ax}, {ay}, {az}) "
            f"gyro=({gx}, {gy}, {gz}) "
            f"battery={battery_mv}mV status={describe_status(status)}")

    if is_benchmark or coordinator_timestamp_us is None:
        return None
    return DecodedImuSample(
        node_id=node_id,
        session_id=session_id,
        sequence=sequence,
        timestamp_us=timestamp_us,
        coordinator_timestamp_us=coordinator_timestamp_us,
        ax=ax,
        ay=ay,
        az=az,
        gx=gx,
        gy=gy,
        gz=gz,
        battery_mv=battery_mv,
        status=status,
        flags=flags,
    )


def process_physical_data(
    frame, stream_state, session_id, assigned_node_mask, clock_mapping,
    sample_rate_hz=50,
):
    """Validate physical-unit samples with strict replay and chronology checks."""
    header = unpack_header(frame)
    message_type, node_id, flags = header[2], header[3], header[4]
    payload_length, received_session_id = header[5], header[6]
    sequence, timestamp_us = header[7], header[8]
    if received_session_id != session_id:
        raise ProtocolError(ERROR_INVALID_SESSION, message_type,
                            f"Invalid Session ID {received_session_id}")
    if message_type != MESSAGE_TYPE_PHYSICAL_DATA:
        raise ProtocolError(ERROR_UNEXPECTED_MESSAGE, message_type,
                            "Expected PHYSICAL_DATA")
    if payload_length != PHYSICAL_DATA_PAYLOAD_SIZE or len(frame) != (
            HEADER_SIZE + PHYSICAL_DATA_PAYLOAD_SIZE):
        raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                            "PHYSICAL_DATA payload has invalid length")
    if node_id < 1 or node_id > 6 or not assigned_node_mask & (1 << node_id):
        raise ProtocolError(ERROR_UNAUTHORIZED_NODE, message_type,
                            f"Connection is not authorized for Node {node_id}")
    ax, ay, az, gx, gy, gz, status = struct.unpack(
        "!ffffffH", frame[HEADER_SIZE:])
    axes = (ax, ay, az, gx, gy, gz)
    if not all(math.isfinite(value) for value in axes):
        raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                            "PHYSICAL_DATA contains NaN or infinity")
    if any(abs(value) > PHYSICAL_MAX_ACCEL_MPS2 for value in axes[:3]) or any(
            abs(value) > PHYSICAL_MAX_GYRO_RAD_S for value in axes[3:]):
        raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                            "PHYSICAL_DATA value is outside plausible range")
    if status & ~KNOWN_STATUS_MASK or not status & STATUS_SENSOR_VALID:
        raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                            "PHYSICAL_DATA status is invalid")
    if clock_mapping is None:
        return None
    key = (session_id, node_id, MESSAGE_TYPE_PHYSICAL_DATA)
    previous = stream_state.get(key)
    if previous is not None:
        previous_sequence, previous_timestamp_us = previous
        if sequence <= previous_sequence:
            raise ProtocolError(ERROR_INVALID_SEQUENCE, message_type,
                                "PHYSICAL_DATA replay or duplicate Sequence")
        if timestamp_us <= previous_timestamp_us:
            raise ProtocolError(ERROR_INVALID_FRAME, message_type,
                                "PHYSICAL_DATA capture timestamp regressed")
        if sequence != (previous_sequence + 1) & 0xFFFFFFFF:
            log_hub(logging.WARNING, node_id,
                    f"PHYSICAL_DATA_GAP node={node_id} previous={previous_sequence} "
                    f"received={sequence}")
    stream_state[key] = (sequence, timestamp_us)
    coordinator_timestamp_us = timestamp_us - clock_mapping.node_minus_coordinator_us
    if sequence % max(1, sample_rate_hz) == 0:
        log_hub(logging.INFO, node_id,
                f"PHYSICAL_DATA node={node_id} sequence={sequence} "
                f"timestamp_us={timestamp_us} "
                f"coordinator_timestamp_us={coordinator_timestamp_us} "
                f"historical={bool(status & STATUS_HISTORICAL)}")
    return DecodedImuSample(
        node_id=node_id, session_id=session_id, sequence=sequence,
        timestamp_us=timestamp_us,
        coordinator_timestamp_us=coordinator_timestamp_us,
        ax=ax, ay=ay, az=az, gx=gx, gy=gy, gz=gz,
        battery_mv=0, status=status, flags=flags,
    )


def process_activity_trigger(frame, connection):
    """Validate a Session/boot-bound trigger before coordinator processing."""
    header = unpack_header(frame)
    if (header[2] != MESSAGE_TYPE_ACTIVITY_TRIGGER or
            header[3] != connection.node_id or header[4] != 0 or
            header[5] != ACTIVITY_TRIGGER_PAYLOAD_SIZE or
            header[6] != connection.session_id or
            len(frame) != HEADER_SIZE + ACTIVITY_TRIGGER_PAYLOAD_SIZE):
        raise ProtocolError(ERROR_INVALID_FRAME, MESSAGE_TYPE_ACTIVITY_TRIGGER,
                            "Invalid ACTIVITY_TRIGGER header")
    boot_id, arm_generation, peak_timestamp_us = struct.unpack(
        "!IIQ", frame[HEADER_SIZE:])
    if (boot_id == 0 or arm_generation == 0 or
            peak_timestamp_us > header[8]):
        raise ProtocolError(ERROR_INVALID_FRAME, MESSAGE_TYPE_ACTIVITY_TRIGGER,
                            "Invalid ACTIVITY_TRIGGER payload")
    return connection.coordinator.activity_triggered(
        connection, header, (boot_id, arm_generation, peak_timestamp_us))


def log_window_summary(window):
    """Log compact window counts and provisional cross-node skew."""
    quality = window.quality
    counts = " ".join(
        f"node{node_id}={quality.sample_counts[node_id]}/"
        f"{quality.expected_per_node}"
        for node_id in sorted(quality.sample_counts)
    )
    prefix = "WINDOW_READY" if quality.accepted else "WINDOW_REJECTED"
    reason = "" if quality.accepted else f" reason={quality.rejection_reason}"
    skew = (
        "None" if quality.max_index_skew_us is None
        else str(quality.max_index_skew_us)
    )
    LOGGER.info(
        "%s run=%d window=%d interval=[%d,%d)%s %s max_index_skew_us=%s",
        prefix, window.run_id, window.window_id, window.start_us, window.end_us,
        reason, counts, skew)


def process_benchmark_report(frame, session_id, hub_node_id):
    """Validate and print one hub benchmark summary for the active Session."""
    header = unpack_header(frame)
    if (header[2] != MESSAGE_TYPE_BENCHMARK_REPORT or
            header[3] != hub_node_id or header[4] != 0 or
            header[6] != session_id or header[5] != 24):
        raise ProtocolError(
            ERROR_INVALID_FRAME, MESSAGE_TYPE_BENCHMARK_REPORT,
            "Invalid BENCHMARK_REPORT header",
        )
    values = struct.unpack("!IIIIII", frame[HEADER_SIZE:])
    log_hub(
        logging.INFO,
        hub_node_id,
        f"BENCHMARK_REPORT node={hub_node_id} session={session_id} "
        f"control_sequence={values[0]} elapsed_ms={values[1]} "
        f"frames={values[2]} bytes={values[3]} queue_drops={values[4]} "
        f"send_failures={values[5]}")


def process_heartbeat(
    frame,
    stream_state,
    session_id,
    assigned_node_mask,
):
    """Validate a source HEARTBEAT and track its sequence and one-second cadence."""
    (
        _magic,
        _version,
        message_type,
        node_id,
        flags,
        payload_length,
        received_session_id,
        sequence,
        timestamp_us,
    ) = unpack_header(frame)

    if received_session_id != session_id:
        raise ProtocolError(
            ERROR_INVALID_SESSION,
            message_type,
            f"Invalid Session ID {received_session_id}",
        )

    if payload_length != struct.calcsize(HEARTBEAT_PAYLOAD_FORMAT):
        raise ProtocolError(
            ERROR_INVALID_FRAME,
            message_type,
            "HEARTBEAT payload must be 8 bytes",
        )

    if node_id < 1 or node_id > 6 or not (
        assigned_node_mask & (1 << node_id)
    ):
        raise ProtocolError(
            ERROR_UNAUTHORIZED_NODE,
            message_type,
            f"Connection is not authorized to report Node {node_id}",
        )

    battery_mv, status, overflow_count = struct.unpack(
        HEARTBEAT_PAYLOAD_FORMAT,
        frame[HEADER_SIZE:],
    )

    stream_key = (session_id, node_id, message_type)
    previous = stream_state.get(stream_key)

    if previous is not None:
        previous_sequence, previous_timestamp_us = previous
        expected_sequence = (previous_sequence + 1) & 0xFFFFFFFF

        if sequence != expected_sequence:
            log_hub(
                logging.WARNING,
                node_id,
                "HEARTBEAT sequence discontinuity: "
                f"node={node_id} expected={expected_sequence} "
                f"received={sequence}")

        # HEARTBEAT is intentionally suppressed while DATA proves liveness.
        # Do not interpret that DATA period as a missed-heartbeat interval;
        # cadence validation resumes with the next consecutive HEARTBEAT.
        data_since_previous_heartbeat = any(
            data_state is not None and
            data_state[1] > previous_timestamp_us
            for data_state in (
                stream_state.get(
                    (session_id, node_id, MESSAGE_TYPE_DATA, False)),
                stream_state.get(
                    (session_id, node_id, MESSAGE_TYPE_DATA, True)),
            )
        )
        interval_us = timestamp_us - previous_timestamp_us
        if (not data_since_previous_heartbeat and
                not 800000 <= interval_us <= 1200000):
            log_hub(
                logging.WARNING,
                node_id,
                "Unusual HEARTBEAT interval: "
                f"node={node_id} delta_us={interval_us}")

    stream_state[stream_key] = (sequence, timestamp_us)

    log_hub(
        logging.INFO,
        node_id,
        f"HEARTBEAT node={node_id} session={session_id} "
        f"sequence={sequence} timestamp_us={timestamp_us} "
        f"flags=0x{flags:02X} battery={battery_mv}mV "
        f"status={describe_status(status)} overflow={overflow_count}")


def process_test_response(frame, session_id, hub_node_id):
    """Validate and decode one hub TEST_RESPONSE.

    A valid response must belong to the active hub Session and carry both the
    ``mock_data`` and ``test_request_processed`` status bits. The returned raw
    payload is retained by the replay tracker for exact duplicate comparison.
    """
    (
        _magic, _version, message_type, node_id, flags, payload_length,
        received_session_id, sequence, timestamp_us,
    ) = unpack_header(frame)

    if received_session_id != session_id:
        raise ProtocolError(
            ERROR_INVALID_SESSION, message_type,
            f"Invalid Session ID {received_session_id}",
        )
    if node_id != hub_node_id:
        raise ProtocolError(
            ERROR_UNAUTHORIZED_NODE, message_type,
            f"TEST_RESPONSE Node {node_id} does not match connection Node {hub_node_id}",
        )
    if flags != 0 or payload_length != DATA_PAYLOAD_SIZE:
        raise ProtocolError(
            ERROR_INVALID_FRAME, message_type,
            "TEST_RESPONSE flags or payload length is invalid",
        )

    values = struct.unpack(DATA_PAYLOAD_FORMAT, frame[HEADER_SIZE:])
    status = values[7]
    required_status = STATUS_MOCK_DATA | STATUS_TEST_REQUEST_PROCESSED
    if status & required_status != required_status:
        raise ProtocolError(
            ERROR_INVALID_FRAME, message_type,
            "TEST_RESPONSE is missing mock_data or test_request_processed status",
        )

    log_hub(
        logging.INFO,
        node_id,
        f"TEST_RESPONSE node={node_id} session={session_id} "
        f"sequence={sequence} timestamp_us={timestamp_us} "
        f"accel={values[:3]} gyro={values[3:6]} "
        f"battery={values[6]}mV status={describe_status(status)}")
    return sequence, values, frame[HEADER_SIZE:]


def process_hub_error(frame, session_id, hub_node_id):
    """Validate and print an ERROR reported by the authenticated hub."""
    (
        _magic, _version, message_type, node_id, flags, payload_length,
        received_session_id, sequence, timestamp_us,
    ) = unpack_header(frame)

    if received_session_id != session_id:
        raise ProtocolError(
            ERROR_INVALID_SESSION, message_type,
            f"Invalid Session ID {received_session_id}",
        )
    if node_id != hub_node_id:
        raise ProtocolError(
            ERROR_UNAUTHORIZED_NODE, message_type,
            f"ERROR Node {node_id} does not match connection Node {hub_node_id}",
        )
    if flags != 0 or payload_length != ERROR_PAYLOAD_SIZE:
        raise ProtocolError(
            ERROR_INVALID_FRAME, message_type,
            "Node ERROR flags or payload length is invalid",
        )

    error_code, offending_type, reserved = struct.unpack(
        ERROR_PAYLOAD_FORMAT, frame[HEADER_SIZE:]
    )
    if reserved != 0:
        raise ProtocolError(
            ERROR_INVALID_FRAME, message_type,
            "Node ERROR reserved byte must be zero",
        )

    log_hub(
        logging.INFO,
        node_id,
        f"NODE_ERROR node={node_id} session={session_id} sequence={sequence} "
        f"timestamp_us={timestamp_us} code={error_code} "
        f"offending_type=0x{offending_type:02X}")


def process_control_ack(frame, connection):
    """Validate an ESP START/STOP result and update group control state."""
    header = unpack_header(frame)
    if (header[2] != MESSAGE_TYPE_CONTROL_ACK or
            header[3] != connection.node_id or header[4] != 0 or
            header[5] != CONTROL_ACK_PAYLOAD_SIZE or
            header[6] != connection.session_id):
        raise ProtocolError(
            ERROR_INVALID_FRAME, MESSAGE_TYPE_CONTROL_ACK,
            "Invalid CONTROL_ACK header",
        )
    command_type, result, reserved, command_sequence, effective_local_us = \
        struct.unpack("!BBHIQ", frame[HEADER_SIZE:])
    if reserved != 0 or header[7] != command_sequence:
        raise ProtocolError(
            ERROR_INVALID_FRAME, MESSAGE_TYPE_CONTROL_ACK,
            "Invalid CONTROL_ACK reserved field or Sequence",
        )
    connection.coordinator.control_acknowledged(
        connection, command_type, command_sequence, result,
        effective_local_us,
    )


def handle_client(client, address, visualizer_publisher=None, coordinator=None,
                  window_assembler=None, sample_rate_hz=50):
    """Run one hub connection's HELLO-gated binary protocol state machine.

    Fragmented and coalesced input is accumulated until a complete framed
    message is available. Every post-HELLO message is checked against the
    connection's active Session and authenticated Hub identity. Protocol errors
    send one ERROR frame and close only this client; other hub workers continue.
    """
    LOGGER.info("Connected: %s", address)
    receive_buffer = bytearray()
    stream_state = {}
    session_id = 0
    assigned_node_mask = 0
    handshake_complete = False
    file_verifier = FileVerifier()
    response_tracker = TestResponseTracker()
    control_connection = None
    last_received_at = time.monotonic()

    def cleanup_connection():
        if coordinator is not None and control_connection is not None:
            coordinator.unregister(control_connection)

    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    client.settimeout(HELLO_TIMEOUT_SECONDS)

    with ManagedClient(client, cleanup_connection):
        while True:
            try:
                data = client.recv(1024)
            except socket.timeout:
                if not handshake_complete:
                    LOGGER.warning("HELLO timeout: %s", address)
                    return
                if control_connection is None:
                    LOGGER.warning("Client inactive for 3 seconds: %s", address)
                    return
                control_connection.tick()
                if time.monotonic() - last_received_at >= CLIENT_INACTIVITY_TIMEOUT_SECONDS:
                    LOGGER.warning("Client inactive for 3 seconds: %s", address)
                    return
                continue
            except ConnectionResetError:
                LOGGER.warning("Connection reset: %s", address)
                return
            except OSError as error:
                LOGGER.warning("Socket error from %s: %s", address, error)
                return

            if not data:
                LOGGER.info("Disconnected: %s", address)
                return

            last_received_at = time.monotonic()
            receive_buffer.extend(data)

            while len(receive_buffer) >= HEADER_SIZE:
                try:
                    (
                        magic,
                        version,
                        message_type,
                        _node_id,
                        _flags,
                        payload_length,
                        _received_session_id,
                        _sequence,
                        _timestamp_us,
                    ) = struct.unpack_from(HEADER_FORMAT, receive_buffer)

                    if magic != MAGIC:
                        raise ProtocolError(
                            ERROR_INVALID_FRAME,
                            message_type,
                            f"Invalid magic 0x{magic:04X}",
                        )

                    if version != PROTOCOL_VERSION:
                        raise ProtocolError(
                            ERROR_INVALID_FRAME,
                            message_type,
                            f"Unsupported version {version}",
                        )

                    if payload_length > MAX_PAYLOAD_SIZE:
                        raise ProtocolError(
                            ERROR_INVALID_FRAME,
                            message_type,
                            f"Payload length {payload_length} exceeds limit",
                        )

                    frame_length = HEADER_SIZE + payload_length

                    if len(receive_buffer) < frame_length:
                        break

                    frame = bytes(receive_buffer[:frame_length])
                    del receive_buffer[:frame_length]

                    if not handshake_complete:
                        (
                            hub_node_id,
                            boot_id,
                            firmware,
                            assigned_node_mask,
                        ) = validate_hello(frame)

                        session_id = allocate_session_id()
                        send_hello_ack(client, session_id)
                        handshake_complete = True
                        client.settimeout(
                            CLIENT_POLL_INTERVAL_SECONDS
                            if coordinator is not None
                            else CLIENT_INACTIVITY_TIMEOUT_SECONDS
                        )

                        log_hub(
                            logging.INFO,
                            hub_node_id,
                            f"HELLO accepted: node={hub_node_id} "
                            f"firmware={'.'.join(map(str, firmware))} "
                            f"boot_id={boot_id} "
                            f"assigned_mask=0x{assigned_node_mask:02X} "
                            f"session={session_id}")
                        if coordinator is not None:
                            control_connection = NodeControlConnection(
                                client, hub_node_id, session_id, coordinator,
                                boot_id=boot_id)
                            coordinator.register(control_connection)
                            control_connection.begin_sync()
                    elif message_type == MESSAGE_TYPE_SYNC_RESP:
                        if control_connection is None:
                            raise ProtocolError(
                                ERROR_UNEXPECTED_MESSAGE,
                                message_type,
                                "SYNC_RESP is not enabled for this connection",
                            )
                        control_connection.process_sync_response(frame)
                    elif message_type == MESSAGE_TYPE_CONTROL_ACK:
                        if control_connection is None:
                            raise ProtocolError(
                                ERROR_UNEXPECTED_MESSAGE,
                                message_type,
                                "CONTROL_ACK is not enabled for this connection",
                            )
                        process_control_ack(frame, control_connection)
                    elif message_type == MESSAGE_TYPE_ACTIVITY_TRIGGER:
                        if control_connection is None:
                            raise ProtocolError(
                                ERROR_UNEXPECTED_MESSAGE, message_type,
                                "ACTIVITY_TRIGGER is not enabled for this connection",
                            )
                        process_activity_trigger(frame, control_connection)
                    elif message_type == MESSAGE_TYPE_PHYSICAL_DATA:
                        sample = process_physical_data(
                            frame, stream_state, session_id, assigned_node_mask,
                            control_connection.get_mapping()
                            if control_connection is not None else None,
                            sample_rate_hz,
                        )
                        if sample is not None and window_assembler is not None:
                            for window in window_assembler.submit(sample):
                                log_window_summary(window)
                    elif message_type == MESSAGE_TYPE_DATA:
                        sample = process_data(
                            frame,
                            stream_state,
                            session_id,
                            assigned_node_mask,
                            control_connection.get_mapping()
                            if control_connection is not None else None,
                            sample_rate_hz,
                        )
                        if sample is not None and window_assembler is not None:
                            for window in window_assembler.submit(sample):
                                log_window_summary(window)
                    elif message_type == MESSAGE_TYPE_HEARTBEAT:
                        process_heartbeat(
                            frame,
                            stream_state,
                            session_id,
                            assigned_node_mask,
                        )
                    elif message_type == MESSAGE_TYPE_TEST_RESPONSE:
                        # Full structural, Session, identity, payload, and status
                        # validation must finish before replay state or publication
                        # is touched.
                        response_sequence, response_values, response_payload = process_test_response(
                            frame,
                            session_id,
                            hub_node_id,
                        )
                        if response_tracker.accept(response_sequence, response_payload):
                            if visualizer_publisher is not None:
                                # The publisher returns identifiers immediately;
                                # socket I/O happens later on its bounded worker.
                                event = visualizer_publisher.publish_for_response()
                                log_hub(
                                    logging.INFO,
                                    hub_node_id,
                                    f"VISUALIZER_EVENT node={hub_node_id} "
                                    f"session={session_id} "
                                    f"request_sequence={response_sequence} "
                                    f"accel={response_values[:3]} "
                                    f"gyro={response_values[3:6]} "
                                    f"battery={response_values[6]}mV "
                                    f"status={describe_status(response_values[7])} "
                                    f"visualizer_run_id={event['run_id']} "
                                    f"visualizer_sequence={event['seq']} "
                                    f"exercise={event['exercise']}")
                    elif message_type == MESSAGE_TYPE_BENCHMARK_REPORT:
                        process_benchmark_report(frame, session_id, hub_node_id)
                    elif message_type in (MESSAGE_TYPE_FILE_BEGIN,
                                           MESSAGE_TYPE_FILE_CHUNK,
                                           MESSAGE_TYPE_FILE_END):
                        file_verifier.process(frame, session_id, hub_node_id)
                    elif message_type == MESSAGE_TYPE_FILE_RESULT:
                        process_file_result(frame, session_id, hub_node_id,
                                            file_verifier)
                    elif message_type == MESSAGE_TYPE_ERROR:
                        process_hub_error(
                            frame,
                            session_id,
                            hub_node_id,
                        )
                    else:
                        raise ProtocolError(
                            ERROR_UNEXPECTED_MESSAGE,
                            message_type,
                            "Unexpected message after HELLO",
                        )
                except (ProtocolError, struct.error) as error:
                    if isinstance(error, ProtocolError):
                        code = error.code
                        offending_type = error.offending_type
                    else:
                        code = ERROR_INVALID_FRAME
                        offending_type = message_type

                    LOGGER.warning("Protocol rejection: %s", error)
                    send_error(
                        client,
                        session_id if handshake_complete else 0,
                        code,
                        offending_type,
                    )
                    return
            if control_connection is not None:
                control_connection.tick()
    file_verifier.reset(remove=file_verifier.completed is None)


def start_client_worker(
    client, address, visualizer_publisher=None, coordinator=None,
    window_assembler=None, sample_rate_hz=50,
):
    """Start one daemon protocol worker for an accepted hub TCP connection."""
    worker = threading.Thread(
        target=handle_client,
        args=(client, address, visualizer_publisher, coordinator,
              window_assembler, sample_rate_hz),
        daemon=True,
        name=f"hub-client-{address[0]}-{address[1]}",
    )
    worker.start()
    return worker


def positive_integer(value):
    """Parse a strictly positive command-line integer."""
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def positive_float(value):
    """Parse a finite, strictly positive command-line number."""
    parsed = float(value)
    if not parsed > 0 or not math.isfinite(parsed):
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return parsed


def nonnegative_integer(value):
    """Parse a command-line integer that permits zero backups."""
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return parsed


def parse_args(argv=None):
    """Parse rotating-log options for the Ultra96 sensor service."""
    parser = argparse.ArgumentParser(
        description="Receive and validate framed FireBeetle node traffic.")
    parser.add_argument(
        "--log-file",
        default=DEFAULT_LOG_FILE,
        help=f"plain-text rotating log path (default: {DEFAULT_LOG_FILE})",
    )
    parser.add_argument(
        "--log-max-mb",
        type=positive_integer,
        default=DEFAULT_LOG_MAX_BYTES // (1024 * 1024),
        help="maximum size of each log file in MiB (default: 10)",
    )
    parser.add_argument(
        "--log-backups",
        type=nonnegative_integer,
        default=DEFAULT_LOG_BACKUP_COUNT,
        help="number of older rotated files to retain (default: 5)",
    )
    parser.add_argument(
        "--sample-rate-hz",
        type=positive_integer,
        default=50,
        help="expected samples per second for window quality (default: 50)",
    )
    parser.add_argument(
        "--window-seconds",
        type=positive_float,
        default=5.0,
        help="sliding-window length in seconds (default: 5.0)",
    )
    parser.add_argument(
        "--hop-seconds",
        type=positive_float,
        default=1.0,
        help="time between window starts in seconds (default: 1.0)",
    )
    args = parser.parse_args(argv)
    if args.hop_seconds > args.window_seconds:
        parser.error("--hop-seconds must not exceed --window-seconds")
    return args


def main(argv=None):
    """Serve hub connections with one shared run-wide visualizer publisher."""
    args = parse_args(argv)
    configure_logging(
        args.log_file,
        args.log_max_mb * 1024 * 1024,
        args.log_backups,
    )
    LOGGER.info(
        "Logging to %s rotation=%dMiB backups=%d",
        args.log_file, args.log_max_mb, args.log_backups)

    window_config = WindowConfig(
        sample_rate_hz=args.sample_rate_hz,
        window_us=round(args.window_seconds * 1_000_000),
        hop_us=round(args.hop_seconds * 1_000_000),
    )
    window_assembler = WindowAssembler(window_config)
    LOGGER.info(
        "WINDOW_CONFIG sample_rate=%dHz window=%.3fs hop=%.3fs "
        "expected_per_node=%d",
        window_config.sample_rate_hz, window_config.window_us / 1_000_000,
        window_config.hop_us / 1_000_000, window_config.expected_per_node)

    visualizer_publisher = VisualizerEventPublisher()
    coordinator = CaptureCoordinator(
        window_assembler=window_assembler, auto_start=True,
        physical_mode=True)
    control_thread = threading.Thread(
        target=run_control_console,
        args=(coordinator,),
        daemon=True,
        name="control-console",
    )
    control_thread.start()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((HOST, PORT))
            server.listen()

            LOGGER.info("Listening on %s:%d", HOST, PORT)

            while True:
                # Give every accepted hub connection an independent worker thread.
                client, address = server.accept()
                start_client_worker(
                    client, address, visualizer_publisher, coordinator,
                    window_assembler, args.sample_rate_hz)
    finally:
        coordinator.stop_all()
        visualizer_publisher.close()


if __name__ == "__main__":
    main()
