"""Operator-only fall settings through real coordinators and binary ACK parsing."""
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.append(str(Path(__file__).resolve().parents[1] / "communications" / "ultra96"))
import server_rep_fall as active_server
from communications.ultra96 import server as communications_server
from rep_control import RepController


class Socket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, how):
        pass


class FallControlChecks:
    def setUp(self):
        self.coordinator = self.api.CaptureCoordinator(auto_start=False)
        self.addCleanup(self.coordinator.feedback_delivery.close)
        self.control = self.coordinator.fall_control
        self.hip = self.connect(1, 101)

    def connect(self, node, session):
        connection = self.api.NodeControlConnection(Socket(), node, session,
            self.coordinator, boot_id=session)
        self.coordinator.register(connection)
        return connection

    def ack(self, request, connection=None, sequence=None, result=0):
        connection = connection or self.hip
        sequence = request.sequence if sequence is None else sequence
        payload = struct.pack("!BBHIQ", 0x0C, result, 0, sequence, 12345)
        frame = self.api.build_header(self.api.MESSAGE_TYPE_CONTROL_ACK,
            connection.node_id, len(payload), connection.session_id, sequence=sequence) + payload
        self.api.process_control_ack(frame, connection)

    def test_connect_sends_no_setting_and_does_not_assume_boot_state(self):
        self.assertEqual(self.hip.client.frames, [])
        self.assertIsNone(self.control.snapshot()["enabled"])
        self.assertEqual(self.control.snapshot()["status"], "unknown")

    def test_console_on_off_confirmed_only_by_hip_ack(self):
        with patch.object(self.api.sys, "stdin", io.StringIO("fall on\nfall status\n")):
            self.api.run_control_console(self.coordinator)
        request = self.control.latest
        header = self.api.unpack_header(self.hip.client.frames[-1])
        self.assertEqual(header[2:7], (0x0C, 0, 0, 4, self.hip.session_id))
        self.assertEqual(self.hip.client.frames[-1][24:], b"\x01\0\0\0")
        self.assertIsNone(self.control.snapshot()["enabled"])
        self.ack(request)
        self.assertTrue(self.control.snapshot()["enabled"])
        with patch.object(self.api.sys, "stdin", io.StringIO("fall off\n")):
            self.api.run_control_console(self.coordinator)
        self.assertEqual(self.hip.client.frames[-1][24:], b"\0\0\0\0")
        self.ack(self.control.latest, result=3)
        self.assertFalse(self.control.snapshot()["enabled"])
        self.assertFalse(self.coordinator.run_active)

    def test_wrong_node_sequence_and_old_connection_cannot_confirm(self):
        request = self.control.submit(True)
        wrist = self.connect(2, 202)
        self.ack(request, wrist)
        self.ack(request, sequence=request.sequence + 1)
        self.assertEqual(request.status, "pending")
        replacement = self.connect(1, 303)
        self.ack(request)
        self.assertEqual(request.status, "unavailable")
        self.assertIsNone(self.control.snapshot()["enabled"])
        self.assertEqual(replacement.client.frames, [])

    def test_pending_command_is_not_superseded(self):
        first = self.control.submit(True)
        second = self.control.submit(False)
        self.assertEqual(second.reason, "command_pending")
        self.assertIs(self.control.latest, first)
        self.assertEqual(len(self.hip.client.frames), 1)

    def test_timeout_late_ack_and_manual_retry(self):
        first = self.control.submit(True)
        first.deadline = 0
        self.coordinator.tick()
        self.assertEqual(first.status, "timeout")
        self.ack(first)
        self.assertIsNone(self.control.snapshot()["enabled"])
        self.assertEqual(len(self.hip.client.frames), 1)
        second = self.control.submit(False)
        self.ack(first)
        self.assertEqual(second.status, "pending")
        self.ack(second)
        self.assertFalse(self.control.snapshot()["enabled"])

    def test_firmware_rejection_and_send_failure_stay_unconfirmed(self):
        request = self.control.submit(True)
        self.ack(request, result=1)
        self.assertEqual(request.status, "rejected")
        self.assertIsNone(self.control.snapshot()["enabled"])
        with patch.object(self.hip, "send", side_effect=OSError("offline")):
            request = self.control.submit(False)
        self.assertEqual(request.reason, "hip_send_failed")
        self.assertIsNone(self.control.snapshot()["enabled"])

    def test_offline_request_is_not_replayed_on_reconnect(self):
        request = self.control.submit(True)
        self.ack(request)
        self.coordinator.unregister(self.hip)
        self.assertIsNone(self.control.snapshot()["enabled"])
        request = self.control.submit(False)
        self.assertEqual(request.reason, "hip_not_connected")
        self.hip = self.connect(1, 404)
        self.coordinator.tick()
        self.assertEqual(self.hip.client.frames, [])

    def test_phone_reconnect_does_not_change_pending_or_applied_setting(self):
        request = self.control.submit(True)
        controller = RepController(self.coordinator, 123)
        for _ in range(2):
            link = controller.connect()
            controller.disconnect(link)
        self.assertEqual(request.status, "pending")
        self.ack(request)
        link = controller.connect()
        controller.disconnect(link)
        self.assertTrue(self.control.snapshot()["enabled"])
        self.assertEqual(len(self.hip.client.frames), 1)


class ActiveServerFallTests(FallControlChecks, unittest.TestCase):
    api = active_server


class CommunicationsServerFallTests(FallControlChecks, unittest.TestCase):
    api = communications_server


if __name__ == "__main__":
    unittest.main()
