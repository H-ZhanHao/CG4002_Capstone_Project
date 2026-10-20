"""Thread-safe capture-time sliding windows for the two sensor nodes."""

from collections import deque
from dataclasses import dataclass
import logging
import threading


LOGGER = logging.getLogger("server")
_MISSING_SEQUENCE_LIMIT = 100


@dataclass(frozen=True)
class WindowConfig:
    sample_rate_hz: int
    window_us: int
    hop_us: int
    required_nodes: tuple[int, ...] = (1, 2)

    def __post_init__(self):
        if self.sample_rate_hz <= 0 or self.window_us <= 0 or self.hop_us <= 0:
            raise ValueError("sample rate, window, and hop must be positive")
        if self.hop_us > self.window_us:
            raise ValueError("hop must not exceed window length")
        if not self.required_nodes or len(set(self.required_nodes)) != len(
                self.required_nodes):
            raise ValueError("required nodes must be non-empty and unique")
        if self.window_us * self.sample_rate_hz % 1_000_000:
            raise ValueError("window length must contain a whole sample count")

    @property
    def expected_per_node(self):
        return self.window_us * self.sample_rate_hz // 1_000_000


@dataclass(frozen=True)
class DecodedImuSample:
    node_id: int
    session_id: int
    sequence: int
    timestamp_us: int
    coordinator_timestamp_us: int
    ax: int
    ay: int
    az: int
    gx: int
    gy: int
    gz: int
    battery_mv: int
    status: int
    flags: int


@dataclass(frozen=True)
class WindowQuality:
    expected_per_node: int
    sample_counts: dict[int, int]
    missing_sequences: dict[int, tuple[int, ...]]
    max_index_skew_us: int | None
    accepted: bool
    rejection_reason: str | None


@dataclass(frozen=True)
class AssembledWindow:
    run_id: int
    window_id: int
    start_us: int
    end_us: int
    samples_by_node: dict[int, tuple[DecodedImuSample, ...]]
    quality: WindowQuality


class WindowAssembler:
    """Build overlapping windows from mapped capture timestamps."""

    def __init__(self, config: WindowConfig):
        self.config = config
        self._lock = threading.Lock()
        self._run_id = None
        self._start_us = None
        self._next_window_id = 0
        self._buffers = {node_id: deque() for node_id in config.required_nodes}
        self._latest = {node_id: None for node_id in config.required_nodes}

    def start_run(self, run_id: int, start_us: int) -> None:
        with self._lock:
            self._clear_locked()
            self._run_id = run_id
            self._start_us = start_us
            self._next_window_id = 0
        LOGGER.info("WINDOW_RUN_STARTED run=%d start_us=%d", run_id, start_us)

    def stop_run(self, reason: str) -> None:
        with self._lock:
            old_run_id = self._run_id
            self._clear_locked()
            self._run_id = None
            self._start_us = None
            self._next_window_id = 0
        if old_run_id is not None:
            LOGGER.info('WINDOW_RUN_RESET run=%d reason="%s"', old_run_id, reason)

    def submit(self, sample: DecodedImuSample) -> list[AssembledWindow]:
        with self._lock:
            if self._run_id is None or sample.node_id not in self._buffers:
                return []
            if sample.coordinator_timestamp_us < self._start_us:
                return []
            latest = self._latest[sample.node_id]
            if latest is not None and sample.coordinator_timestamp_us < latest:
                LOGGER.warning(
                    "WINDOW_TIMESTAMP_REGRESSION node=%d previous=%d received=%d",
                    sample.node_id, latest, sample.coordinator_timestamp_us)
                return []

            self._buffers[sample.node_id].append(sample)
            self._latest[sample.node_id] = sample.coordinator_timestamp_us
            completed = []
            while self._window_ready_locked():
                completed.append(self._assemble_next_locked())
            return completed

    def _clear_locked(self):
        for samples in self._buffers.values():
            samples.clear()
        for node_id in self._latest:
            self._latest[node_id] = None

    def _window_bounds_locked(self):
        start = self._start_us + self._next_window_id * self.config.hop_us
        return start, start + self.config.window_us

    def _window_ready_locked(self):
        _start, end = self._window_bounds_locked()
        return all(timestamp is not None and timestamp >= end
                   for timestamp in self._latest.values())

    @staticmethod
    def _missing_sequences(samples):
        missing = []
        gap_found = False
        for previous, current in zip(samples, samples[1:]):
            expected = (previous.sequence + 1) & 0xFFFFFFFF
            if current.sequence == expected:
                continue
            gap_found = True
            candidate = expected
            while candidate != current.sequence and len(missing) < _MISSING_SEQUENCE_LIMIT:
                missing.append(candidate)
                candidate = (candidate + 1) & 0xFFFFFFFF
        return tuple(missing), gap_found

    def _assemble_next_locked(self):
        start, end = self._window_bounds_locked()
        selected = {
            node_id: tuple(sample for sample in samples
                           if start <= sample.coordinator_timestamp_us < end)
            for node_id, samples in self._buffers.items()
        }
        counts = {node_id: len(samples) for node_id, samples in selected.items()}
        missing = {}
        has_gap = False
        for node_id, samples in selected.items():
            missing[node_id], node_has_gap = self._missing_sequences(samples)
            has_gap = has_gap or node_has_gap

        expected = self.config.expected_per_node
        accepted = all(count == expected for count in counts.values()) and not has_gap
        sample_lists = list(selected.values())
        if (len(sample_lists) == 2 and len(sample_lists[0]) > 0 and
                len(sample_lists[0]) == len(sample_lists[1])):
            max_skew = max(abs(left.coordinator_timestamp_us -
                               right.coordinator_timestamp_us)
                           for left, right in zip(*sample_lists))
        else:
            max_skew = None
        quality = WindowQuality(
            expected_per_node=expected,
            sample_counts=counts,
            missing_sequences=missing,
            max_index_skew_us=max_skew,
            accepted=accepted,
            rejection_reason=None if accepted else "missing_samples",
        )
        window = AssembledWindow(
            run_id=self._run_id,
            window_id=self._next_window_id,
            start_us=start,
            end_us=end,
            samples_by_node=selected,
            quality=quality,
        )
        self._next_window_id += 1
        next_start, _next_end = self._window_bounds_locked()
        for samples in self._buffers.values():
            while samples and samples[0].coordinator_timestamp_us < next_start:
                samples.popleft()
        return window
