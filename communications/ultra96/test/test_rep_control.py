import asyncio
import json
import ssl
import unittest

import websockets
import server
import test_feedback_delivery as peers
import test_server_ai_feedback as ai_fixture
import test_visualizer_gateway as tls_fixture
from rep_control import RepControlServer, RepController
from visualizer_protocol import parse_rep_message, VisualizerProtocolError, build_message
from visualizer_gateway import VisualizerGateway, GatewayConfig


def request(kind="rep_event", count=1, seq=1, identity="setA", run=12345678, exercise="squat"):
    return json.dumps(dict(v=1, type=kind, run_id=run, seq=seq,
                          rep_session_id=identity, rep_index=count, exercise=exercise))


class RepControllerTests(unittest.TestCase):
    def setUp(self):
        self.coordinator = server.CaptureCoordinator(auto_start=False)
        self.controller = RepController(self.coordinator, 12345678)
        self.link = self.controller.connect()

    def send(self, **kwargs):
        return self.controller.handle(self.link, request(**kwargs))

    def test_strict_protocol(self):
        valid = json.loads(request())
        for field, value in (("run_id", True), ("rep_index", -1), ("seq", 2**53),
                             ("type", "STOP"), ("v", True), ("exercise", []),
                             ("rep_session_id", "bad space")):
            bad = dict(valid); bad[field] = value
            with self.assertRaises(VisualizerProtocolError):
                parse_rep_message(json.dumps(bad))
        for payload in ('{}', '[]', '{bad', 'x' * 1025, b'\xff'):
            with self.assertRaises(VisualizerProtocolError):
                parse_rep_message(payload)
        bad = dict(valid, extra=1)
        with self.assertRaises(VisualizerProtocolError):
            parse_rep_message(json.dumps(bad))

    def test_duplicates_sequences_and_explicit_reset(self):
        self.assertEqual(self.send(kind="rep_session_start", count=0, seq=0)["status"], "session_started")
        first = self.send(seq=1)
        self.assertEqual(first["status"], "accepted")
        self.assertEqual(self.send(seq=1)["status"], "duplicate")
        self.assertEqual(self.send(seq=2)["feedback_event_id"], first["feedback_event_id"])
        self.assertEqual(self.send(seq=2, count=2)["reason"], "sequence_conflict")
        self.assertEqual(self.send(seq=3, count=4)["status"], "accepted")
        self.assertEqual(self.send(seq=4, count=2)["status"], "rejected")
        self.assertEqual(self.send(seq=5, count=1)["status"], "duplicate")
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)
        self.assertEqual(self.send(kind="rep_session_start", identity="setB", count=0, seq=6)["status"], "session_started")
        self.assertEqual(self.send(identity="setB", seq=7)["status"], "accepted")
        self.assertEqual(self.send(seq=8)["status"], "rejected")
        self.assertEqual(self.send(kind="rep_session_start", count=0, seq=9)["status"], "rejected")

    def test_reconnect_requires_sync_without_backlog(self):
        self.send(kind="rep_session_start", count=0, seq=0)
        self.send(seq=1)
        old = self.link
        self.link = self.controller.connect()
        self.assertEqual(self.controller.handle(old, request(seq=2))["reason"], "stale_connection")
        self.assertEqual(self.send(seq=1, count=2)["reason"], "sync_required")
        self.assertEqual(self.send(kind="rep_sync", seq=2, count=7)["status"], "synced")
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 1)
        self.assertEqual(self.send(seq=3, count=7)["status"], "duplicate")
        self.assertEqual(self.send(seq=4, count=8)["status"], "accepted")
        self.assertEqual(self.send(seq=5, run=99)["reason"], "stale_run")
        self.assertEqual(self.coordinator.feedback_delivery.event_id, 2)

    def test_request_rate_is_bounded(self):
        self.send(kind="rep_session_start", count=0, seq=0)
        for seq in range(1, 20):
            self.assertNotEqual(self.send(seq=seq)["reason"], "rate_limit")
        self.assertEqual(self.send(seq=20)["reason"], "rate_limit")
        self.assertLessEqual(len(self.link["cache"]), 64)

    def test_invalid_messages_also_consume_the_rate_budget(self):
        for _ in range(20):
            self.controller.handle(self.link, "{}")
        self.assertEqual(self.send(kind="rep_session_start", count=0)["reason"], "rate_limit")

    def test_caches_and_session_history_fail_closed(self):
        import itertools
        from unittest.mock import patch
        clock = itertools.count(0, 0.1)
        with patch("rep_control.time.monotonic", side_effect=lambda: next(clock)):
            self.send(kind="rep_session_start", count=0, seq=0)
            for count in range(1, 141):
                self.assertEqual(self.send(seq=count, count=count)["status"], "accepted")
            self.assertEqual(len(self.controller.receipts), 128)
            self.assertEqual(len(self.link["cache"]), 64)
            self.assertEqual(len(self.coordinator.feedback_delivery.tickets), 128)
            self.assertEqual(self.send(seq=1)["reason"], "stale_sequence")
            self.assertEqual(self.send(seq=141, count=1)["status"], "rejected")
            self.assertEqual(self.coordinator.feedback_delivery.event_id, 140)
            for index in range(1, 256):
                self.assertEqual(self.send(kind="rep_session_start", seq=141+index,
                    identity=f"set{index}", count=0)["status"], "session_started")
            self.assertEqual(len(self.controller.used_sessions), 256)
            self.assertEqual(self.send(kind="rep_session_start", seq=500,
                identity="tooMany", count=0)["status"], "rejected")


