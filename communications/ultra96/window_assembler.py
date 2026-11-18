"""Thread-safe capture-time windows using either or both sensor nodes.

Windows carry only REAL samples. The assembler never fabricates zero-valued
padding samples and never discards real samples to hit an exact count. Each
accepted window hands every deduplicated in-window sample to inference, where
``resample_poly_to_window`` polyphase-resamples the real count (e.g. 298 or
301) to the model's 300-sample grid, the same way training and the reference
tools do.

The resampler only sees a sample count, not timestamps, so a genuine dropout
inside a window slightly compresses time after the hole. To bound that, a
an included node must be missing at most ``missing_limit`` samples
(``max_missing_fraction`` of the expected count, 3% by default). Unavailable or
unusable nodes are omitted. One usable node is sufficient for an accepted
window; a known lagging partner receives only a bounded capture-time grace.
"""

from collections import deque
from dataclasses import dataclass, field
import logging
import math
import threading


LOGGER = logging.getLogger("server")
_MISSING_SEQUENCE_LIMIT = 100


@dataclass(frozen=True)
class WindowConfig:
    sample_rate_hz: int
    window_us: int
    hop_us: int
    required_nodes: tuple[int, ...] = (1, 2)
    max_missing_per_node: int | None = None
    max_extra_per_node: int = 2
    max_missing_fraction: float = 0.03
    node_wait_us: int = 250_000

    def __post_init__(self):
        if self.sample_rate_hz <= 0 or self.window_us <= 0 or self.hop_us <= 0:
            raise ValueError("sample rate, window, and hop must be positive")
        if self.node_wait_us < 0:
            raise ValueError("node wait must not be negative")
        if self.hop_us > self.window_us:
            raise ValueError("hop must not exceed window length")
        if not self.required_nodes or len(set(self.required_nodes)) != len(
                self.required_nodes):
            raise ValueError("required nodes must be non-empty and unique")
        if self.window_us * self.sample_rate_hz % 1_000_000:
            raise ValueError("window length must contain a whole sample count")
        if self.max_missing_per_node is not None and self.max_missing_per_node < 0:
            raise ValueError("maximum missing samples must not be negative")
        if self.max_extra_per_node < 0:
            raise ValueError("maximum extra samples must not be negative")
        if not math.isfinite(self.max_missing_fraction) or not 0 <= self.max_missing_fraction < 1:
            raise ValueError("missing fraction must be finite and in [0, 1)")

    @property
    def missing_limit(self):
        """Most samples a node may be short of ``expected_per_node`` and still
        be accepted. An explicit count overrides the proportional default."""
        if self.max_missing_per_node is not None:
            return self.max_missing_per_node
        return math.floor(self.expected_per_node * self.max_missing_fraction)

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
    status: int
    flags: int
    # Retained for interface compatibility. The assembler no longer creates
    # padding samples, so this is always False for samples in a window.
    is_padding: bool = False


@dataclass(frozen=True)
class WindowQuality:
    expected_per_node: int
    sample_counts: dict[int, int]
    padded_counts: dict[int, int]
    dropped_counts: dict[int, int]
    dropped_sequences: dict[int, tuple[int, ...]]
    missing_sequences: dict[int, tuple[int, ...]]
    max_index_skew_us: int | None
    accepted: bool
    rejection_reason: str | None
    available_nodes: tuple[int, ...] = ()
    node_rejection_reasons: dict[int, str] = field(default_factory=dict)


@dataclass(frozen=True)
class AssembledWindow:
    run_id: int
    window_id: int
    start_us: int
    end_us: int
    samples_by_node: dict[int, tuple[DecodedImuSample, ...]]
    quality: WindowQuality


