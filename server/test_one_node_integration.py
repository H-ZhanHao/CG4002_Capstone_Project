"""Active server integration: dynamic capture, real-only windows and dashboard.

The user deploys dynamic_capture.py separately. Tests use the communications
copy on the import path without copying it into server/.
"""
from pathlib import Path
import sys
import struct
import time
import unittest
from contextlib import ExitStack
import logging
from types import SimpleNamespace
from unittest.mock import Mock, MagicMock, patch

sys.path.append(str(Path(__file__).resolve().parents[1] / "communications" / "ultra96"))

import server_rep_fall as server
from window_assembler_updated import DecodedImuSample, WindowAssembler, WindowConfig


class Socket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, how):
        pass


class ActiveServerTests(unittest.TestCase):
    def setUp(self):
        self.assembler = WindowAssembler(WindowConfig(4, 1_000_000, 1_000_000))
        self.c = server.CaptureCoordinator(window_assembler=self.assembler)
        self.addCleanup(self.c.feedback_delivery.close)

    def connect(self, node, session=None, ready=True):
        conn = server.NodeControlConnection(Socket(), node, session or node * 101,
                                            self.c, boot_id=node)
        self.c.register(conn)
        if ready:
            conn.mapping = server.ClockMapping(node * 1000, 20, time.monotonic_ns() // 1000)
            self.c.mapping_updated(conn)
        return conn

    def ack(self, conn):
        state = self.c.nodes[conn.node_id]
        kind = server.MESSAGE_TYPE_ARM if state.phase == "arming" else server.MESSAGE_TYPE_START_FLUSH
        self.c.control_acknowledged(conn, kind, state.sequence, 0, 12345)

    def start(self, node):
        conn = self.connect(node)
        self.ack(conn)
        state = self.c.nodes[node]
        header = server.unpack_header(server.build_header(
            server.MESSAGE_TYPE_ACTIVITY_TRIGGER, node, 16, conn.session_id, sequence=10))
        self.assertTrue(self.c.activity_triggered(conn, header, (
            conn.boot_id, state.generation,
            state.boundary + conn.mapping.node_minus_coordinator_us + 1)))
        self.ack(conn)
        return conn

    def feed(self, conn, ticks):
        output = []
        origin = self.c._run_origin
        for tick in ticks:
            mapped = origin + tick * 250_000
            local = mapped + conn.mapping.node_minus_coordinator_us
            sample = DecodedImuSample(conn.node_id, conn.session_id, tick,
                local, mapped, 1, 2, 3, 4, 5, 6, 0, 0)
            output.extend(self.c.submit_sample(conn, sample))
        return output

    def feedback_ack(self, conn, sequence, result=0):
        payload = struct.pack("!BBHIQ", server.MESSAGE_TYPE_FEEDBACK_COMMAND,
                              result, 0, sequence, 12345)
        frame = server.build_header(server.MESSAGE_TYPE_CONTROL_ACK, conn.node_id,
                                    len(payload), conn.session_id, sequence=sequence) + payload
        server.process_control_ack(frame, conn)

    def test_activity_led_ack_after_peak_keeps_capture_running(self):
        first = self.start(1)
        manager = self.c.feedback_delivery
        delivery = next(item for item in manager.pending.values()
                        if item.connection is first and item.action == server.FEEDBACK_ACTIVITY_ACTIVE)
        self.feedback_ack(first, delivery.sequence)
        self.assertEqual(delivery.status, "applied")
        self.assertTrue(self.c.run_active)
        self.assertIs(self.c._connections[1], first)
        # Firmware duplicates and delayed ACK retries cannot reset capture.
        self.feedback_ack(first, delivery.sequence, result=3)
        self.assertTrue(self.c.run_active)
        self.assertTrue(self.feed(first, range(5))[0].quality.accepted)

    def test_rep_and_partner_feedback_acks_keep_both_streams_running(self):
        first = self.start(1)
        second = self.connect(2)
        self.ack(second)
        manager = self.c.feedback_delivery
        for item in list(manager.pending.values()):
            self.feedback_ack(item.connection, item.sequence)
        self.c.request_feedback(server.FEEDBACK_REP_BUZZ)
        rep = next(item for item in manager.pending.values()
                   if item.action == server.FEEDBACK_REP_BUZZ)
        self.assertIs(rep.connection, first)
        self.feedback_ack(first, rep.sequence)
        self.assertEqual(rep.status, "applied")
        self.assertEqual(self.c.dashboard_snapshot()["streaming_nodes"], [1, 2])

    def test_invalid_feedback_ack_is_protocol_error(self):
        first = self.start(1)
        delivery = next(iter(self.c.feedback_delivery.pending.values()))
        with self.assertRaises(server.ProtocolError):
            self.feedback_ack(first, delivery.sequence, result=255)

    def test_either_node_starts_and_dashboard_tracks_it(self):
        for node in (1, 2):
            with self.subTest(node=node):
                conn = self.start(node)
                window = self.feed(conn, range(5))[0]
                self.assertTrue(window.quality.accepted)
                self.assertEqual(window.quality.available_nodes, (node,))
                self.assertEqual(set(window.samples_by_node), {node})
                snapshot = self.c.dashboard_snapshot()
                self.assertEqual(snapshot["capture"], "streaming")
                self.assertEqual(snapshot["streaming_nodes"], [node])
                self.assertEqual(snapshot["connections"][str(node)]["phase"], "streaming")
                self.c.unregister(conn)

    def test_late_node_joins_same_run_then_disconnect_falls_back(self):
        first = self.start(1)
        first_window = self.feed(first, range(5))[0]
        second = self.connect(2)
        flush = [f for f in second.client.frames
                 if server.unpack_header(f)[2] == server.MESSAGE_TYPE_START_FLUSH][-1]
        self.assertEqual(struct.unpack("!QqIB3xI", flush[24:])[2], 0)
        self.ack(second)
        self.feed(second, range(4, 8))
        self.feed(first, range(5, 9))
        fused = self.feed(second, [8])[0]
        self.assertEqual(fused.quality.available_nodes, (1, 2))
        self.assertEqual(fused.run_id, first_window.run_id)
        self.assertEqual(fused.window_id, 1)
        self.c.unregister(second)
        fallback = self.feed(first, range(9, 13))[0]
        self.assertEqual(fallback.quality.available_nodes, (1,))
        self.assertEqual(fallback.window_id, 2)
        replacement = self.connect(2, session=999)
        self.ack(replacement)
        self.assertEqual(self.c.submit_sample(second, DecodedImuSample(
            2, second.session_id, 99, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0)), [])
        self.assertEqual(self.c.dashboard_snapshot()["streaming_nodes"], [1, 2])

    def test_unsynchronized_partner_does_not_hold_up_start(self):
        self.connect(2, ready=False)
        first = self.start(1)
        self.assertTrue(self.feed(first, range(5))[0].quality.accepted)
        self.assertEqual(self.c.nodes[2].phase, "connected")

    def test_silent_partner_has_bounded_window_wait(self):
        first = self.start(1)
        second = self.connect(2)
        self.ack(second)
        self.assertEqual(self.feed(first, range(5)), [])
        result = self.feed(first, [5])[0]
        self.assertEqual(result.quality.available_nodes, (1,))

    def test_stop_survives_new_connection_and_idle_rearms(self):
        first = self.start(1)
        self.c.manual_stop()
        seq = self.c._pending_stop_acks[1]
        self.c.control_acknowledged(first, server.MESSAGE_TYPE_STOP, seq, 0, 100)
        self.connect(2)
        self.assertFalse(self.c.arm_pending)
        self.assertFalse(self.c.desired_running)
        self.c.request_start()
        self.ack(first)
        # Acknowledging the first ARM must not wait for the partner's ARM ACK.
        self.assertEqual(self.c.nodes[1].phase, "armed")
        self.assertTrue(self.c.idle_pause())

    def test_cli_preserves_dashboard_and_model_validation(self):
        args = server.parse_args(["--node-wait-ms", "80", "--web-port", "8081"])
        self.assertEqual(args.node_wait_ms, 80)
        self.assertEqual(args.web_port, 8081)
        self.assertFalse(args.no_dashboard)
        self.assertTrue(args.wrist_xmodel.endswith("wrist.xmodel"))
        with patch("sys.stderr"), self.assertRaises(SystemExit):
            server.parse_args(["--node-wait-ms", "-1"])
        with patch("sys.stderr"), self.assertRaises(SystemExit):
            server.parse_args(["--one-node"])

    def test_startup_keeps_teammate_pipeline_dashboard_and_rep_control(self):
        listener = Mock()
        listener.accept.side_effect = KeyboardInterrupt
        sockets = MagicMock()
        sockets.return_value.__enter__.return_value = listener
        publisher = Mock(run_id=10)
        telemetry = Mock()
        pipeline = Mock()
        pipeline_type = Mock(return_value=pipeline)
        rep = Mock(port=9101)
        dashboard = Mock(port=8080)
        with ExitStack() as stack:
            stack.enter_context(patch.dict(sys.modules, {
                "inference_pipeline": SimpleNamespace(InferencePipeline=pipeline_type),
                "dpu_runtime": SimpleNamespace(initialize_dpu_overlay=Mock()),
                "late_fusion_vart": SimpleNamespace(LateFusionInference=Mock()),
            }))
            stack.enter_context(patch.object(server, "configure_logging"))
            stack.enter_context(patch.object(server, "check_model_files"))
            stack.enter_context(patch.object(server, "socket", SimpleNamespace(
                socket=sockets, AF_INET=2, SOCK_STREAM=1, SOL_SOCKET=1, SO_REUSEADDR=2)))
            stack.enter_context(patch.object(server, "VisualizerEventPublisher", return_value=publisher))
            stack.enter_context(patch.object(server, "Telemetry", return_value=telemetry))
            stack.enter_context(patch.object(server, "DashboardLogHandler", return_value=logging.NullHandler()))
            dashboard_type = stack.enter_context(patch.object(server, "DashboardServer", return_value=dashboard))
            stack.enter_context(patch.object(server, "RepControlServer", return_value=rep))
            stack.enter_context(patch.object(server, "run_control_console"))
            with self.assertRaises(KeyboardInterrupt):
                server.main(["--startup-trace-seconds", "0", "--node-wait-ms", "80"])
        args, kwargs = pipeline_type.call_args
        self.assertEqual(args[0].config.node_wait_us, 80_000)
        self.assertIs(kwargs["telemetry"], telemetry)
        self.assertIs(dashboard_type.call_args.args[0], telemetry)
        coordinator = dashboard_type.call_args.kwargs["state_provider"].__self__
        self.addCleanup(coordinator.feedback_delivery.close)
        self.assertTrue(hasattr(coordinator, "nodes"))
        self.assertEqual(pipeline.on_idle, coordinator.handle_idle_classification)
        rep.start.assert_called_once()
        dashboard.start.assert_called_once()
        rep.close.assert_called_once()
        dashboard.close.assert_called_once()
        pipeline.close.assert_called_once()


class RealWindowPolicyTests(unittest.TestCase):
    def make(self, rate=4):
        assembler = WindowAssembler(WindowConfig(rate, 1_000_000, 1_000_000))
        assembler.start_run(1, 0)
        return assembler

    def sample(self, node, seq, timestamp):
        return DecodedImuSample(node, node * 10, seq, timestamp, timestamp,
                                1, 2, 3, 4, 5, 6, 0, 0)

    def test_real_deficit_is_not_zero_padded(self):
        a = self.make(100)
        for tick in range(98):
            self.assertEqual(a.submit(self.sample(1, tick, tick * 10000)), [])
        window = a.submit(self.sample(1, 98, 1_000_000))[0]
        self.assertTrue(window.quality.accepted)
        self.assertEqual(len(window.samples_by_node[1]), 98)
        self.assertEqual(window.quality.padded_counts[1], 0)
        self.assertTrue(all(not s.is_padding for s in window.samples_by_node[1]))

    def test_extra_real_samples_are_not_trimmed(self):
        a = self.make()
        samples = [self.sample(1, i, i * 200000) for i in range(5)]
        for s in samples:
            a.submit(s)
        window = a.submit(self.sample(1, 5, 1_000_000))[0]
        self.assertEqual(window.samples_by_node[1], tuple(samples))
        self.assertTrue(window.quality.accepted)

    def test_bad_node_coverage_is_omitted_not_fabricated(self):
        a = self.make()
        a.submit(self.sample(2, 0, 0))
        for tick in range(4):
            a.submit(self.sample(1, tick, tick * 250000))
        a.submit(self.sample(1, 4, 1_000_000))
        window = a.submit(self.sample(2, 4, 1_000_000))[0]
        self.assertEqual(window.quality.available_nodes, (1,))
        self.assertEqual(window.quality.node_rejection_reasons[2], "missing_samples")
        self.assertNotIn(2, window.samples_by_node)

    def test_no_usable_data_is_rejected(self):
        a = self.make()
        a.submit(self.sample(1, 0, 0))
        window = a.submit(self.sample(1, 1, 1_000_000))[0]
        self.assertFalse(window.quality.accepted)


if __name__ == "__main__":
    unittest.main()
