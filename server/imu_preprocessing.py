"""Shared NumPy/SciPy preprocessing for training and VART inference."""

import math

import numpy as np
from scipy.signal import resample_poly


def resample_poly_to_window(signal: np.ndarray, window_size: int) -> np.ndarray:
    """Resample a fixed-duration [T, C] signal to contiguous float32 samples.

    The caller selects the model's three-second duration first. This function
    normalizes its sample rate using the same polyphase filter as training.
    """
    signal = np.asarray(signal, dtype=np.float32)
    if signal.ndim != 2:
        raise ValueError(f"expected a [T, C] signal, got shape {signal.shape}")
    n, _ = signal.shape
    if n < 2:
        raise ValueError(
            f"window has {n} time sample(s); at least 2 are required to resample")
    if not np.isfinite(signal).all():
        raise ValueError("window contains NaN or infinite values")
    if n == window_size:
        return np.ascontiguousarray(signal, dtype=np.float32)
    g = math.gcd(n, window_size)
    out = resample_poly(
        signal.astype(np.float64, copy=False),
        up=window_size // g, down=n // g, axis=0,
        window=("kaiser", 5.0), padtype="line",
    )
    if out.shape[0] > window_size:
        out = out[:window_size]
    elif out.shape[0] < window_size:
        pad = np.repeat(out[-1:], window_size - out.shape[0], axis=0)
        out = np.concatenate([out, pad], axis=0)
    return np.ascontiguousarray(out, dtype=np.float32)
