"""Final-profile control through the real coordinator, IPC and WSS gateway."""
import asyncio
import json
import socket
import ssl
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

# Exercise the supplied AI server and its dependencies in their deployment directory.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "server"))
import server_rep_fall as server
from fall_control import MESSAGE_TYPE_FALL_DETECTION_SET
from rep_control import RepController, RepControlServer
from visualizer_protocol import parse_fall_detection_message, VisualizerProtocolError


def request(enabled=False, seq=1, run=123):
    return json.dumps(dict(v=1, type="fall_detection_set", run_id=run,
                           seq=seq, enabled=enabled))


class RecordingSocket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, how):
        pass


class FallControlTests(unittest.TestCase):
    def setUp(self):
        self.c = server.CaptureCoordinator(auto_start=False)
        self.addCleanup(self.c.feedback_delivery.close)
        self.controller = RepController(self.c, 123)
        self.link = self.controller.connect()
        self.hip = self.connect(1)
        self.wrist = self.connect(2)

    def connect(self, node, session=None):
        conn = server.NodeControlConnection(RecordingSocket(), node, session or node * 101,
                                            self.c, boot_id=node)
        self.c.register(conn)
        return conn

    def send(self, **kwargs):
        return self.controller.handle_fall(self.link, request(**kwargs))

    def ack(self, result=0, connection=None, sequence=None):
        conn = connection or self.hip
        ticket = self.link["fall_ticket"]
        seq = ticket.sequence if sequence is None else sequence
        import struct
        frame = server.build_header(server.MESSAGE_TYPE_CONTROL_ACK, node_id=conn.node_id,
            payload_length=16, session_id=conn.session_id, sequence=seq) + struct.pack(
                "!BBHIQ", MESSAGE_TYPE_FALL_DETECTION_SET, result, 0, seq, 1000)
        server.process_control_ack(frame, conn)

    def test_setting_is_pending_until_real_control_ack_and_targets_hip_only(self):
        self.assertEqual(self.send(enabled=True)["status"], "pending")
        self.assertIsNone(self.controller.poll_fall(self.link))
        frame = self.hip.client.frames[-1]
        self.assertEqual(frame[3:8], bytes.fromhex("0c00000004"))
        self.assertEqual(frame[24:], bytes.fromhex("01000000"))
        self.assertEqual(self.wrist.client.frames, [])
        self.ack()
        response = self.controller.poll_fall(self.link)
        self.assertEqual((response["status"], response["enabled"]), ("applied", True))
        self.assertFalse(self.c.run_active)  # Works without capture or a rep session.

    def test_disable_has_explicit_zero_payload_and_needs_ack(self):
        self.send(enabled=True)
        self.ack()
        self.assertEqual(self.send(enabled=False, seq=2)["status"], "pending")
        self.assertEqual(self.hip.client.frames[-1][24:], b"\0" * 4)
        self.ack()
        self.assertIs(self.controller.poll_fall(self.link)["enabled"], False)

    def test_wrong_sequence_or_node_cannot_confirm(self):
        self.send(enabled=True)
        self.ack(sequence=999)
        self.ack(connection=self.wrist)
        self.assertIsNone(self.controller.poll_fall(self.link))
        self.ack()
        self.assertEqual(self.controller.poll_fall(self.link)["status"], "applied")

    def test_timeout_does_not_retry_or_accept_late_ack(self):
        self.send(enabled=True)
        ticket = self.link["fall_ticket"]
        with patch("fall_control.time.monotonic", return_value=ticket.deadline + 1):
            response = self.controller.poll_fall(self.link)
        self.assertEqual(response["status"], "timeout")
        self.assertIsNone(response["enabled"])
        self.assertEqual(len(self.hip.client.frames), 1)
        self.ack()
        self.assertIsNone(self.controller.poll_fall(self.link))
        self.assertEqual(self.send(enabled=False, seq=2)["status"], "pending")
        self.ack(sequence=ticket.sequence)
        self.assertIsNone(self.controller.poll_fall(self.link))
        self.ack()
        self.assertIs(self.controller.poll_fall(self.link)["enabled"], False)

    def test_duplicate_does_not_send_again_and_conflict_is_rejected(self):
        self.send(enabled=True)
        self.assertEqual(self.send(enabled=True)["status"], "pending")
        self.assertEqual(self.send(enabled=False)["status"], "rejected")
        self.assertEqual(self.send(enabled=False, seq=2)["reason"], "command_pending")
        self.assertEqual(len(self.hip.client.frames), 1)

    def test_esp_rejection_stays_unconfirmed(self):
        self.send(enabled=True)
        self.ack(result=2)
        result = self.controller.poll_fall(self.link)
        self.assertEqual(result["status"], "rejected")
        self.assertIsNone(result["enabled"])

    def test_disconnect_and_replacement_invalidate_a_confirmed_setting(self):
        self.send(enabled=True)
        self.ack()
        self.controller.poll_fall(self.link)
        self.c.unregister(self.hip)
        self.assertEqual(self.controller.poll_fall(self.link)["status"], "unavailable")
        self.hip = self.connect(1, session=303)
        self.assertEqual(self.send(enabled=False, seq=2)["status"], "pending")
        self.ack()
        self.assertIs(self.controller.poll_fall(self.link)["enabled"], False)

    def test_phone_reconnect_can_reset_off_and_old_link_has_no_authority(self):
        self.send(enabled=True)
        old = self.link
        self.link = self.controller.connect()
        self.assertEqual(self.controller.handle_fall(old, request(True, 2))["status"], "rejected")
        self.assertEqual(self.send(enabled=False)["status"], "pending")
        self.ack()
        self.assertIs(self.controller.poll_fall(self.link)["enabled"], False)

    def test_missing_hip_and_stale_run_do_not_report_off(self):
        self.c.unregister(self.hip)
        self.assertEqual(self.send()["status"], "unavailable")
        self.assertEqual(self.send(seq=2, run=999)["status"], "rejected")

    def test_strict_boolean_and_bounded_phone_schema(self):
        for value in (0, 1, "true", None, []):
            with self.assertRaises(VisualizerProtocolError):
                parse_fall_detection_message(request(value))
        for text in ("{}", "[]", "x" * 1025, request(run=0), request(seq=-1),
                     request(run=True), request(seq=2**53)):
            with self.assertRaises(VisualizerProtocolError):
                parse_fall_detection_message(text)


    def test_actual_ipc_handler_forwards_pending_then_applied(self):
        service = RepControlServer(self.c, 123, port=0)
        service.start()
        try:
            with socket.create_connection(("127.0.0.1", service.port), timeout=3) as client:
                with client.makefile("rb") as reader:
                    self.assertEqual(json.loads(reader.readline())["type"], "rep_welcome")
                    client.sendall((request(True) + "\n").encode())
                    self.assertEqual(json.loads(reader.readline())["status"], "pending")
                    ticket = self.c.fall_control.pending
                    self.c.control_acknowledged(self.hip, MESSAGE_TYPE_FALL_DETECTION_SET,
                                               ticket.sequence, 0, 1000)
                    response = json.loads(reader.readline())
                    self.assertEqual((response["type"], response["enabled"]),
                                     ("fall_detection_result", True))
        finally:
            service.close()


class GatewayTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("OpenSSL is required for temporary TLS certificates")
        cls.directory = tempfile.TemporaryDirectory()
        cls.cert_file = Path(cls.directory.name) / "cert.pem"
        cls.key_file = Path(cls.directory.name) / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(cls.key_file), "-out", str(cls.cert_file), "-days", "1",
            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
            check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    async def test_pinned_wss_control_and_reconnect_off_roundtrip(self):
        import websockets
        from visualizer_gateway import VisualizerGateway, GatewayConfig
        coordinator = server.CaptureCoordinator(auto_start=False)
        hip = server.NodeControlConnection(RecordingSocket(), 1, 101, coordinator, boot_id=1)
        coordinator.register(hip)
        service = RepControlServer(coordinator, 123, port=0)
        service.start()
        gateway = VisualizerGateway(GatewayConfig(websocket_port=0, publisher_port=0,
            control_port=service.port, cert_file=str(self.cert_file), key_file=str(self.key_file)))
        try:
            await gateway.start()
            context = ssl.create_default_context(cafile=str(self.cert_file))
            for enabled in (True, False):  # The new AR connection sends OFF.
                async with websockets.connect(f"wss://127.0.0.1:{gateway.websocket_port}", ssl=context) as phone:
                    self.assertEqual(json.loads(await asyncio.wait_for(phone.recv(), 3))["type"], "rep_welcome")
                    await phone.send(request(enabled))
                    self.assertEqual(json.loads(await asyncio.wait_for(phone.recv(), 3))["status"], "pending")
                    ticket = coordinator.fall_control.pending
                    coordinator.control_acknowledged(hip, MESSAGE_TYPE_FALL_DETECTION_SET,
                                                     ticket.sequence, 0, 1000)
                    response = json.loads(await asyncio.wait_for(phone.recv(), 3))
                    self.assertEqual((response["status"], response["enabled"]), ("applied", enabled))
                await asyncio.sleep(0.1)
        finally:
            await gateway.close()
            service.close()
            coordinator.feedback_delivery.close()


if __name__ == "__main__":
    unittest.main()
