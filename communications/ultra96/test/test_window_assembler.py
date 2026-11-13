import unittest
from dataclasses import replace

from window_assembler import DecodedImuSample, WindowAssembler, WindowConfig


def sample(node, sequence, timestamp):
    return DecodedImuSample(
        node_id=node, session_id=node * 10, sequence=sequence,
        timestamp_us=timestamp, coordinator_timestamp_us=timestamp,
        ax=1, ay=2, az=3, gx=4, gy=5, gz=6,
        status=0, flags=0)


class WindowConfigTest(unittest.TestCase):
    def test_proportional_default_and_explicit_override(self):
        self.assertEqual(WindowConfig(100, 3_000_000, 1_000_000).missing_limit, 9)
        self.assertEqual(WindowConfig(50, 5_000_000, 1_000_000).missing_limit, 7)
        self.assertEqual(WindowConfig(100, 3_000_000, 1_000_000,
                                      max_missing_per_node=5).missing_limit, 5)

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
            dict(sample_rate_hz=1, window_us=1_000_000, hop_us=1,
                 max_missing_per_node=-1),
            dict(sample_rate_hz=1, window_us=1_000_000, hop_us=1,
                 max_extra_per_node=-1),
            dict(sample_rate_hz=100, window_us=3_000_000, hop_us=1_000_000,
                 max_missing_fraction=float("nan")),
            dict(sample_rate_hz=100, window_us=3_000_000, hop_us=1_000_000,
                 max_missing_fraction=1),
            dict(sample_rate_hz=100, window_us=3_000_000, hop_us=1_000_000,
                 max_missing_fraction=-0.01),
        )
        for values in invalid:
            with self.subTest(values=values), self.assertRaises(ValueError):
                WindowConfig(**values)


