"""TLS loopback integration tests for the visualizer gateway."""

from __future__ import annotations

import asyncio
import json
import logging
import shutil
import ssl
import subprocess
import tempfile
import unittest
from pathlib import Path

import websockets

from visualizer_gateway import (
    COLOR_RESET,
    MAX_PUBLISHER_LINE_BYTES,
    VISUALIZER_SENT_COLOR,
    GatewayConfig,
    VisualizerColorFormatter,
    VisualizerGateway,
    create_server_ssl_context,
)
from visualizer_protocol import build_message, serialize_message


class VisualizerColorFormatterTests(unittest.TestCase):
    """Keep purple highlighting specific to interactive successful sends."""

    @staticmethod
    def _record(message: str) -> logging.LogRecord:
        return logging.LogRecord(
            "visualizer_gateway", logging.INFO, __file__, 1, message, (), None
        )

    def test_successful_send_is_purple_on_terminal(self):
        formatter = VisualizerColorFormatter("%(message)s", use_color=True)
        line = "VISUALIZER sent run_id=26844716 seq=15 exercise=push_up"
        self.assertEqual(
            formatter.format(self._record(line)),
            f"{VISUALIZER_SENT_COLOR}{line}{COLOR_RESET}",
        )

    def test_redirected_and_other_output_remain_plain(self):
        sent = "VISUALIZER sent run_id=1 seq=2 exercise=squat"
        plain_formatter = VisualizerColorFormatter("%(message)s", use_color=False)
        color_formatter = VisualizerColorFormatter("%(message)s", use_color=True)
        self.assertEqual(plain_formatter.format(self._record(sent)), sent)
        connected = "Unity visualizer connected from ('127.0.0.1', 12345)"
        self.assertEqual(color_formatter.format(self._record(connected)), connected)


class VisualizerGatewayTests(unittest.IsolatedAsyncioTestCase):
    """Exercise loopback publication, pinned WSS delivery, and reconnection."""

    @classmethod
    def setUpClass(cls):
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("OpenSSL is required for temporary TLS certificates")
        cls._certificate_directory = tempfile.TemporaryDirectory()
        directory = Path(cls._certificate_directory.name)
        cls.cert_file = directory / "test-cert.pem"
        cls.key_file = directory / "test-key.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(cls.key_file), "-out", str(cls.cert_file),
                "-days", "1", "-subj", "/CN=127.0.0.1",
                "-addext", "subjectAltName=IP:127.0.0.1",
            ],
            check=True,
            capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls._certificate_directory.cleanup()

    async def asyncSetUp(self):
        self.gateway = VisualizerGateway(
            GatewayConfig(
                websocket_port=0,
                publisher_port=0,
                max_rate_hz=5.0,
                cert_file=str(self.cert_file),
                key_file=str(self.key_file),
            )
        )
        await self.gateway.start()
        self.uri = f"wss://127.0.0.1:{self.gateway.websocket_port}"
        self.client_ssl = ssl.create_default_context(cafile=str(self.cert_file))

    async def asyncTearDown(self):
        await self.gateway.close()

    async def _connect(self):
        return await websockets.connect(self.uri, ssl=self.client_ssl)

    async def test_wss_uses_validated_temporary_certificate(self):
        client = await self._connect()
        try:
            self.gateway.publish(build_message(run_id=1, seq=0, ts_us=0, exercise="unknown"))
            received = json.loads(await asyncio.wait_for(client.recv(), 1.0))
            self.assertEqual(received["seq"], 0)
        finally:
            await client.close()

    async def test_newest_update_replaces_pending_update_for_slow_client(self):
        self.gateway.publish(build_message(run_id=1, seq=0, ts_us=0, exercise="unknown"))
        client = await self._connect()
        try:
            self.assertEqual(json.loads(await client.recv())["seq"], 0)
            self.gateway.publish(build_message(run_id=1, seq=1, ts_us=1, exercise="squat"))
            await asyncio.sleep(0.02)
            self.gateway.publish(build_message(run_id=1, seq=2, ts_us=2, exercise="push_up"))
            received = json.loads(await asyncio.wait_for(client.recv(), 1.0))
            self.assertEqual(received["seq"], 2)
        finally:
            await client.close()

    async def test_disconnect_and_reconnect_receives_latest_update(self):
        first = await self._connect()
        self.gateway.publish(build_message(run_id=8, seq=10, ts_us=10, exercise="squat"))
        self.assertEqual(json.loads(await first.recv())["seq"], 10)
        await first.close()
        await asyncio.sleep(0.05)

        self.gateway.publish(build_message(run_id=8, seq=11, ts_us=11, exercise="push_up"))
        second = await self._connect()
        try:
            received = json.loads(await asyncio.wait_for(second.recv(), 1.0))
            self.assertEqual(received["seq"], 11)
        finally:
            await second.close()

    async def test_publisher_rejects_malformed_then_accepts_valid_json(self):
        reader, writer = await asyncio.open_connection(
            "127.0.0.1", self.gateway.publisher_port
        )
        del reader
        writer.write(b'{bad json}\n')
        writer.write(
            (serialize_message(build_message(run_id=2, seq=4, ts_us=5, exercise="squat")) + "\n").encode()
        )
        await writer.drain()
        client = await self._connect()
        try:
            self.assertEqual(json.loads(await asyncio.wait_for(client.recv(), 1.0))["seq"], 4)
        finally:
            await client.close()
        writer.close()
        await writer.wait_closed()

    async def test_oversized_publisher_input_is_rejected_without_stopping_gateway(self):
        reader, writer = await asyncio.open_connection(
            "127.0.0.1", self.gateway.publisher_port
        )
        writer.write(b"x" * (MAX_PUBLISHER_LINE_BYTES + 1) + b"\n")
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.read(), 1.0), b"")
        writer.close()
        await writer.wait_closed()

        _, valid_writer = await asyncio.open_connection(
            "127.0.0.1", self.gateway.publisher_port
        )
        valid_writer.write(
            (serialize_message(build_message(run_id=3, seq=7, ts_us=9, exercise="unknown")) + "\n").encode()
        )
        await valid_writer.drain()
        client = await self._connect()
        try:
            self.assertEqual(json.loads(await asyncio.wait_for(client.recv(), 1.0))["seq"], 7)
        finally:
            await client.close()
        valid_writer.close()
        await valid_writer.wait_closed()

    def test_tls_configuration_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "requires both"):
            create_server_ssl_context("", "")
        with self.assertRaisesRegex(ValueError, "does not exist"):
            create_server_ssl_context("missing-cert.pem", "missing-key.pem")


if __name__ == "__main__":
    unittest.main()
