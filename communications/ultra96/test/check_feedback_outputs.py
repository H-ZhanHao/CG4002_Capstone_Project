"""Regression checks for the integrated feedback contract and capture lifecycle.

Run check_feedback_outputs.ps1 to build the production-C wire peer first.
The AI entrypoint's deployment-only assembler import is supplied by the local
assembler for these tests; no production inference implementation is changed.
"""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time
import unittest
from unittest.mock import patch

import server
import window_assembler
from feedback_delivery import (
    encode_feedback, decode_feedback, FEEDBACK_REP_BUZZ,
    FEEDBACK_ACTIVITY_ACTIVE, FEEDBACK_ACTIVITY_IDLE,
)
from rep_control import RepController

spec = importlib.util.spec_from_file_location(
    "_feedback_outputs_ai", Path(__file__).with_name("server_AI.py"))
ai_server = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {
    "window_assembler_updated": window_assembler, spec.name: ai_server,
}):
    spec.loader.exec_module(ai_server)

PEER = Path(__file__).resolve().parents[2] / ".build/integrated_feedback_peer.exe"


def until(predicate):
    deadline = time.monotonic() + 3
    while not predicate():
        if time.monotonic() > deadline:
            raise AssertionError("Feedback worker timed out")
        threading.Event().wait(0.002)


