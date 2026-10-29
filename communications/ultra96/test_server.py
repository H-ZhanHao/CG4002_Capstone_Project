"""Integration and unit coverage for Ultra96 hub protocol and demo services."""

import socket
import hashlib
import logging
import os
import struct
import tempfile
import threading
import time
import unittest
import zlib

import server
from visualizer_protocol import validate_message


def build_hello(hub_node_id, assigned_node_mask, boot_id):
    payload = struct.pack(
        server.HELLO_PAYLOAD_FORMAT,
        hub_node_id,
        1,
        0,
        0,
        boot_id,
        server.SAMPLE_RATE_100_HZ,
        assigned_node_mask,
        0,
        server.DEVELOPMENT_DEVICE_TOKEN,
    )
    return server.build_header(
        server.MESSAGE_TYPE_HELLO,
        node_id=hub_node_id,
        payload_length=len(payload),
        session_id=0,
    ) + payload


def receive_exact(sock, byte_count):
    received = bytearray()
    while len(received) < byte_count:
        chunk = sock.recv(byte_count - len(received))
        if not chunk:
            raise ConnectionError("Connection closed before the frame arrived")
        received.extend(chunk)
    return bytes(received)


def build_control_ack(connection, command_type, command_sequence,
                      result=server.CONTROL_RESULT_APPLIED,
                      effective_local_us=123456):
    """Build the ESP acknowledgement for one START or STOP command."""
    payload = struct.pack(
        "!BBHIQ", command_type, result, 0, command_sequence,
        effective_local_us)
    return server.build_header(
        server.MESSAGE_TYPE_CONTROL_ACK,
        node_id=connection.node_id,
        payload_length=len(payload),
        session_id=connection.session_id,
        sequence=command_sequence,
        timestamp_us=effective_local_us,
    ) + payload


class RecordingSocket:
    """Minimal socket double that records coordinator writes."""

    def __init__(self):
        self.frames = []
        self.shutdown_called = False

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, _how):
        self.shutdown_called = True


class RecordingAssembler:
    def __init__(self):
        self.started = []
        self.stopped = []

    def start_run(self, run_id, start_us):
        self.started.append((run_id, start_us))

    def stop_run(self, reason):
        self.stopped.append(reason)


class SlidingWindowIntegrationTest(unittest.TestCase):
    def build_data(self, status=server.STATUS_MOCK_DATA):
        payload = struct.pack(
            server.DATA_PAYLOAD_FORMAT,
            1, 2, 3, 4, 5, 6, status)
        return server.build_header(
            server.MESSAGE_TYPE_DATA, node_id=1,
            payload_length=len(payload), session_id=123,
            sequence=7, timestamp_us=50_000) + payload

    def test_window_argument_defaults_and_overrides(self):
        defaults = server.parse_args([])
        self.assertEqual(
            (defaults.sample_rate_hz, defaults.window_seconds,
             defaults.hop_seconds),
            (100, 5.0, 1.0))
        explicit = server.parse_args([
            "--sample-rate-hz", "100", "--window-seconds", "4.5",
            "--hop-seconds", "0.5"])
        self.assertEqual(
            (explicit.sample_rate_hz, explicit.window_seconds,
             explicit.hop_seconds),
            (100, 4.5, 0.5))

    def test_hop_greater_than_window_is_rejected(self):
        with self.assertRaises(SystemExit):
            server.parse_args(["--window-seconds", "1", "--hop-seconds", "2"])

    def test_mapped_data_returns_neutral_sample(self):
        mapping = server.ClockMapping(1_000, 10, 0)
        decoded = server.process_data(
            self.build_data(), {}, 123, 1 << 1, mapping)
        self.assertIsInstance(decoded, server.DecodedImuSample)
        self.assertEqual(decoded.timestamp_us, 50_000)
        self.assertEqual(decoded.coordinator_timestamp_us, 49_000)
        self.assertEqual((decoded.ax, decoded.gz), (1, 6))

    def test_unmapped_and_benchmark_data_do_not_enter_windows(self):
        self.assertIsNone(server.process_data(
            self.build_data(), {}, 123, 1 << 1, None))
        mapping = server.ClockMapping(1_000, 10, 0)
        self.assertIsNone(server.process_data(
            self.build_data(server.STATUS_BENCHMARK), {}, 123, 1 << 1,
            mapping))

    def test_sample_interval_uses_configured_rate_with_ten_percent_tolerance(self):
        payload = struct.pack(
            server.DATA_PAYLOAD_FORMAT, 1, 2, 3, 4, 5, 6, 0)

        def frame(sequence, timestamp_us):
            return server.build_header(
                server.MESSAGE_TYPE_DATA, node_id=1,
                payload_length=len(payload), session_id=123,
                sequence=sequence, timestamp_us=timestamp_us) + payload

        stream_state = {}
        server.process_data(frame(1, 10_000), stream_state, 123, 1 << 1,
                            sample_rate_hz=100)
        with self.assertNoLogs(server.LOGGER, level="WARNING"):
            server.process_data(frame(2, 19_200), stream_state, 123, 1 << 1,
                                sample_rate_hz=100)
        with self.assertLogs(server.LOGGER, level="WARNING") as captured:
            server.process_data(frame(3, 26_700), stream_state, 123, 1 << 1,
                                sample_rate_hz=100)
        self.assertIn("expected_us=10000", captured.output[0])

    def test_final_start_ack_uses_command_sequence_and_scheduled_epoch(self):
        assembler = RecordingAssembler()
        coordinator = server.CaptureCoordinator(window_assembler=assembler)
        socket1, socket2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(socket1, 1, 101, coordinator)
        node2 = server.NodeControlConnection(socket2, 2, 202, coordinator)
        node1.mapping = server.ClockMapping(10, 20, 30)
        node2.mapping = server.ClockMapping(10, 20, 30)
        coordinator.register(node1)
        coordinator.register(node2)
        coordinator.mapping_updated(node2)
        sequence = server.unpack_header(socket1.frames[-1])[7]
        start_us = struct.unpack("!Qq", socket1.frames[-1][server.HEADER_SIZE:])[0]
        server.process_control_ack(
            build_control_ack(node1, server.MESSAGE_TYPE_START, sequence), node1)
        self.assertEqual(assembler.started, [])
        server.process_control_ack(
            build_control_ack(node2, server.MESSAGE_TYPE_START, sequence), node2)
        self.assertEqual(assembler.started, [(sequence, start_us)])

    def test_stop_and_disconnect_reset_assembler(self):
        assembler = RecordingAssembler()
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), window_assembler=assembler)
        sock = RecordingSocket()
        node = server.NodeControlConnection(sock, 1, 101, coordinator)
        node.mapping = server.ClockMapping(10, 20, 30)
        coordinator.register(node)
        coordinator.mapping_updated(node)
        sequence = server.unpack_header(sock.frames[-1])[7]
        server.process_control_ack(
            build_control_ack(node, server.MESSAGE_TYPE_START, sequence), node)
        coordinator.request_stop()
        self.assertIn("manual STOP", assembler.stopped)
        coordinator.request_start()
        sequence = server.unpack_header(sock.frames[-1])[7]
        server.process_control_ack(
            build_control_ack(node, server.MESSAGE_TYPE_START, sequence), node)
        coordinator.unregister(node)
        self.assertIn("Node 1 disconnected", assembler.stopped)


