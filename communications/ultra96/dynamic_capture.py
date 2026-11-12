"""Independently acknowledged node capture on one Ultra96 timeline.

Protocol constants and the existing base coordinator are supplied by either
server variant. Clock synchronization remains owned by NodeControlConnection.
"""

from dataclasses import dataclass
import logging
import struct
import time

LOGGER = logging.getLogger("server")
STREAM_TIMEOUT_US = 3_000_000


@dataclass
class NodeState:
    phase: str = "connected"
    sequence: int | None = None
    deadline: int = 0
    generation: int = 0
    boundary: int = 0
    last_sample: int = 0
    live_retry: bool = False


def coordinator_class(api):
    """Build the same dynamic coordinator against either server's protocol API."""

    class DynamicCaptureCoordinator(api.CaptureCoordinatorBase):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            if not self.physical_mode:
                raise ValueError("capture requires physical sensor streaming")
            self.nodes = {}
            self._run_origin = None
            self._run_sequence = None

        def _refresh_flags(self):
            self.arm_pending = any(s.phase == "arming" for s in self.nodes.values())
            self._arm_ready = any(s.phase == "armed" for s in self.nodes.values())
            self.start_pending = any(s.phase == "starting" for s in self.nodes.values())
            self._pending_arm_acks = {n: s.sequence for n, s in self.nodes.items()
                                      if s.phase == "arming"}
            self._pending_start_acks = {n: s.sequence for n, s in self.nodes.items()
                                        if s.phase == "starting"}

        def _forget_samples(self, node):
            target = self.window_assembler
            if target is not None:
                # InferencePipeline exposes its underlying assembler.
                target = getattr(target, "assembler", target)
                target.node_lost(node)

        def _expect_samples(self, node):
            target = self.window_assembler
            if target is not None:
                target = getattr(target, "assembler", target)
                target.node_joined(node)

        def register(self, connection):
            with self._lock:
                if connection.node_id not in self.required_nodes:
                    raise ValueError("unsupported node identity")
                previous = self._connections.get(connection.node_id)
                if previous is not None:
                    self.unregister(previous)
                    try:
                        previous.client.shutdown(2)
                    except OSError:
                        pass
                self._connections[connection.node_id] = connection
                self.nodes[connection.node_id] = NodeState()
                self.feedback_delivery.register(connection)

        def unregister(self, connection):
            with self._lock:
                node = connection.node_id
                if self._connections.get(node) is not connection:
                    return
                self.feedback_delivery.unregister(connection)
                connection.cancel_arm_sync()
                del self._connections[node]
                del self.nodes[node]
                self._pending_stop_acks.pop(node, None)
                self._forget_samples(node)
                self._refresh_flags()
                if self._run_origin is not None and not any(
                        s.phase in ("streaming", "starting") for s in self.nodes.values()):
                    self._stop_locked("all streams lost")

        def _mapping(self, connection, now):
            mapping = connection.get_mapping(now)
            if (mapping is None or now - mapping.synchronized_at_us >
                    api.CLOCK_MAPPING_ARM_HOLDOVER_MAX_AGE_US):
                connection.request_arm_sync("independent node readiness", now)
                return None
            if now - mapping.synchronized_at_us > api.CLOCK_MAPPING_MAX_AGE_US:
                connection.request_arm_sync("mapping holdover refresh", now)
            return mapping

        def mapping_updated(self, connection):
            with self._lock:
                if self._connections.get(connection.node_id) is connection:
                    self._arm_locked("node synchronized")

        def _send(self, connection, kind, payload):
            sequence = self._next_sequence()
            state = self.nodes[connection.node_id]
            state.sequence = sequence
            state.deadline = time.monotonic_ns() // 1000 + int(
                api.CONTROL_ACK_TIMEOUT_SECONDS * 1_000_000)
            frame = api.build_header(kind, node_id=0, payload_length=len(payload),
                                     session_id=connection.session_id,
                                     sequence=sequence) + payload
            try:
                connection.send(frame)
            except OSError:
                # Other nodes still get their commands; isolate this transport.
                self.unregister(connection)
                return False
            LOGGER.info("NODE_CONTROL_SENT node=%d type=%d sequence=%d phase=%s",
                        connection.node_id, kind, sequence, state.phase)
            return True

        def _arm_locked(self, reason):
            if not self.desired_running:
                return False
            now = time.monotonic_ns() // 1000
            sent = False
            for node, connection in list(self._connections.items()):
                state = self.nodes[node]
                if state.phase != "connected" or node in self._pending_stop_acks:
                    continue
                mapping = self._mapping(connection, now)
                if mapping is None:
                    continue
                if self._run_origin is not None:
                    sent = self._start_node(connection, mapping, now, live=True) or sent
                    continue
                self._arm_generation = (self._arm_generation + 1) & 0xFFFFFFFF or 1
                state.generation = self._arm_generation
                state.boundary = now + api.ARM_LEAD_US
                state.phase = "arming"
                self._armed_after_us = state.boundary
                sent = self._send(connection, api.MESSAGE_TYPE_ARM, struct.pack(
                    "!QqI", state.boundary, mapping.node_minus_coordinator_us,
                    state.generation)) or sent
            self._refresh_flags()
            return sent

        def _start_node(self, connection, mapping, peak, live=False, trigger_node=0,
                        trigger_sequence=0):
            state = self.nodes[connection.node_id]
            state.phase = "starting"
            state.live_retry = live
            connection.begin_physical_capture()
            # A zero-duration START_FLUSH is the new profiles' live-only join.
            sent = self._send(connection, api.MESSAGE_TYPE_START_FLUSH, struct.pack(
                "!QqIB3xI", peak, mapping.node_minus_coordinator_us,
                0 if live else api.PHYSICAL_HISTORY_DURATION_US,
                trigger_node or connection.node_id, trigger_sequence))
            self._refresh_flags()
            return sent

        def activity_triggered(self, connection, header, trigger):
            with self._lock:
                if (self._connections.get(connection.node_id) is not connection or
                        not self.desired_running or self._run_origin is not None):
                    return False
                state = self.nodes[connection.node_id]
                if (state.phase != "armed" or trigger[0] != connection.boot_id or
                        trigger[1] != state.generation):
                    return False
                now = time.monotonic_ns() // 1000
                mapping = self._mapping(connection, now)
                if mapping is None:
                    state.phase = "connected"
                    self._refresh_flags()
                    return False
                peak = trigger[2] - mapping.node_minus_coordinator_us
                if peak <= state.boundary:
                    return False
                self._run_origin = peak - api.PHYSICAL_HISTORY_DURATION_US
                self._run_sequence = self._next_sequence()
                self.idle_paused = False
                participants = []
                for node, item in list(self._connections.items()):
                    other = self.nodes[node]
                    node_mapping = self._mapping(item, now)
                    if node_mapping is not None and other.phase in ("armed", "arming"):
                        participants.append((item, node_mapping))
                        other.phase = "starting"
                # Mark all candidates first so one failed send cannot end the
                # run before another candidate has received its start command.
                for item, node_mapping in participants:
                    if self._connections.get(item.node_id) is item:
                        self._start_node(item, node_mapping, peak,
                                         trigger_node=connection.node_id,
                                         trigger_sequence=header[7])
                return True

        def _stop_locked(self, reason):
            super()._stop_locked(reason)
            self._run_origin = self._run_sequence = None
            now = time.monotonic_ns() // 1000
            for state in self.nodes.values():
                state.phase = "stopping"
                state.sequence = None
                state.deadline = now + int(api.CONTROL_ACK_TIMEOUT_SECONDS * 1_000_000)
            self._refresh_flags()

        def _publish_activity(self, active):
            # A newly connected board is not active until its stream starts.
            nodes = tuple(node for node, state in self.nodes.items()
                          if not active or state.phase == "streaming")
            try:
                self.feedback_delivery.submit(
                    api.FEEDBACK_ACTIVITY_ACTIVE if active else api.FEEDBACK_ACTIVITY_IDLE,
                    self._next_sequence(), api.build_header, nodes)
            except Exception:
                LOGGER.exception("ACTIVITY_FEEDBACK_FAILED active=%s", active)

        def control_acknowledged(self, connection, command_type, command_sequence,
                                 result, effective_local_us):
            with self._lock:
                if self._connections.get(connection.node_id) is not connection:
                    return
                if command_type == api.MESSAGE_TYPE_FEEDBACK_COMMAND:
                    # Feedback delivery is independent of capture commands.
                    # The shared base no longer has the legacy group-ACK method.
                    try:
                        return self.feedback_delivery.acknowledge(
                            connection, command_sequence, result, effective_local_us)
                    except ValueError as error:
                        raise api.ProtocolError(
                            api.ERROR_INVALID_FRAME, api.MESSAGE_TYPE_CONTROL_ACK,
                            str(error)) from error
                node = connection.node_id
                state = self.nodes[node]
                now = time.monotonic_ns() // 1000
                if command_type == api.MESSAGE_TYPE_STOP:
                    if self._pending_stop_acks.get(node) != command_sequence:
                        return  # Obsolete/duplicate ACK has no authority.
                    if result not in (api.CONTROL_RESULT_APPLIED,
                                      api.CONTROL_RESULT_ALREADY_STOPPED):
                        self._drop(connection)
                        return
                    del self._pending_stop_acks[node]
                    state.phase = "connected"
                    self._arm_locked("STOP confirmed")
                    return
                expected_type = {"arming": api.MESSAGE_TYPE_ARM,
                                 "starting": api.MESSAGE_TYPE_START_FLUSH}.get(state.phase)
                if command_type != expected_type or command_sequence != state.sequence:
                    return
                LOGGER.info("NODE_CONTROL_ACK node=%d phase=%s sequence=%d result=%d",
                            node, state.phase, command_sequence, result)
                if now >= state.deadline:
                    self._drop(connection)
                    return
                if result != api.CONTROL_RESULT_APPLIED:
                    if state.phase == "starting" and not state.live_retry:
                        mapping = self._mapping(connection, now)
                        if mapping is not None:
                            self._start_node(connection, mapping, now, live=True)
                            return
                    self._drop(connection)
                    return
                if state.phase == "arming":
                    if now >= state.boundary:
                        state.phase = "connected"
                        self._arm_locked("late ARM ACK")
                    else:
                        state.phase = "armed"
                else:
                    state.phase = "streaming"
                    state.last_sample = now
                    if not self.run_active:
                        self.run_active = True
                        if self.window_assembler is not None:
                            self.window_assembler.start_run(self._run_sequence, self._run_origin)
                        for candidate, member in self.nodes.items():
                            if member.phase in ("streaming", "starting"):
                                self._expect_samples(candidate)
                    else:
                        self._expect_samples(node)
                    self._publish_activity(True)
                self._refresh_flags()

        def _drop(self, connection):
            self.unregister(connection)
            try:
                connection.client.shutdown(2)
            except OSError:
                pass

        def submit_sample(self, connection, sample):
            # Serialize capture transitions with submission, including reconnect.
            with self._lock:
                state = self.nodes.get(connection.node_id)
                if (self._connections.get(connection.node_id) is not connection or
                        state is None or state.phase != "streaming" or not self.run_active):
                    return []
                state.last_sample = time.monotonic_ns() // 1000
                return self.window_assembler.submit(sample)

        def tick(self, now_us=None):
            now = time.monotonic_ns() // 1000 if now_us is None else now_us
            self.feedback_delivery.tick(now)
            with self._lock:
                for node, state in list(self.nodes.items()):
                    if ((state.phase in ("arming", "starting", "stopping") and
                         now >= state.deadline) or
                            (state.phase == "streaming" and
                             now - state.last_sample >= STREAM_TIMEOUT_US)):
                        LOGGER.warning("NODE_TIMEOUT node=%d phase=%s", node, state.phase)
                        self._drop(self._connections[node])
                self._arm_locked("readiness tick")
            return False

    return DynamicCaptureCoordinator
