"""One outstanding Hip setting command, confirmed by ESP; no automatic retries."""
from dataclasses import dataclass
import struct
import time

MESSAGE_TYPE_FALL_DETECTION_SET = 0x0C
ACK_TIMEOUT_SECONDS = 2.0


@dataclass
class FallSettingRequest:
    enabled: bool
    connection: object = None
    sequence: int = 0
    deadline: float = 0
    status: str = "pending"
    reason: str = "awaiting_esp"


class FallDetectionControl:
    def __init__(self, coordinator, build_header):
        self.coordinator = coordinator
        self.build_header = build_header
        self.pending = None

    def _refresh(self, request):
        if request.connection is not None and self.coordinator._connections.get(1) is not request.connection:
            request.status, request.reason = "unavailable", "hip_connection_changed"
        elif request.status == "pending" and time.monotonic() >= request.deadline:
            request.status, request.reason = "timeout", "esp_ack_timeout; retry manually"
        if self.pending is request and request.status != "pending":
            self.pending = None
        return request

    def submit(self, enabled):
        with self.coordinator._lock:
            if self.pending is not None and self._refresh(self.pending).status == "pending":
                return FallSettingRequest(enabled, status="rejected", reason="command_pending")
            connection = self.coordinator._connections.get(1)
            if connection is None:
                return FallSettingRequest(enabled, status="unavailable", reason="hip_not_connected")
            sequence = self.coordinator._next_sequence()
            request = FallSettingRequest(enabled, connection, sequence,
                                         time.monotonic() + ACK_TIMEOUT_SECONDS)
            self.pending = request
            payload = struct.pack("!B3x", enabled)
            frame = self.build_header(MESSAGE_TYPE_FALL_DETECTION_SET, node_id=0,
                payload_length=len(payload), session_id=connection.session_id,
                sequence=sequence) + payload
            try:
                connection.send(frame)
            except OSError:
                request.status, request.reason = "unavailable", "hip_send_failed"
                self.pending = None
            return request

    def snapshot(self, request):
        with self.coordinator._lock:
            return self._refresh(request)

    def cancel(self, request):
        with self.coordinator._lock:
            if self.pending is request:
                request.status, request.reason = "timeout", "phone_disconnected"
                self.pending = None

    def acknowledge(self, connection, sequence, result):
        with self.coordinator._lock:
            request = self.pending
            if request is None or self._refresh(request).status != "pending":
                return
            if request.connection is not connection or request.sequence != sequence:
                return
            if result in (0, 3):  # APPLIED / DUPLICATE of the same explicit setting.
                request.status, request.reason = "applied", "esp_confirmed"
            else:
                request.status, request.reason = "rejected", "esp_result_" + str(result)
            self.pending = None
