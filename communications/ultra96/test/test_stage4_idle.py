"""Stage 4 AI threshold -> real coordinator -> bounded feedback integration.

The pipeline fixture is the user's supplied Ultra96 implementation. Hardware
inference is replaced with deterministic results, not a new idle algorithm.
"""
from types import SimpleNamespace as NS
import sys
import threading
import time
import unittest
from unittest.mock import Mock, patch

import test_server_ai_feedback as ai_tests
from fixtures.inference_pipeline import InferencePipeline

server = ai_tests.ai_server


class Socket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, how):
        pass


def result(exercise="idle", confidence=1.0):
    return NS(exercise=exercise, confidence=confidence, capture_run_id=1,
              window_id=0, start_us=0, end_us=3_000_000, class_id=6,
              model_window_count=1)


class IdleIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.assembler = Mock()
        self.assembler.submit.side_effect = lambda sample: [sample]
        self.inference = Mock()
        self.inference.infer_window.side_effect = lambda window: window.result
        self.pipeline = InferencePipeline(self.assembler, self.inference, Mock())
        self.coordinator = server.CaptureCoordinator(
            physical_mode=True, window_assembler=self.pipeline)
        self.pipeline.on_idle = self.coordinator.handle_idle_classification
        self.connections = []
        for node in (1, 2):
            connection = server.NodeControlConnection(
                Socket(), node, node * 101, self.coordinator, boot_id=node)
            connection.mapping = server.ClockMapping(
                0, 20, time.monotonic_ns() // 1000)
            self.coordinator.register(connection)
            self.connections.append(connection)
        self.coordinator.request_start()
        self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
        self.resume()

    def tearDown(self):
        self.pipeline.close()
        manager = self.coordinator.feedback_delivery
        manager.close()
        for outbox in manager.outboxes.values():
            outbox.thread.join(timeout=2)

    def ack_pending(self, attribute, command):
        for node, sequence in list(getattr(self.coordinator, attribute).items()):
            self.coordinator.control_acknowledged(
                self.connections[node - 1], command, sequence,
                server.CONTROL_RESULT_APPLIED, 123456)

    def resume(self):
        c = self.coordinator
        conn = self.connections[0]
        header = server.unpack_header(server.build_header(
            server.MESSAGE_TYPE_ACTIVITY_TRIGGER, node_id=1, payload_length=16,
            session_id=conn.session_id, sequence=c._arm_generation))
        self.assertTrue(c.activity_triggered(conn, header, (
            conn.boot_id, c._arm_generation, c._armed_after_us + 1)))
        self.ack_pending("_pending_start_acks", server.MESSAGE_TYPE_START_FLUSH)
        self.assertTrue(c.run_active)

    def feed(self, classification=None, accepted=True):
        self.pipeline.submit(NS(
            run_id=1, window_id=0, quality=NS(accepted=accepted),
            result=classification or result()))
        deadline = time.monotonic() + 2
        while self.pipeline._queue.unfinished_tasks:
            if time.monotonic() >= deadline:
                self.fail("Inference worker did not finish (possible lock deadlock)")
            threading.Event().wait(0.002)

    def confirm_idle(self):
        for _ in range(5):
            self.feed()

    def test_fifth_confident_idle_submits_silent_idle_to_both(self):
        for _ in range(4):
            self.feed()
        self.assertTrue(self.coordinator.run_active)
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 1)
        self.feed()
        c = self.coordinator
        self.assertFalse(c.run_active)
        self.assertTrue(c.idle_paused)
        self.assertEqual(c.feedback_delivery.event_id, 2)
        self.assertEqual(set(c.feedback_delivery.tickets[2]), {1, 2})
        delivery = c.feedback_delivery.tickets[2][1]
        self.assertEqual(server.decode_feedback(delivery.frame[24:]),
                         (2, server.FEEDBACK_ACTIVITY_IDLE))
        self.assembler.stop_run.assert_called_once_with("AI idle pause")

    def test_non_idle_resets_streak(self):
        for _ in range(4):
            self.feed()
        self.feed(result("squat"))
        for _ in range(4):
            self.feed()
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 1)
        self.feed()
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_low_or_invalid_confidence_resets_streak(self):
        for confidence in (0.79, float("nan"), float("inf"), 1.01):
            for _ in range(4):
                self.feed()
            self.feed(result(confidence=confidence))
            self.assertEqual(self.pipeline._consecutive_idle, 0)
        for _ in range(5):
            self.feed(result(confidence=0.80))
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_rejected_windows_preserve_existing_streak_behavior(self):
        for _ in range(4):
            self.feed()
        self.feed(accepted=False)
        self.assertEqual(self.pipeline._consecutive_idle, 4)
        self.feed()
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_repeated_results_pause_calls_and_delivery_retries_one_event(self):
        old_generation = self.pipeline._generation
        self.confirm_idle()
        c = self.coordinator
        for _ in range(3):
            self.assertFalse(c.handle_idle_classification(result(), old_generation))
            c.idle_pause()  # Waiting for STOP ACKs.
        self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
        c.idle_pause()  # ARM pending.
        self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
        c.idle_pause()  # Already armed.
        item = c.feedback_delivery.tickets[2][1]
        c.feedback_delivery.tick(item.next_retry_us)
        self.assertEqual(c.feedback_delivery.event_id, 2)
        self.assertEqual(set(c.feedback_delivery.tickets), {1, 2})

    def test_fresh_trigger_resumes_and_next_episode_can_notify(self):
        self.confirm_idle()
        old_generation = self.pipeline._generation
        self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
        self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
        self.resume()
        self.assertFalse(self.coordinator.handle_idle_classification(
            result(), old_generation))
        self.assertEqual(self.pipeline._consecutive_idle, 0)
        self.confirm_idle()
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 4)

    def test_feedback_exception_does_not_block_stop_ack_arm(self):
        with patch.object(self.coordinator, "request_feedback",
                          side_effect=RuntimeError("injected delivery failure")) as submit:
            with self.assertLogs("server", level="ERROR"):
                self.confirm_idle()
            submit.assert_called_once_with(server.FEEDBACK_ACTIVITY_IDLE)
        self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
        self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
        self.assertTrue(self.coordinator._arm_ready)
        self.resume()

    def test_feedback_timeout_does_not_block_rearm_or_create_event(self):
        self.confirm_idle()
        item = self.coordinator.feedback_delivery.tickets[2][1]
        self.coordinator.feedback_delivery.tick(item.deadline_us)
        self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
        self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
        self.assertTrue(self.coordinator._arm_ready)
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_manual_stop_and_manual_idle_do_not_notify(self):
        self.coordinator.manual_stop()
        self.assertFalse(self.coordinator.handle_idle_classification(
            result(), self.pipeline._generation))
        self.coordinator.idle_pause()
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_disconnect_does_not_notify(self):
        self.coordinator.unregister(self.connections[0])
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)
        self.assertFalse(self.coordinator.handle_idle_classification(
            result(), self.pipeline._generation))

    def test_arm_timeout_does_not_notify(self):
        self.coordinator.manual_stop()
        self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
        self.coordinator.request_start()
        self.coordinator.tick(self.coordinator._arm_ack_deadline_us)
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_start_timeout_does_not_notify(self):
        self.coordinator.manual_stop()
        self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
        self.coordinator.request_start()
        self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
        c = self.coordinator
        conn = self.connections[0]
        header = server.unpack_header(server.build_header(
            server.MESSAGE_TYPE_ACTIVITY_TRIGGER, node_id=1, payload_length=16,
            session_id=conn.session_id, sequence=c._arm_generation))
        self.assertTrue(c.activity_triggered(conn, header, (
            conn.boot_id, c._arm_generation, c._armed_after_us + 1)))
        c.tick(c._start_ack_deadline_us)
        self.assertEqual(c.feedback_delivery.event_id, 2)

    def test_inflight_result_from_old_capture_is_discarded_after_resume(self):
        entered, release = threading.Event(), threading.Event()

        def delayed(window):
            entered.set()
            if not release.wait(2):
                raise RuntimeError("Test did not release inference")
            return window.result

        self.inference.infer_window.side_effect = delayed
        self.pipeline.submit(NS(run_id=1, window_id=0,
                                quality=NS(accepted=True), result=result()))
        self.assertTrue(entered.wait(1))
        # A second old window waits behind the in-flight inference.
        self.pipeline.submit(NS(run_id=1, window_id=1,
                                quality=NS(accepted=True), result=result()))
        try:
            self.coordinator.manual_stop()
            self.ack_pending("_pending_stop_acks", server.MESSAGE_TYPE_STOP)
            self.coordinator.request_start()
            self.ack_pending("_pending_arm_acks", server.MESSAGE_TYPE_ARM)
            self.resume()
        finally:
            release.set()
        self.feed(result("squat"))
        self.assertEqual(self.pipeline._consecutive_idle, 0)
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 3)
        self.assertEqual(self.inference.infer_window.call_count, 2)

    def test_logging_default_and_explicit_override(self):
        # config.py is an Ultra96 inference dependency, unrelated to logging.
        with patch.dict(sys.modules, {"config": NS(WINDOW_SECONDS=3.0)}), patch.object(
                server.Path, "home", return_value=server.Path("/root")):
            self.assertEqual(server.parse_args([]).log_file,
                             "/home/xilinx/comms/logs/server.log")
            self.assertEqual(server.parse_args(["--log-file", "/tmp/ai.log"]).log_file,
                             "/tmp/ai.log")


