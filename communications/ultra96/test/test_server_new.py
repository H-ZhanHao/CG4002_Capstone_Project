"""Focused coverage for the inference-enabled server/control merge."""

from dataclasses import dataclass
import struct
import threading
import time
import unittest

import server_new as server


class RecordingSocket:
    def __init__(self):
        self.frames = []

    def sendall(self, frame):
        self.frames.append(frame)

    def shutdown(self, _how):
        pass


def build_control_ack(connection, command_type, command_sequence):
    payload = struct.pack(
        "!BBHIQ", command_type, server.CONTROL_RESULT_APPLIED, 0,
        command_sequence, 123456)
    return server.build_header(
        server.MESSAGE_TYPE_CONTROL_ACK,
        node_id=connection.node_id,
        payload_length=len(payload),
        session_id=connection.session_id,
        sequence=command_sequence,
        timestamp_us=123456,
    ) + payload


def build_trigger(connection, sequence, peak_us, arm_generation):
    detected_us = peak_us + 60_000
    payload = struct.pack(
        "!IIQ", connection.boot_id, arm_generation, peak_us)
    return server.build_header(
        server.MESSAGE_TYPE_ACTIVITY_TRIGGER,
        node_id=connection.node_id,
        payload_length=len(payload),
        session_id=connection.session_id,
        sequence=sequence,
        timestamp_us=detected_us,
    ) + payload


def build_physical(sequence, timestamp_us):
    payload = struct.pack(
        "!ffffffH", 1.0, 2.0, 9.81, 0.1, 0.2, 0.3,
        server.STATUS_SENSOR_VALID | server.STATUS_HISTORICAL)
    return server.build_header(
        server.MESSAGE_TYPE_PHYSICAL_DATA,
        node_id=1,
        payload_length=len(payload),
        session_id=101,
        sequence=sequence,
        timestamp_us=timestamp_us,
    ) + payload


@dataclass
class Classification:
    exercise: str


class InferenceIdlePolicyTest(unittest.TestCase):
    def test_three_consecutive_active_idle_results_request_one_pause(self):
        paused = threading.Event()
        calls = []

        def idle_pause():
            calls.append("pause")
            paused.set()

        publisher = server.VisualizerEventPublisher(
            sender=lambda _message: None,
            idle_pause_callback=idle_pause,
            run_id=123,
        )
        try:
            publisher.set_capture(7)
            publisher.publish_classification(Classification("idle"), 7)
            publisher.publish_classification(Classification("idle"), 7)
            self.assertFalse(paused.wait(0.05))

            publisher.publish_classification(Classification("squat"), 7)
            publisher.publish_classification(Classification("idle"), 7)
            publisher.publish_classification(Classification("idle"), 7)
            publisher.publish_classification(Classification("idle"), 7)
            self.assertTrue(paused.wait(1.0))

            publisher.publish_classification(Classification("idle"), 7)
            time.sleep(0.05)
            self.assertEqual(calls, ["pause"])
        finally:
            publisher.close()

    def test_stale_generation_does_not_count_toward_idle_pause(self):
        paused = threading.Event()
        publisher = server.VisualizerEventPublisher(
            sender=lambda _message: None,
            idle_pause_callback=paused.set,
            run_id=123,
        )
        try:
            publisher.set_capture(8)
            for _ in range(3):
                publisher.publish_classification(Classification("idle"), 7)
            self.assertFalse(paused.wait(0.1))
        finally:
            publisher.close()


class MergedCaptureCoordinatorTest(unittest.TestCase):
    def test_physical_overlap_is_scoped_to_capture_generation(self):
        state = {}
        mapping = server.ClockMapping(0, 20, 1)
        server.process_physical_data(
            build_physical(250, 5_000_000), state, 101, 1 << 1, mapping, 50,
            capture_generation=1)
        with self.assertRaises(server.ProtocolError):
            server.process_physical_data(
                build_physical(50, 1_000_000), state, 101, 1 << 1,
                mapping, 50, capture_generation=1)
        decoded = server.process_physical_data(
            build_physical(50, 1_000_000), state, 101, 1 << 1, mapping, 50,
            capture_generation=2)
        self.assertEqual(decoded.sequence, 50)

    def test_idle_pause_waits_for_stop_ack_and_fresh_trigger(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        now_us = time.monotonic_ns() // 1000
        connection.mapping = server.ClockMapping(100, 20, now_us)
        coordinator.register(connection)

        old_peak_us = now_us + 100
        old_trigger = build_trigger(connection, 9, old_peak_us, 1)
        self.assertTrue(coordinator.idle_pause())
        stop_sequence = server.unpack_header(sock.frames[-1])[7]
        self.assertFalse(server.process_activity_trigger(old_trigger, connection))

        server.process_control_ack(build_control_ack(
            connection, server.MESSAGE_TYPE_STOP, stop_sequence), connection)
        arm_sequence = server.unpack_header(sock.frames[-1])[7]
        self.assertEqual(server.unpack_header(sock.frames[-1])[2],
                         server.MESSAGE_TYPE_ARM)
        server.process_control_ack(build_control_ack(
            connection, server.MESSAGE_TYPE_ARM, arm_sequence), connection)
        fresh_peak_us = coordinator._armed_after_us + 101
        fresh_trigger = build_trigger(
            connection, 10, fresh_peak_us, coordinator._arm_generation)
        self.assertFalse(server.process_activity_trigger(old_trigger, connection))
        self.assertTrue(server.process_activity_trigger(fresh_trigger, connection))
        self.assertEqual(connection.get_physical_capture_generation(), 1)
        self.assertEqual(
            server.unpack_header(sock.frames[-1])[2],
            server.MESSAGE_TYPE_START_FLUSH,
        )

    def test_manual_stop_disarms_idle_auto_resume(self):
        coordinator = server.CaptureCoordinator(
            required_nodes=(1,), physical_mode=True)
        sock = RecordingSocket()
        connection = server.NodeControlConnection(
            sock, 1, 101, coordinator, boot_id=0x11111111)
        connection.mapping = server.ClockMapping(
            100, 20, time.monotonic_ns() // 1000)
        coordinator.register(connection)

        self.assertTrue(coordinator.idle_pause())
        stop_sequence = server.unpack_header(sock.frames[-1])[7]
        coordinator.manual_stop()
        server.process_control_ack(build_control_ack(
            connection, server.MESSAGE_TYPE_STOP, stop_sequence), connection)

        peak_us = time.monotonic_ns() // 1000 + 100
        trigger = build_trigger(connection, 9, peak_us, 2)
        self.assertFalse(server.process_activity_trigger(trigger, connection))
        self.assertFalse(coordinator.desired_running)


if __name__ == "__main__":
    unittest.main()
