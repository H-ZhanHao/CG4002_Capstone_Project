"""Bounded, memory-only fall delivery, independent of capture and rep sessions."""
from collections import OrderedDict
import json
import logging
import re
import threading
import time

LOGGER = logging.getLogger("server")
FIELDS = {"v", "type", "node_id", "boot_id", "event_id"}


def _unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate fall field")
        result[key] = value
    return result


def parse_fall(payload, kind):
    if not isinstance(payload, (str, bytes)) or len(payload.encode() if isinstance(payload, str) else payload) > 1024:
        raise ValueError("invalid fall message size")
    message = json.loads(payload, object_pairs_hook=_unique_fields)
    if not isinstance(message, dict) or set(message) != FIELDS:
        raise ValueError("invalid fall fields")
    if (type(message["v"]) is not int or message["v"] != 1 or message["type"] != kind or
            type(message["node_id"]) is not int or message["node_id"] != 1 or
            type(message["boot_id"]) is not int or not 0 < message["boot_id"] <= 0xFFFFFFFF or
            not isinstance(message["event_id"], str) or
            re.fullmatch(r"[1-9][0-9]{0,19}", message["event_id"]) is None or
            int(message["event_id"]) > 0xFFFFFFFFFFFFFFFF):
        raise ValueError("invalid fall identity")
    return message


def identity(message):
    return message["node_id"], message["boot_id"], message["event_id"]


class FallDelivery:
    def __init__(self, *, clock=time.monotonic, capacity=32, lifetime=30.0, retry=1.0):
        self.clock, self.capacity, self.lifetime, self.retry = clock, capacity, lifetime, retry
        self.lock = threading.RLock()
        self.pending = OrderedDict()

    def _expire(self, now):
        for key, item in list(self.pending.items()):
            if now >= item[1]:
                del self.pending[key]
                LOGGER.warning("FALL_AR_EXPIRED identity=%s", key)

    def enqueue(self, node, boot, event, synthetic=False):
        # Test injections remain available for local diagnostics, never real AR alerts.
        if synthetic:
            LOGGER.info("FALL_AR_FILTERED synthetic=1 node=%d boot=%d event=%d", node, boot, event)
            return False
        message = dict(v=1, type="fall_event", node_id=node, boot_id=boot, event_id=str(event))
        parse_fall(json.dumps(message), "fall_event")
        with self.lock:
            now = self.clock()
            self._expire(now)
            key = identity(message)
            if key in self.pending:
                return False
            if len(self.pending) >= self.capacity:
                LOGGER.warning("FALL_AR_OVERFLOW identity=%s", key)
                return False
            self.pending[key] = (message, now + self.lifetime)
            LOGGER.info("FALL_AR_QUEUED identity=%s", key)
            return True

    def due(self, sent, limit=8):
        """Per-link retry state permits immediate replay on reconnect."""
        with self.lock:
            now = self.clock()
            self._expire(now)
            for key in list(sent):
                if key not in self.pending:
                    del sent[key]
            result = []
            for key, (message, deadline) in self.pending.items():
                if key not in sent or now - sent[key] >= self.retry:
                    sent[key] = now
                    result.append(dict(message))
                    if len(result) >= limit:
                        break
            return result

    def acknowledge(self, message, sent):
        with self.lock:
            self._expire(self.clock())
            key = identity(message)
            if key in sent and key in self.pending:
                del self.pending[key]
                LOGGER.info("FALL_AR_ACKED identity=%s", key)
                return True
            return False