class RepGatewayTests(unittest.IsolatedAsyncioTestCase):
    server_module = server

    @classmethod
    def setUpClass(cls):
        tls_fixture.VisualizerGatewayTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        tls_fixture.VisualizerGatewayTests.tearDownClass.__func__(cls)

    async def asyncSetUp(self):
        self.exercise_updates = []
        self.coordinator = self.server_module.CaptureCoordinator(auto_start=False, physical_mode=True)
        self.peers = []
        self.connections = []
        for node in (1, 2):
            peer = peers.PeerSocket(node, 100 + node, self.server_module)
            connection = self.server_module.NodeControlConnection(peer, node, 100 + node, self.coordinator)
            peer.connection = connection
            self.coordinator.register(connection)
            self.peers.append(peer); self.connections.append(connection)
        self.control = RepControlServer(self.coordinator, 12345678, port=0)
        self.control.start()
        self.gateway = VisualizerGateway(GatewayConfig(websocket_port=0, publisher_port=0,
            cert_file=str(self.cert_file), key_file=str(self.key_file), control_port=self.control.port))
        await self.gateway.start()
        self.context = ssl.create_default_context(cafile=str(self.cert_file))
        self.uri = f"wss://127.0.0.1:{self.gateway.websocket_port}"
        self.websocket = await websockets.connect(self.uri, ssl=self.context)
        await self.receive("rep_welcome")
        self.seq = 0
        self.exercise_updates = []

    async def asyncTearDown(self):
        await self.websocket.close()
        await self.gateway.close()
        self.control.close()
        manager = self.coordinator.feedback_delivery
        outboxes = list(manager.outboxes.values())
        manager.close()
        for outbox in outboxes:
            outbox.thread.join(timeout=1)
        for peer in self.peers:
            peer.close()

    async def receive(self, kind):
        while True:
            reply = json.loads(await asyncio.wait_for(self.websocket.recv(), 4))
            if reply["type"] == "exercise_update":
                self.exercise_updates.append(reply)
            if reply["type"] == kind:
                return reply

    async def send(self, kind="rep_event", count=1, identity="setA", run=12345678):
        self.seq += 1
        await self.websocket.send(request(kind, count, self.seq, identity, run))
        reply = await self.receive("rep_result")
        if any(node["status"] == "pending" for node in reply["nodes"]):
            final = await self.receive("rep_delivery")
            self.assertEqual(final["feedback_event_id"], reply["feedback_event_id"])
            reply["nodes"] = final["nodes"]
        return reply

    async def test_two_real_c_peers_duplicate_and_jump(self):
        await self.send("rep_session_start", 0)
        first = await self.send(count=1)
        self.assertEqual(first["status"], "accepted")
        self.assertEqual([node["status"] for node in first["nodes"]], ["applied"])
        duplicate = await self.send(count=1)
        self.assertEqual(duplicate["feedback_event_id"], first["feedback_event_id"])
        await self.send(count=4)
        self.assertEqual((await self.send(count=1))["status"], "duplicate")
        self.assertEqual([peer.outputs for peer in self.peers], [2, 0])

    async def test_reconnect_and_explicit_new_session(self):
        await self.send("rep_session_start", 0)
        await self.send(count=1)
        await self.websocket.close()
        await asyncio.sleep(0.1)
        self.websocket = await websockets.connect(self.uri, ssl=self.context)
        await self.receive("rep_welcome")
        self.seq = 0
        self.assertEqual((await self.send(count=2))["reason"], "sync_required")
        await self.send("rep_sync", 7)
        self.assertEqual((await self.send(count=7))["status"], "duplicate")
        self.assertEqual([peer.outputs for peer in self.peers], [1, 0])
        await self.send(count=8)
        await self.send("rep_session_start", 0, "setB")
        await self.send(count=1, identity="setB")
        self.assertEqual([peer.outputs for peer in self.peers], [3, 0])
        self.assertEqual((await self.send(count=9))["status"], "rejected")

    async def test_unavailable_node_and_timeout_results(self):
        await self.send("rep_session_start", 0)
        self.coordinator.unregister(self.connections[0])
        result = await self.send(count=1)
        self.assertEqual([n["status"] for n in result["nodes"]], ["unavailable"])
        self.coordinator.register(self.connections[0])
        self.peers[0].drop_acks = 100
        result = await self.send(count=2)
        self.assertEqual([n["status"] for n in result["nodes"]], ["timeout"])
        self.assertEqual(self.peers[0].outputs, 1)  # Retries do not reapply.

    async def test_concurrent_exercise_updates_and_single_wss_writer(self):
        await self.send("rep_session_start", 0)
        active = self.gateway._active_client
        original = active.send
        in_flight = 0
        maximum = 0

        async def tracked(payload):
            nonlocal in_flight, maximum
            in_flight += 1; maximum = max(maximum, in_flight)
            await asyncio.sleep(0.003)
            try:
                await original(payload)
            finally:
                in_flight -= 1

        active.send = tracked

        async def updates():
            for seq in range(20):
                self.gateway.publish(build_message(run_id=12345678, seq=seq, ts_us=seq, exercise="squat"))
                await asyncio.sleep(0.01)

        feed = asyncio.create_task(updates())
        for count in range(1, 4):
            self.assertEqual((await self.send(count=count))["status"], "accepted")
        await feed
        if not self.exercise_updates:
            await self.receive("exercise_update")
        self.assertGreaterEqual(self.exercise_updates[-1]["seq"], 0)
        self.assertEqual(maximum, 1)
        self.assertEqual([peer.outputs for peer in self.peers], [3, 0])
        self.assertEqual(self.gateway.updates._queue.maxsize, 1)

    async def test_simulated_phone_cli(self):
        from pathlib import Path
        import sys
        await self.websocket.close()
        await asyncio.sleep(0.1)
        process = await asyncio.create_subprocess_exec(sys.executable,
            str(Path(__file__).with_name("simulate_phone_reps.py")),
            "--url", self.uri, "--ca-cert", str(self.cert_file),
            "--counts", "1,1,4", "--interval", "0.25",
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        output, error = await asyncio.wait_for(process.communicate(), 12)
        self.assertEqual(process.returncode, 0, (output + error).decode())
        self.assertEqual([peer.outputs for peer in self.peers], [2, 0])

    async def test_checked_in_csharp_transport_over_wss(self):
        from pathlib import Path
        executable = Path(__file__).resolve().parents[2] / ".build/unity_host/ProtocolHost.dll"
        if not executable.exists():
            self.skipTest("Run communications/unity/tests/run_protocol_host.ps1 first")
        await self.websocket.close()
        await asyncio.sleep(0.1)
        process = await asyncio.create_subprocess_exec("dotnet", str(executable),
            "wss", self.uri, str(self.cert_file), stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
        output, error = await asyncio.wait_for(process.communicate(), 15)
        self.assertEqual(process.returncode, 0, (output + error).decode())
        self.assertEqual([peer.outputs for peer in self.peers], [1, 0])

    async def test_disconnect_cancels_retry_backlog(self):
        import time
        await self.send("rep_session_start", 0)
        for peer in self.peers:
            peer.drop_acks = 100
        self.seq += 1
        await self.websocket.send(request(count=1, seq=self.seq))
        result = await self.receive("rep_result")
        self.assertEqual(result["status"], "accepted")
        await asyncio.sleep(0.15)
        self.assertEqual([peer.outputs for peer in self.peers], [1, 0])
        await self.websocket.close()
        await asyncio.sleep(0.15)
        self.assertFalse(self.coordinator.feedback_delivery.pending)
        self.coordinator.feedback_delivery.tick(time.monotonic_ns() // 1000 + 800000)
        self.assertEqual([len(peer.frames) for peer in self.peers], [1, 0])

    async def test_stale_run_and_unsupported_phone_commands(self):
        self.assertEqual((await self.send("rep_session_start", 0, run=99))["reason"], "stale_run")
        await self.websocket.send('{"v":1,"type":"STOP"}')
        self.assertEqual((await self.receive("rep_result"))["status"], "rejected")
        self.assertEqual([peer.outputs for peer in self.peers], [0, 0])

    async def test_oversized_phone_packet_closes_bounded_receiver(self):
        await self.websocket.send("x" * 1025)
        with self.assertRaises(websockets.exceptions.ConnectionClosed):
            await self.websocket.recv()
        self.assertEqual([peer.outputs for peer in self.peers], [0, 0])


class AIRepGatewayTests(RepGatewayTests):
    server_module = ai_fixture.ai_server


if __name__ == "__main__":
    unittest.main()
