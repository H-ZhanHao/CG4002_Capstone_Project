"""Transparent TCP relay for protected laptop-to-Ultra96 paths.

The relay intentionally knows nothing about the sensor, TLS, WebSocket, or JSON
protocols. Each accepted client receives one independent upstream connection,
and bytes are copied in both directions until either side closes or fails. The
historical filename is retained because existing runbooks invoke it for both
the FireBeetle and visualizer routes.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import itertools
import logging
import socket
from dataclasses import dataclass


LOGGER = logging.getLogger("tcp_relay")
COPY_CHUNK_SIZE = 4096
STREAM_BUFFER_LIMIT = 64 * 1024
WRITE_BUFFER_HIGH_WATER = 64 * 1024
WRITE_BUFFER_LOW_WATER = 16 * 1024


@dataclass(frozen=True)
class RelayConfig:
    """Network endpoints and timeout used for every relayed connection."""

    listen_host: str = "0.0.0.0"
    listen_port: int = 15000
    upstream_host: str = "127.0.0.1"
    upstream_port: int = 15001
    connect_timeout: float = 5.0


class TransparentRelay:
    """Manage the listener and independent downstream/upstream connection pairs."""

    def __init__(self, config: RelayConfig) -> None:
        """Create relay bookkeeping; the listening socket starts in ``start``."""
        self.config = config
        self._server: asyncio.AbstractServer | None = None
        self._connection_ids = itertools.count(1)
        self._connection_tasks: set[asyncio.Task[None]] = set()
        self._connection_writers: set[asyncio.StreamWriter] = set()

    @property
    def sockets(self) -> tuple[socket.socket, ...]:
        """Return the active listening sockets, or an empty tuple before start."""
        if self._server is None or self._server.sockets is None:
            return ()
        return tuple(self._server.sockets)

    async def start(self) -> None:
        """Open the laptop listener without blocking on incoming connections."""
        if self._server is not None:
            raise RuntimeError("relay is already running")

        self._server = await asyncio.start_server(
            self._handle_connection,
            self.config.listen_host,
            self.config.listen_port,
            limit=STREAM_BUFFER_LIMIT,
        )

        addresses = ", ".join(
            str(sock.getsockname()) for sock in self.sockets
        )
        LOGGER.info(
            "Listening on %s; forwarding to %s:%d",
            addresses,
            self.config.upstream_host,
            self.config.upstream_port,
        )

    async def serve_forever(self) -> None:
        """Accept clients until this coroutine is cancelled or the relay closes."""
        if self._server is None:
            raise RuntimeError("relay has not been started")

        async with self._server:
            await self._server.serve_forever()

    async def close(self) -> None:
        """Stop accepting clients and cleanly close every active connection pair."""
        server = self._server
        self._server = None

        if server is not None:
            server.close()
            await server.wait_closed()

        writers = tuple(self._connection_writers)
        for writer in writers:
            writer.close()
        await asyncio.gather(
            *(self._wait_closed(writer) for writer in writers),
            return_exceptions=True,
        )

        current_task = asyncio.current_task()
        tasks = tuple(
            task
            for task in self._connection_tasks
            if task is not current_task and not task.done()
        )
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    async def _handle_connection(
        self,
        downstream_reader: asyncio.StreamReader,
        downstream_writer: asyncio.StreamWriter,
    ) -> None:
        """Give one downstream client its own upstream socket and two copy tasks.

        Whichever copy direction finishes first ends the connection pair. This
        prevents a half-closed or failed side from leaving the other task stuck.
        """
        task = asyncio.current_task()
        if task is not None:
            self._connection_tasks.add(task)

        connection_id = next(self._connection_ids)
        peer = downstream_writer.get_extra_info("peername")
        upstream_writer: asyncio.StreamWriter | None = None
        forward_tasks: set[asyncio.Task[None]] = set()
        byte_counts = {"upstream": 0, "downstream": 0}

        self._connection_writers.add(downstream_writer)
        self._configure_writer(downstream_writer)
        LOGGER.info("[%d] Client connected from %s", connection_id, peer)

        try:
            upstream_reader, upstream_writer = await asyncio.wait_for(
                asyncio.open_connection(
                    self.config.upstream_host,
                    self.config.upstream_port,
                    limit=STREAM_BUFFER_LIMIT,
                ),
                timeout=self.config.connect_timeout,
            )
            self._connection_writers.add(upstream_writer)
            self._configure_writer(upstream_writer)
            LOGGER.info(
                "[%d] Upstream connected to %s:%d",
                connection_id,
                self.config.upstream_host,
                self.config.upstream_port,
            )

            forward_tasks = {
                asyncio.create_task(
                    self._copy_stream(
                        downstream_reader,
                        upstream_writer,
                        byte_counts,
                        "upstream",
                    ),
                    name=f"relay-{connection_id}-upstream",
                ),
                asyncio.create_task(
                    self._copy_stream(
                        upstream_reader,
                        downstream_writer,
                        byte_counts,
                        "downstream",
                    ),
                    name=f"relay-{connection_id}-downstream",
                ),
            }

            done, pending = await asyncio.wait(
                forward_tasks,
                return_when=asyncio.FIRST_COMPLETED,
            )

            for completed in done:
                completed.result()
            for pending_task in pending:
                pending_task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)

        except asyncio.TimeoutError:
            LOGGER.warning(
                "[%d] Upstream connection timed out after %.1f seconds",
                connection_id,
                self.config.connect_timeout,
            )
        except asyncio.CancelledError:
            raise
        except (ConnectionError, OSError) as error:
            LOGGER.warning("[%d] Connection ended with error: %s", connection_id, error)
        finally:
            for forward_task in forward_tasks:
                if not forward_task.done():
                    forward_task.cancel()
            if forward_tasks:
                await asyncio.gather(*forward_tasks, return_exceptions=True)

            writers = [downstream_writer]
            if upstream_writer is not None:
                writers.append(upstream_writer)

            for writer in writers:
                writer.close()
            await asyncio.gather(
                *(self._wait_closed(writer) for writer in writers),
                return_exceptions=True,
            )
            for writer in writers:
                self._connection_writers.discard(writer)

            if task is not None:
                self._connection_tasks.discard(task)

            LOGGER.info(
                "[%d] Closed %s; client->upstream=%d bytes, upstream->client=%d bytes",
                connection_id,
                peer,
                byte_counts["upstream"],
                byte_counts["downstream"],
            )

    @staticmethod
    async def _copy_stream(
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        byte_counts: dict[str, int],
        counter_name: str,
    ) -> None:
        """Copy opaque bytes in one direction and update its traffic counter."""
        while True:
            data = await reader.read(COPY_CHUNK_SIZE)
            if not data:
                return

            writer.write(data)
            await writer.drain()
            byte_counts[counter_name] += len(data)

    @staticmethod
    def _configure_writer(writer: asyncio.StreamWriter) -> None:
        """Bound queued data and favor low latency for each TCP socket."""
        transport = writer.transport
        transport.set_write_buffer_limits(
            high=WRITE_BUFFER_HIGH_WATER,
            low=WRITE_BUFFER_LOW_WATER,
        )

        sock = writer.get_extra_info("socket")
        if sock is not None:
            with contextlib.suppress(OSError):
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    @staticmethod
    async def _wait_closed(writer: asyncio.StreamWriter) -> None:
        """Wait for socket cleanup while tolerating an already-lost peer."""
        with contextlib.suppress(ConnectionError, OSError):
            await writer.wait_closed()


def positive_port(value: str) -> int:
    """Parse a command-line TCP port and reject values outside its valid range."""
    port = int(value)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def positive_timeout(value: str) -> float:
    """Parse a command-line timeout that must be greater than zero seconds."""
    timeout = float(value)
    if timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be greater than zero")
    return timeout


def parse_args() -> argparse.Namespace:
    """Read the listener, upstream, timeout, and logging command-line options."""
    parser = argparse.ArgumentParser(
        description="Transparent bidirectional TCP relay to an Ultra96 service."
    )
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--listen-port", type=positive_port, default=15000)
    parser.add_argument("--upstream-host", default="127.0.0.1")
    parser.add_argument("--upstream-port", type=positive_port, default=15001)
    parser.add_argument(
        "--connect-timeout",
        type=positive_timeout,
        default=5.0,
        help="seconds allowed for each upstream connection (default: 5)",
    )
    parser.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        default="INFO",
    )
    return parser.parse_args()


async def run(config: RelayConfig) -> None:
    """Run the transparent relay and guarantee cleanup when it stops."""
    relay = TransparentRelay(config)
    await relay.start()
    try:
        await relay.serve_forever()
    finally:
        await relay.close()


def main() -> None:
    """Configure terminal logging, build the relay configuration, and run it."""
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    config = RelayConfig(
        listen_host=args.listen_host,
        listen_port=args.listen_port,
        upstream_host=args.upstream_host,
        upstream_port=args.upstream_port,
        connect_timeout=args.connect_timeout,
    )

    try:
        asyncio.run(run(config))
    except KeyboardInterrupt:
        LOGGER.info("Relay stopped")


if __name__ == "__main__":
    main()
