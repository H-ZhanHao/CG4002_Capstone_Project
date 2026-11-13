"""Feedback ACKs must survive the independent-capture coordinator refactor."""

import struct
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

import server_rep_fall as server


class DynamicCaptureFeedbackTests(unittest.TestCase):
    def setUp(self):
        self.assembler = Mock()
        self.coordinator = server.CaptureCoordinator(window_assembler=self.assembler)
        self.addCleanup(self.coordinator.feedback_delivery.close)
        self.connections = {}
        for node in (1, 2):
            connection = SimpleNamespace(node_id=node, session_id=100 + node,
                                         coordinator=self.coordinator, send=Mock(),
                                         cancel_arm_sync=Mock())
            self.connections[node] = connection
            self.coordinator.register(connection)

    def acknowledge(self, node, command_type, sequence, result=0, connection=None):
        connection = connection or self.connections[node]
        payload = struct.pack("!BBHIQ", command_type, result, 0, sequence, 123456)
        frame = server.build_header(
            server.MESSAGE_TYPE_CONTROL_ACK, node_id=node,
            session_id=connection.session_id, sequence=sequence,
            payload_length=len(payload)) + payload
        server.process_control_ack(frame, connection)

    def test_start_and_feedback_acks_keep_both_streams_running(self):
        coordinator = self.coordinator
        coordinator._run_origin = 10_000_000
        coordinator._run_sequence = 38
        for node, state in coordinator.nodes.items():
            state.phase = "starting"
            state.sequence = 38 + node
            state.deadline = time.monotonic_ns() // 1000 + 30_000_000

        self.acknowledge(1, server.MESSAGE_TYPE_START_FLUSH, 39)
        previous = coordinator.feedback_delivery.last[0]
        self.acknowledge(2, server.MESSAGE_TYPE_START_FLUSH, 40)
        deliveries = list(coordinator.feedback_delivery.last)
        self.assertEqual(previous.status, "superseded")

        # The log shows an older activity event superseded by the second START
        # ACK. Its late ACK must be harmless, and the new event must complete.
        self.acknowledge(1, server.MESSAGE_TYPE_FEEDBACK_COMMAND, previous.sequence)
        for delivery in deliveries:
            self.acknowledge(delivery.connection.node_id,
                             server.MESSAGE_TYPE_FEEDBACK_COMMAND, delivery.sequence)
            self.assertEqual(delivery.status, "applied")
            self.assertEqual(delivery.effective_local_us, 123456)
        self.acknowledge(2, server.MESSAGE_TYPE_FEEDBACK_COMMAND, deliveries[-1].sequence)

        self.assertTrue(coordinator.run_active)
        self.assertEqual([state.phase for state in coordinator.nodes.values()],
                         ["streaming", "streaming"])
        self.assembler.start_run.assert_called_once_with(38, 10_000_000)
        self.assembler.stop_run.assert_not_called()
        self.assertFalse(coordinator.feedback_delivery.pending)

    def test_invalid_feedback_result_becomes_protocol_error(self):
        with self.assertRaises(server.ProtocolError) as raised:
            self.acknowledge(1, server.MESSAGE_TYPE_FEEDBACK_COMMAND, 42, result=9)
        self.assertEqual(raised.exception.code, server.ERROR_INVALID_FRAME)
        self.assertEqual(raised.exception.offending_type, server.MESSAGE_TYPE_CONTROL_ACK)

    def test_obsolete_connection_cannot_acknowledge_current_feedback(self):
        coordinator = self.coordinator
        coordinator.request_feedback(server.FEEDBACK_ACTIVITY_ACTIVE)
        delivery = coordinator.feedback_delivery.last[0]
        obsolete = SimpleNamespace(**vars(self.connections[1]))
        self.acknowledge(1, server.MESSAGE_TYPE_FEEDBACK_COMMAND, delivery.sequence,
                         connection=obsolete)
        self.assertEqual(delivery.status, "pending")
        self.acknowledge(1, server.MESSAGE_TYPE_FEEDBACK_COMMAND, delivery.sequence)
        self.assertEqual(delivery.status, "applied")


if __name__ == "__main__":
    unittest.main()
