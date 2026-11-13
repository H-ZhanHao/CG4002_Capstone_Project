"""Model-free routing, evidence, and stale-result checks for dynamic AI mode."""
from types import SimpleNamespace as NS
import threading
import unittest
from unittest.mock import Mock

from dynamic_inference import InferencePipeline


def window(nodes=(1,), index=0):
    return NS(run_id=7, window_id=index, start_us=index * 1_000_000,
              end_us=(index + 3) * 1_000_000, quality=NS(accepted=True),
              samples_by_node={node: () for node in nodes})


def result(w, exercise="squat"):
    return NS(capture_run_id=w.run_id, window_id=w.window_id, start_us=w.start_us,
              end_us=w.end_us, exercise=exercise, confidence=0.95,
              class_id=0, model_window_count=1)


class DynamicInferenceTests(unittest.TestCase):
    def setUp(self):
        self.assembler = Mock()
        self.assembler.submit.side_effect = lambda w: [w]
        self.model = Mock()
        self.model.infer_window.side_effect = result
        self.publisher = Mock()
        self.pipeline = InferencePipeline(self.assembler, self.model, self.publisher)
        self.addCleanup(self.pipeline.close)
        self.pipeline.start_run(7, 0)

    def feed(self, w):
        self.pipeline.submit(w)
        # Event in FIFO order gives a bounded wait without timing assumptions.
        done = threading.Event()
        # A queue join in a daemon thread avoids an unbounded test hang.
        waiter = threading.Thread(target=lambda: (self.pipeline._queue.join(), done.set()),
                                  daemon=True)
        waiter.start()
        self.assertTrue(done.wait(2), "inference worker stalled")
        waiter.join(0.1)

    def test_routes_either_single_node_and_fusion(self):
        for nodes in ((1,), (2,), (1, 2)):
            self.feed(window(nodes))
        self.assertEqual(self.model.infer_window.call_count, 3)
        self.assertEqual([tuple(call.args[0].samples_by_node)
                          for call in self.model.infer_window.call_args_list],
                         [(1,), (2,), (1, 2)])

    def test_rejected_window_never_reaches_model(self):
        w = window()
        w.quality.accepted = False
        self.feed(w)
        self.model.infer_window.assert_not_called()

    def test_five_idle_results_confirm_single_node_idle(self):
        self.model.infer_window.side_effect = lambda w: result(w, "idle")
        self.pipeline.on_idle = Mock()
        for index in range(4):
            self.feed(window(index=index))
        self.pipeline.on_idle.assert_not_called()
        self.feed(window(index=4))
        self.pipeline.on_idle.assert_called_once()
        r, generation = self.pipeline.on_idle.call_args.args
        self.assertTrue(self.pipeline.accept_idle(r, generation))

    def test_modality_change_resets_idle_evidence(self):
        self.model.infer_window.side_effect = lambda w: result(w, "idle")
        self.pipeline.on_idle = Mock()
        for index in range(4):
            self.feed(window(index=index))
        self.feed(window((1, 2), index=4))
        self.pipeline.on_idle.assert_not_called()
        self.assertEqual(self.pipeline._consecutive_idle, 1)

    def test_reset_discards_in_flight_result(self):
        entered, release = threading.Event(), threading.Event()
        def infer(w):
            entered.set()
            release.wait(2)
            return result(w)
        self.model.infer_window.side_effect = infer
        self.pipeline.submit(window())
        self.assertTrue(entered.wait(1))
        self.pipeline.stop_run("test reset")
        release.set()
        self.pipeline._worker.join(0.02)
        self.publisher.publish_classification.assert_not_called()

    def test_wrong_window_result_is_not_published(self):
        self.model.infer_window.side_effect = lambda w: result(window(index=99))
        with self.assertLogs("server", level="ERROR"):
            self.feed(window())
        self.publisher.publish_classification.assert_not_called()


if __name__ == "__main__":
    unittest.main()