class WindowAssemblerTest(unittest.TestCase):
    def make_assembler(self, rate=2, window_us=1_000_000,
                       hop_us=1_000_000):
        # Legacy low-rate fixtures explicitly exercise the original count policy;
        # production percentage defaults are covered separately below.
        return WindowAssembler(WindowConfig(
            rate, window_us, hop_us, max_missing_per_node=5))

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
        self.assertEqual(windows[0].quality.padded_counts, {1: 0, 2: 0})
        self.assertEqual(windows[0].quality.dropped_counts, {1: 0, 2: 0})
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

    def test_up_to_five_missing_per_node_are_zero_padded(self):
        assembler = self.make_assembler(rate=10)
        assembler.start_run(1, 0)
        node1 = [sample(1, sequence, sequence * 100_000)
                 for sequence in (0, 1, 4, 5, 6, 7)]
        node2 = [sample(2, sequence, sequence * 100_000)
                 for sequence in (0, 2, 3, 7, 8)]
        windows = self.complete(assembler, node1, node2)
        window = windows[0]
        self.assertTrue(window.quality.accepted)
        self.assertEqual(window.quality.sample_counts, {1: 6, 2: 5})
        self.assertEqual(window.quality.padded_counts, {1: 4, 2: 5})
        self.assertEqual(len(window.samples_by_node[1]), 10)
        self.assertEqual(len(window.samples_by_node[2]), 10)
        self.assertEqual(
            [item.sequence for item in window.samples_by_node[1]],
            list(range(10)))
        self.assertEqual(
            [item.sequence for item in window.samples_by_node[2]],
            list(range(10)))
        self.assertTrue(all(
            (item.ax, item.ay, item.az, item.gx, item.gy, item.gz) ==
            (0, 0, 0, 0, 0, 0)
            for samples in window.samples_by_node.values()
            for item in samples if item.is_padding))

    def test_more_than_five_missing_from_either_node_rejects_window(self):
        assembler = self.make_assembler(rate=10)
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler,
            [sample(1, sequence, sequence * 100_000) for sequence in range(4)],
            [sample(2, sequence, sequence * 100_000) for sequence in range(10)],
        )
        self.assertFalse(windows[0].quality.accepted)
        self.assertEqual(windows[0].quality.rejection_reason, "missing_samples")
        self.assertEqual(windows[0].quality.padded_counts, {1: 0, 2: 0})

    def test_small_count_mismatch_is_padded(self):
        assembler = self.make_assembler()
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler, [sample(1, 0, 0)],
            [sample(2, 0, 0), sample(2, 1, 500_000)])
        self.assertTrue(windows[0].quality.accepted)
        self.assertEqual(windows[0].quality.padded_counts, {1: 1, 2: 0})

    def test_gap_without_count_deficit_still_rejects_window(self):
        assembler = self.make_assembler()
        assembler.start_run(1, 0)
        windows = self.complete(
            assembler, [sample(1, 0, 0), sample(1, 2, 500_000)],
            [sample(2, 0, 0), sample(2, 1, 500_000)])
        self.assertFalse(windows[0].quality.accepted)
        self.assertEqual(windows[0].quality.padded_counts, {1: 0, 2: 0})

    def test_one_unique_extra_drops_worst_timestamp_grid_match(self):
        assembler = self.make_assembler(rate=4)
        assembler.start_run(1, 0)
        node1 = [
            sample(1, 0, 0), sample(1, 1, 249_000),
            sample(1, 2, 251_000), sample(1, 3, 500_000),
            sample(1, 4, 750_000),
        ]
        node2 = [sample(2, sequence, sequence * 250_000)
                 for sequence in range(4)]
        window = self.complete(assembler, node1, node2)[0]
        self.assertTrue(window.quality.accepted)
        self.assertEqual(window.quality.sample_counts, {1: 5, 2: 4})
        self.assertEqual(window.quality.dropped_counts, {1: 1, 2: 0})
        self.assertEqual(window.quality.dropped_sequences[1], (2,))
        self.assertEqual(
            [item.sequence for item in window.samples_by_node[1]],
            [0, 1, 3, 4])

    def test_identical_duplicate_is_dropped(self):
        assembler = self.make_assembler(rate=4)
        assembler.start_run(1, 0)
        duplicate = sample(1, 1, 250_000)
        node1 = [sample(1, 0, 0), duplicate, duplicate,
                 sample(1, 2, 500_000), sample(1, 3, 750_000)]
        node2 = [sample(2, sequence, sequence * 250_000)
                 for sequence in range(4)]
        window = self.complete(assembler, node1, node2)[0]
        self.assertTrue(window.quality.accepted)
        self.assertEqual(window.quality.dropped_sequences[1], (1,))
        self.assertEqual(len(window.samples_by_node[1]), 4)

    def test_conflicting_duplicate_is_rejected(self):
        assembler = self.make_assembler(rate=4)
        assembler.start_run(1, 0)
        original = sample(1, 1, 250_000)
        conflicting = replace(original, ax=999)
        node1 = [sample(1, 0, 0), original, conflicting,
                 sample(1, 2, 500_000), sample(1, 3, 750_000)]
        node2 = [sample(2, sequence, sequence * 250_000)
                 for sequence in range(4)]
        window = self.complete(assembler, node1, node2)[0]
        self.assertFalse(window.quality.accepted)
        self.assertEqual(window.quality.rejection_reason,
                         "conflicting_duplicate")

    def test_more_than_two_extra_samples_rejects_window(self):
        assembler = self.make_assembler(rate=4)
        assembler.start_run(1, 0)
        node1 = [sample(1, sequence, sequence * 100_000)
                 for sequence in range(7)]
        node2 = [sample(2, sequence, sequence * 250_000)
                 for sequence in range(4)]
        window = self.complete(assembler, node1, node2)[0]
        self.assertFalse(window.quality.accepted)
        self.assertEqual(window.quality.rejection_reason,
                         "too_many_extra_samples")

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


