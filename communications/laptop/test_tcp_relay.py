"""Local integration tests for tcp_relay.py using real loopback sockets."""

from __future__ import annotations

import asyncio
import contextlib
import unittest

from tcp_relay import RelayConfig, TransparentRelay


async def echo_handler(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
) -> None:
    try:
        while data := await reader.read(4096):
            writer.write(data)
            await writer.drain()
    finally:
        writer.close()
        with contextlib.suppress(ConnectionError, OSError):
            await writer.wait_closed()


class TransparentRelayTests(unittest.IsolatedAsyncioTestCase):
    """Verify opaque forwarding, client isolation, and connection shutdown."""

    async def asyncSetUp(self) -> None:
        self.upstream = await asyncio.start_server(echo_handler, "127.0.0.1", 0)
        upstream_port = self.upstream.sockets[0].getsockname()[1]

        self.relay = TransparentRelay(
            RelayConfig(
                listen_host="127.0.0.1",
                listen_port=0,
                upstream_host="127.0.0.1",
                upstream_port=upstream_port,
                connect_timeout=1.0,
            )
        )
        await self.relay.start()
        self.relay_port = self.relay.sockets[0].getsockname()[1]

    async def asyncTearDown(self) -> None:
        await self.relay.close()
        self.upstream.close()
        await self.upstream.wait_closed()

    async def test_fragmented_and_coalesced_bytes_are_unchanged(self) -> None:
        reader, writer = await asyncio.open_connection(
            "127.0.0.1", self.relay_port
        )
        fragments = [b"\xA5", b"\x5A\x01\x10", bytes(range(64)), b"tail"]
        expected = b"".join(fragments)

        for fragment in fragments:
            writer.write(fragment)
            await writer.drain()

        received = await asyncio.wait_for(
            reader.readexactly(len(expected)), timeout=1.0
        )
        self.assertEqual(received, expected)
        await self._close_writer(writer)

    async def test_multiple_clients_are_independent(self) -> None:
        async def round_trip(client_number: int) -> bytes:
            reader, writer = await asyncio.open_connection(
                "127.0.0.1", self.relay_port
            )
            payload = bytes([client_number]) * 8192
            writer.write(payload)
            await writer.drain()
            received = await asyncio.wait_for(
                reader.readexactly(len(payload)), timeout=2.0
            )
            await self._close_writer(writer)
            return received

        results = await asyncio.gather(*(round_trip(i) for i in range(1, 5)))
        for index, result in enumerate(results, start=1):
            self.assertEqual(result, bytes([index]) * 8192)

    async def test_upstream_eof_closes_hub_side(self) -> None:
        connections: asyncio.Queue[asyncio.StreamWriter] = asyncio.Queue()

        async def capture_handler(
            _reader: asyncio.StreamReader,
            writer: asyncio.StreamWriter,
        ) -> None:
            await connections.put(writer)
            await writer.wait_closed()

        replacement = await asyncio.start_server(
            capture_handler, "127.0.0.1", 0
        )
        replacement_port = replacement.sockets[0].getsockname()[1]
        second_relay = TransparentRelay(
            RelayConfig(
                listen_host="127.0.0.1",
                listen_port=0,
                upstream_host="127.0.0.1",
                upstream_port=replacement_port,
                connect_timeout=1.0,
            )
        )
        await second_relay.start()

        try:
            relay_port = second_relay.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection("127.0.0.1", relay_port)
            upstream_writer = await asyncio.wait_for(connections.get(), timeout=1.0)
            upstream_writer.close()
            await upstream_writer.wait_closed()

            self.assertEqual(
                await asyncio.wait_for(reader.read(1), timeout=1.0), b""
            )
            await self._close_writer(writer)
        finally:
            await second_relay.close()
            replacement.close()
            await replacement.wait_closed()

    @staticmethod
    async def _close_writer(writer: asyncio.StreamWriter) -> None:
        writer.close()
        with contextlib.suppress(ConnectionError, OSError):
            await writer.wait_closed()


if __name__ == "__main__":
    unittest.main()