class Peer:
    def __init__(self, module, node):
        self.module, self.node = module, node
        self.process = subprocess.Popen([str(PEER), str(node), str(100 + node)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.frames = []
        self.outputs = 0
        self.drop_acks = 0

    def sendall(self, frame):
        if frame[3] != self.module.MESSAGE_TYPE_FEEDBACK_COMMAND:
            return
        self.process.stdin.write(frame.hex() + "\n")
        self.process.stdin.flush()
        response = self.process.stdout.readline().split()
        ack, self.outputs = bytes.fromhex(response[0]), int(response[1])
        self.frames.append(frame)
        if self.drop_acks:
            self.drop_acks -= 1
        else:
            self.module.process_control_ack(ack, self.connection)

    def shutdown(self, _how):
        pass

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=3)
        self.process.stdout.close()


class FeedbackOutputChecks(unittest.TestCase):
    module = server

    def setUp(self):
        self.c = self.module.CaptureCoordinator(auto_start=False, physical_mode=True)
        self.peers = []
        for node in (1, 2):
            peer = Peer(self.module, node)
            peer.connection = self.module.NodeControlConnection(
                peer, node, 100 + node, self.c, boot_id=node)
            peer.connection.mapping = self.module.ClockMapping(
                0, 20, time.monotonic_ns() // 1000)
            self.c.register(peer.connection)
            self.peers.append(peer)

    def tearDown(self):
        outboxes = list(self.c.feedback_delivery.outboxes.values())
        self.c.feedback_delivery.close()
        for outbox in outboxes:
            outbox.thread.join(timeout=3)
        for peer in self.peers:
            peer.close()

    def drain(self):
        until(lambda: not self.c.feedback_delivery.pending)

    def ack_all(self, attribute, kind):
        for node, seq in list(getattr(self.c, attribute).items()):
            self.c.control_acknowledged(self.peers[node - 1].connection,
                kind, seq, self.module.CONTROL_RESULT_APPLIED, 123456)

    def start(self, confirm=True):
        m, c = self.module, self.c
        c.request_start()
        self.ack_all("_pending_arm_acks", m.MESSAGE_TYPE_ARM)
        conn = self.peers[0].connection
        header = m.unpack_header(m.build_header(m.MESSAGE_TYPE_ACTIVITY_TRIGGER,
            node_id=1, payload_length=16, session_id=conn.session_id,
            sequence=c._arm_generation))
        self.assertTrue(c.activity_triggered(conn, header,
            (conn.boot_id, c._arm_generation, c._armed_after_us + 1)))
        if confirm:
            self.ack_all("_pending_start_acks", m.MESSAGE_TYPE_START_FLUSH)
            self.drain()

    def actions(self, node):
        return [decode_feedback(frame[24:])[1] for frame in self.peers[node - 1].frames]

    def test_wire_layout_and_retired_actions(self):
        payload = encode_feedback(0x0123456789ABCDEF, FEEDBACK_ACTIVITY_ACTIVE)
        self.assertEqual(payload.hex(), "0123456789abcdef0500000000000000")
        self.assertEqual(len(payload), 16)
        for old in (0, 1, 2, 3, 7):
            with self.assertRaises(ValueError):
                encode_feedback(1, old)
            with self.assertRaises(ValueError):
                decode_feedback(struct.pack("!QB7x", 1, old))
        # Production C decoder must also reject retired actions without outputs.
        for old in (1, 2, 3):
            self.peers[0].sendall(self.module.build_header(0x0B, node_id=0,
                payload_length=16, session_id=101, sequence=old) +
                struct.pack("!QB7x", old, old))
        self.assertEqual(self.peers[0].outputs, 0)

    def test_active_requires_both_start_acks_and_idle_reaches_both(self):
        self.start(confirm=False)
        self.assertEqual([p.frames for p in self.peers], [[], []])
        seq = self.c._pending_start_acks[1]
        self.c.control_acknowledged(self.peers[0].connection,
            self.module.MESSAGE_TYPE_START_FLUSH, seq, 0, 123456)
        self.assertFalse(self.c.run_active)
        self.assertEqual(self.c.feedback_delivery.event_id, 0)
        self.ack_all("_pending_start_acks", self.module.MESSAGE_TYPE_START_FLUSH)
        self.drain()
        self.assertTrue(self.c.run_active)
        self.c.idle_pause()
        self.drain()
        for node in (1, 2):
            self.assertEqual(self.actions(node),
                [FEEDBACK_ACTIVITY_ACTIVE, FEEDBACK_ACTIVITY_IDLE])
        self.ack_all("_pending_stop_acks", self.module.MESSAGE_TYPE_STOP)
        self.ack_all("_pending_arm_acks", self.module.MESSAGE_TYPE_ARM)
        self.assertTrue(self.c._arm_ready)
        self.assertEqual(self.c.feedback_delivery.event_id, 2) # Arming stays dark.

    def test_partial_start_timeout_never_announces_active(self):
        self.start(confirm=False)
        self.c.tick(self.c._start_ack_deadline_us)
        self.assertFalse(self.c.run_active)
        self.assertEqual(self.c.feedback_delivery.event_id, 0)

    def test_manual_stop_disconnect_and_reconnect_clear_activity(self):
        self.start()
        self.c.manual_stop()
        self.drain()
        self.assertEqual(self.actions(2)[-1], FEEDBACK_ACTIVITY_IDLE)
        self.c.unregister(self.peers[0].connection)
        # Reconnection cannot inherit ACTIVE; a fresh dual START is required.
        self.c.register(self.peers[0].connection)
        self.assertFalse(self.c.run_active)

    def test_disconnect_during_active_clears_remaining_board(self):
        self.start()
        self.c.unregister(self.peers[0].connection)
        self.drain()
        self.assertFalse(self.c.run_active)
        self.assertEqual(self.actions(2)[-1], FEEDBACK_ACTIVITY_IDLE)

    def test_rep_uses_hip_only_and_lost_ack_does_not_replay(self):
        self.peers[0].drop_acks = 1
        self.c.request_feedback(FEEDBACK_REP_BUZZ)
        until(lambda: self.peers[0].outputs == 1)
        item = next(iter(self.c.feedback_delivery.pending.values()))
        self.c.feedback_delivery.tick(item.next_retry_us)
        self.drain()
        self.assertEqual(self.peers[0].outputs, 1)
        self.assertEqual(self.peers[0].frames[0], self.peers[0].frames[1])
        self.assertEqual(self.peers[1].frames, [])
        self.c.feedback_delivery.submit(FEEDBACK_REP_BUZZ, 20,
            self.module.build_header, (2,))
        self.drain()
        self.assertEqual(self.c.feedback_delivery.status()[0][2:], ("rejected", 7))

    def test_idle_supersedes_unacknowledged_active_retries(self):
        for peer in self.peers:
            peer.drop_acks = 1
        old = self.c.request_feedback(FEEDBACK_ACTIVITY_ACTIVE)
        until(lambda: all(p.outputs == 1 for p in self.peers))
        self.c.request_feedback(FEEDBACK_ACTIVITY_IDLE)
        self.drain()
        self.assertTrue(all(n["status"] == "superseded"
            for n in self.c.feedback_delivery.event_status(old)))
        self.c.feedback_delivery.tick(time.monotonic_ns() // 1000 + 3_000_000)
        self.assertEqual([p.outputs for p in self.peers], [2, 2])

    def test_ar_session_duplicates_and_catchup_keep_existing_semantics(self):
        import json
        reps = RepController(self.c, 123)
        link = reps.connect()
        def send(kind, count, seq):
            return reps.handle(link, json.dumps(dict(v=1, type=kind,
                run_id=123, seq=seq, rep_session_id="setA", rep_index=count,
                exercise="squat")))
        self.assertEqual(send("rep_session_start", 0, 0)["status"], "session_started")
        self.assertEqual(send("rep_event", 1, 1)["status"], "accepted")
        self.drain()
        self.assertEqual(send("rep_event", 1, 2)["status"], "duplicate")
        self.assertEqual(send("rep_event", 5, 3)["status"], "accepted")
        self.drain()
        self.assertEqual([p.outputs for p in self.peers], [2, 0])

    def test_feedback_failure_does_not_block_capture(self):
        with patch.object(self.c, "request_feedback", side_effect=RuntimeError("test")):
            with self.assertLogs("server", level="ERROR"):
                self.start()
                self.c.manual_stop()
        self.assertFalse(self.c.run_active)
        self.assertEqual(set(self.c._pending_stop_acks), {1, 2})


class AIFeedbackOutputChecks(FeedbackOutputChecks):
    module = ai_server


if __name__ == "__main__":
    unittest.main()
