"""Unit tests for interactive relay framing, state, diagnostics, and tasks."""

import asyncio
import struct
import hashlib
import logging
import zlib
import unittest

import interactive_hub_relay as demo


class InteractiveHubRelayCodecTests(unittest.TestCase):
    """Protect binary layouts, readable output, stream health, and hub state."""

    def test_five_editable_payloads_are_complete(self):
        self.assertGreaterEqual(len(demo.DUMMY_PAYLOADS), 5)
        for payload in demo.DUMMY_PAYLOADS:
            self.assertEqual(len(payload), 7)
            self.assertTrue(payload[6] & demo.STATUS_MOCK_DATA)
            struct.pack(demo.IMU_FORMAT, *payload)

    def test_status_description_is_human_readable(self):
        self.assertEqual(demo.describe_status(demo.STATUS_MOCK_DATA), "mock_data")
        self.assertEqual(
            demo.describe_status(
                demo.STATUS_MOCK_DATA | demo.STATUS_TEST_REQUEST_PROCESSED),
            "mock_data|test_request_processed")

    def test_error_diagnostic_names_match_wire_values(self):
        self.assertEqual(demo.ERROR_NAMES[8], "SEQUENCE_REUSE")
        self.assertEqual(demo.MESSAGE_TYPE_NAMES[demo.TEST_REQUEST], "TEST_REQUEST")
        self.assertEqual(
            demo.MESSAGE_TYPE_NAMES[demo.PHYSICAL_DATA], "PHYSICAL_DATA")
        self.assertEqual(demo.MESSAGE_TYPE_NAMES[demo.HEARTBEAT], "HEARTBEAT")
        self.assertEqual(
            demo.MESSAGE_TYPE_NAMES[demo.ACTIVITY_TRIGGER], "ACTIVITY_TRIGGER")
        self.assertEqual(demo.MESSAGE_TYPE_NAMES[demo.ARM], "ARM")

    def test_activity_trigger_wire_shape_is_named_and_decodable(self):
        payload = (0x12345678, 7, 987654321)
        encoded = struct.pack(demo.ACTIVITY_TRIGGER_FORMAT, *payload)
        self.assertEqual(len(encoded), demo.ACTIVITY_TRIGGER_SIZE)
        self.assertEqual(struct.unpack(demo.ACTIVITY_TRIGGER_FORMAT, encoded),
                         payload)

    def test_physical_status_description_is_human_readable(self):
        self.assertEqual(
            demo.describe_status(
                demo.STATUS_SENSOR_VALID
                | demo.STATUS_HISTORICAL
                | demo.STATUS_HISTORY_OVERWRITE),
            "sensor_valid|historical|history_overwrite",
        )

    def test_request_uses_session_sequence_and_canonical_payload(self):
        payload = demo.DUMMY_PAYLOADS[0]
        frame = demo.build_test_request(0x11223344, 0x55667788, payload)
        header = struct.unpack(demo.HEADER_FORMAT, frame[:demo.HEADER_SIZE])
        self.assertEqual(header[2], demo.TEST_REQUEST)
        self.assertEqual(header[3], 0)
        self.assertEqual(header[5], demo.IMU_SIZE)
        self.assertEqual(header[6], 0x11223344)
        self.assertEqual(header[7], 0x55667788)
        self.assertEqual(struct.unpack(demo.IMU_FORMAT, frame[demo.HEADER_SIZE:]), payload)

    def test_benchmark_control_layout(self):
        frame = demo.build_benchmark_control(7, 9, 1, 16, 10000)
        header = struct.unpack(demo.HEADER_FORMAT, frame[:demo.HEADER_SIZE])
        self.assertEqual(header[2], demo.BENCHMARK_CONTROL)
        self.assertEqual(header[6:8], (7, 9))
        self.assertEqual(struct.unpack("!BBHI", frame[demo.HEADER_SIZE:]),
                         (1, 0, 16, 10000))

    def test_observer_handles_fragmented_and_coalesced_frames_once(self):
        observed = []
        observer = demo.FrameObserver(lambda header, frame: observed.append((header, frame)))
        one = demo.build_test_request(1, 1, demo.DUMMY_PAYLOADS[0])
        two = demo.build_test_request(1, 2, demo.DUMMY_PAYLOADS[1])
        observer.feed(one[:7])
        self.assertEqual(observed, [])
        observer.feed(one[7:] + two)
        self.assertEqual([item[0][7] for item in observed], [1, 2])

    def test_stream_stats_reports_rate_and_sequence_health(self):
        stats = demo.StreamStats()
        payload = demo.DUMMY_PAYLOADS[0]

        for sequence, now in ((10, 100.0), (11, 100.02), (13, 100.04)):
            frame = struct.pack(
                demo.HEADER_FORMAT,
                demo.MAGIC, demo.VERSION, demo.DATA, 1, 0, demo.IMU_SIZE,
                7, sequence, sequence * 20000,
            ) + struct.pack(demo.IMU_FORMAT, *payload)
            header = struct.unpack(demo.HEADER_FORMAT, frame[:demo.HEADER_SIZE])
            stats.record(header, frame, now=now)

        report = stats.report(now=101.0)
        self.assertEqual(report["total_frames"], 3)
        self.assertEqual(report["total_bytes"], 3 * (demo.HEADER_SIZE + demo.IMU_SIZE))
        self.assertEqual(report["sequence_gaps"], 1)
        self.assertEqual(report["duplicates"], 0)
        self.assertEqual(report["out_of_order"], 0)
        self.assertEqual(report["timestamp_regressions"], 0)
        self.assertAlmostEqual(report["current_kbps"], 0.912)

    def test_stream_stats_decodes_physical_data_payload(self):
        stats = demo.StreamStats()
        payload = (
            1.25, -2.5, 9.81, 0.125, -0.25, 0.5,
            demo.STATUS_SENSOR_VALID | demo.STATUS_HISTORICAL,
        )
        frame = struct.pack(
            demo.HEADER_FORMAT,
            demo.MAGIC, demo.VERSION, demo.PHYSICAL_DATA, 1, 0,
            demo.PHYSICAL_IMU_SIZE, 7, 42, 123456,
        ) + struct.pack(demo.PHYSICAL_IMU_FORMAT, *payload)
        header = struct.unpack(demo.HEADER_FORMAT, frame[:demo.HEADER_SIZE])

        stats.record(header, frame, now=100.0)

        self.assertEqual(stats.latest_message_type, demo.PHYSICAL_DATA)
        self.assertEqual(stats.latest_payload[-1], payload[-1])
        for actual, expected in zip(stats.latest_payload[:6], payload[:6]):
            self.assertAlmostEqual(actual, expected, places=5)
        self.assertEqual(stats.total_bytes,
                         demo.HEADER_SIZE + demo.PHYSICAL_IMU_SIZE)

    def test_heartbeat_clears_latest_data_for_matching_source(self):
        stats = demo.StreamStats()
        payload = (1.25, -2.5, 9.81, 0.125, -0.25, 0.5,
                   demo.STATUS_SENSOR_VALID)
        frame = struct.pack(
            demo.HEADER_FORMAT,
            demo.MAGIC, demo.VERSION, demo.PHYSICAL_DATA, 2, 0,
            demo.PHYSICAL_IMU_SIZE, 7, 42, 123456,
        ) + struct.pack(demo.PHYSICAL_IMU_FORMAT, *payload)
        header = struct.unpack(demo.HEADER_FORMAT, frame[:demo.HEADER_SIZE])
        stats.record(header, frame, now=100.0)

        self.assertFalse(stats.mark_idle(1, now=101.0))
        self.assertIsNotNone(stats.latest_payload)
        self.assertTrue(stats.mark_idle(2, now=101.0))
        self.assertIsNone(stats.latest_payload)
        self.assertEqual(stats.idle_since, 101.0)
        self.assertFalse(stats.mark_idle(2, now=102.0))

    def test_new_data_reactivates_stream_after_heartbeat(self):
        stats = demo.StreamStats()
        payload = demo.DUMMY_PAYLOADS[0]
        frame = struct.pack(
            demo.HEADER_FORMAT,
            demo.MAGIC, demo.VERSION, demo.DATA, 1, 0, demo.IMU_SIZE,
            7, 1, 123456,
        ) + struct.pack(demo.IMU_FORMAT, *payload)
        header = struct.unpack(demo.HEADER_FORMAT, frame[:demo.HEADER_SIZE])

        stats.record(header, frame, now=100.0)
        stats.mark_idle(1, now=101.0)
        stats.record(header, frame, now=102.0)

        self.assertEqual(stats.latest_payload, payload)
        self.assertIsNone(stats.idle_since)

    def test_file_frames_have_streaming_metadata_crc_and_sha256(self):
        data = bytes(range(256)) * 4
        file_id, begin, chunks, end, digest = demo.build_file_frames(
            7, 100, data, file_id=42, chunk_size=300)
        self.assertEqual(file_id, 42)
        self.assertEqual(digest, hashlib.sha256(data).digest())
        self.assertEqual(struct.unpack(demo.HEADER_FORMAT, begin[:demo.HEADER_SIZE])[2],
                         demo.FILE_BEGIN)
        begin_values = struct.unpack("!IIHH32s", begin[demo.HEADER_SIZE:])
        self.assertEqual(begin_values, (42, len(data), 300, 0, digest))
        rebuilt = bytearray()
        for expected_offset, frame in zip(range(0, len(data), 300), chunks):
            file_id, offset, length, reserved, crc = struct.unpack(
                "!IIHHI", frame[demo.HEADER_SIZE:demo.HEADER_SIZE + 16])
            chunk = frame[demo.HEADER_SIZE + 16:]
            self.assertEqual((file_id, offset, length, reserved),
                             (42, expected_offset, len(chunk), 0))
            self.assertEqual(crc, zlib.crc32(chunk) & 0xFFFFFFFF)
            rebuilt.extend(chunk)
        self.assertEqual(bytes(rebuilt), data)
        self.assertEqual(struct.unpack("!II32s", end[demo.HEADER_SIZE:]),
                         (42, len(data), digest))

    def test_file_fault_modes_corrupt_crc_or_offset(self):
        data = b"bounded transfer" * 100
        _, _, bad_crc, _, _ = demo.build_file_frames(
            1, 1, data, file_id=9, corrupt_crc=True)
        fields = struct.unpack("!IIHHI", bad_crc[0][demo.HEADER_SIZE:demo.HEADER_SIZE + 16])
        chunk = bad_crc[0][demo.HEADER_SIZE + 16:]
        self.assertNotEqual(fields[4], zlib.crc32(chunk) & 0xFFFFFFFF)
        _, _, bad_offset, _, _ = demo.build_file_frames(
            1, 1, data, file_id=9, wrong_offset=True)
        self.assertEqual(struct.unpack("!IIHHI",
                         bad_offset[0][demo.HEADER_SIZE:demo.HEADER_SIZE + 16])[1], 1)

    def test_two_node_selection_keeps_independent_state(self):
        relay = demo.InteractiveHubRelay("127.0.0.1", 0, "127.0.0.1", 0)
        node1 = demo.HubConnection(demo.ControllerState(101, 1), None, None,
                                  demo.StreamStats(), demo.StreamStats())
        node2 = demo.HubConnection(demo.ControllerState(202, 2), None, None,
                                  demo.StreamStats(), demo.StreamStats())
        relay.hubs = {1: node1, 2: node2}

        relay.selected_hub = 1
        self.assertIs(relay.choose_hub(), node1)
        relay.selected_hub = 2
        self.assertIs(relay.choose_hub(), node2)
        node1.state.next_sequence = 8
        self.assertEqual(node2.state.next_sequence, 1)

        relay.selected_hub = None
        self.assertIn(relay.choose_hub(), (node1, node2))

    def test_node_color_formatter_colors_complete_single_node_lines(self):
        formatter = demo.HubColorFormatter("%(message)s", use_color=True)
        node1 = logging.LogRecord("test", logging.INFO, "", 0,
                                  "STREAM node=1 frames=10", (), None)
        node2 = logging.LogRecord("test", logging.INFO, "", 0,
                                  "Node 2 ready", (), None)
        self.assertEqual(formatter.format(node1),
                         "\033[38;5;208mSTREAM node=1 frames=10\033[0m")
        self.assertEqual(formatter.format(node2),
                         "\033[34mNode 2 ready\033[0m")

    def test_hub_color_formatter_leaves_combined_lines_neutral(self):
        formatter = demo.HubColorFormatter("%(message)s", use_color=True)
        combined = logging.LogRecord("test", logging.INFO, "", 0,
                                     "COMBINED_STREAM nodes=[1, 2]", (), None)
        self.assertEqual(formatter.format(combined),
                         "COMBINED_STREAM nodes=[1, 2]")

    def test_log_format_omits_milliseconds_and_logger_name(self):
        formatter = demo.HubColorFormatter(
            "%(asctime)s %(levelname)s: %(message)s", use_color=False,
            datefmt="%Y-%m-%d %H:%M:%S")
        record = logging.LogRecord("interactive_hub_relay", logging.INFO, "", 0,
                                   "ready", (), None)
        record.created = 0.123
        rendered = formatter.format(record)
        self.assertNotIn(",123", rendered)
        self.assertNotIn("interactive_hub_relay", rendered)
        self.assertTrue(rendered.endswith(" INFO: ready"))


