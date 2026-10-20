import unittest

from window_assembler import DecodedImuSample, WindowAssembler, WindowConfig


def sample(node, sequence, timestamp):
    return DecodedImuSample(
        node_id=node, session_id=node * 10, sequence=sequence,
        timestamp_us=timestamp, coordinator_timestamp_us=timestamp,
        ax=1, ay=2, az=3, gx=4, gy=5, gz=6,
        battery_mv=3800, status=0, flags=0)


class WindowConfigTest(unittest.TestCase):
    def test_invalid_configuration_is_rejected(self):
        invalid = (
            dict(sample_rate_hz=0, window_us=1_000_000, hop_us=1_000_000),
            dict(sample_rate_hz=1, window_us=0, hop_us=1),
            dict(sample_rate_hz=1, window_us=1, hop_us=0),
            dict(sample_rate_hz=1, window_us=1, hop_us=2),
            dict(sample_rate_hz=1, window_us=1_000_000, hop_us=1, required_nodes=()),
            dict(sample_rate_hz=1, window_us=1_000_000, hop_us=1,
                 required_nodes=(1, 1)),
            dict(sample_rate_hz=3, window_us=500_000, hop_us=1),
        )
        for values in invalid:
            with self.subTest(values=values), self.assertRaises(ValueError):
                WindowConfig(**values)


