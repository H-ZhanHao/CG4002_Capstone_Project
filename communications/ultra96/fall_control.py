"""Server-terminal Hip settings, confirmed by ESP ACK; no automatic retries."""
from dataclasses import dataclass
import logging
import struct
import time

MESSAGE_TYPE_FALL_DETECTION_SET = 0x0C
ACK_TIMEOUT_SECONDS = 2.0
LOGGER = logging.getLogger("server")


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
        self.latest = None

    def _log(self, request):
        LOGGER.info("FALL_SETTING status=%s requested=%s enabled=%s sequence=%d reason=%s",
                    request.status, "ON" if request.enabled else "OFF",
                    ("ON" if request.enabled else "OFF") if request.status == "applied" else "unknown",
                    request.sequence, request.reason)

    def _refresh(self):
        request = self.latest
        if request is None:
            return
        if (request.connection is not None and
                self.coordinator._connections.get(1) is not request.connection and
                request.status != "unavailable"):
            request.status, request.reason = "unavailable", "hip_connection_changed"
            self._log(request)
        elif request.status == "pending" and time.monotonic() >= request.deadline:
            request.status, request.reason = "timeout", "esp_ack_timeout; retry manually"
            self._log(request)

    def tick(self):
        with self.coordinator._lock:
            self._refresh()

    def submit(self, enabled):
        if type(enabled) is not bool:
            raise ValueError("enabled must be a boolean")
        with self.coordinator._lock:
            self._refresh()
            if self.latest is not None and self.latest.status == "pending":
                request = FallSettingRequest(enabled, status="rejected", reason="command_pending")
                self._log(request)
                return request
            connection = self.coordinator._connections.get(1)
            request = FallSettingRequest(enabled, connection)
            self.latest = request
            if connection is None:
                request.status, request.reason = "unavailable", "hip_not_connected"
            else:
                request.sequence = self.coordinator._next_sequence()
                request.deadline = time.monotonic() + ACK_TIMEOUT_SECONDS
                payload = struct.pack("!B3x", enabled)
                frame = self.build_header(MESSAGE_TYPE_FALL_DETECTION_SET, node_id=0,
                    payload_length=len(payload), session_id=connection.session_id,
                    sequence=request.sequence) + payload
                try:
                    connection.send(frame)
                except OSError:
                    request.status, request.reason = "unavailable", "hip_send_failed"
            self._log(request)
            return request

    def snapshot(self):
        with self.coordinator._lock:
            self._refresh()
            request = self.latest
            return {
                "connected": 1 in self.coordinator._connections,
                "enabled": request.enabled if request is not None and request.status == "applied" else None,
                "status": request.status if request is not None else "unknown",
                "reason": request.reason if request is not None else "no_setting_confirmed",
            }

    def acknowledge(self, connection, sequence, result):
        with self.coordinator._lock:
            self._refresh()
            request = self.latest
            if (request is None or request.status != "pending" or
                    request.connection is not connection or request.sequence != sequence):
                return
            if result in (0, 3):  # APPLIED / DUPLICATE of the same explicit setting.
                request.status, request.reason = "applied", "esp_confirmed"
            else:
                request.status, request.reason = "rejected", "esp_result_" + str(result)
            self._log(request)

    def handle_console(self, command):
        if command in ("fall on", "fall off"):
            self.submit(command == "fall on")
        elif command == "fall status":
            state = self.snapshot()
            LOGGER.info("FALL_STATUS enabled=%s connected=%s status=%s reason=%s",
                        {True: "ON", False: "OFF", None: "unknown"}[state["enabled"]],
                        state["connected"], state["status"], state["reason"])
        else:
            return False
        return True
