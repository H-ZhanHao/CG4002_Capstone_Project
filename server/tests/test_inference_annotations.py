"""Hardware-free checks for the dashboard's inference provenance and lifecycle."""

import json
import threading
import time
import unittest
from dataclasses import replace
from types import SimpleNamespace

from inference_pipeline import InferencePipeline
from live_dashboard import Telemetry
from window_assembler_updated import AssembledWindow, DecodedImuSample, WindowQuality


def make_window(number=0, accepted=True, duration=3_000_000):
    start = 10_000_000 + number * 1_000_000
    samples = {node: tuple(DecodedImuSample(
        node, 7, number * 100 + i, start + i * 10_000, start + i * 10_000,
        i, i, i, i, i, i, 1, 0) for i in range(duration // 10_000)) for node in (1, 2)}
    quality = WindowQuality(duration // 10_000, {n: len(s) for n, s in samples.items()},
                            {}, {}, {}, {}, 0, accepted, None if accepted else "missing_samples")
    return AssembledWindow(1, number, start, start + duration, samples, quality)


def result_for(window):
    return SimpleNamespace(capture_run_id=window.run_id, window_id=window.window_id,
                           start_us=window.start_us, end_us=window.end_us,
                           class_id=1, exercise="squat", confidence=0.94, model_window_count=1)


class ControlledInference:
    def __init__(self):
        self.entered = threading.Event()
        self.release = threading.Event()
        self.calls = []
        self.fail = False
        self.confidence = 0.94

    @staticmethod
    def model_intervals(window):
        return [(window.start_us, window.end_us)]

    def infer_window(self, window):
        self.calls.append(window.window_id)
        self.entered.set()
        if not self.release.wait(3):
            raise TimeoutError("test did not release inference")
        if self.fail:
            raise ValueError("simulated inference failure")
        result = result_for(window)
        result.confidence = self.confidence
        return result


class AnnotationTests(unittest.TestCase):
    def setUp(self):
        self.telemetry = Telemetry()

    def register(self, window, generation=1):
        for samples in window.samples_by_node.values():
            self.telemetry.sample(samples[-1], generation=8)
        self.telemetry.inference_window(window, generation, [(window.start_us, window.end_us)])

    def records(self):
        return self.telemetry.snapshot()["inference_windows"]

    def wait_for(self, predicate):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.005)
        self.fail("timed out waiting for worker")

    def pipeline(self):
        model = ControlledInference()
        assembler = SimpleNamespace(start_run=lambda *_: None, stop_run=lambda *_: None,
                                    submit=lambda window: [window])
        publisher = SimpleNamespace(set_capture=lambda *_: None, publish_classification=lambda *_: None)
        pipeline = InferencePipeline(assembler, model, publisher, telemetry=self.telemetry)
        self.addCleanup(pipeline.close)
        self.addCleanup(model.release.set)
        pipeline.start_run(1, 10_000_000)
        return pipeline, model

    def test_result_updates_original_window_and_snapshot_is_stable(self):
        window = make_window()
        self.register(window)
        snapshot = self.telemetry.snapshot()
        self.telemetry.inference_status(window, 1, "processing")
        self.telemetry.prediction(result_for(window), generation=1)
        self.telemetry.prediction(result_for(window), confirmed=True, generation=1)
        record = self.records()[0]
        self.assertEqual(snapshot["inference_windows"][0]["status"], "queued")
        self.assertEqual(record["start_us"], window.start_us)
        self.assertEqual(record["end_us"], window.end_us)
        self.assertEqual(record["exercise"], "squat")
        self.assertTrue(record["confirmed"])
        self.assertTrue(record["sent_for_inference"])
        self.assertEqual(record["nodes"]["1"]["generation"], 8)

    def test_provenance_uses_input_epoch_even_if_new_capture_has_arrived(self):
        window = make_window()
        self.register(window)
        for samples in window.samples_by_node.values():
            self.telemetry.sample(replace(samples[-1], sequence=0, coordinator_timestamp_us=99_000_000),
                                  generation=9)
        self.telemetry.inference_window(window, 2, [(window.start_us, window.end_us)])
        self.assertEqual(self.records()[-1]["nodes"]["1"]["generation"], 8)

    def test_reconnect_restores_bounded_history_and_revision_updates(self):
        for number in range(105):
            self.register(make_window(number))
        snapshot = self.telemetry.snapshot()
        self.assertEqual(len(snapshot["inference_windows"]), 100)
        self.assertEqual(snapshot["inference_windows"][0]["window_id"], 5)
        batch = self.telemetry.snapshot(snapshot["cursor"], False, snapshot["inference_revision"])
        self.assertNotIn("inference_windows", batch)
        self.telemetry.inference_status(make_window(104), 1, "processing")
        batch = self.telemetry.snapshot(snapshot["cursor"], False, snapshot["inference_revision"])
        self.assertEqual(batch["inference_windows"][-1]["status"], "processing")
        self.assertEqual(len(self.telemetry.snapshot(snapshot["cursor"])["inference_windows"]), 100)
        json.dumps(batch, allow_nan=False)

    def test_slow_worker_replaces_pending_window_without_false_classification(self):
        pipeline, model = self.pipeline()
        pipeline.submit(make_window(0))
        self.assertTrue(model.entered.wait(1))
        self.assertEqual(self.records()[0]["status"], "processing")
        pipeline.submit(make_window(1))
        pipeline.submit(make_window(2))
        self.assertEqual([r["status"] for r in self.records()], ["processing", "skipped", "queued"])
        self.assertFalse(self.records()[1]["sent_for_inference"])
        model.release.set()
        self.wait_for(lambda: self.records()[-1]["confirmed"])
        self.assertEqual(model.calls, [0, 2])
        self.assertTrue(self.records()[-1]["confirmed"])

    def test_low_confidence_result_stays_visible_without_confirmation(self):
        pipeline, model = self.pipeline()
        model.confidence = 0.65
        model.release.set()
        pipeline.submit(make_window())
        self.wait_for(lambda: self.records()[0]["status"] == "classified")
        record = self.records()[0]
        self.assertEqual(record["exercise"], "squat")
        self.assertEqual(record["confidence"], 0.65)
        self.assertFalse(record["confirmed"])

    def test_rejected_and_failed_windows_are_not_classified(self):
        pipeline, model = self.pipeline()
        pipeline.submit(make_window(0, accepted=False))
        self.assertEqual(self.records()[0]["status"], "rejected")
        self.assertFalse(model.entered.is_set())
        model.fail = True
        model.release.set()
        with self.assertLogs("server", level="ERROR"):
            pipeline.submit(make_window(1))
            self.wait_for(lambda: self.records()[-1]["status"] == "failed" and
                          pipeline._queue.unfinished_tasks == 0)
        self.assertTrue(self.records()[-1]["sent_for_inference"])
        self.assertNotIn("exercise", self.records()[-1])

    def test_late_result_cannot_attach_to_reused_window_after_reset(self):
        pipeline, model = self.pipeline()
        window = make_window()
        pipeline.submit(window)
        self.assertTrue(model.entered.wait(1))
        pipeline.submit(make_window(1))
        pipeline.stop_run("test reset")
        pipeline.start_run(1, window.start_us)
        pipeline.submit(window)
        self.assertEqual([r["status"] for r in self.records()], ["cancelled", "cancelled", "queued"])
        model.release.set()
        self.wait_for(lambda: self.records()[-1]["status"] == "classified")
        self.assertEqual(self.records()[0]["status"], "cancelled")
        self.assertNotIn("exercise", self.records()[0])
        self.assertNotEqual(self.records()[0]["key"], self.records()[-1]["key"])

    def test_model_intervals_match_consumed_samples_and_exclude_unused_tail(self):
        import numpy as np
        from config import ModelConfig
        from late_fusion_vart import LateFusionInference

        inference = LateFusionInference.__new__(LateFusionInference)
        inference.cfg = ModelConfig()
        inference.stride_us = 500_000
        inference.min_samples = 240
        consumed = []

        def predict(wrist, thigh):
            consumed.append((wrist[0, 0], wrist[-1, 0], thigh.shape))
            probabilities = np.zeros(inference.cfg.num_classes, dtype=np.float32)
            probabilities[1] = 1
            return probabilities

        inference.predict_pair = predict
        window = make_window(duration=5_200_000)
        result = inference.infer_window(window)
        intervals = inference.model_intervals(window)
        self.assertEqual(intervals[-1][1], window.end_us - 200_000)
        self.assertEqual(result.model_window_count, 5)
        self.assertEqual(consumed, [(i * 50, i * 50 + 299, (300, 6)) for i in range(5)])


class SingleNodeInferenceTests(unittest.TestCase):
    def setUp(self):
        import numpy as np
        from unittest.mock import Mock
        from config import ModelConfig
        from late_fusion_vart import LateFusionInference

        self.np = np
        self.inference = LateFusionInference.__new__(LateFusionInference)
        self.inference.cfg = ModelConfig()
        self.inference.stride_us = 500_000
        self.inference.min_samples = 240
        self.inference.wrist = SimpleNamespace(run=Mock(return_value=np.full(64, 2.0)))
        self.inference.thigh = SimpleNamespace(run=Mock(return_value=np.full(64, 3.0)))
        self.inference.fusion = SimpleNamespace(run=Mock(return_value=np.arange(7)))

    def assert_fusion_input(self, nodes):
        vector = self.inference.fusion.run.call_args.args[0]
        self.assertEqual(vector.shape, (130,))
        self.np.testing.assert_array_equal(vector[-2:], [2 in nodes, 1 in nodes])
        self.np.testing.assert_array_equal(vector[:64], 2.0 if 2 in nodes else 0.0)
        self.np.testing.assert_array_equal(vector[64:128], 3.0 if 1 in nodes else 0.0)
        self.assertEqual(self.inference.wrist.run.call_count, int(2 in nodes))
        self.assertEqual(self.inference.thigh.run.call_count, int(1 in nodes))

    def test_available_nodes_set_the_correct_fusion_mask(self):
        for nodes in ((1,), (2,), (1, 2)):
            with self.subTest(nodes=nodes):
                for model in (self.inference.wrist, self.inference.thigh, self.inference.fusion):
                    model.run.reset_mock()
                window = make_window()
                window = replace(window, samples_by_node={n: window.samples_by_node[n] for n in nodes})
                result = self.inference.infer_window(window)
                self.assertEqual(result.exercise, "idle")
                self.assertEqual(result.model_window_count, 1)
                self.assert_fusion_input(nodes)

    def test_lagging_node_is_omitted_by_assembler_and_masked_by_inference(self):
        from window_assembler_updated import WindowAssembler, WindowConfig

        window = make_window()
        assembler = WindowAssembler(WindowConfig(100, 3_000_000, 1_000_000))
        assembler.start_run(1, window.start_us)
        for node in (1, 2):
            assembler.node_joined(node)
        # Reproduce the reported window: the lagging thigh has 110 samples,
        # while the wrist reaches the end plus the bounded waiting allowance.
        for sample in window.samples_by_node[1][:110]:
            self.assertFalse(assembler.submit(sample))
        for sample in window.samples_by_node[2]:
            self.assertFalse(assembler.submit(sample))
        tail = window.samples_by_node[2][-1]
        emitted = assembler.submit(replace(tail, sequence=325,
                                           timestamp_us=window.end_us + 250_000,
                                           coordinator_timestamp_us=window.end_us + 250_000))
        self.assertEqual(len(emitted), 1)
        accepted = emitted[0]
        self.assertEqual(accepted.quality.sample_counts[1], 110)
        self.assertEqual(accepted.quality.available_nodes, (2,))
        self.assertEqual(accepted.quality.node_rejection_reasons, {1: "missing_samples"})
        self.inference.infer_window(accepted)
        self.assert_fusion_input((2,))

    def test_no_usable_node_still_fails(self):
        window = replace(make_window(), samples_by_node={})
        with self.assertRaisesRegex(ValueError, "at least one IMU"):
            self.inference.infer_window(window)
        self.inference.fusion.run.assert_not_called()

    def test_present_but_invalid_node_is_not_silently_masked(self):
        window = make_window()
        samples = window.samples_by_node[1]
        malformed = replace(window, samples_by_node={1: (samples[1], samples[0])})
        with self.assertRaisesRegex(ValueError, "invalid timestamps"):
            self.inference.infer_window(malformed)
        insufficient = replace(window, samples_by_node={1: samples[:100]})
        with self.assertRaisesRegex(ValueError, "insufficient model-window coverage"):
            self.inference.infer_window(insufficient)
        self.inference.fusion.run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
