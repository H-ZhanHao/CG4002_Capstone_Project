"""Stage 3: explicit rep sessions and bounded gateway->coordinator loopback IPC.

Only the coordinator owns ESP connections. This service binds IPv4 loopback,
accepts at most four local links, limits lines to 1024 bytes and phone requests
to 20/second/link. One link owns rep input at a time. JSON results are emitted
by a single handler, with at most 16 outstanding delivery notifications.
"""
from collections import OrderedDict, deque
import json
import socket
import socketserver
import threading
import time

from visualizer_protocol import parse_rep_message, VisualizerProtocolError, MAX_PHONE_BYTES

CONTROL_PORT = 9101


class RepController:
    def __init__(self, coordinator, run_id):
        self.coordinator = coordinator
        self.run_id = run_id
        self.lock = threading.RLock()
        self.epoch = 0
        self.active = None
        self.session_id = None
        self.exercise = None
        self.count = 0
        self.used_sessions = set()
        self.receipts = OrderedDict()

    def _cancel_pending(self):
        for event_id in self.receipts.values():
            self.coordinator.feedback_delivery.cancel_event(event_id)

    def connect(self):
        with self.lock:
            self._cancel_pending()
            self.epoch += 1
            self.active = self.epoch
            return {"epoch": self.epoch, "ready": False, "seq": -1,
                    "cache": OrderedDict(), "times": deque()}

    def disconnect(self, link):
        with self.lock:
            if self.active == link["epoch"]:
                self._cancel_pending()
                self.active = None

    def welcome(self):
        return {"v": 1, "type": "rep_welcome", "run_id": self.run_id,
                "requires_sync": True}

    def response(self, request=None, status="rejected", reason="invalid_request", event_id=0):
        request = request or {}
        return {"v": 1, "type": "rep_result", "run_id": self.run_id,
                "seq": request.get("seq", -1),
                "rep_session_id": request.get("rep_session_id", ""),
                "rep_index": request.get("rep_index", -1), "status": status,
                "reason": reason, "feedback_event_id": event_id,
                "nodes": self.coordinator.feedback_delivery.event_status(event_id) if event_id else []}

    def handle(self, link, payload):
        with self.lock:
            if link["epoch"] != self.active:
                return self.response(reason="stale_connection")
            now = time.monotonic()
            while link["times"] and now - link["times"][0] >= 1:
                link["times"].popleft()
            if len(link["times"]) >= 20:
                return self.response(reason="rate_limit")
            link["times"].append(now)
            try:
                message = parse_rep_message(payload)
            except VisualizerProtocolError as error:
                return self.response(reason=str(error))
            if message["run_id"] != self.run_id:
                return self.response(message, reason="stale_run")
            seq = message["seq"]
            fingerprint = json.dumps(message, sort_keys=True)
            cached = link["cache"].get(seq)
            if cached is not None:
                old_fingerprint, status, reason, event_id = cached
                if old_fingerprint != fingerprint:
                    return self.response(message, reason="sequence_conflict")
                return self.response(message, "duplicate" if status == "accepted" else status,
                                     reason, event_id)
            if seq <= link["seq"]:
                return self.response(message, reason="stale_sequence")
            link["seq"] = seq
            result = self._apply(link, message)
            link["cache"][seq] = (fingerprint, result["status"], result["reason"], result["feedback_event_id"])
            while len(link["cache"]) > 64:
                link["cache"].popitem(last=False)
            return result

    def _apply(self, link, message):
        identity, count, exercise = (message[k] for k in ("rep_session_id", "rep_index", "exercise"))
        kind = message["type"]
        if kind == "rep_session_start":
            if identity in self.used_sessions:
                return self.response(message, reason="session_id_used; sync or use a new ID")
            if len(self.used_sessions) >= 256:
                return self.response(message, reason="session_capacity; restart server")
            self._cancel_pending()
            self.used_sessions.add(identity)
            self.session_id, self.exercise, self.count = identity, exercise, count
            self.receipts.clear()
            link["ready"] = True
            return self.response(message, "session_started", "baseline_only")
        if identity != self.session_id or exercise != self.exercise:
            return self.response(message, reason="stale_session_or_exercise")
        if kind == "rep_sync":
            if count < self.count:
                return self.response(message, reason="count_decreased; start a new session")
            self._cancel_pending()
            self.count = count
            link["ready"] = True
            return self.response(message, "synced", "baseline_only")
        if not link["ready"]:
            return self.response(message, reason="sync_required")
        if count in self.receipts:
            return self.response(message, "duplicate", "rep_already_accepted", self.receipts[count])
        if count < self.count:
            return self.response(message, reason="stale_count; start a new session to reset")
        if count == self.count:
            return self.response(message, "duplicate", "baseline_count")
        # One current pulse even if the cumulative count jumps over missed reps.
        event_id = self.coordinator.request_feedback(1)
        self.count = count
        self.receipts[count] = event_id
        while len(self.receipts) > 128:
            self.receipts.popitem(last=False)
        return self.response(message, "accepted", "new_rep", event_id)


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, controller):
        self.controller = controller
        self.slots = threading.BoundedSemaphore(4)
        self.stopping = threading.Event()
        super().__init__(address, _Handler)

    def verify_request(self, request, address):
        return address[0] == "127.0.0.1" and self.slots.acquire(blocking=False)

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class _Handler(socketserver.BaseRequestHandler):
    def handle(self):
        controller = self.server.controller
        link = controller.connect()
        self.request.settimeout(0.05)
        buffer = bytearray()
        notifications = []

        def send(message):
            self.request.sendall((json.dumps(message, separators=(",", ":")) + "\n").encode())

        try:
            send(controller.welcome())
            while not self.server.stopping.is_set():
                controller.coordinator.feedback_delivery.tick()
                for original, deadline in list(notifications):
                    nodes = controller.coordinator.feedback_delivery.event_status(original["feedback_event_id"])
                    if all(node["status"] != "pending" for node in nodes) or time.monotonic() >= deadline:
                        result = dict(original, type="rep_delivery", status="complete", nodes=nodes)
                        send(result)
                        notifications.remove((original, deadline))
                try:
                    data = self.request.recv(1025)
                except socket.timeout:
                    continue
                if not data:
                    return
                buffer.extend(data)
                while b"\n" in buffer:
                    line, _, rest = buffer.partition(b"\n")
                    buffer = bytearray(rest)
                    if len(line) > MAX_PHONE_BYTES:
                        return
                    if len(notifications) >= 16:
                        send(controller.response(reason="delivery_queue_full"))
                        continue
                    result = controller.handle(link, bytes(line))
                    send(result)
                    if result["feedback_event_id"] and any(n["status"] == "pending" for n in result["nodes"]):
                        notifications.append((result, time.monotonic() + 2.6))
                if len(buffer) > MAX_PHONE_BYTES:
                    return
        except (OSError, ConnectionError):
            pass
        finally:
            controller.disconnect(link)


class RepControlServer:
    def __init__(self, coordinator, run_id, port=CONTROL_PORT):
        self.controller = RepController(coordinator, run_id)
        self.server = _Server(("127.0.0.1", port), self.controller)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.1}, daemon=True,
                                       name="rep-loopback-control")

    @property
    def port(self):
        return self.server.server_address[1]

    def start(self):
        self.thread.start()

    def close(self):
        self.server.stopping.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=1)
