"""Automatic Hip event delivery after removal of the AR enable/disable feature."""
import asyncio
import json
from pathlib import Path
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.append(str(Path(__file__).resolve().parents[1] / "communications" / "ultra96"))
import server_rep_fall as server
from rep_control import RepController, RepControlServer
from visualizer_protocol import parse_rep_message, VisualizerProtocolError


class RecordingSocket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, how):
        pass


def emit(coordinator, connection, event=1):
    occurred = 1_000_000 + event
    frame = server.build_header(0x13, node_id=1, payload_length=32,
        session_id=connection.session_id, sequence=event, timestamp_us=occurred)
    frame += struct.pack("!BBHIQQII", 1, 0, 0, connection.boot_id, event, occurred, 3000, 0)
    return coordinator.receive_device_event(frame, connection)


class AutomaticFallTests(unittest.TestCase):
    def setUp(self):
        self.coordinator = server.CaptureCoordinator(auto_start=False)
        self.addCleanup(self.coordinator.feedback_delivery.close)
        self.hip = server.NodeControlConnection(RecordingSocket(), 1, 101,
            self.coordinator, boot_id=1234)
        self.coordinator.register(self.hip)

    def test_real_event_is_accepted_without_phone_or_rep_session(self):
        self.assertEqual(emit(self.coordinator, self.hip), 0)
        message = self.coordinator.device_events.falls.due({})[0]
        self.assertEqual(message, dict(v=1, type="fall_event", node_id=1,
                                      boot_id=1234, event_id="1"))
        self.assertEqual(self.hip.client.frames[-1][3], 0x14)
        self.assertFalse(self.coordinator.run_active)

    def test_phone_connect_disconnect_sends_no_detection_commands(self):
        controller = RepController(self.coordinator, 123)
        for _ in range(2):
            link = controller.connect()
            controller.disconnect(link)
        self.assertEqual(self.hip.client.frames, [])

    def test_removed_setting_command_is_rejected(self):
        old_command = json.dumps(dict(v=1, type="fall_detection_set", run_id=123,
                                      seq=1, enabled=False))
        with self.assertRaises(VisualizerProtocolError):
            parse_rep_message(old_command)
        controller = RepController(self.coordinator, 123)
        link = controller.connect()
        self.assertEqual(controller.handle(link, old_command)["status"], "rejected")
        self.assertEqual(self.hip.client.frames, [])


class AutomaticFallGatewayTests(unittest.IsolatedAsyncioTestCase):
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

    async def test_fall_reconnect_and_receipt_without_enable_request(self):
        import websockets
        from visualizer_gateway import VisualizerGateway, GatewayConfig
        coordinator = server.CaptureCoordinator(auto_start=False)
        hip = server.NodeControlConnection(RecordingSocket(), 1, 101, coordinator, boot_id=1234)
        coordinator.register(hip)
        service = RepControlServer(coordinator, 123, port=0)
        service.start()
        gateway = VisualizerGateway(GatewayConfig(websocket_port=0, publisher_port=0,
            control_port=service.port, cert_file=str(self.cert_file), key_file=str(self.key_file)))
        try:
            await gateway.start()
            context = ssl.create_default_context(cafile=str(self.cert_file))
            uri = f"wss://127.0.0.1:{gateway.websocket_port}"
            for reconnect in (False, True):
                async with websockets.connect(uri, ssl=context) as phone:
                    welcome = json.loads(await asyncio.wait_for(phone.recv(), 3))
                    self.assertEqual(welcome["type"], "rep_welcome")
                    if not reconnect:
                        emit(coordinator, hip)
                    message = json.loads(await asyncio.wait_for(phone.recv(), 3))
                    self.assertEqual(message, dict(v=1, type="fall_event", node_id=1,
                                                   boot_id=1234, event_id="1"))
                    if reconnect:
                        await phone.send(json.dumps(dict(message, type="fall_ack")))
                        for _ in range(50):
                            if not coordinator.device_events.falls.pending:
                                break
                            await asyncio.sleep(0.02)
                        self.assertFalse(coordinator.device_events.falls.pending)
                await asyncio.sleep(0.1)
            self.assertEqual([frame[3] for frame in hip.client.frames], [0x14])
        finally:
            await gateway.close()
            service.close()
            coordinator.feedback_delivery.close()


if __name__ == "__main__":
    unittest.main()
