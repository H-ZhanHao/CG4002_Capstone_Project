"""Dataset adapters that map external IMU datasets into the model's format.

Two adapters are provided (Requirement 6):

* :class:`SingleLocationDataset` yields ``(window[300, 6], label)`` pairs for
  pretraining one :class:`BranchCNN` on a public single-location dataset
  (Requirement 4).
* :class:`SynchronizedDataset` yields ``(windows[2, 300, 6], mask[2], label)``
  triples for fusion training, with optional IMU-failure simulation
  (Requirement 5.5).

Both adapters take **in-memory source samples** so they can be driven directly
with synthetic data in unit tests as well as with real dataset loaders. A
"source sample" is a light dict:

``SingleLocationDataset`` source item::

    {
        "signal": array-like of shape [T, C],   # raw stream, any length/rate
        "label": <external label>,              # mapped via ``label_map``
        "channels": ("ax", "ay", "az", ...),    # optional channel names for C
        "scale": {"ax": 9.81, ...},             # optional per-channel scale
    }

``SynchronizedDataset`` source item::

    {
        "wrist": {"signal": [T, C], "channels": (...), "scale": {...}},
        "thigh": {...},
        "label": <external label>,
    }

Adapter responsibilities per the design's Data Models section:

* **Resample / segment** every source stream to exactly ``cfg.window_size``
  (300) samples via polyphase FIR resampling over the time axis (Req 6.4).
* **Channel reorder / scale** to the fixed order ``ax, ay, az, gx, gy, gz``
  applying optional per-channel unit conversions (Req 6.1).
* **Label mapping** through ``label_map`` into the six fixed classes (Req 6.2).
* **Exclude-and-log** samples whose label maps to none of the six classes; the
  count and list of excluded labels are kept in :attr:`exclusion_log`
  (Req 6.3).
"""

from __future__ import annotations

import math
import random
from collections import Counter
from dataclasses import dataclass, field
from typing import Any, Mapping, Sequence

import numpy as np
import torch
from imu_preprocessing import resample_poly_to_window
from torch.utils.data import Dataset

from config import BRANCH_LOCATIONS, EXERCISE_CLASSES, ModelConfig, location_index

# Fixed target channel order for every produced Input_Window (Req 6.1).
TARGET_CHANNELS = ("ax", "ay", "az", "gx", "gy", "gz")


@dataclass
class ExclusionLog:
    """Record of samples excluded because their label was unmappable (Req 6.3)."""

    count: int = 0
    labels: list = field(default_factory=list)
    label_counts: Counter = field(default_factory=Counter)

    def record(self, label: Any) -> None:
        self.count += 1
        self.labels.append(label)
        # ``Counter`` needs hashable keys; fall back to the string form.
        try:
            self.label_counts[label] += 1
        except TypeError:
            self.label_counts[str(label)] += 1

    def __len__(self) -> int:
        return self.count

    def __repr__(self) -> str:
        return f"ExclusionLog(count={self.count}, labels={list(self.label_counts)})"


def _build_label_map(label_map: Mapping | None) -> dict:
    """Return the external->class-index mapping.

    ``label_map`` may map external labels either to a class *name* (one of the
    six ``EXERCISE_CLASSES``) or directly to a class *index* in ``0..5``. When
    ``None`` an identity map over the class names is used, so a source already
    labelled with the canonical exercise names works out of the box.
    """
    name_to_index = {name: i for i, name in enumerate(EXERCISE_CLASSES)}
    if label_map is None:
        return dict(name_to_index)

    resolved: dict = {}
    for external, target in label_map.items():
        if isinstance(target, str):
            if target not in name_to_index:
                raise ValueError(
                    f"label_map target {target!r} is not one of {EXERCISE_CLASSES}"
                )
            resolved[external] = name_to_index[target]
        elif isinstance(target, (int, np.integer)):
            idx = int(target)
            if not 0 <= idx < len(EXERCISE_CLASSES):
                raise ValueError(
                    f"label_map target index {idx} outside 0..{len(EXERCISE_CLASSES) - 1}"
                )
            resolved[external] = idx
        else:
            raise ValueError(
                f"label_map target for {external!r} must be a class name or index"
            )
    return resolved


def _map_label(label: Any, resolved_map: dict) -> int | None:
    """Map one external label to a class index, or ``None`` if unmappable."""
    if label in resolved_map:
        return resolved_map[label]
    # Allow an already-valid class index to pass through unchanged.
    if isinstance(label, (int, np.integer)) and 0 <= int(label) < len(EXERCISE_CLASSES):
        # Only accept a bare index when the map does not override it.
        return int(label)
    return None