class PhysicalIntegrationTest(unittest.TestCase):
    def confirm_arm(self, coordinator, *connections):
        if not coordinator.arm_pending:
            self.assertTrue(coordinator._arm_locked("test"))
        sequence = server.unpack_header(connections[0].client.frames[-1])[7]
        self.assertEqual(
            server.unpack_header(connections[0].client.frames[-1])[2],
            server.MESSAGE_TYPE_ARM)
        decoded = [
            struct.unpack("!QqI", connection.client.frames[-1][server.HEADER_SIZE:])
            for connection in connections
        ]
        self.assertTrue(all(item[0] == decoded[0][0] for item in decoded))
        self.assertEqual(
            [item[1] for item in decoded],
            [connection.mapping.node_minus_coordinator_us
             for connection in connections])
        self.assertTrue(all(item[2] == coordinator._arm_generation
                            for item in decoded))
        for connection in connections:
            server.process_control_ack(build_control_ack(
                connection, server.MESSAGE_TYPE_ARM, sequence), connection)
        self.assertTrue(coordinator._arm_ready)
        return coordinator._arm_generation, coordinator._armed_after_us

    def build_physical(self, node_id=1, session_id=101, sequence=7,
                       timestamp_us=7_000_000, status=None, values=None):
        if status is None:
            status = server.STATUS_SENSOR_VALID | server.STATUS_HISTORICAL
        if values is None:
            values = (1.0, -2.0, 0.5, -0.25, 3.0, 0.0)
        payload = struct.pack("!ffffffH", *values, status)
        return server.build_header(
            server.MESSAGE_TYPE_PHYSICAL_DATA, node_id=node_id,
            payload_length=len(payload), session_id=session_id,
            sequence=sequence, timestamp_us=timestamp_us) + payload

    def build_trigger(self, connection, sequence=9, peak_us=7_000_000,
                      arm_generation=1, detected_us=7_060_000):
        payload = struct.pack(
            "!IIQ", connection.boot_id, arm_generation, peak_us)
        return server.build_header(
            server.MESSAGE_TYPE_ACTIVITY_TRIGGER,
            node_id=connection.node_id, payload_length=len(payload),
            session_id=connection.session_id, sequence=sequence,
            timestamp_us=detected_us) + payload

    def test_physical_data_matches_c_golden_and_rejects_invalid_values(self):
        frame = self.build_physical(
            sequence=0x01020304,
            timestamp_us=0x0102030405060708,
            status=server.STATUS_SENSOR_VALID | server.STATUS_HISTORICAL)
        self.assertEqual(frame.hex(),
            "a55a01120100001a00000065010203040102030405060708"
            "3f800000c00000003f000000be80000040400000000000000003")
        mapping = server.ClockMapping(100, 20, 1)
        decoded = server.process_physical_data(
            frame, {}, 101, 1 << 1, mapping, 50)
        self.assertEqual(decoded.coordinator_timestamp_us,
                         0x0102030405060708 - 100)
        nan_frame = self.build_physical(values=(float("nan"), 0, 0, 0, 0, 0))
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(
                nan_frame, {}, 101, 1 << 1, mapping, 50)
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(
                self.build_physical(session_id=999), {}, 101, 1 << 1,
                mapping, 50)

    def test_physical_replay_duplicate_and_timestamp_regression_rejected(self):
        mapping = server.ClockMapping(0, 20, 1)
        state = {}
        first = self.build_physical(sequence=10, timestamp_us=1_000_000)
        server.process_physical_data(first, state, 101, 1 << 1, mapping, 50)
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(first, state, 101, 1 << 1, mapping, 50)
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(
                self.build_physical(sequence=11, timestamp_us=999_999),
                state, 101, 1 << 1, mapping, 50)

    def test_physical_history_overlap_is_allowed_in_new_capture_generation(self):
        mapping = server.ClockMapping(0, 20, 1)
        state = {}
        newest = self.build_physical(sequence=250, timestamp_us=5_000_000)
        overlapping = self.build_physical(sequence=50, timestamp_us=1_000_000)

        server.process_physical_data(
            newest, state, 101, 1 << 1, mapping, 50,
            capture_generation=1)
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(
                overlapping, state, 101, 1 << 1, mapping, 50,
                capture_generation=1)

        decoded = server.process_physical_data(
            overlapping, state, 101, 1 << 1, mapping, 50,
            capture_generation=2)
        self.assertEqual(decoded.sequence, 50)
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(
                overlapping, state, 101, 1 << 1, mapping, 50,
                capture_generation=2)

    def test_two_node_trigger_maps_one_t0_and_duplicate_is_idempotent(self):
        assembler = RecordingAssembler()
        coordinator = server.CaptureCoordinator(
            window_assembler=assembler, auto_start=True, physical_mode=True)
        sock1, sock2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(
            sock1, 1, 101, coordinator, boot_id=0x11111111)
        node2 = server.NodeControlConnection(
            sock2, 2, 202, coordinator, boot_id=0x22222222)
        now_us = time.monotonic_ns() // 1000
        node1.mapping = server.ClockMapping(100, 20, now_us)
        node2.mapping = server.ClockMapping(-200, 20, now_us)
        coordinator.register(node1)
        coordinator.register(node2)
        arm_generation, arm_at_us = self.confirm_arm(
            coordinator, node1, node2)
        trigger = self.build_trigger(
            node1, peak_us=arm_at_us + node1.mapping.node_minus_coordinator_us + 100,
            arm_generation=arm_generation, detected_us=arm_at_us + 60_100)
        self.assertTrue(server.process_activity_trigger(trigger, node1))
        self.assertEqual(node1.get_physical_capture_generation(), 1)
        self.assertEqual(node2.get_physical_capture_generation(), 1)
        self.assertEqual(server.unpack_header(sock1.frames[-1])[2],
                         server.MESSAGE_TYPE_START_FLUSH)
        left = struct.unpack("!QqIB3xI", sock1.frames[-1][server.HEADER_SIZE:])
        right = struct.unpack("!QqIB3xI", sock2.frames[-1][server.HEADER_SIZE:])
        self.assertEqual(left[0], right[0])
        self.assertEqual((left[1], right[1]), (100, -200))
        frame_counts = (len(sock1.frames), len(sock2.frames))
        self.assertFalse(server.process_activity_trigger(trigger, node1))
        self.assertEqual((len(sock1.frames), len(sock2.frames)), frame_counts)
        command_sequence = server.unpack_header(sock1.frames[-1])[7]
        server.process_control_ack(build_control_ack(
            node1, server.MESSAGE_TYPE_START_FLUSH, command_sequence), node1)
        server.process_control_ack(build_control_ack(
            node2, server.MESSAGE_TYPE_START_FLUSH, command_sequence), node2)
        self.assertEqual(
            assembler.started,
            [(command_sequence, left[0] - server.PHYSICAL_HISTORY_DURATION_US)])

    def test_both_physical_nodes_synced_is_announced_once_per_session_pair(self):
        coordinator = server.CaptureCoordinator(physical_mode=True)
        node1 = server.NodeControlConnection(
            RecordingSocket(), 1, 101, coordinator)
        node2 = server.NodeControlConnection(
            RecordingSocket(), 2, 202, coordinator)
        coordinator.register(node1)
        coordinator.register(node2)

        now_us = time.monotonic_ns() // 1000
        node1.mapping = server.ClockMapping(10, 20, now_us)
        with self.assertNoLogs(server.LOGGER, level="INFO"):
            coordinator.mapping_updated(node1)
        node2.mapping = server.ClockMapping(20, 20, now_us)
        with self.assertLogs(server.LOGGER, level="INFO") as captured:
            coordinator.mapping_updated(node2)
        output = "\n".join(captured.output)
        self.assertIn("BOTH_NODES_SYNCED", output)
        self.assertIn("arming_activity_detection", output)
        self.assertTrue(coordinator.arm_pending)

        with self.assertNoLogs(server.LOGGER, level="INFO"):
            coordinator.mapping_updated(node1)

    def test_arm_ack_timeout_stops_and_requires_manual_start(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        node.mapping = server.ClockMapping(
            0, 20, time.monotonic_ns() // 1000)
        coordinator.register(node)

        self.assertTrue(coordinator._arm_locked("test timeout"))
        deadline_us = coordinator._arm_ack_deadline_us
        self.assertFalse(coordinator.tick(deadline_us - 1))
        with self.assertLogs(server.LOGGER, level="ERROR") as output:
            self.assertTrue(coordinator.tick(deadline_us))
        self.assertIn("ARM_ACK_TIMEOUT", "\n".join(output.output))
        self.assertFalse(coordinator.desired_running)
        self.assertFalse(coordinator.arm_pending)
        self.assertEqual(server.unpack_header(sock.frames[-1])[2],
                         server.MESSAGE_TYPE_STOP)

    def test_manual_start_with_expired_mapping_requests_sync_then_arms(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False, physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        now_us = time.monotonic_ns() // 1000
        node.mapping = server.ClockMapping(
            0, 20,
            now_us - server.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US - 1)
        coordinator.register(node)

        with self.assertLogs(server.LOGGER, level="INFO") as output:
            self.assertFalse(coordinator.request_start())
        rendered = "\n".join(output.output)
        self.assertIn("ARM_DEFERRED reason=missing_or_expired_mapping", rendered)
        self.assertIn("nodes=[1]", rendered)
        self.assertTrue(node.arm_sync_requested())

        node.tick(now_us + 1)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_SYNC_REQ)

        refreshed_at_us = time.monotonic_ns() // 1000
        node.mapping = server.ClockMapping(10, 20, refreshed_at_us)
        coordinator.mapping_updated(node)
        self.assertFalse(node.arm_sync_requested())
        self.assertTrue(coordinator.arm_pending)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2], server.MESSAGE_TYPE_ARM)

    def test_pending_arm_sync_retries_quickly_after_rejection(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False, physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(sock, 1, 101, coordinator)
        now_us = time.monotonic_ns() // 1000
        node.mapping = server.ClockMapping(
            0, 20,
            now_us - server.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US - 1)
        coordinator.register(node)
        coordinator.request_start()

        with self.assertLogs(server.LOGGER, level="INFO") as output:
            node._pause_failed_initial_sync(now_us)
        self.assertIn("delay_seconds=1.0", "\n".join(output.output))
        frame_count = len(sock.frames)
        node.tick(now_us + 999_999)
        self.assertEqual(len(sock.frames), frame_count)
        node.tick(now_us + 1_000_000)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_SYNC_REQ)

    def test_duplicate_manual_start_reports_already_armed(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        node.mapping = server.ClockMapping(
            0, 20, time.monotonic_ns() // 1000)
        coordinator.register(node)
        self.confirm_arm(coordinator, node)
        frame_count = len(sock.frames)

        with self.assertLogs(server.LOGGER, level="INFO") as output:
            self.assertFalse(coordinator.request_start())
        self.assertIn("ARM_ALREADY_READY", "\n".join(output.output))
        self.assertEqual(len(sock.frames), frame_count)

    def test_manual_start_arms_with_holdover_and_refreshes_in_background(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False, physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        now_us = time.monotonic_ns() // 1000
        node.mapping = server.ClockMapping(
            10, 20, now_us - server.CLOCK_MAPPING_MAX_AGE_US - 1)
        coordinator.register(node)

        with self.assertLogs(server.LOGGER, level="INFO") as output:
            self.assertTrue(coordinator.request_start())
        self.assertIn("ARM_USING_MAPPING_HOLDOVER", "\n".join(output.output))
        self.assertTrue(node.arm_sync_requested())
        self.assertTrue(coordinator.arm_pending)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2], server.MESSAGE_TYPE_ARM)

        arm_generation, arm_at_us = self.confirm_arm(coordinator, node)
        peak_us = arm_at_us + node.mapping.node_minus_coordinator_us + 100
        trigger = self.build_trigger(
            node, peak_us=peak_us, arm_generation=arm_generation,
            detected_us=peak_us + 60_000)
        self.assertTrue(server.process_activity_trigger(trigger, node))
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_START_FLUSH)

    def test_trigger_after_holdover_expiry_requests_sync_and_rearm(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False, physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        node.mapping = server.ClockMapping(
            10, 20, time.monotonic_ns() // 1000)
        coordinator.register(node)
        self.assertTrue(coordinator.request_start())
        arm_generation, arm_at_us = self.confirm_arm(coordinator, node)

        node.mapping = server.ClockMapping(
            10, 20,
            time.monotonic_ns() // 1000 -
            server.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US - 1)
        peak_us = arm_at_us + node.mapping.node_minus_coordinator_us + 100
        trigger = self.build_trigger(
            node, peak_us=peak_us, arm_generation=arm_generation,
            detected_us=peak_us + 60_000)
        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            self.assertFalse(server.process_activity_trigger(trigger, node))
        self.assertIn("mapping_holdover_expired", "\n".join(output.output))
        self.assertFalse(coordinator._arm_ready)
        self.assertTrue(node.arm_sync_requested())

    def test_idle_stop_ack_with_stale_mapping_requests_sync(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        now_us = time.monotonic_ns() // 1000
        node.mapping = server.ClockMapping(
            0, 20,
            now_us - server.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US - 1)
        coordinator.register(node)

        self.assertTrue(coordinator.idle_pause())
        stop_sequence = server.unpack_header(sock.frames[-1])[7]
        with self.assertLogs(server.LOGGER, level="INFO") as output:
            server.process_control_ack(build_control_ack(
                node, server.MESSAGE_TYPE_STOP, stop_sequence), node)
        self.assertIn(
            "ARM_DEFERRED reason=missing_or_expired_mapping",
            "\n".join(output.output))
        self.assertTrue(node.arm_sync_requested())
        self.assertFalse(coordinator.arm_pending)

    def test_physical_stop_disarms_triggers_until_manual_start(self):
        coordinator = server.CaptureCoordinator(
            auto_start=True, physical_mode=True)
        sock1, sock2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(
            sock1, 1, 101, coordinator, boot_id=0x11111111)
        node2 = server.NodeControlConnection(
            sock2, 2, 202, coordinator, boot_id=0x22222222)
        now_us = time.monotonic_ns() // 1000
        node1.mapping = server.ClockMapping(100, 20, now_us)
        node2.mapping = server.ClockMapping(-200, 20, now_us)
        coordinator.register(node1)
        coordinator.register(node2)

        coordinator.manual_stop()
        stop_sequence = server.unpack_header(sock1.frames[-1])[7]
        server.process_control_ack(build_control_ack(
            node1, server.MESSAGE_TYPE_STOP, stop_sequence), node1)
        server.process_control_ack(build_control_ack(
            node2, server.MESSAGE_TYPE_STOP, stop_sequence), node2)

        old_peak_us = time.monotonic_ns() // 1000 + 100
        trigger = self.build_trigger(
            node1, peak_us=old_peak_us, detected_us=old_peak_us + 60_000)
        frame_counts = (len(sock1.frames), len(sock2.frames))
        self.assertFalse(server.process_activity_trigger(trigger, node1))
        self.assertEqual((len(sock1.frames), len(sock2.frames)), frame_counts)

        coordinator.request_start()
        self.assertFalse(server.process_activity_trigger(trigger, node1))
        arm_generation, arm_at_us = self.confirm_arm(
            coordinator, node1, node2)
        fresh_peak_us = arm_at_us + 100 + 1
        fresh_trigger = self.build_trigger(
            node1, sequence=10, peak_us=fresh_peak_us,
            arm_generation=arm_generation, detected_us=fresh_peak_us + 60_000)
        self.assertTrue(server.process_activity_trigger(fresh_trigger, node1))
        self.assertEqual(server.unpack_header(sock1.frames[-1])[2],
                         server.MESSAGE_TYPE_START_FLUSH)

    def test_idle_pause_stays_armed_and_resumes_on_fresh_trigger(self):
        assembler = RecordingAssembler()
        coordinator = server.CaptureCoordinator(
            auto_start=True, physical_mode=True, window_assembler=assembler)
        sock1, sock2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(
            sock1, 1, 101, coordinator, boot_id=0x11111111)
        node2 = server.NodeControlConnection(
            sock2, 2, 202, coordinator, boot_id=0x22222222)
        now_us = time.monotonic_ns() // 1000
        node1.mapping = server.ClockMapping(100, 20, now_us)
        node2.mapping = server.ClockMapping(-200, 20, now_us)
        coordinator.register(node1)
        coordinator.register(node2)

        arm_generation, arm_at_us = self.confirm_arm(
            coordinator, node1, node2)

        initial_peak_us = (
            arm_at_us + node1.mapping.node_minus_coordinator_us + 100)
        initial_trigger = self.build_trigger(
            node1, peak_us=initial_peak_us,
            arm_generation=arm_generation,
            detected_us=initial_peak_us + 60_000)
        self.assertTrue(server.process_activity_trigger(initial_trigger, node1))
        start_sequence = server.unpack_header(sock1.frames[-1])[7]
        server.process_control_ack(build_control_ack(
            node1, server.MESSAGE_TYPE_START_FLUSH, start_sequence), node1)
        server.process_control_ack(build_control_ack(
            node2, server.MESSAGE_TYPE_START_FLUSH, start_sequence), node2)
        self.assertTrue(coordinator.run_active)

        self.assertTrue(coordinator.idle_pause())
        self.assertTrue(coordinator.desired_running)
        self.assertTrue(coordinator.idle_paused)
        self.assertFalse(coordinator.run_active)
        self.assertIn("AI idle pause", assembler.stopped)
        stop_sequence = server.unpack_header(sock1.frames[-1])[7]

        # A peak cannot race STOP acknowledgements and restart the old feeder.
        self.assertFalse(server.process_activity_trigger(initial_trigger, node1))
        server.process_control_ack(build_control_ack(
            node1, server.MESSAGE_TYPE_STOP, stop_sequence), node1)
        server.process_control_ack(build_control_ack(
            node2, server.MESSAGE_TYPE_STOP, stop_sequence), node2)

        arm_generation, arm_at_us = self.confirm_arm(
            coordinator, node1, node2)
        fresh_peak_us = arm_at_us + 100 + 1
        fresh_trigger = self.build_trigger(
            node1, sequence=10, peak_us=fresh_peak_us,
            arm_generation=arm_generation, detected_us=fresh_peak_us + 60_000)

        frame_counts = (len(sock1.frames), len(sock2.frames))
        self.assertFalse(server.process_activity_trigger(initial_trigger, node1))
        self.assertEqual((len(sock1.frames), len(sock2.frames)), frame_counts)
        self.assertFalse(coordinator.idle_pause())
        self.assertEqual((len(sock1.frames), len(sock2.frames)), frame_counts)

        self.assertTrue(server.process_activity_trigger(fresh_trigger, node1))
        self.assertFalse(coordinator.idle_paused)
        self.assertEqual(server.unpack_header(sock1.frames[-1])[2],
                         server.MESSAGE_TYPE_START_FLUSH)

    def test_idle_pause_is_rejected_outside_physical_mode(self):
        coordinator = server.CaptureCoordinator(physical_mode=False)
        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            self.assertFalse(coordinator.idle_pause())
        self.assertIn("physical_mode_is_disabled", "\n".join(output.output))

    def test_manual_stop_overrides_pending_idle_pause_without_new_stop(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        coordinator.register(node)

        self.assertTrue(coordinator.idle_pause())
        stop_sequence = server.unpack_header(sock.frames[-1])[7]
        frame_count = len(sock.frames)
        coordinator.manual_stop()

        self.assertFalse(coordinator.desired_running)
        self.assertFalse(coordinator.idle_paused)
        self.assertEqual(len(sock.frames), frame_count)
        server.process_control_ack(build_control_ack(
            node, server.MESSAGE_TYPE_STOP, stop_sequence), node)
        self.assertEqual(coordinator._pending_stop_acks, {})

    def test_operator_shutdown_does_not_rearm_after_stop_ack(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        node = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        node.mapping = server.ClockMapping(
            0, 20, time.monotonic_ns() // 1000)
        coordinator.register(node)

        coordinator.stop_all()
        stop_sequence = server.unpack_header(sock.frames[-1])[7]
        frame_count = len(sock.frames)

        self.assertFalse(coordinator.desired_running)
        self.assertFalse(coordinator.idle_paused)
        server.process_control_ack(build_control_ack(
            node, server.MESSAGE_TYPE_STOP, stop_sequence), node)
        self.assertEqual(len(sock.frames), frame_count)
        self.assertFalse(coordinator.arm_pending)
        self.assertFalse(coordinator._arm_ready)

    def test_five_seconds_history_then_live_builds_one_two_node_window(self):
        config = server.WindowConfig(
            sample_rate_hz=50, window_us=5_000_000, hop_us=1_000_000)
        assembler = server.WindowAssembler(config)
        assembler.start_run(12, 2_000_000)
        mappings = {1: server.ClockMapping(100, 10, 1),
                    2: server.ClockMapping(-200, 10, 1)}
        states = {1: {}, 2: {}}
        completed = []
        for index in range(251):
            coordinator_us = 2_000_000 + index * 20_000
            for node_id, session_id in ((1, 101), (2, 202)):
                local_us = coordinator_us + mappings[node_id].node_minus_coordinator_us
                status = server.STATUS_SENSOR_VALID
                if index < 250:
                    status |= server.STATUS_HISTORICAL
                decoded = server.process_physical_data(
                    self.build_physical(
                        node_id=node_id, session_id=session_id, sequence=index,
                        timestamp_us=local_us, status=status),
                    states[node_id], session_id, 1 << node_id,
                    mappings[node_id], 50)
                completed.extend(assembler.submit(decoded))
        self.assertEqual(len(completed), 1)
        self.assertTrue(completed[0].quality.accepted)
        self.assertEqual(completed[0].quality.sample_counts, {1: 250, 2: 250})


class ClockSynchronizationTest(unittest.TestCase):
    """Verify NTP-style mapping, scheduled START, periodic refresh, and STOP."""

    def complete_sync(self, connection, sock, measurements):
        """Complete five symmetric exchanges from (offset, delay) pairs."""
        result = None
        received_at_us = None
        for offset_us, delay_us in measurements:
            request = sock.frames[-1]
            header = server.unpack_header(request)
            t1 = struct.unpack("!Q", request[server.HEADER_SIZE:])[0]
            outbound_us = delay_us // 2
            inbound_us = delay_us - outbound_us
            t2 = t1 + offset_us + outbound_us
            t3 = t2 + 10
            received_at_us = t1 + outbound_us + inbound_us + 10
            response = server.build_header(
                server.MESSAGE_TYPE_SYNC_RESP,
                node_id=connection.node_id,
                payload_length=server.SYNC_RESP_PAYLOAD_SIZE,
                session_id=connection.session_id,
                sequence=header[7],
                timestamp_us=t3,
            ) + struct.pack("!QQQ", t1, t2, t3)
            result = connection.process_sync_response(
                response, received_at_us)
        return result, received_at_us

    def test_urgent_refresh_completion_automatically_sends_arm(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False, physical_mode=True)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(
            sock, 1, 1234, coordinator, boot_id=0x11111111)
        now_us = time.monotonic_ns() // 1000
        connection.mapping = server.ClockMapping(
            1_000, 20,
            now_us - server.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US - 1)
        connection.last_sync_completed_us = now_us
        coordinator.register(connection)

        self.assertFalse(coordinator.request_start())
        connection.tick(now_us + 1)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_SYNC_REQ)

        self.complete_sync(connection, sock, [(1_000, 100)] * 5)

        self.assertFalse(connection.arm_sync_requested())
        self.assertTrue(coordinator.arm_pending)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2], server.MESSAGE_TYPE_ARM)

    def test_lowest_delay_mapping_drives_scheduled_start(self):
        coordinator = server.CaptureCoordinator(required_nodes=(1,))
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1234, coordinator)
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        for one_way_us in (100, 80, 20, 60, 50):
            request = sock.frames[-1]
            header = server.unpack_header(request)
            t1 = struct.unpack("!Q", request[server.HEADER_SIZE:])[0]
            t2 = t1 + 1000 + one_way_us
            t3 = t2 + 10
            t4 = t1 + 2 * one_way_us + 10
            response = server.build_header(
                server.MESSAGE_TYPE_SYNC_RESP,
                node_id=1,
                payload_length=server.SYNC_RESP_PAYLOAD_SIZE,
                session_id=1234,
                sequence=header[7],
                timestamp_us=t3,
            ) + struct.pack("!QQQ", t1, t2, t3)
            connection.process_sync_response(response, t4)

        self.assertEqual(connection.mapping.node_minus_coordinator_us, 1000)
        self.assertEqual(connection.mapping.round_trip_us, 40)
        start = sock.frames[-1]
        self.assertEqual(server.unpack_header(start)[2], server.MESSAGE_TYPE_START)
        coordinator_start, offset = struct.unpack(
            "!Qq", start[server.HEADER_SIZE:])
        self.assertEqual(offset, 1000)
        self.assertGreater(coordinator_start, 0)
        self.assertTrue(coordinator.start_pending)
        self.assertFalse(coordinator.run_active)

        sequence = server.unpack_header(start)[7]
        server.process_control_ack(
            build_control_ack(connection, server.MESSAGE_TYPE_START, sequence),
            connection)
        self.assertFalse(coordinator.start_pending)
        self.assertTrue(coordinator.run_active)

    def test_reconnect_mapping_is_empty_and_disconnect_stops_peer(self):
        coordinator = server.CaptureCoordinator()
        socket1, socket2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(socket1, 1, 101, coordinator)
        node2 = server.NodeControlConnection(socket2, 2, 202, coordinator)
        mapping = server.ClockMapping(10, 20, 30)
        node1.mapping = mapping
        node2.mapping = mapping
        coordinator.register(node1)
        coordinator.register(node2)
        coordinator.mapping_updated(node2)
        sequence = server.unpack_header(socket1.frames[-1])[7]
        server.process_control_ack(
            build_control_ack(node1, server.MESSAGE_TYPE_START, sequence), node1)
        self.assertFalse(coordinator.run_active)
        server.process_control_ack(
            build_control_ack(node2, server.MESSAGE_TYPE_START, sequence), node2)
        self.assertTrue(coordinator.run_active)

        coordinator.unregister(node2)
        self.assertFalse(coordinator.run_active)
        self.assertEqual(
            server.unpack_header(socket1.frames[-1])[2],
            server.MESSAGE_TYPE_STOP,
        )

        replacement = server.NodeControlConnection(
            RecordingSocket(), 2, 303, coordinator)
        self.assertIsNone(replacement.mapping)


    def test_start_requires_matching_ack_from_both_nodes(self):
        coordinator = server.CaptureCoordinator()
        socket1, socket2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(socket1, 1, 101, coordinator)
        node2 = server.NodeControlConnection(socket2, 2, 202, coordinator)
        node1.mapping = server.ClockMapping(10, 20, 30)
        node2.mapping = server.ClockMapping(10, 20, 30)
        coordinator.register(node1)
        coordinator.register(node2)

        coordinator.mapping_updated(node2)
        sequence = server.unpack_header(socket1.frames[-1])[7]
        self.assertTrue(coordinator.start_pending)
        self.assertFalse(coordinator.run_active)

        server.process_control_ack(
            build_control_ack(node1, server.MESSAGE_TYPE_START, sequence,
                              effective_local_us=1000),
            node1)
        self.assertTrue(coordinator.start_pending)
        self.assertFalse(coordinator.run_active)

        server.process_control_ack(
            build_control_ack(node2, server.MESSAGE_TYPE_START, sequence,
                              effective_local_us=2000),
            node2)
        self.assertFalse(coordinator.start_pending)
        self.assertTrue(coordinator.run_active)

    def test_start_ack_timeout_stops_both_and_requires_manual_restart(self):
        coordinator = server.CaptureCoordinator()
        socket1, socket2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(socket1, 1, 101, coordinator)
        node2 = server.NodeControlConnection(socket2, 2, 202, coordinator)
        node1.mapping = server.ClockMapping(10, 20, 30)
        node2.mapping = server.ClockMapping(10, 20, 30)
        coordinator.register(node1)
        coordinator.register(node2)

        coordinator.mapping_updated(node2)
        start_sequence = server.unpack_header(socket1.frames[-1])[7]
        coordinator_start_us, _offset = struct.unpack(
            "!Qq", socket1.frames[-1][server.HEADER_SIZE:])
        server.process_control_ack(
            build_control_ack(node1, server.MESSAGE_TYPE_START, start_sequence),
            node1)
        deadline_us = coordinator._start_ack_deadline_us
        self.assertLess(deadline_us, coordinator_start_us)

        self.assertFalse(coordinator.tick(deadline_us - 1))
        with self.assertLogs(server.LOGGER, level="ERROR") as output:
            self.assertTrue(coordinator.tick(deadline_us))
        self.assertIn("missing_nodes=[2]", "\n".join(output.output))
        self.assertFalse(coordinator.start_pending)
        self.assertFalse(coordinator.run_active)
        self.assertFalse(coordinator.desired_running)
        self.assertEqual(
            server.unpack_header(socket1.frames[-1])[2],
            server.MESSAGE_TYPE_STOP)
        self.assertEqual(
            server.unpack_header(socket2.frames[-1])[2],
            server.MESSAGE_TYPE_STOP)

        # A delayed acknowledgement for the cancelled START is diagnostic only.
        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            server.process_control_ack(
                build_control_ack(
                    node2, server.MESSAGE_TYPE_START, start_sequence),
                node2)
        self.assertIn("LATE_START_ACK_IGNORED", "\n".join(output.output))

        frame_count = len(socket1.frames)
        coordinator.mapping_updated(node1)
        self.assertEqual(len(socket1.frames), frame_count)

        self.assertTrue(coordinator.request_start())
        self.assertTrue(coordinator.desired_running)
        self.assertTrue(coordinator.start_pending)
        self.assertEqual(
            server.unpack_header(socket1.frames[-1])[2],
            server.MESSAGE_TYPE_START)

    def test_stale_control_ack_is_rejected(self):
        coordinator = server.CaptureCoordinator(required_nodes=(1,))
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 101, coordinator)
        connection.mapping = server.ClockMapping(10, 20, 30)
        coordinator.register(connection)
        coordinator.mapping_updated(connection)
        sequence = server.unpack_header(sock.frames[-1])[7]

        with self.assertRaises(server.ProtocolError) as context:
            server.process_control_ack(
                build_control_ack(
                    connection, server.MESSAGE_TYPE_START, sequence + 1),
                connection)
        self.assertEqual(context.exception.code, server.ERROR_INVALID_SEQUENCE)

    def test_stop_ack_accepts_already_stopped_result(self):
        coordinator = server.CaptureCoordinator(required_nodes=(1,),
                                                auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 101, coordinator)
        coordinator.register(connection)

        coordinator.request_stop()
        stop = sock.frames[-1]
        sequence = server.unpack_header(stop)[7]
        server.process_control_ack(
            build_control_ack(
                connection, server.MESSAGE_TYPE_STOP, sequence,
                result=server.CONTROL_RESULT_ALREADY_STOPPED),
            connection)
        self.assertEqual(coordinator._pending_stop_acks, {})

    def test_periodic_refresh_starts_after_mapping_age_limit(self):
        coordinator = server.CaptureCoordinator(required_nodes=(1,))
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        connection.mapping = server.ClockMapping(0, 10, 100)
        connection.last_sync_completed_us = 100
        due = 100 + int(server.CLOCK_SYNC_PERIOD_SECONDS * 1_000_000)
        connection.tick(due)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_SYNC_REQ,
        )

    def test_excessive_sync_delay_discards_round_without_closing_session(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1234, coordinator)
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        request = sock.frames[-1]
        header = server.unpack_header(request)
        t1 = struct.unpack("!Q", request[server.HEADER_SIZE:])[0]
        t2 = t1 + 301_000
        t3 = t2 + 10
        received_at_us = t1 + server.CLOCK_SYNC_MAX_DELAY_US + 100_010
        response = server.build_header(
            server.MESSAGE_TYPE_SYNC_RESP,
            node_id=connection.node_id,
            payload_length=server.SYNC_RESP_PAYLOAD_SIZE,
            session_id=connection.session_id,
            sequence=header[7],
            timestamp_us=t3,
        ) + struct.pack("!QQQ", t1, t2, t3)

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            mapping = connection.process_sync_response(response, received_at_us)

        self.assertIsNone(mapping)
        self.assertEqual(connection.samples, [])
        self.assertIsNotNone(connection.pending_sync)
        self.assertEqual(len(sock.frames), 2)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_SYNC_REQ,
        )
        self.assertIn("CLOCK_SYNC_ROUND_DISCARDED", "\n".join(output.output))
        self.assertIn("reason=excessive_delay", "\n".join(output.output))

        mapping, _completed_at = self.complete_sync(connection, sock, (
            (900, 40),
            (1000, 60),
            (1100, 80),
            (1200, 100),
            (1300, 120),
        ))
        self.assertEqual(mapping.node_minus_coordinator_us, 1000)

    def test_repeated_unusable_rounds_pause_initial_sync_before_retry(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1234, coordinator)
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        received_at_us = 0
        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            for _attempt in range(server.CLOCK_SYNC_MAX_ATTEMPTS):
                request = sock.frames[-1]
                header = server.unpack_header(request)
                t1 = struct.unpack("!Q", request[server.HEADER_SIZE:])[0]
                t2 = t1 + 301_000
                t3 = t2 + 10
                received_at_us = (
                    t1 + server.CLOCK_SYNC_MAX_DELAY_US + 100_010)
                response = server.build_header(
                    server.MESSAGE_TYPE_SYNC_RESP,
                    node_id=connection.node_id,
                    payload_length=server.SYNC_RESP_PAYLOAD_SIZE,
                    session_id=connection.session_id,
                    sequence=header[7],
                    timestamp_us=t3,
                ) + struct.pack("!QQQ", t1, t2, t3)
                connection.process_sync_response(response, received_at_us)

        self.assertIsNone(connection.pending_sync)
        self.assertIsNone(connection.mapping)
        self.assertEqual(len(sock.frames), server.CLOCK_SYNC_MAX_ATTEMPTS)
        self.assertIn("reason=attempt_limit", "\n".join(output.output))

        connection.tick(connection.next_sync_allowed_us - 1)
        self.assertEqual(len(sock.frames), server.CLOCK_SYNC_MAX_ATTEMPTS)
        connection.tick(connection.next_sync_allowed_us)
        self.assertEqual(len(sock.frames), server.CLOCK_SYNC_MAX_ATTEMPTS + 1)
        self.assertEqual(connection.sync_attempts, 1)

    def test_stale_sync_response_does_not_replace_current_request(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1234, coordinator)
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        stale_request = sock.frames[-1]
        stale_header = server.unpack_header(stale_request)
        stale_t1 = struct.unpack("!Q", stale_request[server.HEADER_SIZE:])[0]
        with self.assertLogs(server.LOGGER, level="WARNING"):
            connection.tick(stale_t1 + 1_000_000)
        current_pending = connection.pending_sync
        self.assertNotEqual(current_pending[0], stale_header[7])

        t2 = stale_t1 + 100
        t3 = t2 + 10
        stale_response = server.build_header(
            server.MESSAGE_TYPE_SYNC_RESP,
            node_id=connection.node_id,
            payload_length=server.SYNC_RESP_PAYLOAD_SIZE,
            session_id=connection.session_id,
            sequence=stale_header[7],
            timestamp_us=t3,
        ) + struct.pack("!QQQ", stale_t1, t2, t3)

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            mapping = connection.process_sync_response(
                stale_response, stale_t1 + 1_100_000)

        self.assertIsNone(mapping)
        self.assertEqual(connection.pending_sync, current_pending)
        self.assertIn("reason=stale_response", "\n".join(output.output))

    def test_initial_mapping_uses_median_of_three_fastest_rounds(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        mapping, _completed_at = self.complete_sync(connection, sock, (
            (900, 40),
            (1000, 60),
            (1100, 80),
            (40_000, 300_000),
            (-40_000, 400_000),
        ))

        self.assertEqual(mapping.node_minus_coordinator_us, 1000)
        self.assertEqual(mapping.round_trip_us, 40)
        self.assertIsNone(connection.target_mapping)

    def test_small_consistent_periodic_change_is_applied_directly(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        connection.mapping = server.ClockMapping(1000, 40, 900_000)
        connection.mapping_adjusted_at_us = 900_000
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        mapping, _completed_at = self.complete_sync(connection, sock, (
            (3400, 40),
            (3500, 60),
            (3600, 80),
            (50_000, 300_000),
            (-50_000, 400_000),
        ))

        self.assertEqual(mapping.node_minus_coordinator_us, 3500)
        self.assertIsNone(connection.target_mapping)
        self.assertIsNone(connection.pending_confirmation)

    def test_large_consistent_periodic_change_requires_confirmation_then_slews(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        connection.mapping = server.ClockMapping(1000, 40, 900_000)
        connection.mapping_adjusted_at_us = 900_000
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        mapping, _first_completed_at = self.complete_sync(connection, sock, (
            (30_000, 40),
            (31_000, 60),
            (32_000, 80),
            (100_000, 300_000),
            (-100_000, 400_000),
        ))

        self.assertEqual(mapping.node_minus_coordinator_us, 1000)
        self.assertIsNone(connection.target_mapping)
        self.assertIsNotNone(connection.pending_confirmation)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_SYNC_REQ,
        )

        mapping, completed_at = self.complete_sync(connection, sock, (
            (31_000, 40),
            (32_000, 60),
            (33_000, 80),
            (100_000, 300_000),
            (-100_000, 400_000),
        ))

        self.assertEqual(mapping.node_minus_coordinator_us, 1000)
        self.assertIsNone(connection.pending_confirmation)
        self.assertEqual(
            connection.target_mapping.node_minus_coordinator_us, 31_500)
        ten_seconds_later = connection.get_mapping(completed_at + 10_000_000)
        self.assertEqual(ten_seconds_later.node_minus_coordinator_us, 11_000)
        reached = connection.get_mapping(completed_at + 31_000_000)
        self.assertEqual(reached.node_minus_coordinator_us, 31_500)
        self.assertIsNone(connection.target_mapping)

    def test_large_periodic_change_with_disagreeing_confirmation_is_rejected(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        original = server.ClockMapping(1000, 40, 900_000)
        connection.mapping = original
        connection.mapping_adjusted_at_us = 900_000
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        self.complete_sync(connection, sock, (
            (30_000, 40),
            (31_000, 60),
            (32_000, 80),
            (100_000, 300_000),
            (-100_000, 400_000),
        ))

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            mapping, _completed_at = self.complete_sync(connection, sock, (
                (49_000, 40),
                (50_000, 60),
                (51_000, 80),
                (100_000, 300_000),
                (-100_000, 400_000),
            ))

        self.assertEqual(mapping, original)
        self.assertIsNone(connection.pending_confirmation)
        self.assertIsNone(connection.target_mapping)
        self.assertIn(
            "reason=confirmation_disagreement", "\n".join(output.output))

    def test_invalid_confirmation_batch_clears_pending_candidate(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        original = server.ClockMapping(1000, 40, 900_000)
        connection.mapping = original
        connection.mapping_adjusted_at_us = 900_000
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        self.complete_sync(connection, sock, (
            (30_000, 40),
            (31_000, 60),
            (32_000, 80),
            (100_000, 300_000),
            (-100_000, 400_000),
        ))
        self.assertIsNotNone(connection.pending_confirmation)

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            mapping, _completed_at = self.complete_sync(connection, sock, (
                (-20_000, 40),
                (0, 60),
                (30_000, 80),
                (1000, 300_000),
                (1000, 400_000),
            ))

        self.assertEqual(mapping, original)
        self.assertIsNone(connection.pending_confirmation)
        self.assertIsNone(connection.target_mapping)
        self.assertIn("reason=offset_spread", "\n".join(output.output))

    def test_inconsistent_periodic_result_is_rejected(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        original = server.ClockMapping(1000, 40, 900_000)
        connection.mapping = original
        connection.mapping_adjusted_at_us = 900_000
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            mapping, _completed_at = self.complete_sync(connection, sock, (
                (-20_000, 40),
                (0, 60),
                (30_000, 80),
                (1000, 300_000),
                (1000, 400_000),
            ))

        self.assertEqual(mapping, original)
        self.assertIsNone(connection.target_mapping)
        self.assertIn("CLOCK_SYNC_REJECTED", "\n".join(output.output))
        self.assertIn("offset_spread", "\n".join(output.output))

    def test_periodic_result_with_too_few_quality_rounds_is_rejected(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), auto_start=False)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(sock, 1, 1, coordinator)
        original = server.ClockMapping(1000, 40, 900_000)
        connection.mapping = original
        connection.mapping_adjusted_at_us = 900_000
        coordinator.register(connection)
        connection.begin_sync(1_000_000)

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            mapping, _completed_at = self.complete_sync(connection, sock, (
                (2000, 40),
                (2100, 60),
                (2200, 260_000),
                (2300, 300_000),
                (2400, 400_000),
            ))

        self.assertEqual(mapping, original)
        self.assertIn(
            "insufficient_quality_rounds usable=2 required=3",
            "\n".join(output.output),
        )

    def test_manual_stop_suppresses_auto_start_until_requested(self):
        coordinator = server.CaptureCoordinator(auto_start=False)
        socket1, socket2 = RecordingSocket(), RecordingSocket()
        node1 = server.NodeControlConnection(socket1, 1, 11, coordinator)
        node2 = server.NodeControlConnection(socket2, 2, 22, coordinator)
        node1.mapping = server.ClockMapping(1, 10, 100)
        node2.mapping = server.ClockMapping(2, 10, 100)
        coordinator.register(node1)
        coordinator.register(node2)
        coordinator.mapping_updated(node2)
        self.assertFalse(coordinator.run_active)
        self.assertEqual(socket1.frames, [])

        self.assertTrue(coordinator.request_start())
        self.assertEqual(
            server.unpack_header(socket1.frames[-1])[2],
            server.MESSAGE_TYPE_START,
        )
        coordinator.request_stop()
        self.assertFalse(coordinator.desired_running)
        self.assertEqual(
            server.unpack_header(socket1.frames[-1])[2],
            server.MESSAGE_TYPE_STOP,
        )
        frame_count = len(socket1.frames)
        coordinator.mapping_updated(node1)
        self.assertEqual(len(socket1.frames), frame_count)


class HeartbeatCadenceTest(unittest.TestCase):
    """Verify that DATA cleanly separates HEARTBEAT cadence periods."""

    session_id = 1234
    node_id = 1
    assigned_node_mask = 1 << node_id

    def build_heartbeat(self, sequence, timestamp_us):
        payload = struct.pack(
            server.HEARTBEAT_PAYLOAD_FORMAT,
            server.STATUS_MOCK_DATA | server.STATUS_SOURCE_UNAVAILABLE,
            0,
        )
        return server.build_header(
            server.MESSAGE_TYPE_HEARTBEAT,
            node_id=self.node_id,
            payload_length=len(payload),
            session_id=self.session_id,
            sequence=sequence,
            timestamp_us=timestamp_us,
        ) + payload

    def build_data(self, sequence, timestamp_us):
        payload = struct.pack(
            server.DATA_PAYLOAD_FORMAT,
            1000, -500, 16384, 0, 0, 0,
            server.STATUS_MOCK_DATA,
        )
        return server.build_header(
            server.MESSAGE_TYPE_DATA,
            node_id=self.node_id,
            payload_length=len(payload),
            session_id=self.session_id,
            sequence=sequence,
            timestamp_us=timestamp_us,
        ) + payload

    def process_heartbeat(self, frame, stream_state):
        server.process_heartbeat(
            frame,
            stream_state,
            self.session_id,
            self.assigned_node_mask,
        )

    def test_first_heartbeat_after_data_ignores_the_data_period(self):
        stream_state = {}
        self.process_heartbeat(self.build_heartbeat(0, 1_000_000), stream_state)
        server.process_data(
            self.build_data(1, 2_000_000),
            stream_state,
            self.session_id,
            self.assigned_node_mask,
        )

        # This 676-second gap is legitimate because DATA occurred after the
        # previous HEARTBEAT. It matches the physical failure case.
        with self.assertNoLogs(server.LOGGER, level="WARNING"):
            self.process_heartbeat(
                self.build_heartbeat(1, 677_000_000), stream_state)

        # Cadence checking resumes immediately within the new heartbeat run.
        with self.assertNoLogs(server.LOGGER, level="WARNING"):
            self.process_heartbeat(
                self.build_heartbeat(2, 678_000_000), stream_state)

    def test_late_consecutive_heartbeat_still_warns(self):
        stream_state = {}
        self.process_heartbeat(self.build_heartbeat(0, 1_000_000), stream_state)

        with self.assertLogs(server.LOGGER, level="WARNING") as output:
            self.process_heartbeat(
                self.build_heartbeat(1, 3_000_000), stream_state)

        self.assertIn(
            "Unusual HEARTBEAT interval: node=1 delta_us=2000000",
            "\n".join(output.output),
        )


class ConcurrentHubTest(unittest.TestCase):
    """Verify simultaneous physical-node identities receive independent Sessions."""

    def test_node1_and_node2_receive_independent_sessions_concurrently(self):
        workers = []

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            port = listener.getsockname()[1]

            def accept_two_clients():
                for _ in range(2):
                    client, address = listener.accept()
                    workers.append(server.start_client_worker(client, address))

            acceptor = threading.Thread(target=accept_two_clients, daemon=True)
            acceptor.start()

            with socket.create_connection(("127.0.0.1", port)) as node1:
                node1.settimeout(1.0)
                node1.sendall(build_hello(1, 0x02, 1001))
                node1_ack = receive_exact(node1, server.HEADER_SIZE)

                with socket.create_connection(("127.0.0.1", port)) as node2:
                    node2.settimeout(1.0)
                    node2.sendall(build_hello(2, 0x04, 2001))
                    node2_ack = receive_exact(node2, server.HEADER_SIZE)

                    node1_header = server.unpack_header(node1_ack)
                    node2_header = server.unpack_header(node2_ack)

                    self.assertEqual(
                        node1_header[3],
                        0,
                    )
                    self.assertEqual(
                        node1_header[2],
                        server.MESSAGE_TYPE_HELLO_ACK,
                    )
                    self.assertEqual(
                        node2_header[2],
                        server.MESSAGE_TYPE_HELLO_ACK,
                    )
                    self.assertNotEqual(node1_header[6], 0)
                    self.assertNotEqual(node2_header[6], 0)
                    self.assertNotEqual(node1_header[6], node2_header[6])

            acceptor.join(timeout=1.0)
            self.assertFalse(acceptor.is_alive())

        for worker in workers:
            worker.join(timeout=1.0)


class TestResponseTest(unittest.TestCase):
    """Verify TEST_RESPONSE validation, readable status, and hub-specific logs."""

    def test_status_description_is_human_readable_and_preserves_unknown_bits(self):
        self.assertEqual(server.describe_status(server.STATUS_MOCK_DATA), "mock_data")
        self.assertEqual(
            server.describe_status(
                server.STATUS_MOCK_DATA | server.STATUS_TEST_REQUEST_PROCESSED),
            "mock_data|test_request_processed",
        )
        self.assertEqual(server.describe_status(0x0001), "sensor_valid")

    def test_node_lines_use_orange_for_one_and_blue_for_two(self):
        self.assertEqual(
            server.colorize_hub_line(1, "node=1", use_color=True),
            "\033[38;5;208mnode=1\033[0m")
        self.assertEqual(
            server.colorize_hub_line(2, "node=2", use_color=True),
            "\033[34mnode=2\033[0m")
        self.assertEqual(
            server.colorize_hub_line(3, "node=3", use_color=True),
            "node=3")

    def test_valid_test_response_is_decoded(self):
        session_id = 1234
        payload = struct.pack(
            server.DATA_PAYLOAD_FORMAT,
            13, -2, 3, 110, 5, -6,
            server.STATUS_MOCK_DATA | server.STATUS_TEST_REQUEST_PROCESSED,
        )
        frame = server.build_header(
            server.MESSAGE_TYPE_TEST_RESPONSE,
            node_id=1,
            payload_length=len(payload),
            session_id=session_id,
            sequence=77,
        ) + payload

        with self.assertLogs(server.LOGGER, level="INFO") as output:
            server.process_test_response(frame, session_id, 1)
        self.assertIn("sequence=77", "\n".join(output.output))

    def test_wrong_hub_or_missing_processed_status_is_rejected(self):
        payload = struct.pack(
            server.DATA_PAYLOAD_FORMAT,
            1, 2, 3, 4, 5, 6, server.STATUS_MOCK_DATA,
        )
        frame = server.build_header(
            server.MESSAGE_TYPE_TEST_RESPONSE,
            node_id=2,
            payload_length=len(payload),
            session_id=1234,
            sequence=1,
        ) + payload
        with self.assertRaises(server.ProtocolError):
            server.process_test_response(frame, 1234, 1)
        with self.assertRaises(server.ProtocolError):
            server.process_test_response(frame, 1234, 2)

    def test_hub_error_is_logged_without_rejecting_the_session(self):
        payload = struct.pack(
            server.ERROR_PAYLOAD_FORMAT,
            8,
            server.MESSAGE_TYPE_TEST_RESPONSE - 1,
            0,
        )
        frame = server.build_header(
            server.MESSAGE_TYPE_ERROR,
            node_id=1,
            payload_length=len(payload),
            session_id=1234,
            sequence=77,
        ) + payload

        with self.assertLogs(server.LOGGER, level="INFO") as output:
            server.process_hub_error(frame, 1234, 1)
        self.assertIn("code=8", "\n".join(output.output))


class LoggingTest(unittest.TestCase):
    """Verify persistent logs rotate as plain readable text without ANSI codes."""

    def test_rotating_file_is_plain_text_and_keeps_an_older_segment(self):
        original_handlers = list(server.LOGGER.handlers)
        original_level = server.LOGGER.level
        original_propagate = server.LOGGER.propagate

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "server.log")
            try:
                server.configure_logging(path, max_bytes=250, backup_count=1)
                # Keep the test output quiet; the second handler still writes
                # every INFO record to the temporary rotating file.
                server.LOGGER.handlers[0].setLevel(logging.CRITICAL)
                for index in range(12):
                    server.log_hub(
                        logging.INFO,
                        1,
                        f"DATA node=1 sequence={index} payload={'x' * 40}",
                    )
                for handler in server.LOGGER.handlers:
                    handler.flush()

                self.assertTrue(os.path.exists(path))
                self.assertTrue(os.path.exists(path + ".1"))
                contents = ""
                for candidate in (path, path + ".1"):
                    with open(candidate, encoding="utf-8") as log_file:
                        contents += log_file.read()
                self.assertNotIn("\033[", contents)
                self.assertIn("DATA node=1", contents)
            finally:
                for handler in server.LOGGER.handlers:
                    handler.close()
                server.LOGGER.handlers = original_handlers
                server.LOGGER.setLevel(original_level)
                server.LOGGER.propagate = original_propagate


class Phase10PipelineTest(unittest.TestCase):
    """Protect exactly-once publication, Run IDs, sequencing, and isolation."""

    class CapturingPublisher:
        """Thread-safe in-memory publisher used to inspect generated events."""

        def __init__(self):
            self.messages = []
            self.lock = threading.Lock()

        def publish_for_response(self):
            with self.lock:
                sequence = len(self.messages)
                message = server.build_message(
                    run_id=9001, seq=sequence, ts_us=sequence,
                    exercise="squat")
                self.messages.append(message)
                return message

    def build_response(self, session_id, sequence=1, node_id=1,
                       status=None, ax=1):
        if status is None:
            status = server.STATUS_MOCK_DATA | server.STATUS_TEST_REQUEST_PROCESSED
        payload = struct.pack(
            server.DATA_PAYLOAD_FORMAT,
            ax, 2, 3, 4, 5, 6, status)
        return server.build_header(
            server.MESSAGE_TYPE_TEST_RESPONSE, node_id, len(payload),
            session_id, sequence) + payload

    def run_session(self, hub_id, frames, publisher):
        client, peer = socket.socketpair()
        worker = threading.Thread(
            target=server.handle_client,
            args=(peer, ("local", hub_id), publisher), daemon=True)
        worker.start()
        client.settimeout(1.0)
        client.sendall(build_hello(
            hub_id, server.HUB_CONFIG[hub_id]["assigned_node_mask"], hub_id))
        ack = receive_exact(client, server.HEADER_SIZE)
        session_id = server.unpack_header(ack)[6]
        for frame_factory in frames:
            client.sendall(frame_factory(session_id))
        client.shutdown(socket.SHUT_WR)
        worker.join(timeout=1.0)
        client.close()
        self.assertFalse(worker.is_alive())
        return session_id

    def test_valid_response_publishes_required_fields_once(self):
        publisher = self.CapturingPublisher()
        self.run_session(1, [lambda session: self.build_response(session)], publisher)
        self.assertEqual(len(publisher.messages), 1)
        message = publisher.messages[0]
        self.assertEqual(validate_message(message), message)
        self.assertEqual(
            set(message), {"v", "type", "run_id", "seq", "ts_us", "exercise"})

    def test_generated_run_id_is_exactly_eight_digits(self):
        publisher = server.VisualizerEventPublisher(sender=lambda _message: None)
        try:
            self.assertGreaterEqual(publisher.run_id, 10_000_000)
            self.assertLessEqual(publisher.run_id, 99_999_999)
        finally:
            publisher.close()

    def test_simulated_exercise_set_contains_all_demo_labels(self):
        self.assertEqual(
            server.SUPPORTED_EXERCISES,
            ("unknown", "squat", "push_up", "sit_up", "lunge", "leg_raise",
             "jumping_jack"),
        )

    def test_exact_duplicate_and_conflict_do_not_publish_again(self):
        publisher = self.CapturingPublisher()
        self.run_session(1, [
            lambda session: self.build_response(session, sequence=7),
            lambda session: self.build_response(session, sequence=7),
        ], publisher)
        self.assertEqual(len(publisher.messages), 1)

        publisher = self.CapturingPublisher()
        self.run_session(1, [
            lambda session: self.build_response(session, sequence=7),
            lambda session: self.build_response(session, sequence=7, ax=2),
        ], publisher)
        self.assertEqual(len(publisher.messages), 1)

    def test_malformed_unauthorized_and_stale_session_publish_nothing(self):
        cases = (
            lambda session: self.build_response(session, status=server.STATUS_MOCK_DATA),
            lambda session: self.build_response(session, node_id=2),
            lambda session: self.build_response((session + 1) & 0xFFFFFFFF),
        )
        for frame_factory in cases:
            with self.subTest(frame_factory=frame_factory):
                publisher = self.CapturingPublisher()
                self.run_session(1, [frame_factory], publisher)
                self.assertEqual(publisher.messages, [])

    def test_new_session_clears_exactly_once_state(self):
        publisher = self.CapturingPublisher()
        self.run_session(
            1, [lambda session: self.build_response(session, sequence=5)], publisher)
        self.run_session(
            1, [lambda session: self.build_response(session, sequence=5)], publisher)
        self.assertEqual([item["seq"] for item in publisher.messages], [0, 1])

    def test_concurrent_hubs_receive_unique_monotonic_visualizer_sequences(self):
        publisher = self.CapturingPublisher()
        workers = [
            threading.Thread(
                target=self.run_session,
                args=(hub_id, [lambda session, hub_id=hub_id:
                      self.build_response(session, sequence=1, node_id=hub_id)],
                      publisher))
            for hub_id in (1, 2)
        ]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join()
        self.assertEqual([item["seq"] for item in publisher.messages], [0, 1])

    def test_real_publisher_assigns_thread_safe_monotonic_sequences(self):
        sent = []
        sent_lock = threading.Lock()

        def sender(message):
            with sent_lock:
                sent.append(message)

        publisher = server.VisualizerEventPublisher(
            sender=sender, exercise_choice=lambda choices: choices[1],
            run_id=123, queue_size=16)
        try:
            threads = [threading.Thread(target=publisher.publish_for_response)
                       for _ in range(8)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            publisher._queue.join()
            self.assertEqual(sorted(item["seq"] for item in sent), list(range(8)))
            self.assertTrue(all(validate_message(item) == item for item in sent))
        finally:
            publisher.close()

    def test_publisher_failure_isolated_and_worker_continues(self):
        calls = []

        def unavailable(message):
            calls.append(message["seq"])
            raise ConnectionRefusedError("gateway unavailable")

        publisher = server.VisualizerEventPublisher(
            sender=unavailable, run_id=124, queue_size=4)
        try:
            publisher.publish_for_response()
            publisher.publish_for_response()
            publisher._queue.join()
            self.assertEqual(calls, [0, 1])
            self.assertTrue(publisher._worker.is_alive())
        finally:
            publisher.close()


class FileVerifierTest(unittest.TestCase):
    """Verify ordered file reconstruction, CRC/SHA checks, and failure cleanup."""

    def make_frame(self, message_type, payload, sequence=1):
        return server.build_header(message_type, node_id=0,
                                   payload_length=len(payload), session_id=1234,
                                   sequence=sequence) + payload

    def test_valid_file_is_reconstructed_and_matches_esp_result(self):
        data = bytes(range(256)) * 4
        digest = hashlib.sha256(data).digest()
        verifier = server.FileVerifier()
        try:
            verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_BEGIN,
                struct.pack("!IIHH32s", 7, len(data), 300, 0, digest)), 1234, 1)
            for sequence, offset in enumerate(range(0, len(data), 300), 2):
                chunk = data[offset:offset + 300]
                payload = struct.pack("!IIHHI", 7, offset, len(chunk), 0,
                                      zlib.crc32(chunk) & 0xFFFFFFFF) + chunk
                verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_CHUNK,
                                                  payload, sequence), 1234, 1)
            verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_END,
                struct.pack("!II32s", 7, len(data), digest), 9), 1234, 1)
            with open(verifier.path, "rb") as received:
                self.assertEqual(received.read(), data)
            result = server.build_header(server.MESSAGE_TYPE_FILE_RESULT, 1, 44,
                                         1234, 9) + struct.pack(
                                             "!IHHI32s", 7, 0, 0, len(data), digest)
            server.process_file_result(result, 1234, 1, verifier)
        finally:
            path = verifier.path
            verifier.reset(remove=True)
            self.assertFalse(path and os.path.exists(path))

    def test_wrong_offset_and_crc_are_rejected(self):
        data = b"abc"
        digest = hashlib.sha256(data).digest()
        for offset, crc in ((1, zlib.crc32(data) & 0xFFFFFFFF), (0, 0)):
            verifier = server.FileVerifier()
            verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_BEGIN,
                struct.pack("!IIHH32s", 8, len(data), 3, 0, digest)), 1234, 1)
            payload = struct.pack("!IIHHI", 8, offset, 3, 0, crc) + data
            with self.assertRaises(server.ProtocolError):
                verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_CHUNK,
                                                  payload, 2), 1234, 1)
            verifier.reset(remove=True)

    def test_failed_esp_result_clears_incomplete_transfer(self):
        data = b"abc"
        digest = hashlib.sha256(data).digest()
        verifier = server.FileVerifier()
        verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_BEGIN,
            struct.pack("!IIHH32s", 8, len(data), 3, 0, digest)), 1234, 1)
        incomplete_path = verifier.path
        result = server.build_header(server.MESSAGE_TYPE_FILE_RESULT, 1, 44,
                                     1234, 2) + struct.pack(
                                         "!IHHI32s", 8, 3, 0, 0, bytes(32))
        server.process_file_result(result, 1234, 1, verifier)
        self.assertIsNone(verifier.file)
        self.assertEqual(verifier.file_id, 0)
        self.assertFalse(os.path.exists(incomplete_path))

        # A new transfer in the same Session must no longer look overlapping.
        verifier.process(self.make_frame(server.MESSAGE_TYPE_FILE_BEGIN,
            struct.pack("!IIHH32s", 9, len(data), 3, 0, digest), 3), 1234, 1)
        self.assertEqual(verifier.file_id, 9)
        verifier.reset(remove=True)


if __name__ == "__main__":
    unittest.main()
