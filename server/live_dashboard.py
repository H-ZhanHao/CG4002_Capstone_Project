"""Bounded, read-only LAN telemetry dashboard. Uses only the Python stdlib.

Sensor workers only copy small records into memory. HTTP serialization and slow
browser writes happen on separate threads; no sensor thread does web I/O.
Run ``python live_dashboard.py --demo`` for a hardware-free demonstration.
"""

import argparse
from collections import deque
from dataclasses import asdict, is_dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
import math
from pathlib import Path
from socketserver import TCPServer
import threading
import time
from types import SimpleNamespace
from urllib.parse import urlsplit
import uuid


def json_safe(value):
    """Keep diagnostics JSON-compatible, including nonfinite model results."""
    if is_dataclass(value) and not isinstance(value, type):
        value = asdict(value)
    if isinstance(value, dict):
        return {str(k): json_safe(v) for k, v in value.items()}
    if isinstance(value, (tuple, list)):
        return [json_safe(v) for v in value]
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    return str(value)[:1000]


class Telemetry:
    def __init__(self, sample_capacity=6000, demo=False, connection_timeout_s=3.0,
                 clock=None):
        if not math.isfinite(connection_timeout_s) or connection_timeout_s <= 0:
            raise ValueError("connection timeout must be finite and positive")
        self.connection_timeout_s = connection_timeout_s
        self._clock = clock or time.monotonic
        self.run = uuid.uuid4().hex
        self.demo = demo
        self._lock = threading.Lock()
        self._samples = deque(maxlen=sample_capacity)
        self._events = deque(maxlen=100)
        self._nodes = {}
        self._cursor = 0
        self._event_id = 0
        self._inference_windows = {}
        self._inference_revision = 0
        self._state = {"capture": "starting", "prediction": None,
                       "confirmed": None, "window": None, "device_event": None}

    def connection_opened(self, session_id, node_ids):
        """A HELLO establishes identity, but heartbeat/DATA establishes liveness."""
        now = self._clock()
        with self._lock:
            for node_id in node_ids:
                self._nodes[str(node_id)] = {
                    "session": session_id, "transport_connected": True,
                    "opened_at": now,
                }

    def connection_closed(self, session_id):
        """Ignore old connection cleanup after a node has a newer Session."""
        with self._lock:
            for node in self._nodes.values():
                if node.get("session") == session_id:
                    node["transport_connected"] = False

    def _active_node_locked(self, node_id, session_id, now):
        key = str(node_id)
        node = self._nodes.get(key)
        if node is None:
            # Also supports the standalone demo and telemetry-only callers.
            node = {"session": session_id, "transport_connected": True,
                    "opened_at": now}
            self._nodes[key] = node
        if node["session"] != session_id or not node["transport_connected"]:
            return None
        return node

    def _connection_state_locked(self, node, now):
        seen = node.get("last_seen_at")
        age = None if seen is None else max(0, now - seen)
        if not node:
            status, reason = "waiting", "No device connection yet"
        elif not node.get("transport_connected"):
            status, reason = "disconnected", "Connection closed"
        elif node.get("source_unavailable"):
            status, reason = "disconnected", "Device reports source unavailable"
        elif seen is None:
            if now - node["opened_at"] >= self.connection_timeout_s:
                status, reason = "disconnected", "No heartbeat or IMU data received"
            else:
                status, reason = "connecting", "Waiting for first heartbeat or IMU data"
        elif age >= self.connection_timeout_s:
            status, reason = "disconnected", "Heartbeat / IMU data timed out"
        else:
            status = "connected"
            reason = "Receiving heartbeats" if node["liveness_source"] == "heartbeat" else "Receiving IMU data"
        return {"status": status, "reason": reason, "last_seen_age_s": age,
                "timeout_s": self.connection_timeout_s,
                "source": node.get("liveness_source"), "session": node.get("session")}

    def sample(self, sample, physical=True, generation=0):
        now = self._clock()
        record = {
            "node": sample.node_id, "session": sample.session_id,
            "sequence": sample.sequence, "timestamp_us": sample.timestamp_us,
            "capture_us": sample.coordinator_timestamp_us,
            "axes": [sample.ax, sample.ay, sample.az, sample.gx, sample.gy, sample.gz],
            "status": sample.status, "historical": bool(sample.status & 2),
            "physical": physical, "generation": generation,
        }
        with self._lock:
            node = self._active_node_locked(sample.node_id, sample.session_id, now)
            if node is None:
                return
            self._cursor += 1
            record["id"] = self._cursor
            self._samples.append(record)
            node.update(last_seen_at=now, liveness_source="imu",
                        source_unavailable=bool(sample.status & 0x4000))
            # Capture-time cadence excludes history bursts and reception jitter.
            previous = node.get("sample")
            rate = None
            if previous and not record["historical"] and not previous["historical"]:
                dt = record["timestamp_us"] - previous["timestamp_us"]
                ds = record["sequence"] - previous["sequence"]
                if (record["session"] == previous["session"] and
                        record["generation"] == previous["generation"] and dt > 0 and ds > 0):
                    rate = round(ds * 1_000_000 / dt, 1)
            node.update(sample=record, received_at=now, sample_rate_hz=rate)

    def heartbeat(self, node_id, **values):
        now = self._clock()
        with self._lock:
            node = self._active_node_locked(node_id, values.get("session"), now)
            if node is None:
                return
            node.update(heartbeat=json_safe(values), heartbeat_at=now,
                        last_seen_at=now, liveness_source="heartbeat",
                        source_unavailable=bool(values.get("status", 0) & 0x4000))

    def update(self, **values):
        with self._lock:
            self._state.update(json_safe(values))

    def event(self, kind, message, **details):
        with self._lock:
            self._event_id += 1
            self._events.append({"id": self._event_id, "time": time.time(),
                                 "kind": kind, "message": str(message)[:1500],
                                 "details": json_safe(details)})

    def device_event(self, node_id, session_id, sequence, result):
        # The device-events module owns event meaning/deduplication. Do not
        # invent a fall count from transport packets that may be retries.
        details = dict(node=node_id, session=session_id, sequence=sequence,
                       result=json_safe(result), time=time.time())
        self.update(device_event=details)
        self.event("device", "Validated device-event report", **details)

    def window(self, window):
        self.update(window={"run_id": window.run_id, "id": window.window_id,
                            "start_us": window.start_us, "end_us": window.end_us,
                            "quality": asdict(window.quality)})

    def inference_window(self, window, generation, model_intervals):
        """Retain window provenance, independently of the latest status cards."""
        with self._lock:
            nodes = {}
            for node_id, samples in window.samples_by_node.items():
                if not samples:
                    continue
                last = samples[-1]
                # Sensor capture generations and pipeline generations are separate
                # counters. Recover the epoch of an actual input sample, not the
                # latest node state (which may already belong to a new capture).
                source = next((record for record in reversed(self._samples)
                               if record["node"] == node_id and
                               record["session"] == last.session_id and
                               record["sequence"] == last.sequence and
                               record["capture_us"] == last.coordinator_timestamp_us), {})
                nodes[str(node_id)] = {
                    "session": samples[0].session_id,
                    "generation": source.get("generation"),
                    "first_sequence": samples[0].sequence,
                    "last_sequence": samples[-1].sequence,
                }
            key = f"{generation}:{window.run_id}:{window.window_id}"
            self._inference_windows[key] = json_safe({
                "key": key, "generation": generation, "run_id": window.run_id,
                "window_id": window.window_id, "start_us": window.start_us,
                "end_us": window.end_us, "nodes": nodes,
                "model_intervals": model_intervals,
                "status": "queued" if window.quality.accepted else "rejected",
                "reason": window.quality.rejection_reason,
                "quality": asdict(window.quality), "confirmed": False,
                "sent_for_inference": False,
            })
            while len(self._inference_windows) > 100:
                del self._inference_windows[next(iter(self._inference_windows))]
            self._inference_revision += 1

    def inference_status(self, window, generation, status, reason=None):
        key = f"{generation}:{window.run_id}:{window.window_id}"
        with self._lock:
            record = self._inference_windows.get(key)
            if record is not None:
                record.update(status=status, reason=reason)
                if status == "processing":
                    record["sent_for_inference"] = True
                self._inference_revision += 1

    def cancel_inference(self, reason):
        with self._lock:
            for record in self._inference_windows.values():
                if record["status"] in ("queued", "processing"):
                    record.update(status="cancelled", reason=reason)
                    self._inference_revision += 1

    def prediction(self, result, confirmed=False, generation=None):
        value = {name: getattr(result, name, None) for name in (
            "capture_run_id", "window_id", "exercise", "confidence", "class_id",
            "start_us", "end_us", "model_window_count")}
        value["received_at"] = time.time()
        value = json_safe(value)
        with self._lock:
            self._state["confirmed" if confirmed else "prediction"] = value
            key = f"{generation}:{result.capture_run_id}:{result.window_id}"
            record = self._inference_windows.get(key)
            if record is not None:
                record.update(value, status="classified", reason=None,
                              sent_for_inference=True,
                              confirmed=record["confirmed"] or confirmed)
                self._inference_revision += 1

    def snapshot(self, cursor=None, include_state=True, inference_revision=None):
        """Copy under lock, serialize outside it. No per-client pending queues."""
        now = self._clock()
        with self._lock:
            reset = cursor is None or cursor > self._cursor
            lost = 0
            if self._samples and cursor is not None and not reset:
                lost = max(0, self._samples[0]["id"] - cursor - 1)
            if reset:
                samples = list(self._samples)[-2000:]
            else:
                samples = []
                for sample in reversed(self._samples):
                    if sample["id"] <= cursor:
                        break
                    samples.append(sample)
                samples.reverse()
            result = {"run": self.run, "cursor": self._cursor, "reset": reset,
                      "lost_samples": lost, "samples": samples, "demo": self.demo}
            result["inference_revision"] = self._inference_revision
            if reset or inference_revision != self._inference_revision:
                result["inference_windows"] = [dict(record) for record in
                                               self._inference_windows.values()]
            if include_state:
                result["state"] = dict(self._state)
                result["events"] = list(self._events)
                result["nodes"] = {
                    key: {**value,
                          "connection": self._connection_state_locked(value, now),
                          "sample_age_s": now - value["received_at"]
                          if "received_at" in value else None,
                          "heartbeat_age_s": now - value["heartbeat_at"]
                          if "heartbeat_at" in value else None}
                    for key, value in {"1": {}, "2": {}, **self._nodes}.items()
                }
        return result


