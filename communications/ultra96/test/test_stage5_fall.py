"""Stage 5 local reporting only: production C peer and both real server paths."""
from pathlib import Path
from types import SimpleNamespace as NS
import socket
import struct
import subprocess
import threading
import unittest
from unittest.mock import Mock, patch

import server
import test_server_ai_feedback as ai_fixture
from test_server import build_hello, receive_exact
import device_events as events

PEER = Path(__file__).resolve().parents[2] / ".build/device_event_peer.exe"


def event_frame(module=server, *, session=101, boot=0x01020304, event=1,
                occurred=1000000, synthetic=0, node=1):
    return module.build_header(0x13, node_id=node, payload_length=32,
                               session_id=session, sequence=event & 0xFFFFFFFF,
                               timestamp_us=occurred) + struct.pack(
                                   "!BBHIQQII", 1, synthetic, 0, boot, event,
                                   occurred, 3000, 0)


class Peer:
    def __init__(self):
        if not PEER.exists():
            raise AssertionError("Build device_event_peer.exe with run_stage5_tests.ps1")
        self.process = subprocess.Popen([str(PEER)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)

    def command(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        response = self.process.stdout.readline().strip()
        if not response:
            raise AssertionError("C peer exited without response")
        return response

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=3)
        self.process.stdout.close()


class DeviceEventTests(unittest.TestCase):
    def setUp(self):
        self.receiver = events.DeviceEventReceiver()
        self.connection = NS(node_id=1, session_id=101, boot_id=0x01020304, send=Mock())

    def receive(self, frame=None):
        return self.receiver.receive(frame or event_frame(), self.connection, server.build_header)

    def test_independent_expected_event_and_ack_bytes(self):
        peer = Peer()
        try:
            self.assertEqual(peer.command("push 1000000 1"), "1")
            frame = bytes.fromhex(peer.command("due 1000000 101"))
            self.assertEqual(frame.hex(),
                "a55a011301000020000000650000000100000000000f4240"
                "0101000001020304000000000000000100000000000f424000000bb800000000")
            with self.assertLogs("server", level="WARNING"):
                self.assertEqual(self.receive(frame), 0)
            ack = self.connection.send.call_args.args[0]
            self.assertEqual(ack[:16].hex(), "a55a0114000000100000006500000001")
            self.assertEqual(ack[24:].hex(), "01020304000000000000000100000000")
            self.assertEqual(peer.command("ack " + ack.hex()), "1")
            self.assertEqual(peer.command("due 1500000 101"), "-")
        finally:
            peer.close()

    def test_duplicate_has_no_second_acceptance_log(self):
        with self.assertLogs("server", level="INFO") as captured:
            self.assertEqual(self.receive(), 0)
            self.assertEqual(self.receive(), 1)
        self.assertEqual(sum("DEVICE_EVENT_ACCEPTED" in x for x in captured.output), 1)
        self.assertEqual(self.connection.send.call_count, 2)

    def test_conflicting_identity_is_rejected_without_ack(self):
        self.receive()
        self.connection.send.reset_mock()
        with self.assertRaises(ValueError):
            self.receive(event_frame(occurred=1000001))
        self.connection.send.assert_not_called()

    def test_malformed_fields_and_lengths_are_rejected(self):
        valid = event_frame()
        for offset in (0, 2, 3, 4, 5, 7, 8, 12, 16, 24, 25, 26, 27, 28, 36, 40, 48, 52, 55):
            mutated = bytearray(valid)
            mutated[offset] ^= 0x80
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.receive(bytes(mutated))
        for frame in (valid[:-1], valid+b"\0", event_frame(boot=0),
                      event_frame(event=0), event_frame(node=2)):
            with self.assertRaises(ValueError):
                self.receive(frame)
        self.connection.send.assert_not_called()

    def test_reconnect_reuses_receipt_after_failed_ack(self):
        self.connection.send.side_effect = OSError("lost ACK")
        with self.assertRaises(OSError):
            self.receive()
        self.connection.session_id = 202
        self.connection.send.side_effect = None
        self.assertEqual(self.receive(event_frame(session=202)), 1)

    def test_new_boot_can_restart_event_numbers_old_boot_cannot(self):
        self.receive()
        self.connection.boot_id = 42
        self.assertEqual(self.receive(event_frame(boot=42)), 0)
        with self.assertRaises(ValueError):
            self.receive(event_frame())

    def test_receipt_eviction_fails_closed_and_boot_capacity_is_bounded(self):
        with patch.object(events, "RECEIPTS_PER_BOOT", 2), patch.object(events, "MAX_BOOTS", 1):
            for identity in range(1, 4):
                self.receive(event_frame(event=identity))
            self.assertEqual(len(self.receiver.boots[(1, self.connection.boot_id)][1]), 2)
            with self.assertRaises(ValueError):
                self.receive(event_frame(event=1))
            self.connection.boot_id = 42
            with self.assertRaises(ValueError):
                self.receive(event_frame(boot=42))

    def test_expired_offline_event_is_not_replayed(self):
        peer = Peer()
        try:
            self.assertEqual(peer.command("push 1000000 1"), "1")
            self.assertEqual(peer.command("due 31000000 202"), "-")
        finally:
            peer.close()

    def test_ack_validation_c_peer_wrong_boot_reserved_session_flags(self):
        peer = Peer()
        try:
            peer.command("push 1000000 1")
            frame = bytes.fromhex(peer.command("due 1000000 101"))
            self.receive(frame)
            ack = self.connection.send.call_args.args[0]
            for offset in (4, 5, 7, 24, 36, 37, 38, 39):
                malformed = bytearray(ack); malformed[offset] ^= 0x80
                self.assertEqual(peer.command("ack " + malformed.hex()), "0")
            self.assertEqual(peer.command("ack " + ack.hex()), "1")
        finally:
            peer.close()


class BothServerIntegrationTests(unittest.TestCase):
    def test_real_binary_handler_and_c_retry_across_new_session(self):
        for module in (server, ai_fixture.ai_server):
            with self.subTest(server=module.__name__):
                coordinator = module.CaptureCoordinator(auto_start=False, physical_mode=True)
                publisher = Mock()
                peer = Peer()
                sockets, workers = [], []

                def connect():
                    client, accepted = socket.socketpair()
                    client.settimeout(3)
                    sockets.append(client)
                    worker = threading.Thread(target=module.handle_client,
                        args=(accepted, ("127.0.0.1", 1)),
                        kwargs=dict(coordinator=coordinator, visualizer_publisher=publisher),
                        daemon=True)
                    workers.append(worker); worker.start()
                    client.sendall(build_hello(1, 1 << 1, 0x01020304))
                    header = receive_exact(client, 24)
                    self.assertEqual(header[3], module.MESSAGE_TYPE_HELLO_ACK)
                    return client, module.unpack_header(header)[6]

                def ack_from(client):
                    while True:
                        h = receive_exact(client, 24)
                        size = module.unpack_header(h)[5]
                        payload = receive_exact(client, size)
                        if h[3] == 0x14:
                            return h+payload

                try:
                    client, session = connect()
                    self.assertEqual(peer.command("push 1000000 1"), "1")
                    frame = bytes.fromhex(peer.command(f"due 1000000 {session}"))
                    with self.assertLogs("server", level="INFO") as captured:
                        # Exercise real stream reassembly with split frames.
                        client.sendall(frame[:17]); client.sendall(frame[17:])
                        first = ack_from(client)  # Deliberately drop application ACK.
                        self.assertEqual(events.ACK_PAYLOAD.unpack(first[24:])[2], 0)
                        # Reconnect same boot; receipt must survive replacement.
                        client2, session2 = connect()
                        retry = bytes.fromhex(peer.command(f"due 1500000 {session2}"))
                        self.assertEqual(frame[24:], retry[24:])
                        client2.sendall(retry)
                        ack = ack_from(client2)
                        self.assertEqual(events.ACK_PAYLOAD.unpack(ack[24:])[2], 1)
                        self.assertEqual(peer.command("ack " + ack.hex()), "1")
                        self.assertEqual(peer.command(f"due 2000000 {session2}"), "-")
                    self.assertEqual(sum("DEVICE_EVENT_ACCEPTED" in x for x in captured.output), 1)
                    self.assertFalse(coordinator.run_active)
                    self.assertFalse(coordinator.desired_running)
                    self.assertEqual(coordinator.feedback_delivery.event_id, 0)
                    publisher.publish_classification.assert_not_called()
                    publisher.publish_test_response.assert_not_called()
                finally:
                    for client in sockets:
                        try:
                            client.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        client.close()
                    for worker in workers:
                        worker.join(timeout=3)
                        self.assertFalse(worker.is_alive())
                    coordinator.feedback_delivery.close()
                    peer.close()

    def test_current_connection_required_and_no_capture_or_feedback_side_effects(self):
        for module in (server, ai_fixture.ai_server):
            with self.subTest(server=module.__name__):
                c = module.CaptureCoordinator(auto_start=False, physical_mode=True)
                conn = module.NodeControlConnection(Mock(), 1, 101, c, boot_id=0x01020304)
                c.register(conn)
                try:
                    self.assertEqual(c.receive_device_event(event_frame(), conn), 0)
                    self.assertFalse(c.desired_running)
                    self.assertEqual(c.feedback_delivery.event_id, 0)
                    wrong = module.NodeControlConnection(Mock(), 1, 101, c, boot_id=0x01020304)
                    with self.assertRaises(module.ProtocolError):
                        c.receive_device_event(event_frame(), wrong)
                finally:
                    c.feedback_delivery.close()


if __name__ == "__main__":
    unittest.main()
