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
    max_missing_per_node: int = 5
    max_extra_per_node: int = 2

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
        if self.max_missing_per_node < 0:
            raise ValueError("maximum missing samples must not be negative")
        if self.max_extra_per_node < 0:
            raise ValueError("maximum extra samples must not be negative")

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

    @staticmethod
    def _padding_sample(node_id, sequence, timestamp_us,
                        coordinator_timestamp_us, session_id=0):
        """Create an inference-only zero sample; it is never put on the wire."""
        return DecodedImuSample(
            node_id=node_id,
            session_id=session_id,
            sequence=sequence & 0xFFFFFFFF,
            timestamp_us=timestamp_us,
            coordinator_timestamp_us=coordinator_timestamp_us,
            ax=0,
            ay=0,
            az=0,
            gx=0,
            gy=0,
            gz=0,
            status=0,
            flags=0,
            is_padding=True,
        )

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

    @staticmethod
    def _better_grid_match(candidate, existing):
        if existing is None:
            return True
        candidate_cost, _candidate_kept, candidate_dropped = candidate
        existing_cost, _existing_kept, existing_dropped = existing
        if candidate_cost != existing_cost:
            return candidate_cost < existing_cost
        # On an exact timing tie, retain earlier samples by dropping later ones.
        return candidate_dropped > existing_dropped

    def _select_on_timestamp_grid(self, samples, expected, start):
        """Select the expected count with minimum total timestamp error."""
        extra = len(samples) - expected
        # State value: total error, retained sample indices, dropped indices.
        states = {0: (0, (), ())}
        for sample_index, sample in enumerate(samples):
            next_states = {}
            for dropped_count, (cost, kept, dropped) in states.items():
                target_index = len(kept)
                if target_index < expected:
                    target_us = start + round(
                        target_index * self.config.window_us / expected)
                    candidate = (
                        cost + abs(sample.coordinator_timestamp_us - target_us),
                        kept + (sample_index,), dropped,
                    )
                    if self._better_grid_match(
                            candidate, next_states.get(dropped_count)):
                        next_states[dropped_count] = candidate
                if dropped_count < extra:
                    candidate = (cost, kept, dropped + (sample_index,))
                    if self._better_grid_match(
                            candidate, next_states.get(dropped_count + 1)):
                        next_states[dropped_count + 1] = candidate
            states = next_states

        _cost, kept_indices, dropped_indices = states[extra]
        retained = tuple(samples[index] for index in kept_indices)
        dropped_sequences = tuple(samples[index].sequence
                                  for index in dropped_indices)
        return retained, dropped_sequences

    def _pad_node_samples(self, node_id, samples, expected, start, end,
                          missing_sequences, has_gap):
        """Return an expected-length tuple, or None when padding is unsafe."""
        padding_needed = expected - len(samples)
        if padding_needed < 0 or padding_needed > self.config.max_missing_per_node:
            return None
        if has_gap and len(missing_sequences) > padding_needed:
            # A gap without a corresponding count deficit indicates a duplicate,
            # reordering, or a replacement sample rather than simple packet loss.
            return None
        if padding_needed == 0:
            return samples

        period_us = 1_000_000 / self.config.sample_rate_hz
        if not samples:
            return tuple(
                self._padding_sample(
                    node_id,
                    sequence=index,
                    timestamp_us=start + round(index * period_us),
                    coordinator_timestamp_us=start + round(index * period_us),
                )
                for index in range(expected)
            )

        internal = []
        internal_padding = 0
        for previous, current in zip(samples, samples[1:]):
            gap_sequences, gap_found = self._missing_sequences((previous, current))
            if gap_found:
                internal_padding += len(gap_sequences)
                if internal_padding > padding_needed:
                    return None
            internal.append((previous, current, gap_sequences))

        remaining = padding_needed - internal_padding
        estimated_leading = round(
            (samples[0].coordinator_timestamp_us - start) / period_us)
        leading_count = min(remaining, max(0, estimated_leading))
        trailing_count = remaining - leading_count

        padded = []
        first = samples[0]
        for offset in range(leading_count, 0, -1):
            padded.append(self._padding_sample(
                node_id,
                sequence=first.sequence - offset,
                timestamp_us=first.timestamp_us - round(offset * period_us),
                coordinator_timestamp_us=max(
                    start,
                    first.coordinator_timestamp_us - round(offset * period_us)),
                session_id=first.session_id,
            ))

        for previous, current, gap_sequences in internal:
            padded.append(previous)
            gap_size = len(gap_sequences)
            for index, sequence in enumerate(gap_sequences, start=1):
                padded.append(self._padding_sample(
                    node_id,
                    sequence=sequence,
                    timestamp_us=previous.timestamp_us + round(
                        (current.timestamp_us - previous.timestamp_us) *
                        index / (gap_size + 1)),
                    coordinator_timestamp_us=(
                        previous.coordinator_timestamp_us + round(
                            (current.coordinator_timestamp_us -
                             previous.coordinator_timestamp_us) *
                            index / (gap_size + 1))),
                    session_id=previous.session_id,
                ))
        padded.append(samples[-1])

        last = samples[-1]
        for offset in range(1, trailing_count + 1):
            padded.append(self._padding_sample(
                node_id,
                sequence=last.sequence + offset,
                timestamp_us=last.timestamp_us + round(offset * period_us),
                coordinator_timestamp_us=min(
                    end - 1,
                    last.coordinator_timestamp_us + round(offset * period_us)),
                session_id=last.session_id,
            ))
        return tuple(padded)

    def _assemble_next_locked(self):
        start, end = self._window_bounds_locked()
        selected = {
            node_id: tuple(sample for sample in samples
                           if start <= sample.coordinator_timestamp_us < end)
            for node_id, samples in self._buffers.items()
        }
        counts = {node_id: len(samples) for node_id, samples in selected.items()}
        normalized = {}
        dropped = {}
        invalid = {}
        invalid_reasons = {}
        missing = {}
        gaps = {}
        for node_id, samples in selected.items():
            unique, duplicate_sequences, conflicting_duplicate = (
                self._deduplicate_samples(samples))
            missing[node_id], gaps[node_id] = self._missing_sequences(unique)
            dropped_sequences = list(duplicate_sequences)
            node_invalid = conflicting_duplicate
            node_reason = "conflicting_duplicate" if conflicting_duplicate else None

            if len(dropped_sequences) > self.config.max_extra_per_node:
                node_invalid = True
                node_reason = "too_many_extra_samples"
            if len(unique) > self.config.expected_per_node:
                unique_extra = len(unique) - self.config.expected_per_node
                if gaps[node_id]:
                    node_invalid = True
                    node_reason = "sequence_gap_with_extra_samples"
                elif (len(dropped_sequences) + unique_extra >
                      self.config.max_extra_per_node):
                    node_invalid = True
                    node_reason = "too_many_extra_samples"
                else:
                    unique, grid_dropped = self._select_on_timestamp_grid(
                        unique, self.config.expected_per_node, start)
                    dropped_sequences.extend(grid_dropped)

            normalized[node_id] = unique
            dropped[node_id] = tuple(dropped_sequences)
            invalid[node_id] = node_invalid
            invalid_reasons[node_id] = node_reason

        expected = self.config.expected_per_node
        padded = {
            node_id: None if invalid[node_id] else self._pad_node_samples(
                node_id, normalized[node_id], expected, start, end,
                missing[node_id], gaps[node_id])
            for node_id in selected
        }
        accepted = all(samples is not None for samples in padded.values())
        padded_counts = {
            node_id: (sum(sample.is_padding for sample in padded[node_id])
                      if accepted else 0)
            for node_id in selected
        }
        dropped_counts = {
            node_id: len(dropped[node_id]) for node_id in selected
        }
        rejection_reason = None
        if not accepted:
            rejection_reason = next(
                (reason for reason in invalid_reasons.values()
                 if reason is not None),
                "missing_samples",
            )
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
            padded_counts=padded_counts,
            dropped_counts=dropped_counts,
            dropped_sequences=dropped,
            missing_sequences=missing,
            max_index_skew_us=max_skew,
            accepted=accepted,
            rejection_reason=rejection_reason,
        )
        window = AssembledWindow(
            run_id=self._run_id,
            window_id=self._next_window_id,
            start_us=start,
            end_us=end,
            samples_by_node=padded if accepted else selected,
            quality=quality,
        )
        self._next_window_id += 1
        next_start, _next_end = self._window_bounds_locked()
        for samples in self._buffers.values():
            while samples and samples[0].coordinator_timestamp_us < next_start:
                samples.popleft()
        return window
