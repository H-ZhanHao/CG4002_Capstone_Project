#!/usr/bin/env python3
"""VART deployment harness for the late-fusion IMU CNN on the Ultra96v2.

Runs the three COMPILED xmodels (from quantize_late_fusion.py -> vai_c_xir) on
the board's DPU (DPUCZDX8G_ISA1_B1600):

    wrist_branch.xmodel   BranchCore   [1, 1, 300, 6] -> [1, 64]
    thigh_branch.xmodel   BranchCore   [1, 1, 300, 6] -> [1, 64]
    fusion_mlp.xmodel     FusionMLP    [1, 130]       -> [1, 6]

This is the on-board twin of run_imu_test_inference.py: same CSV loading, same
timestamp-based windowing, and the SAME polyphase resample
(imu_preprocessing.resample_poly_to_window), so the preprocessing matches training exactly.
The difference is where the math runs -- the branch conv stacks and the fusion
MLP execute on the DPU via VART instead of in float PyTorch.

CPU vs DPU split (from the inspector reports)
--------------------------------------------
The DPU runs the two BranchCore graphs and the FusionMLP graph. Everything else
is CPU glue, done here in NumPy:

    raw [T,6] window
      -> (CPU) resample_poly_to_window -> [300, 6]
      -> (CPU) reshape to the branch input tensor layout   <- the "transpose"
      -> (DPU) wrist BranchCore  -> wrist_feat [64]
      -> (DPU) thigh BranchCore  -> thigh_feat [64]
      -> (CPU) feat * mask, concat with the 2-bit mask -> [130]
      -> (DPU) FusionMLP -> logits [6]
      -> (CPU) softmax + argmax

The BranchCore input tensor is [1, 1, 300, 6] (N, H=1, W=time, C=6), which is
byte-identical to the raw [300, 6] window, so the "transpose" the reports assign
to the CPU is a layout no-op -- we write the window straight into the buffer.

VART fixed-point handling
-------------------------
DPU tensors are INT8 with a per-tensor fix_point exponent. Floats are quantized
in with scale 2**fix_point and dequantized out with 2**-fix_point, read from each
tensor's fix_point attribute. This mirrors the standard Vitis AI runtime idiom.

Usage (on the board):
    python3 late_fusion_vart.py \
        --wrist-xmodel  compiled/wrist_branch.xmodel \
        --thigh-xmodel  compiled/thigh_branch.xmodel \
        --fusion-xmodel compiled/fusion_mlp.xmodel \
        --wrist-csv     wrist_capture.csv \
        [--thigh-csv    thigh_capture.csv] \
        [--range START END | --all] [--stride 0.5]

Provide one or both of --wrist-csv / --thigh-csv. A branch whose CSV is omitted
is masked off (availability bit 0), exactly the degraded mode the fusion model
was trained for. At least one branch is required.

Live inference and the CSV command share LateFusionInference. Deploy config.py
and imu_preprocessing.py alongside this file. The legacy hardware_ai config
layout is also supported. Runtime dependencies are numpy, scipy and the board's
vart + xir Python modules; PyTorch is not required for inference.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import logging
import os
import sys
import time

# Reuse the exact model config + preprocessing from the training code.
SRC_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hardware_ai"
)
if not os.path.isdir(SRC_DIR):
    # On the board the layout may be flattened: hardware_ai next to this file.
    SRC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hardware_ai")
if (not os.path.isfile(os.path.join(os.path.dirname(__file__), "config.py"))
        and SRC_DIR not in sys.path):
    sys.path.insert(0, SRC_DIR)

import numpy as np

from config import WINDOW_SECONDS, ModelConfig, location_index
from imu_preprocessing import resample_poly_to_window

# Same knobs as run_imu_test_inference.py.
DEFAULT_STRIDE_SECONDS = 0.5
MIN_SAMPLE_COVERAGE = 0.8
IMU_COLUMNS = ["ax_m_s2", "ay_m_s2", "az_m_s2", "gx_rad_s", "gy_rad_s", "gz_rad_s"]
LOGGER = logging.getLogger("server")


@contextmanager
def _model_load_stage(model, stage):
    """Locate a slow/native startup call without changing runner behavior."""
    started = time.monotonic()
    LOGGER.info("MODEL_LOAD_BEGIN model=%s stage=%s", model, stage)
    try:
        yield
    except BaseException:
        LOGGER.exception("MODEL_LOAD_FAILED model=%s stage=%s elapsed=%.3fs",
                         model, stage, time.monotonic() - started)
        raise
    else:
        LOGGER.info("MODEL_LOAD_COMPLETE model=%s stage=%s elapsed=%.3fs",
                    model, stage, time.monotonic() - started)


# ---------------------------------------------------------------------------
# CSV loading + windowing (copied from run_imu_test_inference.py so the
# preprocessing is identical; sample_id range filtering is optional)
# ---------------------------------------------------------------------------
def load_csv(path, start=None, end=None):
    """Return (ts_seconds[T] relative to the first kept sample, signal[T, 6])."""
    with open(path) as f:
        header = f.readline().strip().split(",")
    col = {name: i for i, name in enumerate(header)}
    for c in IMU_COLUMNS + ["timestamp_us"]:
        if c not in col:
            raise SystemExit(f"{path}: missing required column {c!r}")
    chan_idx = [col[c] for c in IMU_COLUMNS]

    data = np.genfromtxt(path, delimiter=",", skip_header=1, dtype=np.float64)
    if data.ndim == 1:
        data = data[None, :]

    keep = np.ones(data.shape[0], dtype=bool)
    if (start is not None or end is not None) and "sample_id" in col:
        sample_ids = data[:, col["sample_id"]].astype(int)
        if start is not None:
            keep &= sample_ids >= start
        if end is not None:
            keep &= sample_ids <= end
    rows = data[keep]
    if rows.shape[0] == 0:
        raise SystemExit(f"no samples in range {start}..{end}")

    ts = (rows[:, col["timestamp_us"]] - rows[0, col["timestamp_us"]]) / 1e6
    signal = rows[:, chan_idx].astype(np.float32)
    if not np.isfinite(signal).all():
        raise SystemExit(f"{path}: recording contains NaN/inf values")
    return ts, signal


def slide_windows(ts, signal, stride_s, min_samples):
    """Cut fixed WINDOW_SECONDS windows on timestamps -> [(t_start, segment)]."""
    out = []
    t_end = float(ts[-1])
    t0 = 0.0
    while t0 + WINDOW_SECONDS <= t_end + 1e-9:
        m = (ts >= t0) & (ts < t0 + WINDOW_SECONDS)
        seg = signal[m]
        if seg.shape[0] >= min_samples:
            out.append((t0, seg))
        t0 += stride_s
    return out


def _prepare(csv_path, stride, cfg, lo, hi):
    """load_csv (optional sample_id range) + slide + polyphase resample.

    Returns ``(t0_list, windows)`` where each window is a ``[window_size, 6]``
    float32 array produced by the SAME polyphase resample used in training.
    """
    ts, signal = load_csv(csv_path, lo, hi)
    rate = len(ts) / float(ts[-1]) if ts[-1] > 0 else 0.0
    min_samples = MIN_SAMPLE_COVERAGE * WINDOW_SECONDS * rate
    raw = slide_windows(ts, signal, stride, min_samples)
    t0_list = [t0 for t0, _ in raw]
    windows = [resample_poly_to_window(seg, cfg.window_size) for _, seg in raw]
    return t0_list, windows


# ---------------------------------------------------------------------------
# VART runner wrapper
# ---------------------------------------------------------------------------
def _dpu_subgraph(graph):
    """Return the single DPU subgraph of a compiled xmodel."""
    root = graph.get_root_subgraph()
    if root.is_leaf:
        raise RuntimeError("xmodel root subgraph is a leaf; not a compiled model")
    dpu = [
        s for s in root.toposort_child_subgraph()
        if s.has_attr("device") and s.get_attr("device").upper() == "DPU"
    ]
    if len(dpu) != 1:
        raise RuntimeError(f"expected exactly one DPU subgraph, found {len(dpu)}")
    return dpu[0]


class DpuModel:
    """One compiled xmodel wrapped as a float-in / float-out callable.

    Handles the INT8 fixed-point conversion using each tensor's fix_point, and
    the NCHW->NHWC layout the DPU input tensor already expects (our data is fed
    in the tensor's native shape, so no permutation is done here).
    """

    def __init__(self, xmodel_path):
        with _model_load_stage(xmodel_path, "import_vart_xir"):
            import vart
            import xir

        if not os.path.exists(xmodel_path):
            raise FileNotFoundError(f"missing xmodel: {xmodel_path}")
        with _model_load_stage(xmodel_path, "deserialize_graph"):
            self.graph = xir.Graph.deserialize(xmodel_path)
        with _model_load_stage(xmodel_path, "select_dpu_subgraph"):
            subgraph = _dpu_subgraph(self.graph)
        with _model_load_stage(xmodel_path, "create_runner"):
            self.runner = vart.Runner.create_runner(subgraph, "run")
        with _model_load_stage(xmodel_path, "read_tensor_metadata"):
            self.in_tensor = self.runner.get_input_tensors()[0]
            self.out_tensor = self.runner.get_output_tensors()[0]
            self.in_shape = tuple(self.in_tensor.dims)
            self.out_shape = tuple(self.out_tensor.dims)
            self.in_fp = self._fix_point(self.in_tensor)
            self.out_fp = self._fix_point(self.out_tensor)
            self.in_scale = float(2 ** self.in_fp)
            self.out_scale = float(2 ** -self.out_fp)
        LOGGER.info("MODEL_READY path=%s input=%s output=%s input_fix_point=%d output_fix_point=%d",
                    xmodel_path, self.in_shape, self.out_shape, self.in_fp, self.out_fp)

    @staticmethod
    def _fix_point(tensor):
        # Compiled DPU tensors carry an integer fix_point exponent.
        if tensor.has_attr("fix_point"):
            return int(tensor.get_attr("fix_point"))
        return 0

    def run(self, x_float):
        """x_float: np.ndarray shaped like the input tensor (batch 1). -> [out]."""
        x = np.ascontiguousarray(x_float, dtype=np.float32).reshape(self.in_shape)
        x_q = np.clip(np.round(x * self.in_scale), -128, 127).astype(np.int8)

        out_q = np.empty(self.out_shape, dtype=np.int8)
        job = self.runner.execute_async([x_q], [out_q])
        self.runner.wait(job)

        return (out_q.astype(np.float32) * self.out_scale).reshape(-1)


# ---------------------------------------------------------------------------
# Fusion glue (CPU) -- mirrors FusionNetwork.forward
# ---------------------------------------------------------------------------
def assemble_fusion_input(cfg, feats, mask):
    """Build the [130] fusion input: feats*mask flattened, then the mask.

    feats: [num_branches, feature_dim] (zeros in masked slots); mask: [num_branches].
    """
    masked = feats * mask[:, None]
    return np.concatenate([masked.reshape(-1), mask]).astype(np.float32)


def softmax(v):
    e = np.exp(v - np.max(v))
    return e / e.sum()


@dataclass(frozen=True)
class ClassificationResult:
    """One late-fusion decision for the usable IMUs in a capture window."""

    capture_run_id: int
    window_id: int
    start_us: int
    end_us: int
    class_id: int
    exercise: str
    confidence: float
    probabilities: tuple[float, ...]
    model_window_count: int


class LateFusionInference:
    """Persistent VART runners shared by the CSV and live-array paths.

    One worker owns this instance; VART runners are never invoked concurrently.
    Live samples are already in m/s² and rad/s: Node 1 is thigh, Node 2 is wrist.
    """

    def __init__(self, wrist_xmodel="./compiled/wrist.xmodel",
                 thigh_xmodel="./compiled/thigh.xmodel",
                 fusion_xmodel="./compiled/fusion.xmodel", *,
                 model_factory=None, stride_seconds=DEFAULT_STRIDE_SECONDS,
                 sample_rate_hz=100):
        with _model_load_stage("late_fusion", "config"):
            self.cfg = ModelConfig()
        if not np.isfinite(stride_seconds) or stride_seconds <= 0:
            raise ValueError("model stride must be finite and positive")
        self.stride_us = round(stride_seconds * 1_000_000)
        if self.stride_us < 1:
            raise ValueError("model stride must be at least one microsecond")
        if not np.isfinite(sample_rate_hz) or sample_rate_hz <= 0:
            raise ValueError("sample rate must be finite and positive")
        self.min_samples = MIN_SAMPLE_COVERAGE * WINDOW_SECONDS * sample_rate_hz
        factory = model_factory or DpuModel
        with _model_load_stage("wrist", "initialize"):
            self.wrist = factory(wrist_xmodel) if wrist_xmodel is not None else None
        with _model_load_stage("thigh", "initialize"):
            self.thigh = factory(thigh_xmodel) if thigh_xmodel is not None else None
        with _model_load_stage("fusion", "initialize"):
            self.fusion = factory(fusion_xmodel)

    def predict_pair(self, wrist, thigh):
        """Run available resampled branches and return class probabilities."""
        cfg = self.cfg
        feats = np.zeros((cfg.num_branches, cfg.feature_dim), dtype=np.float32)
        mask = np.zeros(cfg.num_branches, dtype=np.float32)
        for location, signal, model in (
                ("wrist", wrist, self.wrist), ("thigh", thigh, self.thigh)):
            if signal is None:
                continue
            if model is None:
                raise ValueError(f"{location} model is not loaded")
            signal = np.asarray(signal, dtype=np.float32)
            if signal.shape != (cfg.window_size, cfg.in_channels):
                raise ValueError(f"invalid {location} input shape {signal.shape}")
            if not np.isfinite(signal).all():
                raise ValueError(f"non-finite {location} input")
            idx = location_index(location)
            feats[idx] = model.run(signal)
            mask[idx] = 1.0
        if not mask.any():
            raise ValueError("at least one IMU is required")
        logits = np.asarray(self.fusion.run(assemble_fusion_input(cfg, feats, mask)))
        if logits.shape != (cfg.num_classes,) or not np.isfinite(logits).all():
            raise ValueError("invalid fusion output")
        return softmax(logits)

    def model_intervals(self, window):
        """Exact coordinator-time intervals consumed by the model."""
        model_us = round(WINDOW_SECONDS * 1_000_000)
        return [(start, start + model_us) for start in
                range(window.start_us, window.end_us - model_us + 1, self.stride_us)]

    def infer_window(self, window):
        """Average three-second predictions from the segment's usable IMUs.

        Bounds come from the assembler, not the last sample timestamp (4.99 s
        at 100 Hz). Thus [2 s, 5 s) is included in a complete five-second window.
        Both branches use the same coordinator-time bounds, without individually
        rebasing their first samples and losing their relative alignment.
        The assembler omits unavailable or unusable nodes. Pass those branches
        as None so predict_pair supplies zero features and a zero presence mask.
        """
        if not window.quality.accepted:
            raise ValueError("capture window failed quality checks")
        model_us = round(WINDOW_SECONDS * 1_000_000)
        duration_us = window.end_us - window.start_us
        if duration_us < model_us:
            raise ValueError("capture window is shorter than the model window")
        streams = {}
        for node_id in (1, 2):
            samples = window.samples_by_node.get(node_id, ())
            if not samples:
                continue
            ts = np.array([s.coordinator_timestamp_us - window.start_us
                           for s in samples], dtype=np.int64)
            signal = np.array([(s.ax, s.ay, s.az, s.gx, s.gy, s.gz)
                               for s in samples], dtype=np.float32)
            if (np.any(np.diff(ts) <= 0) or np.any(ts < 0) or
                    np.any(ts >= duration_us)):
                raise ValueError(f"invalid timestamps for Node {node_id}")
            streams[node_id] = ts, signal
        if not streams:
            raise ValueError("at least one IMU is required")

        all_probs = []
        for start_us, end_us in self.model_intervals(window):
            start, end = start_us - window.start_us, end_us - window.start_us
            pair = []
            # predict_pair expects wrist first, then thigh.
            for node_id in (2, 1):
                if node_id not in streams:
                    pair.append(None)
                    continue
                ts, signal = streams[node_id]
                segment = signal[(ts >= start) & (ts < end)]
                if len(segment) < self.min_samples:
                    raise ValueError(f"insufficient model-window coverage: Node {node_id}")
                pair.append(resample_poly_to_window(segment, self.cfg.window_size))
            all_probs.append(self.predict_pair(*pair))
        mean_probs = np.asarray(all_probs, dtype=np.float32).mean(axis=0)
        class_id = int(mean_probs.argmax())
        return ClassificationResult(
            capture_run_id=window.run_id, window_id=window.window_id,
            start_us=window.start_us, end_us=window.end_us,
            class_id=class_id, exercise=self.cfg.exercise_classes[class_id],
            confidence=float(mean_probs[class_id]),
            probabilities=tuple(float(p) for p in mean_probs),
            model_window_count=len(all_probs),
        )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main(argv=None):
    p = argparse.ArgumentParser(
        description="Run the late-fusion IMU CNN on the Ultra96 DPU via VART."
    )
    p.add_argument("--wrist-csv", help="wrist capture CSV")
    p.add_argument("--thigh-csv", help="thigh capture CSV")
    p.add_argument(
        "--range", type=int, nargs=2, metavar=("START", "END"), default=None,
        help="Inclusive sample_id range to classify (needs a sample_id column).",
    )
    p.add_argument("--all", action="store_true", help="(default) classify the whole capture")
    p.add_argument("--stride", type=float, default=DEFAULT_STRIDE_SECONDS)
    p.add_argument("--wrist-xmodel", default="./compiled/wrist.xmodel")
    p.add_argument("--thigh-xmodel", default="./compiled/thigh.xmodel")
    p.add_argument("--fusion-xmodel", default="./compiled/fusion.xmodel")
    args = p.parse_args(argv)

    if not np.isfinite(args.stride) or args.stride <= 0:
        p.error("--stride must be finite and positive")

    if not args.wrist_csv and not args.thigh_csv:
        p.error("provide at least one of --wrist-csv / --thigh-csv")

    cfg = ModelConfig()
    classes = cfg.exercise_classes

    # --range applies to both captures identically (same convention as the
    # reference script). Only used when a sample_id column exists.
    lo, hi = (None, None) if (args.all or not args.range) else (args.range[0], args.range[1])

    wrist_t0, wrist_windows = ([], [])
    thigh_t0, thigh_windows = ([], [])
    if args.wrist_csv:
        wrist_t0, wrist_windows = _prepare(args.wrist_csv, args.stride, cfg, lo, hi)
    if args.thigh_csv:
        thigh_t0, thigh_windows = _prepare(args.thigh_csv, args.stride, cfg, lo, hi)

    have_wrist = len(wrist_windows) > 0
    have_thigh = len(thigh_windows) > 0

    if have_wrist and have_thigh:
        if len(wrist_windows) != len(thigh_windows):
            raise SystemExit(
                f"wrist and thigh produced different window counts "
                f"({len(wrist_windows)} vs {len(thigh_windows)}); align the "
                f"captures or run one branch at a time"
            )
        n = len(wrist_windows)
        t0_ref = wrist_t0
    elif have_wrist:
        n, t0_ref = len(wrist_windows), wrist_t0
    else:
        n, t0_ref = len(thigh_windows), thigh_t0
    if n == 0:
        raise SystemExit("no full windows fit in the capture(s)")

    print("Loading xmodels...")
    inference = LateFusionInference(
        wrist_xmodel=args.wrist_xmodel if have_wrist else None,
        thigh_xmodel=args.thigh_xmodel if have_thigh else None,
        fusion_xmodel=args.fusion_xmodel,
    )

    active = "+".join([b for b, ok in (("wrist", have_wrist), ("thigh", have_thigh)) if ok])
    print(
        f"Windows: {n} x {WINDOW_SECONDS:g}s at {args.stride:g}s stride "
        f"({active}) -> resampled to {cfg.window_size} samples each\n"
    )

    all_probs = np.zeros((n, cfg.num_classes), dtype=np.float32)
    votes = {}

    # Per-window inference time includes branch/fusion runs, input checks,
    # CPU mask/concat glue, and softmax. Resampling and CSV loading are
    # EXCLUDED: they are identical preprocessing on the DPU board and on the CPU
    # reference, so timing them would muddy the DPU-vs-CPU comparison. The first
    # window is timed too but reported separately, since the DPU's first call
    # pays one-time setup/warm-up cost that later calls do not.
    infer_ms = np.zeros(n, dtype=np.float64)

    print("Per-window predictions:")
    for i in range(n):
        t_start = time.perf_counter()
        probs = inference.predict_pair(
            wrist_windows[i] if have_wrist else None,
            thigh_windows[i] if have_thigh else None,
        )
        infer_ms[i] = (time.perf_counter() - t_start) * 1e3
        pred = int(np.argmax(probs))

        all_probs[i] = probs
        votes[classes[pred]] = votes.get(classes[pred], 0) + 1
        t0 = t0_ref[i]
        print(
            f"  t={t0:6.2f}-{t0 + WINDOW_SECONDS:5.2f}s: "
            f"{classes[pred]:<13} (confidence {float(probs[pred]):.3f})"
            f"  [{infer_ms[i]:.3f} ms]"
        )

    # Inference-time summary. The first window carries the DPU's one-time warm-up
    # (runner setup, first DMA), so report it separately and base the steady-state
    # stats on windows 2..n when there is more than one.
    print("\nInference time (models + CPU glue, resample/CSV excluded):")
    print(f"    first window (incl. warm-up) : {infer_ms[0]:.3f} ms")
    steady = infer_ms[1:] if n > 1 else infer_ms
    label = "windows 2.." + str(n) if n > 1 else "single window"
    print(
        f"    steady-state ({label}): "
        f"mean {steady.mean():.3f} ms  "
        f"min {steady.min():.3f}  "
        f"max {steady.max():.3f}  "
        f"median {np.median(steady):.3f}"
    )
    if steady.mean() > 0:
        print(f"    throughput (steady-state)    : {1e3 / steady.mean():.1f} windows/s")

    mean_probs = all_probs.mean(axis=0)
    mean_pred = int(mean_probs.argmax())

    print("\nMajority vote across windows:")
    for name, count in sorted(votes.items(), key=lambda kv: -kv[1]):
        print(f"    {name:<13} {count}/{n} windows")

    print("\nMean-probability across windows:")
    for rank, ci in enumerate(np.argsort(mean_probs)[::-1], 1):
        print(f"    {rank}. {classes[int(ci)]:<13} {float(mean_probs[ci]):.3f}")

    print(
        f"\nConclusion: {classes[mean_pred]}  "
        f"(mean confidence {float(mean_probs[mean_pred]):.3f})"
    )


if __name__ == "__main__":
    main()