class DashboardLogHandler(logging.Handler):
    def __init__(self, telemetry):
        super().__init__(logging.INFO)
        self.telemetry = telemetry

    def emit(self, record):
        try:
            self.telemetry.event(record.levelname.lower(), record.getMessage())
        except Exception:
            self.handleError(record)


class _LimitedHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # HTTPServer.server_bind calls socket.getfqdn(), which can wait on a
        # board's DNS/hostname resolver even for a numeric bind address. This
        # read-only service has no need for a canonical hostname.
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]

    def __init__(self, address, handler):
        self._slots = threading.BoundedSemaphore(16)
        super().__init__(address, handler)

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()


class DashboardServer:
    def __init__(self, telemetry, host="0.0.0.0", port=8080, state_provider=None):
        self.telemetry = telemetry
        self.state_provider = state_provider
        self.stopping = threading.Event()
        assets = Path(__file__).resolve().parent / "dashboard"
        # Read only explicitly named assets. No arbitrary file serving.
        self.assets = {
            "/": ("text/html; charset=utf-8", (assets / "index.html").read_bytes()),
            "/dashboard.js": ("text/javascript; charset=utf-8", (assets / "dashboard.js").read_bytes()),
            "/dashboard.css": ("text/css; charset=utf-8", (assets / "dashboard.css").read_bytes()),
        }
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def setup(self):
                super().setup()
                self.connection.settimeout(3)

            def log_message(self, *_args):
                pass

            def do_GET(self):
                try:
                    self.serve()
                except (OSError, TimeoutError):
                    pass  # Client left or cannot keep up; acquisition continues.

            def serve(self):
                path = urlsplit(self.path).path
                if path == "/events":
                    self.stream()
                    return
                if path == "/api/state":
                    mime = "application/json"
                    body = json.dumps(owner.telemetry.snapshot(), allow_nan=False).encode()
                elif path in owner.assets:
                    mime, body = owner.assets[path]
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                self.wfile.write(body)

            def stream(self):
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("X-Accel-Buffering", "no")
                self.end_headers()
                cursor = None
                try:
                    run, number = self.headers.get("Last-Event-ID", "").split(":", 1)
                    if run == owner.telemetry.run and int(number) >= 0:
                        cursor = int(number)
                except ValueError:
                    pass
                next_state = 0
                inference_revision = None
                while not owner.stopping.is_set():
                    now = time.monotonic()
                    include_state = now >= next_state
                    batch = owner.telemetry.snapshot(cursor, include_state, inference_revision)
                    inference_revision = batch["inference_revision"]
                    if include_state:
                        next_state = now + 1
                    payload = json.dumps(batch, separators=(",", ":"), allow_nan=False)
                    event_id = "{}:{}".format(batch["run"], batch["cursor"])
                    self.wfile.write(("id: " + event_id + "\nretry: 1000\ndata: " + payload + "\n\n").encode())
                    self.wfile.flush()
                    cursor = batch["cursor"]
                    if owner.stopping.wait(0.1):
                        break

        self.httpd = _LimitedHTTPServer((host, port), Handler)
        self.port = self.httpd.server_address[1]
        self._thread = threading.Thread(target=self.httpd.serve_forever,
                                        kwargs={"poll_interval": 0.1}, daemon=True,
                                        name="dashboard-http")
        self._monitor = threading.Thread(target=self._refresh_state, daemon=True,
                                         name="dashboard-state")

    def _refresh_state(self):
        while not self.stopping.is_set():
            if self.state_provider:
                try:
                    self.telemetry.update(**self.state_provider())
                except Exception as error:
                    self.telemetry.event("warning", "Dashboard state unavailable: " + str(error))
            self.stopping.wait(0.5)

    def start(self):
        self._thread.start()
        self._monitor.start()

    def close(self):
        self.stopping.set()
        if self._thread.is_alive():
            self.httpd.shutdown()
            self._thread.join(timeout=2)
        self.httpd.server_close()
        if self._monitor.is_alive():
            self._monitor.join(timeout=2)