def random_rotation_matrix(rng, max_deg: float = 180.0) -> np.ndarray:
    """Random 3x3 rotation matrix, at most ``max_deg`` degrees from identity.

    Used for rotation augmentation: it models the sensor being mounted at a
    different angle than it was during recording.

    * ``max_deg <= 0``  -> identity (augmentation disabled).
    * ``max_deg >= 180`` -> Haar-uniform over SO(3), i.e. a genuinely arbitrary
      orientation, drawn from a random unit quaternion.
    * otherwise -> a uniformly random axis with the angle drawn uniformly from
      ``[0, max_deg]`` (Rodrigues' formula), so the rotation stays within
      ``max_deg`` of the original orientation.

    ``rng`` is a ``numpy.random.Generator``, so augmentation is reproducible
    from a seed.
    """
    if max_deg <= 0.0:
        return np.eye(3, dtype=np.float64)

    if max_deg >= 180.0:
        # Haar-uniform rotation: normalize a 4D Gaussian to a unit quaternion.
        q = rng.normal(size=4)
        n = np.linalg.norm(q)
        while n < 1e-12:  # pragma: no cover - vanishingly unlikely
            q = rng.normal(size=4)
            n = np.linalg.norm(q)
        w, x, y, z = q / n
        return np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w)],
            [2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)],
        ], dtype=np.float64)

    # Bounded rotation: uniform axis, angle uniform in [0, max_deg].
    axis = rng.normal(size=3)
    n = np.linalg.norm(axis)
    while n < 1e-12:  # pragma: no cover - vanishingly unlikely
        axis = rng.normal(size=3)
        n = np.linalg.norm(axis)
    axis = axis / n
    angle = np.deg2rad(rng.uniform(0.0, max_deg))
    K = np.array([
        [0.0, -axis[2], axis[1]],
        [axis[2], 0.0, -axis[0]],
        [-axis[1], axis[0], 0.0],
    ], dtype=np.float64)
    return np.eye(3) + math.sin(angle) * K + (1.0 - math.cos(angle)) * (K @ K)


