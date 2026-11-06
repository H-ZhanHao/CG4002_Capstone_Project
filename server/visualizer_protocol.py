"""Versioned JSON protocol for Ultra96 exercise visualizer updates."""

from __future__ import annotations

import json
import re
from typing import Any, Mapping


PROTOCOL_VERSION = 1
MESSAGE_TYPE = "exercise_update"
# These fields form the stable contract shared by the Ultra96 gateway and
# Unity. Extra fields remain permitted so a future sender can add optional
# metadata without breaking an older visualizer.
REQUIRED_FIELDS = ("v", "type", "run_id", "seq", "ts_us", "exercise")


class VisualizerProtocolError(ValueError):
    """Raised when a visualizer message doesn't satisfy the version-1 contract."""


REP_TYPES = {"rep_session_start", "rep_sync", "rep_event"}
REP_EXERCISES = {"squat", "push_up", "sit_up", "lunge", "leg_raise", "jumping_jack"}
REP_FIELDS = {"v", "type", "run_id", "seq", "rep_session_id", "rep_index", "exercise"}
MAX_PHONE_BYTES = 1024
MAX_SAFE_INTEGER = 9007199254740991


def parse_rep_message(payload):
    """Strict bounded phone contract; no arbitrary hardware/control commands."""
    if not isinstance(payload, (str, bytes)) or len(payload.encode() if isinstance(payload, str) else payload) > MAX_PHONE_BYTES:
        raise VisualizerProtocolError("phone message exceeds 1024 bytes")
    try:
        message = json.loads(payload)
    except (ValueError, UnicodeError) as error:
        raise VisualizerProtocolError("invalid phone JSON") from error
    if not isinstance(message, dict) or set(message) != REP_FIELDS:
        raise VisualizerProtocolError("unsupported phone fields")
    if type(message["v"]) is not int or message["v"] != 1:
        raise VisualizerProtocolError("unsupported phone version")
    if not isinstance(message["type"], str) or message["type"] not in REP_TYPES:
        raise VisualizerProtocolError("unsupported phone command")
    for field in ("run_id", "seq", "rep_index"):
        maximum = 1_000_000_000 if field == "rep_index" else MAX_SAFE_INTEGER
        if type(message[field]) is not int or not 0 <= message[field] <= maximum:
            raise VisualizerProtocolError("invalid " + field)
    if message["run_id"] == 0:
        raise VisualizerProtocolError("run_id must be positive")
    identity = message["rep_session_id"]
    if not isinstance(identity, str) or re.fullmatch(r"[A-Za-z0-9_-]{1,64}", identity) is None:
        raise VisualizerProtocolError("invalid rep_session_id")
    if not isinstance(message["exercise"], str) or message["exercise"] not in REP_EXERCISES:
        raise VisualizerProtocolError("unsupported exercise")
    return message


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