def main():
    from inference_pipeline import InferencePipeline
    from window_assembler_updated import WindowAssembler, WindowConfig

    class DemoInference:
        @staticmethod
        def model_intervals(window):
            return [(window.start_us, window.end_us)]

        @staticmethod
        def infer_window(window):
            # Deliberate delay makes the processing highlight visible in demo mode.
            time.sleep(0.35)
            exercise = ("squat", "push_up", "jumping_jack")[(window.window_id // 6) % 3]
            return SimpleNamespace(
                capture_run_id=window.run_id, window_id=window.window_id,
                start_us=window.start_us, end_us=window.end_us,
                exercise=exercise, class_id=0, confidence=0.94,
                model_window_count=1)

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", action="store_true", required=True,
                        help="generate simulated IMU samples; does not use sensors or DPU")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    telemetry = Telemetry(demo=True)
    telemetry.update(capture="streaming", connections={
        "1": {"session": 1}, "2": {"session": 1}})
    server = DashboardServer(telemetry, args.host, args.port)
    server.start()
    print("SIMULATED DATA: open http://<this-machine-IP>:{}".format(server.port), flush=True)
    start = time.monotonic()
    assembler = WindowAssembler(WindowConfig(sample_rate_hz=100, window_us=3_000_000,
                                              hop_us=1_000_000))
    publisher = SimpleNamespace(set_capture=lambda *_: None, publish_classification=lambda *_: None)
    pipeline = InferencePipeline(assembler, DemoInference(), publisher, telemetry=telemetry)
    pipeline.start_run(1, int(start * 1_000_000))
    sequence = 0
    try:
        while True:
            t = sequence / 100
            timestamp = int((start + t) * 1_000_000)
            for node in (1, 2):
                phase = t * 3 + node
                sample = SimpleNamespace(
                    node_id=node, session_id=1, sequence=sequence,
                    timestamp_us=timestamp, coordinator_timestamp_us=timestamp,
                    ax=2 * math.sin(phase), ay=math.cos(phase), az=9.81 + math.sin(phase * 2),
                    gx=0.4 * math.cos(phase), gy=0.7 * math.sin(phase), gz=0.2 * math.cos(phase * 2),
                    status=1, flags=0)
                telemetry.sample(sample)
                pipeline.submit(sample)
            sequence += 1
            time.sleep(max(0, start + sequence / 100 - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        pipeline.close()
        server.close()


if __name__ == "__main__":
    main()