class PhysicalWindowPolicyTest(unittest.TestCase):
    """Exercise real 100 Hz/3-second defaults and the AI timestamp contract."""

    def assemble(self, node1, node2=None, config=None):
        assembler = WindowAssembler(config or WindowConfig(100, 3_000_000, 1_000_000))
        assembler.start_run(1, 1_000_000)
        output = []
        if node2 is None:
            node2 = [sample(2, i, 1_000_000 + i * 10_000) for i in range(300)]
        for node_samples in (node1, node2):
            for item in node_samples:
                # Keep a nonzero offset between local and coordinator clocks.
                output.extend(assembler.submit(replace(
                    item, timestamp_us=item.coordinator_timestamp_us + 8_000_000)))
        for node in (1, 2):
            output.extend(assembler.submit(sample(node, 1000, 4_000_000)))
        self.assertEqual(len(output), 1)
        return output[0]

    def assert_valid_times(self, window):
        self.assertTrue(window.quality.accepted, window.quality.rejection_reason)
        for samples in window.samples_by_node.values():
            self.assertEqual(len(samples), 300)
            self.assertTrue(all(1_000_000 <= x.coordinator_timestamp_us < 4_000_000
                                for x in samples))
            for left, right in zip(samples, samples[1:]):
                self.assertGreater(right.timestamp_us, left.timestamp_us)
                self.assertGreater(right.coordinator_timestamp_us,
                                   left.coordinator_timestamp_us)
            self.assertTrue(all(x.timestamp_us - x.coordinator_timestamp_us == 8_000_000
                                for x in samples))

    def test_nine_missing_accepted_ten_rejected(self):
        for missing, accepted in ((9, True), (10, False)):
            with self.subTest(missing=missing):
                # Spread missing captures rather than introduce a long outage.
                removed = set(range(10, 10 + missing * 20, 20))
                readings = [sample(1, i, 1_000_000 + i * 10_000)
                            for i in range(300) if i not in removed]
                window = self.assemble(readings)
                self.assertEqual(window.quality.accepted, accepted)
                if accepted:
                    self.assert_valid_times(window)
                    self.assertEqual(window.quality.padded_counts[1], 9)
                else:
                    self.assertEqual(window.quality.rejection_reason, "missing_samples")

    def test_timing_slowdown_does_not_collapse_trailing_padding(self):
        # Reproduces the log's ~296/300 with contiguous capture sequences:
        # real samples span almost the whole window, leaving little edge space.
        readings = [sample(1, i, 1_000_000 + round(i * 2_990_000 / 295))
                    for i in range(296)]
        window = self.assemble(readings)
        self.assert_valid_times(window)
        actual = [x for x in window.samples_by_node[1] if not x.is_padding]
        self.assertEqual([x.coordinator_timestamp_us for x in actual],
                         [x.coordinator_timestamp_us for x in readings])

    def test_leading_internal_and_trailing_padding(self):
        for removed in (set(range(9)), set(range(140, 149)), set(range(291, 300))):
            with self.subTest(removed=removed):
                readings = [sample(1, i, 1_000_000 + i * 10_000)
                            for i in range(300) if i not in removed]
                self.assert_valid_times(self.assemble(readings))

    def test_long_gap_alone_does_not_reject_valid_full_window(self):
        readings = [sample(1, i, 1_000_000 + round(i * 2_800_000 / 299)
                           + (150_000 if i >= 150 else 0))
                    for i in range(300)]
        window = self.assemble(readings)
        self.assert_valid_times(window)

    def test_long_edge_gap_alone_does_not_reject_valid_full_window(self):
        readings = [sample(1, i, 1_150_000 + round(i * 2_840_000 / 299))
                    for i in range(300)]
        window = self.assemble(readings)
        self.assert_valid_times(window)

    def test_equal_real_timestamps_rejected(self):
        readings = [sample(1, i, 1_000_000 + i * 10_000) for i in range(300)]
        readings[100] = replace(readings[100], timestamp_us=1_990_000,
                                coordinator_timestamp_us=1_990_000)
        self.assertEqual(self.assemble(readings).quality.rejection_reason,
                         "invalid_timestamps")

    def test_insufficient_integer_timestamp_space_rejects_padding(self):
        readings = [sample(1, i, 1_000_000 + round(i * 2_999_999 / 295))
                    for i in range(296)]
        window = self.assemble(readings)
        self.assertFalse(window.quality.accepted)

    def test_zero_missing_policy_rejects_any_padding(self):
        readings = [sample(1, i, 1_000_000 + i * 10_000) for i in range(299)]
        window = self.assemble(readings, config=WindowConfig(
            100, 3_000_000, 1_000_000, max_missing_fraction=0))
        self.assertFalse(window.quality.accepted)


if __name__ == "__main__":
    unittest.main()
