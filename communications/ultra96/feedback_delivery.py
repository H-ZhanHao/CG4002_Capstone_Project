"""Bounded, session-scoped feedback delivery, independent of capture controls.

0x0B payload: !QB7x (16 bytes): increasing nonzero uint64 event_id, uint8
action (1 REP_LED, 2 READY_LED, 3 IDLE_BUZZ), seven reserved zero bytes.
Header stays !HBBBBHIIQ: node=0, flags=0, current transport session/sequence.
Retries keep the entire frame unchanged. ACK is the existing !BBHIQ payload,
command_type=0x0B; APPLIED/DUPLICATE mean firmware accepted the output request,
not that its GPIO or connected LED/buzzer physically worked.
"""

from collections import deque, OrderedDict
from dataclasses import dataclass
import logging
import queue
import struct
import threading
import time

MESSAGE_TYPE_FEEDBACK_COMMAND = 0x0B
FEEDBACK_PAYLOAD_SIZE = 16
FEEDBACK_REP_LED = 1
FEEDBACK_READY_LED = 2
FEEDBACK_IDLE_BUZZ = 3
FEEDBACK_RETRY_US = 750_000
FEEDBACK_DEADLINE_US = 2_500_000
FEEDBACK_MAX_ATTEMPTS = 3
FEEDBACK_MAX_PENDING_PER_NODE = 8
LOGGER = logging.getLogger("server")


def encode_feedback(event_id, action):
    if (type(event_id) is not int or not 0 < event_id <= 0xFFFFFFFFFFFFFFFF or
            type(action) is not int or action not in (1, 2, 3)):
        raise ValueError("Invalid feedback identity or action")
    return struct.pack("!QB7x", event_id, action)


def decode_feedback(payload):
    if len(payload) != FEEDBACK_PAYLOAD_SIZE or any(payload[9:]):
        raise ValueError("Invalid feedback payload length or reserved bytes")
    event_id, action = struct.unpack("!QB7x", payload)
    encode_feedback(event_id, action)
    return event_id, action


@dataclass
class Delivery:
    connection: object
    event_id: int
    sequence: int
    frame: bytes
    deadline_us: int
    next_retry_us: int
    attempts: int = 0
    status: str = "pending"
    result: int | None = None
    effective_local_us: int | None = None


class _Outbox:
    """One bounded sender per participating connection; never send under locks."""

    def __init__(self, owner, connection):
        self.owner = owner
        self.connection = connection
        self.queue = queue.Queue(maxsize=FEEDBACK_MAX_PENDING_PER_NODE)
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True,
                                       name=f"feedback-node-{connection.node_id}")
        self.thread.start()

    def _run(self):
        while not self.closed.is_set():
            try:
                delivery, repeat = self.queue.get(timeout=0.1)
            except queue.Empty:
                continue
            # Expired, replaced, or disconnected work cannot cross a reconnect.
            with self.owner.lock:
                valid = (not self.closed.is_set() and
                         self.owner.connections.get(self.connection.node_id) is self.connection and
                         self.owner.clock() < delivery.deadline_us and
                         (delivery.status == "pending" or
                          (repeat and delivery.status == "applied")))
            if not valid:
                continue
            try:
                self.connection.send(delivery.frame)
            except OSError:
                self.owner.failed(delivery, "send_failed")


