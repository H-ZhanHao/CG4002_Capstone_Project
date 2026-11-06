"""Independent startup, joining, failures, and capture-time fallback coverage."""
import struct
import time
import unittest
from contextlib import ExitStack
from types import SimpleNamespace
from unittest.mock import Mock, MagicMock, patch

import server
import server_AI as ai_server
from dynamic_capture import STREAM_TIMEOUT_US
from window_assembler import WindowAssembler, WindowConfig, DecodedImuSample


class RecordingSocket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, how):
        pass


def sample(node, sequence, timestamp):
    return DecodedImuSample(node, node * 101, sequence, timestamp, timestamp,
                            1, 2, 3, 4, 5, 6, 0, 0)


class DynamicCaptureTests(unittest.TestCase):
    api = server

    def setUp(self):
        self.assembler = Mock()
        self.c = self.api.CaptureCoordinator(window_assembler=self.assembler)
        self.addCleanup(self.c.feedback_delivery.close)

    def connect(self, node, synced=True, session=None):
        conn = self.api.NodeControlConnection(RecordingSocket(), node,
            session or node * 101, self.c, boot_id=node + 50)
        self.c.register(conn)
        if synced:
            conn.mapping = self.api.ClockMapping(node * 1000, 20, time.monotonic_ns() // 1000)
            self.c.mapping_updated(conn)
        return conn

    def test_default_cli_needs_no_mode_flag(self):
        args = self.api.parse_args([])
        self.assertFalse(hasattr(args, "one_node"))
        self.assertEqual(args.node_wait_ms, 250)
        with patch("sys.stderr"), self.assertRaises(SystemExit):
            self.api.parse_args(["--one-node"])

    def test_main_uses_dynamic_capture_without_flag(self):
        listener = Mock()
        listener.accept.side_effect = KeyboardInterrupt
        sockets = MagicMock()
        sockets.return_value.__enter__.return_value = listener
        instances = []
        coordinator_type = self.api.CaptureCoordinator
        def create(**kwargs):
            coordinator = coordinator_type(**kwargs)
            instances.append(coordinator)
            return coordinator
        argv = []
        with ExitStack() as stack:
            stack.enter_context(patch.object(self.api, "configure_logging"))
            stack.enter_context(patch.object(self.api, "socket", Mock(socket=sockets,
                AF_INET=2, SOCK_STREAM=1, SOL_SOCKET=1, SO_REUSEADDR=2)))
            stack.enter_context(patch.object(self.api, "VisualizerEventPublisher",
                return_value=Mock(run_id=1)))
            stack.enter_context(patch.object(self.api, "RepControlServer",
                return_value=Mock(port=9101)))
            stack.enter_context(patch.object(self.api, "run_control_console"))
            stack.enter_context(patch.object(self.api, "CaptureCoordinator", side_effect=create))
            if self.api is ai_server:
                stack.enter_context(patch.dict("sys.modules", {
                    "test_dynamic_adapter": SimpleNamespace(create=lambda args: Mock()),
                    # The AI teammate supplies this dependency on Ultra96.
                    # This test verifies server wiring, not their worker.
                    "inference_pipeline": SimpleNamespace(InferencePipeline=
                        lambda assembler, inference, publisher, queue_size:
                            Mock(assembler=assembler)),
                }))
                argv = ["--inference-adapter", "test_dynamic_adapter:create"]
            with self.assertRaises(KeyboardInterrupt):
                self.api.main(argv)
        self.assertEqual(len(instances), 1)
        self.assertTrue(hasattr(instances[0], "nodes"))
        target = instances[0].window_assembler
        assembler = getattr(target, "assembler", target)
        self.assertEqual(assembler.config.node_wait_us, 250_000)
        self.assertFalse(hasattr(assembler.config, "allow_single_node"))

    def ack(self, conn, kind=None, result=0):
        state = self.c.nodes[conn.node_id]
        if kind is None:
            kind = (self.api.MESSAGE_TYPE_ARM if state.phase == "arming"
                    else self.api.MESSAGE_TYPE_START_FLUSH)
        self.c.control_acknowledged(conn, kind, state.sequence, result, 123456)

    def trigger(self, conn):
        state = self.c.nodes[conn.node_id]
        header = self.api.unpack_header(self.api.build_header(
            self.api.MESSAGE_TYPE_ACTIVITY_TRIGGER, conn.node_id, 16,
            conn.session_id, sequence=9))
        return self.c.activity_triggered(conn, header, (
            conn.boot_id, state.generation,
            state.boundary + conn.mapping.node_minus_coordinator_us + 1))

    def start(self, node=1):
        conn = self.connect(node)
        self.ack(conn)
        self.assertTrue(self.trigger(conn))
        self.ack(conn)
        self.assertTrue(self.c.run_active)
        return conn

    def test_either_node_can_start_alone(self):
        conn = self.start(2)
        self.assertEqual(self.c.nodes[2].phase, "streaming")
        self.assembler.start_run.assert_called_once()
        self.c.submit_sample(conn, sample(2, 0, 0))
        self.assembler.submit.assert_called_once()

    def test_late_join_preserves_run_and_uses_live_boundary(self):
        first = self.start()
        origin = self.c._run_origin
        second = self.connect(2)
        flushes = [f for f in second.client.frames
                   if self.api.unpack_header(f)[2] == self.api.MESSAGE_TYPE_START_FLUSH]
        self.assertEqual(struct.unpack("!QqIB3xI", flushes[-1][24:])[2], 0)
        self.ack(second)
        self.assertEqual(self.c._run_origin, origin)
        self.assembler.start_run.assert_called_once()
        self.assembler.stop_run.assert_not_called()
        self.assertEqual(self.c.nodes[first.node_id].phase, "streaming")

    def test_partner_sync_does_not_invalidate_first_arm(self):
        first = self.connect(1)
        self.ack(first)
        generation = self.c.nodes[1].generation
        second = self.connect(2)
        self.ack(second)
        self.assertEqual(self.c.nodes[1].generation, generation)
        self.assertTrue(self.trigger(first))
        self.ack(first)
        self.assertTrue(self.c.run_active)
        self.ack(second)
        self.assembler.start_run.assert_called_once()

    def test_unsynced_partner_does_not_block_trigger(self):
        self.connect(2, synced=False)
        first = self.start()
        self.assertTrue(self.c.run_active)
        self.assertEqual(self.c.nodes[2].phase, "connected")

    def test_start_ack_timeout_is_isolated(self):
        first = self.connect(1)
        second = self.connect(2)
        self.ack(first)
        self.ack(second)
        self.assertTrue(self.trigger(first))
        self.ack(first)
        deadline = self.c.nodes[2].deadline
        # Keep the acknowledged node live while the other command expires.
        self.c.nodes[1].last_sample = deadline
        self.c.tick(deadline)
        self.assertTrue(self.c.run_active)
        self.assertNotIn(2, self.c.nodes)
        self.assembler.stop_run.assert_not_called()

    def test_arm_timeout_does_not_disarm_acknowledged_partner(self):
        first = self.connect(1)
        second = self.connect(2)
        self.ack(first)
        self.c.tick(self.c.nodes[2].deadline)
        self.assertEqual(self.c.nodes[1].phase, "armed")
        self.assertNotIn(2, self.c.nodes)
        self.assertTrue(self.trigger(first))

    def test_failed_start_send_does_not_cancel_other_participant(self):
        first = self.connect(1)
        second = self.connect(2)
        self.ack(first)
        self.ack(second)
        first.send = Mock(side_effect=OSError("lost transport"))
        self.assertTrue(self.trigger(second))
        self.assertNotIn(1, self.c.nodes)
        self.ack(second)
        self.assertTrue(self.c.run_active)

    def test_expired_mapping_and_stale_boot_cannot_trigger(self):
        first = self.connect(1)
        self.ack(first)
        state = self.c.nodes[1]
        header = self.api.unpack_header(self.api.build_header(
            self.api.MESSAGE_TYPE_ACTIVITY_TRIGGER, 1, 16, first.session_id))
        self.assertFalse(self.c.activity_triggered(first, header,
            (first.boot_id + 1, state.generation, state.boundary + 1)))
        first.mapping = self.api.ClockMapping(0, 20,
            time.monotonic_ns() // 1000 - self.api.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US - 1)
        self.assertFalse(self.trigger(first))
        self.assertIsNone(self.c._run_origin)

    def test_failed_history_falls_back_to_live(self):
        first = self.connect(1)
        self.ack(first)
        self.trigger(first)
        old_sequence = self.c.nodes[1].sequence
        self.ack(first, result=2)
        self.assertTrue(self.c.nodes[1].live_retry)
        self.assertNotEqual(self.c.nodes[1].sequence, old_sequence)
        self.ack(first)
        self.assertTrue(self.c.run_active)

    def test_disconnect_and_reconnect_only_changes_one_stream(self):
        first = self.start()
        second = self.connect(2)
        self.ack(second)
        self.c.unregister(second)
        self.assertTrue(self.c.run_active)
        replacement = self.connect(2, session=999)
        self.ack(replacement)
        self.c.unregister(second)  # Old worker cleanup cannot remove replacement.
        self.assertIs(self.c._connections[2], replacement)
        self.assertEqual(self.c.submit_sample(second, sample(2, 1, 0)), [])
        self.assembler.start_run.assert_called_once()
        self.c.unregister(first)
        self.assertTrue(self.c.run_active)

    def test_last_disconnect_requires_fresh_peak(self):
        first = self.start()
        self.c.unregister(first)
        self.assertFalse(self.c.run_active)
        self.assertIsNone(self.c._run_origin)
        first = self.connect(1, session=999)
        self.assertEqual(self.c.nodes[1].phase, "arming")
        self.ack(first)
        self.assertFalse(self.c.run_active)
        self.assertTrue(self.trigger(first))

    def test_stalled_stream_is_removed_without_stopping_partner(self):
        first = self.start()
        second = self.connect(2)
        self.ack(second)
        deadline = self.c.nodes[1].last_sample + STREAM_TIMEOUT_US
        self.c.nodes[2].last_sample = deadline
        self.c.tick(deadline)
        self.assertNotIn(1, self.c.nodes)
        self.assertTrue(self.c.run_active)

    def test_manual_stop_persists_across_join_and_old_ack(self):
        first = self.start()
        self.c.manual_stop()
        second = self.connect(2)
        self.assertEqual(self.c.nodes[2].phase, "connected")
        seq = self.c._pending_stop_acks[1]
        self.c.control_acknowledged(first, self.api.MESSAGE_TYPE_STOP, seq, 0, 123)
        self.assertEqual(self.c.nodes[1].phase, "connected")
        self.c.control_acknowledged(first, self.api.MESSAGE_TYPE_ARM, 0, 0, 123)
        self.assertFalse(self.c.run_active)
        self.assertFalse(self.c.arm_pending)

    def test_idle_rearms_each_node_without_waiting_for_partner_ack(self):
        first = self.start()
        second = self.connect(2)
        self.ack(second)
        self.assertTrue(self.c.idle_pause())
        seq = self.c._pending_stop_acks[1]
        self.c.control_acknowledged(first, self.api.MESSAGE_TYPE_STOP, seq, 0, 123)
        self.assertEqual(self.c.nodes[1].phase, "arming")
        self.ack(first)
        self.assertTrue(self.trigger(first))
        self.ack(first)
        self.assertTrue(self.c.run_active)


class AIDynamicCaptureTests(DynamicCaptureTests):
    api = ai_server


class DynamicWindowTests(unittest.TestCase):
    def setUp(self):
        self.a = WindowAssembler(WindowConfig(4, 1_000_000, 1_000_000,
            node_wait_us=250_000))
        self.a.start_run(11, 0)

    def feed(self, node, ticks):
        windows = []
        for tick in ticks:
            windows.extend(self.a.submit(sample(node, tick, tick * 250_000)))
        return windows

    def test_missing_node_is_absent_not_zero_filled(self):
        window = self.feed(2, range(5))[0]
        self.assertTrue(window.quality.accepted)
        self.assertEqual(window.quality.available_nodes, (2,))
        self.assertEqual(set(window.samples_by_node), {2})
        self.assertEqual(window.quality.sample_counts[1], 0)

    def test_starting_partner_without_samples_gets_bounded_grace(self):
        self.a.node_joined(2)
        self.assertEqual(self.feed(1, range(5)), [])
        window = self.feed(1, [5])[0]
        self.assertEqual(window.quality.available_nodes, (1,))

    def test_join_then_fusion_without_window_reset(self):
        first = self.feed(1, range(5))[0]
        self.feed(2, range(4, 8))
        self.feed(1, range(5, 9))
        second = self.feed(2, [8])[0]
        self.assertEqual(second.quality.available_nodes, (1, 2))
        self.assertEqual((first.run_id, second.run_id, second.window_id), (11, 11, 1))

    def test_stalled_partner_only_waits_bounded_time(self):
        self.feed(2, [0])
        self.assertEqual(self.feed(1, range(5)), [])
        window = self.feed(1, [5])[0]
        self.assertEqual(window.quality.available_nodes, (1,))

    def test_bad_partner_does_not_reject_good_node(self):
        self.feed(2, [0, 3])
        self.feed(1, range(5))
        window = self.feed(2, [4])[0]
        self.assertTrue(window.quality.accepted)
        self.assertEqual(window.quality.available_nodes, (1,))
        self.assertIn(2, window.quality.node_rejection_reasons)

    def test_neither_usable_rejects_window(self):
        self.feed(2, [0])
        window = self.feed(1, [0, 5])[0]
        self.assertFalse(window.quality.accepted)

    def test_node_loss_preserves_window_number(self):
        self.feed(2, [0])
        self.feed(1, range(6))
        self.a.node_lost(2)
        window = self.feed(1, [6, 7, 8])[0]
        self.assertEqual(window.window_id, 1)
        self.assertEqual(window.quality.available_nodes, (1,))


if __name__ == "__main__":
    unittest.main()
