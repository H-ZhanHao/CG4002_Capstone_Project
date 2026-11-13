"""Presence-aware inference worker for the opt-in one-node server mode.

Preserves the deployed pipeline's bounded queue, generation checks, five idle
results and two exercise evidence scores (recorded in stage4_fixtures).
Model implementation is supplied separately; missing-node inputs are never
fabricated or sent to the two-node model.
"""

import logging
import math
import queue
import threading

LOGGER = logging.getLogger("server")
IDLE_CLASSIFICATIONS_REQUIRED = 5
EXERCISE_SCORE_REQUIRED = 2
MIN_CLASSIFICATION_CONFIDENCE = 0.80


class InferencePipeline:
    """Wrap an assembler with the same start_run/stop_run/submit interface.

    Serializing submission preserves window order across node receiver threads.
    A generation token prevents completed or queued work surviving a capture
    reset, even if a later capture reuses a protocol run ID.
    """

    def __init__(self, assembler, inference, publisher, queue_size=8, *, on_idle=None):
        if queue_size <= 0:
            raise ValueError("inference queue size must be positive")
        self.assembler = assembler
        self.inference = inference
        self.publisher = publisher
        self.on_idle = on_idle
        self._queue = queue.Queue(maxsize=queue_size)
        self._lock = threading.Lock()
        self._generation = 0
        self._active = False
        self._consecutive_idle = 0
        self._input_nodes = None
        self._exercise_scores = {}
        # Preserve the last published state across captures; only evidence resets.
        self._last_published_exercise = None
        self._stopping = threading.Event()
        self._worker = threading.Thread(
            target=self._run, name="imu-inference", daemon=True)
        self._worker.start()

    def _discard_queued_locked(self):
        while True:
            try:
                self._queue.get_nowait()
            except queue.Empty:
                break
            self._queue.task_done()

    def start_run(self, run_id, start_us):
        with self._lock:
            if self._stopping.is_set():
                return
            self._generation += 1
            self._active = True
            self._consecutive_idle = 0
            self._exercise_scores.clear()
            self._discard_queued_locked()
            self.publisher.set_capture(self._generation)
            self.assembler.start_run(run_id, start_us)

    def stop_run(self, reason):
        with self._lock:
            self._active = False
            self._consecutive_idle = 0
            self._exercise_scores.clear()
            self._generation += 1
            self._discard_queued_locked()
            self.publisher.set_capture(None)
            self.assembler.stop_run(reason)

    def submit(self, sample):
        with self._lock:
            if not self._active or self._stopping.is_set():
                return []
            windows = self.assembler.submit(sample)
            for window in windows:
                if not window.quality.accepted:
                    continue
                try:
                    self._queue.put_nowait((self._generation, window))
                except queue.Full:
                    LOGGER.warning("INFERENCE_DROP run=%d window=%d reason=queue_full",
                                   window.run_id, window.window_id)
            return windows

    def accept_idle(self, result, generation):
        """Commit a confirmed idle streak while the coordinator holds its lock.

        The coordinator serializes this with capture transitions, then calls
        idle_pause after this method releases the pipeline lock. The terminal
        idle update is independent of the capture reset that follows it.
        """
        with self._lock:
            if (self._stopping.is_set() or not self._active or
                    generation != self._generation or result.exercise != "idle" or
                    not self._confident(result) or
                    self._consecutive_idle < IDLE_CLASSIFICATIONS_REQUIRED):
                return False
            self._active = False
            self._discard_queued_locked()
            self.publisher.set_capture(None)
            self.publisher.publish_classification(result, None)
            self._last_published_exercise = "idle"
            return True

    @staticmethod
    def _confident(result):
        return (math.isfinite(result.confidence) and
                MIN_CLASSIFICATION_CONFIDENCE <= result.confidence <= 1.0)

    def _run(self):
        while not self._stopping.is_set():
            try:
                generation, window = self._queue.get(timeout=0.1)
            except queue.Empty:
                continue
            try:
                with self._lock:
                    if not self._active or generation != self._generation:
                        continue
                nodes = tuple(sorted(window.samples_by_node))
                if nodes not in ((1,), (2,), (1, 2)):
                    raise ValueError(f"unsupported AI input nodes: {nodes}")
                # Agreed AI contract: infer_window accepts the available-node
                # dictionary directly, for either single node or both nodes.
                result = self.inference.infer_window(window)
                if (result.capture_run_id, result.window_id, result.start_us, result.end_us) != (
                        window.run_id, window.window_id, window.start_us, window.end_us):
                    raise ValueError("AI result does not match the submitted capture window")
                with self._lock:
                    if not self._active or generation != self._generation:
                        LOGGER.info("INFERENCE_DISCARDED run=%d window=%d reason=capture_reset",
                                    window.run_id, window.window_id)
                        continue
                    LOGGER.info(
                        "CLASSIFICATION run=%d window=%d interval=[%d,%d) "
                        "class_id=%d exercise=%s mean_probability=%.6f model_windows=%d",
                        result.capture_run_id, result.window_id, result.start_us,
                        result.end_us, result.class_id, result.exercise,
                        result.confidence, result.model_window_count)
                    # Evidence from one modality must not confirm another's idle.
                    if nodes != self._input_nodes:
                        self._input_nodes = nodes
                        self._consecutive_idle = 0
                        self._exercise_scores.clear()
                    confident = self._confident(result)
                    candidate = (result.exercise if confident and
                                 result.exercise != "idle" else None)
                    if candidate is not None:
                        self._exercise_scores.setdefault(candidate, 0)
                    for exercise, score in self._exercise_scores.items():
                        self._exercise_scores[exercise] = (
                            min(EXERCISE_SCORE_REQUIRED, score + 1)
                            if exercise == candidate else max(0, score - 1))

                    if result.exercise == "idle" and confident:
                        self._consecutive_idle = min(
                            IDLE_CLASSIFICATIONS_REQUIRED, self._consecutive_idle + 1)
                    else:
                        self._consecutive_idle = 0

                    if result.exercise == "idle":
                        LOGGER.info(
                            "IDLE_STREAK run=%d window=%d count=%d required=%d",
                            window.run_id, window.window_id,
                            self._consecutive_idle, IDLE_CLASSIFICATIONS_REQUIRED)
                        # Suppress both the visualizer update and capture
                        # control until five consecutive confident idle results.
                        if self._consecutive_idle < IDLE_CLASSIFICATIONS_REQUIRED:
                            continue
                        idle_handler = self.on_idle
                    else:
                        if (candidate is None or
                                self._exercise_scores[candidate] < EXERCISE_SCORE_REQUIRED):
                            continue
                        idle_handler = None
                    if (idle_handler is None and
                            result.exercise != self._last_published_exercise):
                        self.publisher.publish_classification(result, generation)
                        self._last_published_exercise = result.exercise
                # Capture control takes coordinator -> pipeline locks. Calling
                # it under our lock would deadlock against stop_run/start_run.
                # The handler must revalidate generation under that lock order.
                if idle_handler is not None:
                    idle_handler(result, generation)
            except Exception:
                LOGGER.exception("INFERENCE_FAILED run=%d window=%d",
                                 window.run_id, window.window_id)
            finally:
                self._queue.task_done()

    def close(self):
        self._stopping.set()
        self.stop_run("inference shutdown")
        self._worker.join(timeout=1.0)