class WindowAssembler:
    """Build overlapping windows of real samples from mapped capture timestamps."""

    def __init__(self, config: WindowConfig):
        self.config = config
        self._lock = threading.Lock()
        self._run_id = None
        self._start_us = None
        self._next_window_id = 0
        self._buffers = {node_id: deque() for node_id in config.required_nodes}
        self._latest = {node_id: None for node_id in config.required_nodes}
        self._active_nodes = set()

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
            self._active_nodes.add(sample.node_id)
            self._latest[sample.node_id] = sample.coordinator_timestamp_us
            completed = []
            while self._window_ready_locked():
                completed.append(self._assemble_next_locked())
            return completed

    def _clear_locked(self):
        self._active_nodes.clear()
        for samples in self._buffers.values():
            samples.clear()
        for node_id in self._latest:
            self._latest[node_id] = None

    def node_joined(self, node_id):
        """Give an acknowledged/starting producer a bounded first-sample grace."""
        with self._lock:
            self._active_nodes.add(node_id)

    def node_lost(self, node_id):
        """Discard obsolete-session samples without resetting the window origin."""
        with self._lock:
            self._buffers[node_id].clear()
            self._latest[node_id] = None
            self._active_nodes.discard(node_id)

    def _window_bounds_locked(self):
        start = self._start_us + self._next_window_id * self.config.hop_us
        return start, start + self.config.window_us

    def _window_ready_locked(self):
        _start, end = self._window_bounds_locked()
        timestamps = [t for t in self._latest.values() if t is not None]
        return bool(timestamps) and (
            all(self._latest[node] is not None and self._latest[node] >= end
                for node in self._active_nodes) or
            max(timestamps) >= end + self.config.node_wait_us)

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

    @staticmethod
    def _same_transmitted_sample(left, right):
        """Return whether a repeated Sequence carries the same wire data."""
        return (
            left.node_id, left.session_id, left.sequence, left.timestamp_us,
            left.ax, left.ay, left.az, left.gx, left.gy, left.gz,
            left.status, left.flags,
        ) == (
            right.node_id, right.session_id, right.sequence, right.timestamp_us,
            right.ax, right.ay, right.az, right.gx, right.gy, right.gz,
            right.status, right.flags,
        )

    def _deduplicate_samples(self, samples):
        """Keep the first identical Sequence and reject conflicting reuse."""
        by_sequence = {}
        unique = []
        dropped = []
        for sample in samples:
            previous = by_sequence.get(sample.sequence)
            if previous is None:
                by_sequence[sample.sequence] = sample
                unique.append(sample)
                continue
            if not self._same_transmitted_sample(previous, sample):
                return tuple(unique), tuple(dropped), True
            dropped.append(sample.sequence)
        return tuple(unique), tuple(dropped), False

    def _validate_node_samples(self, unique, duplicate_sequences,
                               conflicting_duplicate, missing_sequences, has_gap):
        """Return ``None`` if a node's real samples are usable, else a reason.

        The samples themselves are never modified: no padding, no dropping.
        Only exact re-transmissions (same Sequence, same data) were removed
        earlier by deduplication.
        """
        expected = self.config.expected_per_node
        if conflicting_duplicate:
            return "conflicting_duplicate"
        if not unique:
            return "missing_samples"
        if len(duplicate_sequences) > self.config.max_extra_per_node:
            return "too_many_extra_samples"
        if any(right.timestamp_us <= left.timestamp_us or
               right.coordinator_timestamp_us <= left.coordinator_timestamp_us
               for left, right in zip(unique, unique[1:])):
            return "invalid_timestamps"

        extra = len(unique) - expected
        if extra > 0:
            # A few extra real samples come from the device clock running
            # slightly fast relative to the window boundary; the resampler
            # absorbs them. A sequence gap together with extra samples, or more
            # extras than allowed, points to broken timing rather than drift.
            if has_gap:
                return "sequence_gap_with_extra_samples"
            if len(duplicate_sequences) + extra > self.config.max_extra_per_node:
                return "too_many_extra_samples"
            return None

        deficit = -extra
        if deficit > self.config.missing_limit:
            return "missing_samples"
        if has_gap and len(missing_sequences) > deficit:
            # A gap without a matching count deficit indicates a duplicate,
            # reordering, or replacement sample rather than simple packet loss.
            return "sequence_gap_without_deficit"
        return None

    def _assemble_next_locked(self):
        start, end = self._window_bounds_locked()
        selected = {
            node_id: tuple(sample for sample in samples
                           if start <= sample.coordinator_timestamp_us < end)
            for node_id, samples in self._buffers.items()
        }
        counts = {node_id: len(samples) for node_id, samples in selected.items()}
        real_samples = {}
        dropped = {}
        missing = {}
        invalid_reasons = {}
        for node_id, samples in selected.items():
            unique, duplicate_sequences, conflicting_duplicate = (
                self._deduplicate_samples(samples))
            missing[node_id], has_gap = self._missing_sequences(unique)
            dropped[node_id] = tuple(duplicate_sequences)
            invalid_reasons[node_id] = self._validate_node_samples(
                unique, duplicate_sequences, conflicting_duplicate,
                missing[node_id], has_gap)
            real_samples[node_id] = unique

        available = tuple(node for node, reason in invalid_reasons.items()
                          if reason is None)
        accepted = bool(available)
        rejection_reason = None
        if not accepted:
            rejection_reason = next(
                reason for reason in invalid_reasons.values() if reason is not None)

        sample_lists = [real_samples[node] for node in available]
        if (len(sample_lists) == 2 and len(sample_lists[0]) > 0 and
                len(sample_lists[0]) == len(sample_lists[1])):
            max_skew = max(abs(left.coordinator_timestamp_us -
                               right.coordinator_timestamp_us)
                           for left, right in zip(*sample_lists))
        else:
            max_skew = None
        quality = WindowQuality(
            expected_per_node=self.config.expected_per_node,
            sample_counts=counts,
            padded_counts={node_id: 0 for node_id in selected},
            dropped_counts={node_id: len(dropped[node_id]) for node_id in selected},
            dropped_sequences=dropped,
            missing_sequences=missing,
            max_index_skew_us=max_skew,
            accepted=accepted,
            rejection_reason=rejection_reason,
            available_nodes=available,
            node_rejection_reasons={node: reason for node, reason in invalid_reasons.items()
                                    if reason is not None},
        )
        window = AssembledWindow(
            run_id=self._run_id,
            window_id=self._next_window_id,
            start_us=start,
            end_us=end,
            samples_by_node=({node: real_samples[node] for node in available}
                             if accepted else selected),
            quality=quality,
        )
        self._next_window_id += 1
        next_start, _next_end = self._window_bounds_locked()
        for samples in self._buffers.values():
            while samples and samples[0].coordinator_timestamp_us < next_start:
                samples.popleft()
        return window
