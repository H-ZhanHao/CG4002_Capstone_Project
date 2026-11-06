"""Central configuration for the late-fusion IMU CNN (Requirement 10).

Architecture parameters live in one place as a frozen dataclass so that every
component (``BranchCNN``, ``FusionNetwork``, ``LateFusionModel``, datasets, and
training scripts) reads the same values. ``__post_init__`` assertions enforce
the design invariants: exactly two branches, six classes, the fixed branch
locations (wrist, thigh) and the fixed exercise-class order.
"""

from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Fixed analysis window (time-based, NOT per-repetition).
#
# Classification operates on a fixed-duration slice of IMU data rather than on
# one segmented repetition. This decouples the classifier from rep detection:
# at run time the device simply keeps the last WINDOW_SECONDS of samples and
# classifies, with no need to find rep boundaries first.
#
# The window is normalized to WINDOW_RATE_HZ so every source (MM-Fit wrist
# ~104 Hz, MM-Fit pocket phone ~217 Hz, the ESP32 node) lands on one common
# time grid: window_size = WINDOW_SECONDS * WINDOW_RATE_HZ = 300 samples.
# Because the window duration is fixed, resampling to this grid normalizes the
# *rate* only; it no longer time-warps away rep duration, so cadence remains a
# usable feature.
# ---------------------------------------------------------------------------
WINDOW_SECONDS = 3.0
WINDOW_RATE_HZ = 100.0

# ---------------------------------------------------------------------------
# Rotation augmentation (TRAINING ONLY).
#
# During training each window's sensor frame is rotated by a random matrix, with
# the label unchanged. The same motion is therefore seen from many orientations,
# so the network cannot lower its loss by keying on one fixed gravity direction
# and is pushed towards orientation-independent features (cadence, magnitude
# envelopes, relative axis geometry). Validation, test and inference never
# rotate anything.
#
# ROTATION_AUG_MAX_DEG is the largest rotation, in degrees, away from the
# recorded orientation:
#     0    -> augmentation off (the sensor is assumed always mounted as in
#             training)
#     1-179 -> bounded: mounting varies, but only within this cone
#     180  -> unrestricted: any orientation is equally likely
#
# Default 180 (unrestricted). Measured sweep on the untouched test workouts
# w00 + w20 (both IMUs unless stated; "any angle" = test windows rotated
# arbitrarily; wrist/thigh = single-IMU degraded mode, un-rotated):
#
#     setting   clean   rot45   any angle   wrist only   thigh only   ESP32 capture
#     0 (off)   0.963   0.941       0.330        0.919        0.993   wrong (sit_up)
#     30 deg    0.995   0.990       0.451        0.867        0.995   -
#     60 deg    0.998   1.000       0.579        0.916        0.990   wrong (sit_up)
#     180 deg   0.966   0.941       0.919        0.746        0.916   correct (push_up)
#
# Reading the table:
#   * 60 deg is the best choice if the sensor's mounting is known to stay within
#     roughly that cone -- it beats no augmentation on every column, including
#     single-IMU mode, and is essentially free.
#   * Only 180 deg survives an arbitrary mounting angle (0.919 vs 0.330). It is
#     the default because the real external capture (hip-mounted ESP32, whose
#     orientation convention differs from MM-Fit's by more than 60 deg) is
#     classified correctly ONLY at this setting.
#   * The cost of 180 deg is single-IMU accuracy: wrist-only falls 0.919 -> 0.746.
#     With one IMU there is no second sensor to cross-reference, so removing
#     absolute gravity direction hurts more. Absolute gravity direction is
#     genuinely informative -- it is what separates squat (gravity mostly along
#     +z) from sit-up (about 95 deg away) -- and full augmentation discards it.
#
# So: lower this towards 60 if both IMUs are always present and mounting is
# consistent; keep 180 if the device may be worn at any angle or you rely on
# single-IMU operation across different hardware.
# ---------------------------------------------------------------------------
ROTATION_AUG_MAX_DEG = 180.0

# Fixed body locations, in slot order (Req 10.2).
# wrist = right smartwatch (sw_r); thigh = right smartphone in the trouser
# pocket (sp_r). Two branches.
BRANCH_LOCATIONS = ("wrist", "thigh")

# Fixed exercise-class order (Req 10.3).
EXERCISE_CLASSES = (
    "push_up",
    "squat",
    "lunge",
    "leg_raise",
    "sit_up",
    "jumping_jack",
    "idle",
)


@dataclass(frozen=True)
class ModelConfig:
    """Immutable architecture configuration for the Late_Fusion_Model."""

    branch_locations: tuple = BRANCH_LOCATIONS
    exercise_classes: tuple = EXERCISE_CLASSES
    window_size: int = 300         # 3.0 s @ 100 Hz fixed window; Req 1.2
    in_channels: int = 6           # ax, ay, az, gx, gy, gz
    num_branches: int = 2          # two IMUs: wrist + thigh (Req 2.1 / 10)
    feature_dim: int = 64          # per-branch Branch_Feature_Vector length, Req 2.4
    conv_channels: tuple = (16, 32, 64)   # small, HLS/quant friendly, Req 9
    kernel_size: int = 5
    pool_size: int = 2
    fusion_hidden: int = 128
    num_classes: int = 7           # Req 3.4 / 10.3
    # Max training-time sensor rotation in degrees; 0 disables augmentation.
    # Affects data preparation only -- no weights, shapes or inference cost.
    rotation_aug_max_deg: float = ROTATION_AUG_MAX_DEG

    def __post_init__(self):
        assert self.num_branches == 2, "design is 2 independent branches"
        assert len(self.branch_locations) == 2
        assert tuple(self.branch_locations) == BRANCH_LOCATIONS
        assert self.num_branches == len(self.branch_locations)
        assert self.num_classes == 7
        assert len(self.exercise_classes) == 7
        assert tuple(self.exercise_classes) == EXERCISE_CLASSES
        assert self.in_channels == 6
        # window_size is the number of samples in one fixed-duration window
        # (WINDOW_SECONDS at WINDOW_RATE_HZ). It must be positive and divisible
        # by pool_size**2 (two MaxPool2d stages) so the temporal-CNN time
        # dimension stays integer-valued.
        assert self.window_size > 0
        assert self.window_size % (self.pool_size ** 2) == 0
        assert self.window_size == round(WINDOW_SECONDS * WINDOW_RATE_HZ), (
            "window_size must equal WINDOW_SECONDS * WINDOW_RATE_HZ"
        )
        # Rotation augmentation range: 0 = off, 180 = any orientation. Values
        # above 180 are meaningless (180 already covers all of SO(3)).
        assert 0.0 <= self.rotation_aug_max_deg <= 180.0, (
            "rotation_aug_max_deg must be between 0 and 180 degrees"
        )

    @property
    def window_seconds(self) -> float:
        """Duration of one Input_Window in seconds."""
        return WINDOW_SECONDS

    @property
    def window_rate_hz(self) -> float:
        """Sample rate the Input_Window is normalized to."""
        return WINDOW_RATE_HZ

    @property
    def rotation_aug_enabled(self) -> bool:
        """True when training should randomly rotate the sensor frame."""
        return self.rotation_aug_max_deg > 0.0


def location_index(name: str) -> int:
    """Return the fixed branch slot for a Branch_Location name.

    Ordering is fixed as wrist=0, thigh=1 so every component agrees
    on branch/feature ordering (Req 3.1).
    """
    return BRANCH_LOCATIONS.index(name)