class PeerIdleIntegrationTests(unittest.TestCase):
    """Use both real desktop C firmware delivery engines for the AI event."""
    server_module = server
    setUp = ai_tests.AIServerFeedbackTests.setUp
    tearDown = ai_tests.AIServerFeedbackTests.tearDown
    add_peer = ai_tests.AIServerFeedbackTests.add_peer

    def test_threshold_and_lost_ack_retry_produce_one_hip_output(self):
        assembler = Mock()
        assembler.submit.side_effect = lambda window: [window]
        inference = Mock()
        inference.infer_window.side_effect = lambda window: window.result
        pipeline = InferencePipeline(assembler, inference, Mock())
        c = self.coordinator
        c.window_assembler = pipeline
        c.desired_running = c.run_active = True
        pipeline.on_idle = c.handle_idle_classification
        pipeline.start_run(7, 0)
        generation = pipeline._generation
        self.peers[0].drop_acks = 1
        try:
            for index in range(5):
                pipeline.submit(NS(run_id=7, window_id=index,
                                   quality=NS(accepted=True), result=result()))
            ai_tests.feedback_tests.until(lambda: c.feedback_delivery.event_id == 1)
            ai_tests.feedback_tests.until(lambda: self.peers[0].outputs == 1)
            self.assertFalse(c.handle_idle_classification(result(), generation))
            item = c.feedback_delivery.tickets[1][1]
            c.feedback_delivery.tick(item.next_retry_us)
            ai_tests.feedback_tests.until(lambda: not c.feedback_delivery.pending)
            self.assertEqual(len(self.peers[0].frames), 2)
            self.assertEqual(self.peers[0].outputs, 1)
            self.assertEqual(self.peers[1].outputs, 1)
            self.assertEqual(len(self.peers[1].frames), 1)
            self.assertEqual(c.feedback_delivery.event_id, 1)
        finally:
            pipeline.close()


if __name__ == "__main__":
    unittest.main()
