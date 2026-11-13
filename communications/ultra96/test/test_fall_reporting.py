"""Fall outbox and real TLS gateway tests; no physical hardware required."""
import asyncio
import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

import websockets
import server
from fall_delivery import FallDelivery, parse_fall
from device_events import DeviceEventReceiver
from test_stage5_fall import event_frame
import test_rep_control as fixtures


class OutboxTests(unittest.TestCase):
    def test_retry_ack_reconnect_expiry_overflow_and_filter(self):
        now = [0.0]
        box = FallDelivery(clock=lambda: now[0], capacity=1)
        self.assertFalse(box.enqueue(1, 2, 1, True))
        self.assertTrue(box.enqueue(1, 2, 1))
        self.assertFalse(box.enqueue(1, 2, 2))
        sent = {}
        first = box.due(sent)[0]
        self.assertEqual(box.due(sent), [])
        self.assertEqual(box.due({}), [first])
        now[0] = 1
        self.assertEqual(box.due(sent), [first])
        self.assertFalse(box.acknowledge(first, {}))
        self.assertTrue(box.acknowledge(first, sent))
        self.assertFalse(box.acknowledge(first, sent))
        self.assertTrue(box.enqueue(1, 2, 2))
        now[0] = 31
        self.assertEqual(box.due(sent), [])
        self.assertEqual(len(box.pending), 0)

    def test_strict_wire_identity_and_uint64_precision(self):
        valid = dict(v=1, type="fall_event", node_id=1, boot_id=4294967295,
                     event_id="18446744073709551615")
        self.assertEqual(parse_fall(json.dumps(valid), "fall_event"), valid)
        for key, value in (("v", True), ("node_id", 2), ("boot_id", 0),
                           ("event_id", "01"), ("event_id", "18446744073709551616"),
                           ("event_id", 42), ("type", "fall_ack"), ("extra", 1)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                parse_fall(json.dumps(dict(valid, **{key: value})), "fall_event")

    def test_failed_esp_ack_and_duplicate_enqueue_once(self):
        receiver = DeviceEventReceiver()
        conn = SimpleNamespace(node_id=1, boot_id=0x01020304, session_id=101, send=Mock())
        conn.send.side_effect = OSError("lost ESP ACK")
        with self.assertRaises(OSError):
            receiver.receive(event_frame(), conn, server.build_header)
        conn.send.side_effect = None
        self.assertEqual(receiver.receive(event_frame(), conn, server.build_header), 1)
        self.assertEqual(len(receiver.falls.pending), 1)


class FallGatewayTests(unittest.IsolatedAsyncioTestCase):
    server_module = server
    setUpClass = classmethod(fixtures.RepGatewayTests.setUpClass.__func__)
    tearDownClass = classmethod(fixtures.RepGatewayTests.tearDownClass.__func__)
    asyncSetUp = fixtures.RepGatewayTests.asyncSetUp
    asyncTearDown = fixtures.RepGatewayTests.asyncTearDown
    receive = fixtures.RepGatewayTests.receive
    send = fixtures.RepGatewayTests.send

    def emit(self, event=1, synthetic=0):
        conn = self.connections[0]
        conn.boot_id = 0x01020304
        return self.coordinator.receive_device_event(event_frame(
            self.server_module, event=event, synthetic=synthetic), conn)

    async def ack(self, message):
        await self.websocket.send(json.dumps(dict(message, type="fall_ack")))
        for _ in range(50):
            if not self.coordinator.device_events.falls.pending:
                return
            await asyncio.sleep(.02)
        self.fail("AR ACK did not clear pending fall")

    async def test_receipt_retry_reconnect_and_ack_without_rep_session(self):
        self.assertFalse(self.coordinator.run_active)
        self.assertEqual(self.emit(), 0)
        first = await self.receive("fall_event")
        self.assertEqual(first, dict(v=1, type="fall_event", node_id=1,
                                     boot_id=0x01020304, event_id="1"))
        self.assertEqual(self.emit(), 1)
        self.assertEqual(await self.receive("fall_event"), first)
        await self.websocket.close()
        await asyncio.sleep(.15)
        self.websocket = await websockets.connect(self.uri, ssl=self.context)
        await self.receive("rep_welcome")
        self.assertEqual(await self.receive("fall_event"), first)
        await self.ack(first)
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(self.websocket.recv(), 1.2)

    async def test_synthetic_filtered_and_exercise_not_overwriting_fall(self):
        self.emit(synthetic=1)
        self.assertEqual(len(self.coordinator.device_events.falls.pending), 0)
        from visualizer_protocol import build_message
        self.emit(event=2)
        for seq in range(20):
            self.gateway.publish(build_message(run_id=1, seq=seq, ts_us=seq, exercise="squat"))
        fall = await self.receive("fall_event")
        self.assertEqual(fall["event_id"], "2")
        await self.ack(fall)

    async def test_gateway_restart_replays_unacknowledged_event(self):
        from visualizer_gateway import VisualizerGateway
        self.emit()
        first = await self.receive("fall_event")
        await self.websocket.close()
        await self.gateway.close()
        await asyncio.sleep(.15)
        self.gateway = VisualizerGateway(self.gateway.config)
        await self.gateway.start()
        self.uri = f"wss://127.0.0.1:{self.gateway.websocket_port}"
        self.websocket = await websockets.connect(self.uri, ssl=self.context)
        await self.receive("rep_welcome")
        self.assertEqual(await self.receive("fall_event"), first)
        await self.ack(first)

    async def test_wrong_ack_does_not_clear_pending_and_reps_still_work(self):
        await self.send("rep_session_start", 0)
        self.emit()
        fall = await self.receive("fall_event")
        await self.websocket.send(json.dumps(dict(fall, type="fall_ack", event_id="99")))
        await asyncio.sleep(.1)
        self.assertEqual(len(self.coordinator.device_events.falls.pending), 1)
        self.assertEqual((await self.send(count=1))["status"], "accepted")
        await self.ack(fall)


class AIFallGatewayTests(FallGatewayTests):
    server_module = fixtures.ai_fixture.ai_server