def rotate_imu_window(window: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    """Rotate the sensor frame of a ``[T, C]`` window (``C >= 6``).

    The accelerometer triple (channels 0:3) and the gyroscope triple (channels
    3:6) are rotated by the SAME matrix, because they share one physical sensor
    frame: re-mounting the device turns both together. Under proper rotations a
    gyroscope's pseudovector transforms exactly like a vector, so one matrix is
    correct for both.

    Any channels beyond the first six are passed through untouched. That is
    deliberate: derived channels such as ``||acc||`` and ``||gyr||`` are
    rotation-invariant, so leaving them alone is the physically correct
    behaviour if they are ever added.

    Returns a new array; the input is not modified.
    """
    window = np.asarray(window, dtype=np.float32)
    if window.ndim != 2 or window.shape[1] < 6:
        raise ValueError(
            f"expected a [T, C>=6] window, got shape {window.shape}"
        )
    rotation = np.asarray(rotation, dtype=np.float32)
    if rotation.shape != (3, 3):
        raise ValueError(f"rotation must be [3, 3], got {rotation.shape}")

    out = window.copy()
    # Row-vector convention: each sample is a row, so multiply by R transposed.
    out[:, 0:3] = window[:, 0:3] @ rotation.T
    out[:, 3:6] = window[:, 3:6] @ rotation.T
    return out


def reorder_and_scale(
    signal: np.ndarray,
    channels: Sequence[str] | None,
    scale: Mapping[str, float] | None,
) -> np.ndarray:
    """Reorder/scale ``signal`` columns into fixed ``ax,ay,az,gx,gy,gz`` order.

    ``channels`` names each column of ``signal``. When omitted the columns are
    assumed to already be in the target order. ``scale`` optionally maps a
    target channel name to a multiplicative factor for unit conversion
    (e.g. g -> m/s^2, deg/s -> rad/s), applied after reordering (Req 6.1).
    """
    signal = np.asarray(signal, dtype=np.float32)
    n_ch = signal.shape[1]

    if channels is None:
        if n_ch != len(TARGET_CHANNELS):
            raise ValueError(
                f"signal has {n_ch} channels but no channel names given; "
                f"expected {len(TARGET_CHANNELS)} in order {TARGET_CHANNELS}"
            )
        ordered = signal.astype(np.float32, copy=True)
    else:
        if len(channels) != n_ch:
            raise ValueError(
                f"channels names ({len(channels)}) do not match signal columns ({n_ch})"
            )
        name_to_col = {name: i for i, name in enumerate(channels)}
        missing = [c for c in TARGET_CHANNELS if c not in name_to_col]
        if missing:
            raise ValueError(f"source is missing required channels: {missing}")
        cols = [name_to_col[c] for c in TARGET_CHANNELS]
        ordered = signal[:, cols].astype(np.float32, copy=True)

    if scale:
        for i, name in enumerate(TARGET_CHANNELS):
            if name in scale:
                ordered[:, i] *= float(scale[name])
    return ordered


def _prepare_window(item: Mapping, window_size: int) -> np.ndarray:
    """Resample + reorder/scale one source item into a ``[window_size, 6]`` array."""
    signal = item["signal"]
    channels = item.get("channels")
    scale = item.get("scale")
    ordered = reorder_and_scale(signal, channels, scale)  # [T, 6]
    window = resample_poly_to_window(ordered, window_size)  # [300, 6]
    return window


class SingleLocationDataset(Dataset):
    """Adapts a public single-location IMU dataset into ``(window, label)`` pairs.

    Used to pretrain one :class:`BranchCNN` (Reqs 4, 6). Unmappable samples are
    excluded and recorded in :attr:`exclusion_log` (Req 6.3).
    """

    def __init__(
        self,
        source: Sequence[Mapping],
        location: str,
        cfg: ModelConfig,
        label_map: Mapping | None = None,
        source_rate_hz: float | None = None,
    ) -> None:
        if location not in BRANCH_LOCATIONS:
            raise ValueError(
                f"location {location!r} must be one of {BRANCH_LOCATIONS}"
            )
        self.location = location
        self.cfg = cfg
        self.source_rate_hz = source_rate_hz
        self._resolved_map = _build_label_map(label_map)
        self.exclusion_log = ExclusionLog()

        self._windows: list[np.ndarray] = []
        self._labels: list[int] = []

        for item in source:
            label_idx = _map_label(item["label"], self._resolved_map)
            if label_idx is None:
                self.exclusion_log.record(item["label"])
                continue
            window = _prepare_window(item, cfg.window_size)
            self._windows.append(window)
            self._labels.append(label_idx)

    def __len__(self) -> int:
        return len(self._windows)

    def __getitem__(self, i: int) -> tuple[torch.Tensor, int]:
        """Return ``(window [300, 6] float32, label in 0..5)``."""
        window = torch.from_numpy(self._windows[i]).to(torch.float32)
        label = int(self._labels[i])
        return window, label


class SynchronizedDataset(Dataset):
    """Yields synchronized two-location samples for fusion training (Reqs 5, 6).

    Each item is ``(windows[2, 300, 6] in wrist/thigh order, mask[2],
    label)``. When ``failure_sim`` is enabled, each sample randomly marks one
    branch as missing (never zero, never all) and zeroes the missing
    windows so their contribution matches the mask (Req 5.5).
    """

    def __init__(
        self,
        source: Sequence[Mapping],
        cfg: ModelConfig,
        failure_sim: bool = True,
        label_map: Mapping | None = None,
        seed: int | None = None,
    ) -> None:
        self.cfg = cfg
        self.failure_sim = failure_sim
        self._resolved_map = _build_label_map(label_map)
        self.exclusion_log = ExclusionLog()
        self._rng = random.Random(seed)

        self._windows: list[np.ndarray] = []   # each [2, 300, 6]
        self._labels: list[int] = []

        for item in source:
            label_idx = _map_label(item["label"], self._resolved_map)
            if label_idx is None:
                self.exclusion_log.record(item["label"])
                continue
            stacked = np.empty(
                (cfg.num_branches, cfg.window_size, cfg.in_channels),
                dtype=np.float32,
            )
            for loc in BRANCH_LOCATIONS:
                if loc not in item:
                    raise ValueError(
                        f"synchronized source item missing location {loc!r}"
                    )
                stacked[location_index(loc)] = _prepare_window(
                    item[loc], cfg.window_size
                )
            self._windows.append(stacked)
            self._labels.append(label_idx)

    def __len__(self) -> int:
        return len(self._windows)

    def _sample_mask(self) -> np.ndarray:
        """Return a length-``num_branches`` availability mask.

        With failure simulation on, mark between one and ``num_branches - 1``
        branches missing, so at least one branch is always available (never
        zero missing, never all missing). Off, all branches are available.
        """
        mask = np.ones(self.cfg.num_branches, dtype=np.float32)
        if not self.failure_sim:
            return mask
        # Never mark all branches missing: at most num_branches - 1 missing so
        # at least one branch is always available (Req 5.5).
        max_missing = self.cfg.num_branches - 1
        num_missing = self._rng.randint(1, max_missing)
        missing = self._rng.sample(range(self.cfg.num_branches), num_missing)
        for idx in missing:
            mask[idx] = 0.0
        return mask

    def __getitem__(self, i: int) -> tuple[torch.Tensor, torch.Tensor, int]:
        """Return ``(windows [2, 300, 6], mask [2], label in 0..5)``."""
        windows = self._windows[i].copy()  # [2, 300, 6]
        mask = self._sample_mask()         # [2]
        # Zero the windows of missing branches so their contribution matches
        # the mask (the model also enforces this, but keep the data consistent).
        for b in range(self.cfg.num_branches):
            if mask[b] == 0.0:
                windows[b] = 0.0
        windows_t = torch.from_numpy(windows).to(torch.float32)
        mask_t = torch.from_numpy(mask).to(torch.float32)
        return windows_t, mask_t, int(self._labels[i])