class InteractiveHubRelayAsyncTests(unittest.IsolatedAsyncioTestCase):
    """Protect background file-task and Session-scoped replay behavior."""

    async def test_stale_diagnostic_is_not_converted_to_new_request(self):
        relay = demo.InteractiveHubRelay("127.0.0.1", 0, "127.0.0.1", 0)
        state = demo.ControllerState(101, 1)
        hub = demo.HubConnection(state, None, None, demo.StreamStats(),
                                 demo.StreamStats())

        await relay.send_test_request(hub, "d")

        self.assertEqual(state.next_sequence, 1)
        self.assertEqual(state.pending, {})
        self.assertIsNone(state.last_frame)

    async def test_file_operation_runs_in_tracked_background_task(self):
        relay = demo.InteractiveHubRelay("127.0.0.1", 0, "127.0.0.1", 0)
        hub = demo.HubConnection(demo.ControllerState(101, 1), None, None,
                                 demo.StreamStats(), demo.StreamStats())
        started = asyncio.Event()
        release = asyncio.Event()
        calls = []

        async def fake_send_file(selected_hub, key):
            calls.append((selected_hub, key))
            started.set()
            await release.wait()

        relay.send_file = fake_send_file
        relay.start_file(hub, "f")
        await started.wait()
        self.assertIn(1, relay.file_tasks)

        relay.start_file(hub, "r")
        self.assertEqual(calls, [(hub, "f")])

        release.set()
        await relay.file_tasks[1]
        await asyncio.sleep(0)
        self.assertNotIn(1, relay.file_tasks)


if __name__ == "__main__":
    unittest.main()