class WindowAssemblerTest(unittest.TestCase):
    def make_assembler(self, rate=2, window_us=1_000_000,
                       hop_us=1_000_000):
        return WindowAssembler(WindowConfig(rate, window_us, hop_us))

    def complete(self, assembler, node1, node2, boundary=1_000_000):
        output = []
        for item in node1:
            output.extend(assembler.submit(item))
        for item in node2:
            output.extend(assembler.submit(item))
        output.extend(assembler.submit(sample(1, 99, boundary)))
        output.extend(assembler.submit(sample(2, 99, boundary)))
        return output

    def test_samples_before_start_are_ignored(self):
        assembler = self.make_assembler()
        self.assertEqual(assembler.submit(sample(1, 0, 0)), [])
        assembler.start_run(1, 100)
        self.assertEqual(assembler.submit(sample(1, 0, 99)), [])

    def test_complete_two_node_window_is_accepted_and_skew_reported(self):
        assembler = self.make_assembler()
        assembler.start_run(7, 0)
        windows = self.complete(
            assembler,
            [sample(1, 0, 0), sample(1, 1, 500_000)],
            [sample(2, 0, 1_000), sample(2, 1, 502_000)],
        )
        self.assertEqual(len(windows), 1)
        self.assertTrue(windows[0].quality.accepted)
        self.assertEqual(windows[0].quality.max_index_skew_us, 2_000)

    def test_half_open_boundary_is_preserved_for_next_window(self):
        assembler = self.make_assembler(rate=1)
        assembler.start_run(1, 0)
        self.assertEqual(assembler.submit(sample(1, 0, 0)), [])
        self.assertEqual(assembler.submit(sample(2, 0, 0)), [])
        self.assertEqual(assembler.submit(sample(1, 1, 1_000_000)), [])
        first = assembler.submit(sample(2, 1, 1_000_000))[0]
        self.assertEqual([item.sequence for item in first.samples_by_node[1]], [0])
        assembler.submit(sample(1, 2, 2_000_000))
        second = assembler.submit(sample(2, 2, 2_000_000))[0]
        self.assertEqual([item.sequence for item in second.samples_by_node[1]], [1])

    def test_one_second_hop_produces_overlapping_windows(self):
        assembler = self.make_assembler(rate=1, window_us=2_000_000,
                                        hop_us=1_000_000)
        assembler.start_run(1, 0)
        output = []
        for timestamp in range(4):
            for node in (1, 2):
                output.extend(assembler.submit(
                    sample(node, timestamp, timestamp * 1_000_000)))
        self.assertEqual([window.window_id for window in output], [0, 1])
        self.assertEqual([item.sequence for item in output[1].samples_by_node[1]],
                         [1, 2])

    def test_arrival_order_does_not_change_capture_membership(self):
        assembler = self.make_assembler()
        assembler.start_run(1, 0)
        for item in (sample(2, 0, 1_000), sample(2, 1, 501_000),
                     sample(2, 2, 1_000_000), sample(1, 0, 0),
                     sample(1, 1, 500_000)):
            output = assembler.submit(item)
        output = assembler.submit(sample(1, 2, 1_000_000))
        self.assertEqual(len(output), 1)
        self.assertEqual(output[0].quality.sample_counts, {1: 2, 2: 2})

    def test_missing_sequence_rejects_window(self):
        assembler = self.make_assembler()
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler, [sample(1, 0, 0), sample(1, 2, 500_000)],
            [sample(2, 0, 0), sample(2, 1, 500_000)])
        self.assertFalse(windows[0].quality.accepted)
        self.assertEqual(windows[0].quality.missing_sequences[1], (1,))

    def test_count_mismatch_rejects_window(self):
        assembler = self.make_assembler()
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler, [sample(1, 0, 0)],
            [sample(2, 0, 0), sample(2, 1, 500_000)])
        self.assertEqual(windows[0].quality.rejection_reason, "missing_samples")

    def test_timestamp_regression_is_ignored(self):
        assembler = self.make_assembler(rate=1)
        assembler.start_run(1, 0)
        assembler.submit(sample(1, 0, 100))
        with self.assertLogs("server", level="WARNING"):
            assembler.submit(sample(1, 1, 99))
        assembler.submit(sample(2, 0, 100))
        assembler.submit(sample(1, 1, 1_000_000))
        window = assembler.submit(sample(2, 1, 1_000_000))[0]
        self.assertEqual([item.sequence for item in window.samples_by_node[1]], [0])

    def test_stop_is_idempotent_and_clears_samples(self):
        assembler = self.make_assembler(rate=1)
        assembler.start_run(1, 0)
        assembler.submit(sample(1, 0, 0))
        assembler.stop_run("test")
        assembler.stop_run("test again")
        assembler.start_run(2, 0)
        assembler.submit(sample(1, 1, 1_000_000))
        window = assembler.submit(sample(2, 1, 1_000_000))[0]
        self.assertEqual(window.quality.sample_counts[1], 0)

    def test_new_run_resets_window_id_and_samples(self):
        assembler = self.make_assembler(rate=1)
        assembler.start_run(1, 0)
        assembler.submit(sample(1, 0, 0))
        assembler.start_run(2, 10_000_000)
        assembler.submit(sample(1, 0, 10_000_000))
        assembler.submit(sample(2, 0, 10_000_000))
        assembler.submit(sample(1, 1, 11_000_000))
        window = assembler.submit(sample(2, 1, 11_000_000))[0]
        self.assertEqual((window.run_id, window.window_id), (2, 0))

    def test_one_submission_can_complete_multiple_buffered_windows(self):
        assembler = self.make_assembler(rate=1)
        assembler.start_run(1, 0)
        for second in range(4):
            assembler.submit(sample(1, second, second * 1_000_000))
        assembler.submit(sample(2, 0, 0))
        windows = assembler.submit(sample(2, 3, 3_000_000))
        self.assertEqual([window.window_id for window in windows], [0, 1, 2])

    def test_missing_sequence_diagnostic_is_capped(self):
        assembler = self.make_assembler(rate=2)
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler, [sample(1, 0, 0), sample(1, 1000, 500_000)],
            [sample(2, 0, 0), sample(2, 1, 500_000)])
        self.assertEqual(len(windows[0].quality.missing_sequences[1]), 100)
        self.assertFalse(windows[0].quality.accepted)

    def test_skew_does_not_reject_complete_window(self):
        assembler = self.make_assembler(rate=1)
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler, [sample(1, 0, 0)], [sample(2, 0, 900_000)])
        self.assertTrue(windows[0].quality.accepted)
        self.assertEqual(windows[0].quality.max_index_skew_us, 900_000)


if __name__ == "__main__":
    unittest.main()
