"""Hip fall receipt, ESP ACK and nonblocking AR outbox insertion.

Identity is (node, boot, event), independent of the reconnect transport Session.
Retention is bounded; evicted old IDs fail closed. Receipt state is in memory
for the server process lifetime, not a durable emergency notification service.
"""
from collections import OrderedDict
import logging
import struct
try:
    from fall_delivery import FallDelivery
except ImportError:
    from .fall_delivery import FallDelivery

MESSAGE_TYPE_DEVICE_EVENT = 0x13
MESSAGE_TYPE_DEVICE_EVENT_ACK = 0x14
EVENT_PAYLOAD = struct.Struct("!BBHIQQII")
ACK_PAYLOAD = struct.Struct("!IQB3x")
HEADER = struct.Struct("!HBBBBHIIQ")
MAX_BOOTS = 64
RECEIPTS_PER_BOOT = 64
LOGGER = logging.getLogger("server")


class DeviceEventReceiver:
    """Called under the owning coordinator lock, including the ACK write."""

    def __init__(self):
        self.boots = {}
        self.falls = FallDelivery()

    def receive(self, frame, connection, build_header):
        if len(frame) != HEADER.size + EVENT_PAYLOAD.size:
            raise ValueError("DEVICE_EVENT must be exactly 56 bytes")
        magic, version, kind, node, flags, size, session, seq, timestamp = HEADER.unpack_from(frame)
        event_kind, synthetic, reserved, boot, event, occurred, duration, reserved2 = EVENT_PAYLOAD.unpack_from(frame, HEADER.size)
        if (magic != 0xA55A or version != 1 or kind != MESSAGE_TYPE_DEVICE_EVENT or
                node != 1 or node != connection.node_id or flags or
                size != EVENT_PAYLOAD.size or not session or session != connection.session_id or
                not boot or boot != connection.boot_id or not event or
                event_kind != 1 or synthetic not in (0, 1) or reserved or reserved2 or
                not occurred or occurred != timestamp or duration != 3000 or
                seq != event & 0xFFFFFFFF):
            raise ValueError("Invalid DEVICE_EVENT identity, fields or Session")
        key = (node, boot)
        if key not in self.boots:
            if len(self.boots) >= MAX_BOOTS:
                raise ValueError("DEVICE_EVENT boot receipt capacity exhausted")
            self.boots[key] = [0, OrderedDict()]
        state = self.boots[key]
        fingerprint = frame[HEADER.size:]
        if event in state[1]:
            if state[1][event] != fingerprint:
                raise ValueError("DEVICE_EVENT identity reused with different contents")
            result = 1
        else:
            if event <= state[0]:
                raise ValueError("DEVICE_EVENT is older than retained receipts")
            state[0] = event
            state[1][event] = fingerprint
            while len(state[1]) > RECEIPTS_PER_BOOT:
                state[1].popitem(last=False)
            result = 0
            LOGGER.warning(
                "DEVICE_EVENT_ACCEPTED node=%d boot=%d event=%d kind=FALL_STARTED "
                "occurred_node_us=%d alarm_duration_ms=%d synthetic=%s",
                node, boot, event, occurred, duration, bool(synthetic))
            # Nonblocking memory enqueue precedes the ESP ACK, including failed ACK writes.
            self.falls.enqueue(node, boot, event, bool(synthetic))
        ack = build_header(MESSAGE_TYPE_DEVICE_EVENT_ACK, node_id=0,
                           payload_length=ACK_PAYLOAD.size, session_id=session,
                           sequence=seq) + ACK_PAYLOAD.pack(boot, event, result)
        # Cache before ACK: a failed write followed by reconnect is a duplicate.
        connection.send(ack)
        LOGGER.info("DEVICE_EVENT_ACK_SENT node=%d boot=%d event=%d result=%s",
                    node, boot, event, "accepted" if result == 0 else "duplicate")
        return result
