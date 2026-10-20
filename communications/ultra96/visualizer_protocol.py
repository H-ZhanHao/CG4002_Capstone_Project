"""Versioned JSON protocol for Ultra96 exercise visualizer updates."""

from __future__ import annotations

import json
from typing import Any, Mapping


PROTOCOL_VERSION = 1
MESSAGE_TYPE = "exercise_update"
# These fields form the stable contract shared by the Ultra96 gateway and
# Unity. Extra fields remain permitted so a future sender can add optional
# metadata without breaking an older visualizer.
REQUIRED_FIELDS = ("v", "type", "run_id", "seq", "ts_us", "exercise")


class VisualizerProtocolError(ValueError):
    """Raised when a visualizer message doesn't satisfy the version-1 contract."""


def _is_integer(value: Any) -> bool:
    """Accept JSON integers but reject booleans, which Python treats as ints."""
    return isinstance(value, int) and not isinstance(value, bool)


def validate_message(message: Mapping[str, Any]) -> dict[str, Any]:
    """Validate and return a shallow plain-dict copy of an update.

    Unknown fields are retained for forwards compatibility. JSON values are
    checked during serialization.
    """
    if not isinstance(message, Mapping):
        raise VisualizerProtocolError("message must be a JSON object")

    missing = [field for field in REQUIRED_FIELDS if field not in message]
    if missing:
        raise VisualizerProtocolError(
            "missing required field(s): " + ", ".join(missing)
        )
    if not _is_integer(message["v"]) or message["v"] != PROTOCOL_VERSION:
        raise VisualizerProtocolError("v must be integer 1")
    if message["type"] != MESSAGE_TYPE:
        raise VisualizerProtocolError("type must be exercise_update")

    for field in ("run_id", "seq", "ts_us"):
        if not _is_integer(message[field]) or message[field] < 0:
            raise VisualizerProtocolError(f"{field} must be a non-negative integer")

    exercise = message["exercise"]
    if not isinstance(exercise, str) or not exercise.strip():
        raise VisualizerProtocolError("exercise must be a non-empty string")

    return dict(message)


def parse_message(payload: str | bytes) -> dict[str, Any]:
    """Decode one UTF-8 JSON object and validate the visualizer contract."""
    try:
        message = json.loads(payload)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        raise VisualizerProtocolError(f"malformed JSON: {error.msg}") from error
    return validate_message(message)


def serialize_message(message: Mapping[str, Any]) -> str:
    """Validate and serialize one compact JSON WebSocket message."""
    validated = validate_message(message)
    try:
        return json.dumps(
            validated,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        )
    except (TypeError, ValueError) as error:
        raise VisualizerProtocolError(f"message is not JSON serializable: {error}") from error


def build_message(
    *,
    run_id: int,
    seq: int,
    ts_us: int,
    exercise: str,
) -> dict[str, Any]:
    """Construct and validate a version-1 exercise update.

    ``run_id`` separates independent server runs, ``seq`` orders updates within
    one run, and ``ts_us`` carries the Ultra96 monotonic timestamp in microseconds.
    """
    message: dict[str, Any] = {
        "v": PROTOCOL_VERSION,
        "type": MESSAGE_TYPE,
        "run_id": run_id,
        "seq": seq,
        "ts_us": ts_us,
        "exercise": exercise,
    }
    return validate_message(message)