class FeedbackDelivery:
    def __init__(self, clock=None):
        self.clock = clock or (lambda: time.monotonic_ns() // 1000)
        self.lock = threading.RLock()
        self.connections = {}
        self.outboxes = {}
        self.pending = {}
        self.completed = deque(maxlen=128)
        self.last = []
        self.event_id = 0
        self.tickets = OrderedDict()

    def register(self, connection):
        with self.lock:
            old = self.connections.get(connection.node_id)
            if old is not None and old is not connection:
                self.unregister(old)
            self.connections[connection.node_id] = connection

    def unregister(self, connection):
        with self.lock:
            if self.connections.get(connection.node_id) is not connection:
                return
            del self.connections[connection.node_id]
            outbox = self.outboxes.pop(connection.node_id, None)
            if outbox is not None:
                outbox.closed.set()
            for item in list(self.pending.values()):
                if item.connection is connection:
                    self._finish(item, "disconnected")
            # Do not retain repeat-test requests from an old Session.
            self.last = [item for item in self.last if item.connection is not connection]

    def close(self):
        with self.lock:
            for connection in list(self.connections.values()):
                self.unregister(connection)

    def _key(self, item):
        return item.connection.node_id, item.connection.session_id, item.sequence

    def _finish(self, item, status, result=None, effective_local_us=None):
        if item.status != "pending":
            return
        self.pending.pop(self._key(item), None)
        item.status, item.result = status, result
        item.effective_local_us = effective_local_us
        self.completed.append(item)
        LOGGER.info("FEEDBACK_RESULT node=%d session=%d event=%d sequence=%d "
                    "status=%s result=%s effective_local_us=%s attempts=%d",
                    item.connection.node_id, item.connection.session_id, item.event_id,
                    item.sequence, status, result, effective_local_us, item.attempts)

    def failed(self, item, reason):
        with self.lock:
            self._finish(item, reason)

    def _enqueue(self, item, repeat=False):
        node = item.connection.node_id
        outbox = self.outboxes.get(node)
        if outbox is None:
            outbox = self.outboxes[node] = _Outbox(self, item.connection)
        try:
            outbox.queue.put_nowait((item, repeat))
        except queue.Full:
            self._finish(item, "queue_full")
            return False
        item.attempts += 1
        LOGGER.info("FEEDBACK_SENT node=%d session=%d event=%d sequence=%d attempt=%d",
                    node, item.connection.session_id, item.event_id, item.sequence, item.attempts)
        return True

    def submit(self, action, sequence, build_header, nodes):
        """Create one event, independently target each requested current Session."""
        with self.lock:
            if self.event_id == 0xFFFFFFFFFFFFFFFF:
                raise ValueError("Feedback event identity exhausted; restart the server")
            self.event_id += 1
            payload = encode_feedback(self.event_id, action)
            now = self.clock()
            self.last = []
            ticket = self.tickets[self.event_id] = {}
            while len(self.tickets) > 128:
                self.tickets.popitem(last=False)
            for node in nodes:
                connection = self.connections.get(node)
                if connection is None:
                    ticket[node] = None
                    LOGGER.info("FEEDBACK_UNAVAILABLE node=%d event=%d", node, self.event_id)
                    continue
                frame = build_header(MESSAGE_TYPE_FEEDBACK_COMMAND, node_id=0,
                    payload_length=FEEDBACK_PAYLOAD_SIZE, session_id=connection.session_id,
                    sequence=sequence) + payload
                item = Delivery(connection, self.event_id, sequence, frame,
                                now + FEEDBACK_DEADLINE_US, now + FEEDBACK_RETRY_US)
                self.last.append(item)
                ticket[node] = item
                if sum(key[0] == node for key in self.pending) >= FEEDBACK_MAX_PENDING_PER_NODE:
                    self._finish(item, "queue_full")
                    continue
                self.pending[self._key(item)] = item
                self._enqueue(item)
            return self.event_id

    def tick(self, now_us=None):
        now = self.clock() if now_us is None else now_us
        with self.lock:
            for item in list(self.pending.values()):
                if now >= item.deadline_us:
                    self._finish(item, "timeout")
                elif now >= item.next_retry_us and item.attempts < FEEDBACK_MAX_ATTEMPTS:
                    item.next_retry_us = now + FEEDBACK_RETRY_US
                    self._enqueue(item)

    def acknowledge(self, connection, sequence, result, effective_local_us):
        if result not in range(9):
            raise ValueError("Invalid feedback acknowledgement result")
        with self.lock:
            if self.connections.get(connection.node_id) is not connection:
                return False
            key = connection.node_id, connection.session_id, sequence
            item = self.pending.get(key)
            if item is None:
                # Late/duplicate feedback ACKs never interfere with capture controls.
                LOGGER.info("FEEDBACK_LATE_ACK_IGNORED node=%d sequence=%d result=%d",
                            connection.node_id, sequence, result)
                return False
            if self.clock() >= item.deadline_us:
                self._finish(item, "timeout")
                return False
            self._finish(item, "applied" if result in (0, 3) else "rejected",
                         result, effective_local_us)
            return True

    def repeat_last(self):
        """Bench duplicate-suppression test: resend the exact unexpired frames."""
        with self.lock:
            repeated = 0
            for item in self.last:
                if (self.connections.get(item.connection.node_id) is item.connection and
                        self.clock() < item.deadline_us and
                        item.attempts < FEEDBACK_MAX_ATTEMPTS and
                        item.status in ("pending", "applied")):
                    repeated += self._enqueue(item, repeat=True)
            return repeated

    def status(self):
        with self.lock:
            return [(item.event_id, item.connection.node_id, item.status, item.result)
                    for item in self.last]

    def event_status(self, event_id):
        """Snapshot one logical event independently of later console/AR events."""
        with self.lock:
            ticket = self.tickets.get(event_id)
            if ticket is None:
                return [{"node_id": n, "status": "expired", "result": -1} for n in (1, 2)]
            return [{"node_id": n, "status": item.status if item else "unavailable",
                     "result": item.result if item and item.result is not None else -1}
                    for n, item in sorted(ticket.items())]

    def cancel_event(self, event_id):
        """Cancel unsent/retry work when a phone disconnects or resets its set.

        A frame already inside transport.send is in flight and cannot be undone.
        """
        with self.lock:
            for item in list(self.pending.values()):
                if item.event_id == event_id:
                    self._finish(item, "cancelled")
