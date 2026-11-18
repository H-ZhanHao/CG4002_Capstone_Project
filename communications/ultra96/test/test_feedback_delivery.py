"""Stage 2 server-to-two-ESP coverage using the actual desktop C delivery engine."""
from pathlib import Path
import struct
import subprocess
import threading
import time
import unittest
from unittest.mock import patch
import io

import server
from feedback_delivery import (
    FeedbackDelivery, encode_feedback, decode_feedback, FEEDBACK_RETRY_US,
    FEEDBACK_DEADLINE_US, FEEDBACK_MAX_PENDING_PER_NODE,
)

PEER = Path(__file__).resolve().parents[2] / ".build/feedback_peer.exe"


def until(predicate):
    deadline = time.monotonic() + 3
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("Timed out waiting for feedback worker")
        threading.Event().wait(0.005)


class PeerSocket:
    def __init__(self, node, session, server_module=server):
        self.server = server_module
        self.process = subprocess.Popen([str(PEER), str(node), str(session)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.frames = []
        self.outputs = 0
        self.drop_acks = 0
        self.connection = None
        self.lock = threading.Lock()
        self.gate = None

    def sendall(self, frame):
        if self.gate is not None:
            self.gate.wait(3)
        if frame[3] != self.server.MESSAGE_TYPE_FEEDBACK_COMMAND:
            return
        with self.lock:
            self.process.stdin.write(frame.hex() + "\n")
            self.process.stdin.flush()
            response = self.process.stdout.readline().split()
            ack, outputs = bytes.fromhex(response[0]), int(response[1])
            self.outputs = outputs
            self.frames.append(frame)
            if self.drop_acks:
                self.drop_acks -= 1
                return
        self.server.process_control_ack(ack, self.connection)

    def shutdown(self, _how):
        pass

    def close(self):
        if self.gate is not None:
            self.gate.set()
        self.process.stdin.close()
        self.process.wait(timeout=4)
        self.process.stdout.close()


class FeedbackTests(unittest.TestCase):
    server_module = server

    def setUp(self):
        self.server = self.server_module
        if not PEER.exists():
            self.fail("Build .build/feedback_peer.exe using run_stage2_tests.ps1 first")
        self.now = 100_000
        self.coordinator = self.server.CaptureCoordinator(auto_start=False, physical_mode=True)
        self.coordinator.feedback_delivery.clock = lambda: self.now
        self.peers = []
        self.connections = []
        self.add_peer(1, 101)
        self.add_peer(2, 202)

    def add_peer(self, node, session):
        peer = PeerSocket(node, session, self.server)
        connection = self.server.NodeControlConnection(peer, node, session, self.coordinator)
        peer.connection = connection
        self.peers.append(peer)
        self.connections.append(connection)
        self.coordinator.register(connection)
        return peer, connection

    def tearDown(self):
        manager = self.coordinator.feedback_delivery
        outboxes = list(manager.outboxes.values())
        manager.close()
        for peer in self.peers:
            if peer.gate is not None:
                peer.gate.set()
        for outbox in outboxes:
            outbox.thread.join(timeout=4)
        for peer in self.peers:
            peer.close()

    def test_expected_bytes_and_validation(self):
        expected = bytes.fromhex("0123456789abcdef0500000000000000")
        self.assertEqual(encode_feedback(0x0123456789abcdef, 5), expected)
        self.assertEqual(decode_feedback(expected), (0x0123456789abcdef, 5))
        frame = self.server.build_header(0x0b, node_id=0, payload_length=16,
            session_id=0x01020304, sequence=0x05060708, timestamp_us=9) + expected
        self.assertEqual(frame, bytes.fromhex(
            "a55a010b0000001001020304050607080000000000000009"
            "0123456789abcdef0500000000000000"))
        for payload in (expected[:-1], expected + b"\0", b"\0" * 16,
                        expected[:8] + b"\x01" + expected[9:]):
            with self.assertRaises(ValueError):
                decode_feedback(payload)
        for index in range(9, 16):
            bad = bytearray(expected); bad[index] = 1
            with self.assertRaises(ValueError):
                decode_feedback(bad)
        for event, action in ((0, 1), (-1, 1), (1 << 64, 1), (1, 7), (True, 1)):
            with self.assertRaises(ValueError):
                encode_feedback(event, action)

    def test_lost_ack_retry_once_and_independent_capture_ack(self):
        self.peers[0].drop_acks = 1
        self.coordinator.start_pending = True
        self.coordinator._pending_start_acks = {1: 99, 2: 99}
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: all(peer.outputs == 1 for peer in self.peers))
        until(lambda: len(self.coordinator.feedback_delivery.pending) == 1)
        self.now += FEEDBACK_RETRY_US
        self.coordinator.tick(self.now)
        until(lambda: not self.coordinator.feedback_delivery.pending)
        self.assertEqual(self.peers[0].frames[0], self.peers[0].frames[1])
        self.assertEqual([peer.outputs for peer in self.peers], [1, 1])
        self.assertEqual(self.coordinator._pending_start_acks, {1: 99, 2: 99})
        self.assertTrue(self.coordinator.start_pending)
        self.assertEqual(self.coordinator.feedback_delivery.status()[0][-1], 3)

    def test_delayed_duplicate_conflict_and_wrong_session(self):
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: not self.coordinator.feedback_delivery.pending)
        old = self.peers[0].frames[0]
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_IDLE)
        until(lambda: not self.coordinator.feedback_delivery.pending)
        self.peers[0].sendall(old)
        self.assertEqual(self.peers[0].outputs, 2)
        conflict = bytearray(old); conflict[32] = 6
        self.peers[0].sendall(bytes(conflict))
        self.assertEqual(self.peers[0].outputs, 2)
        stale = bytearray(old); stale[8:12] = struct.pack("!I", 999)
        self.peers[0].sendall(bytes(stale))
        self.assertEqual(self.peers[0].outputs, 2)
        malformed = bytearray(old); malformed[39] = 1
        self.peers[0].sendall(bytes(malformed))
        self.assertEqual(self.peers[0].outputs, 2)

    def test_rep_targets_hip_only_and_wrist_rejects_direct_rep(self):
        self.coordinator.request_feedback(self.server.FEEDBACK_REP_BUZZ)
        until(lambda: not self.coordinator.feedback_delivery.pending)
        self.assertEqual([peer.outputs for peer in self.peers], [1, 0])
        manager = self.coordinator.feedback_delivery
        manager.submit(4, 20, self.server.build_header, (2,))
        until(lambda: not manager.pending)
        self.assertEqual(manager.status()[0][2:], ("rejected", 7))
        self.assertEqual(self.peers[1].outputs, 0)

    def test_deadline_and_reconnect_do_not_replay(self):
        manager = self.coordinator.feedback_delivery
        self.peers[0].drop_acks = 100
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: self.peers[0].outputs == 1)
        self.now += FEEDBACK_DEADLINE_US
        manager.tick(self.now)
        self.assertEqual(manager.status()[0][2], "timeout")
        self.assertEqual(manager.repeat_last(), 0)
        self.coordinator.unregister(self.connections[0])
        peer, _ = self.add_peer(1, 303)
        self.now += FEEDBACK_DEADLINE_US
        manager.tick(self.now)
        self.assertEqual(peer.frames, [])
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: not manager.pending)
        self.assertEqual(peer.outputs, 1)

    def test_pending_delivery_cleared_on_disconnect_other_node_still_works(self):
        self.peers[0].drop_acks = 100
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: self.peers[0].outputs == 1)
        self.coordinator.unregister(self.connections[0])
        self.assertFalse(any(key[0] == 1 for key in self.coordinator.feedback_delivery.pending))
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: not self.coordinator.feedback_delivery.pending)
        self.assertEqual(self.peers[1].outputs, 2)

    def test_blocked_sender_does_not_block_other_node_and_pending_is_bounded(self):
        self.peers[0].gate = threading.Event()
        manager = self.coordinator.feedback_delivery
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        for _ in range(FEEDBACK_MAX_PENDING_PER_NODE + 2):
            self.coordinator.request_feedback(self.server.FEEDBACK_REP_BUZZ)
        until(lambda: self.peers[1].outputs >= 1)
        self.assertLessEqual(sum(key[0] == 1 for key in manager.pending), 8)
        self.assertEqual(manager.status()[0][2], "queue_full")
        self.peers[0].gate.set()

    def test_console_controls_and_duplicate_test(self):
        with patch.object(self.server.sys, "stdin", io.StringIO(
                "feedback rep\nfeedback status\nfeedback repeat\nfeedback idle\n")):
            self.server.run_control_console(self.coordinator)
        until(lambda: not self.coordinator.feedback_delivery.pending)
        self.assertEqual(self.peers[0].outputs, 2)
        self.assertEqual(self.peers[1].outputs, 1)

    def test_retry_limit_and_ack_header_validation(self):
        for peer in self.peers:
            peer.drop_acks = 100
        manager = self.coordinator.feedback_delivery
        self.coordinator.request_feedback(self.server.FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: all(len(peer.frames) == 1 for peer in self.peers))
        for count in (2, 3):
            self.now += FEEDBACK_RETRY_US
            manager.tick(self.now)
            until(lambda: all(len(peer.frames) == count for peer in self.peers))
        self.now += FEEDBACK_RETRY_US
        manager.tick(self.now)
        self.assertEqual([len(peer.frames) for peer in self.peers], [3, 3])
        self.now += FEEDBACK_RETRY_US
        manager.tick(self.now)
        self.assertFalse(manager.pending)
        self.assertEqual([peer.outputs for peer in self.peers], [1, 1])
        connection = self.connections[0]
        payload = struct.pack("!BBHIQ", 0x0b, 0, 0, 0, 123)
        ack = self.server.build_header(9, node_id=1, payload_length=16,
                                  session_id=101) + payload
        for index in (0, 2, 3, 4, 5, 7, 11, 15, 26):
            malformed = bytearray(ack); malformed[index] ^= 1
            with self.assertRaises(self.server.ProtocolError):
                self.server.process_control_ack(bytes(malformed), connection)
        with self.assertRaises(self.server.ProtocolError):
            self.server.process_control_ack(ack[:-1], connection)

    def test_evicted_ids_cannot_replay(self):
        manager = self.coordinator.feedback_delivery
        for sequence in range(35):
            manager.submit(4, sequence, self.server.build_header, (1,))
            until(lambda: not manager.pending)
        old = self.peers[0].frames[0]
        self.peers[0].sendall(old)
        new_sequence = bytearray(old)
        new_sequence[12:16] = struct.pack("!I", 100)
        self.peers[0].sendall(bytes(new_sequence))
        self.assertEqual(self.peers[0].outputs, 35)


if __name__ == "__main__":
    unittest.main()
