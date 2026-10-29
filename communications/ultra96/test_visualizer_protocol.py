"""Focused tests for the version-1 visualizer message contract."""

import unittest

from visualizer_protocol import (
    VisualizerProtocolError,
    build_message,
    parse_message,
    serialize_message,
    validate_message,
)


def required_message():
    return build_message(
        run_id=312, seq=481, ts_us=9841200, exercise="squat"
    )


class VisualizerProtocolTests(unittest.TestCase):
    """Protect the required JSON fields, ranges, parsing, and compact encoding."""

    def test_valid_required_fields(self):
        message = required_message()
        self.assertEqual(validate_message(message), message)

    def test_compact_serialization(self):
        self.assertEqual(
            serialize_message(required_message()),
            '{"v":1,"type":"exercise_update","run_id":312,"seq":481,'
            '"ts_us":9841200,"exercise":"squat"}',
        )

    def test_malformed_json_is_rejected(self):
        with self.assertRaisesRegex(VisualizerProtocolError, "malformed JSON"):
            parse_message('{"v":1')

    def test_invalid_versions_types_and_ranges_are_rejected(self):
        mutations = (
            ("v", 2),
            ("v", True),
            ("type", "feedback"),
            ("run_id", -1),
            ("seq", -1),
            ("ts_us", -1),
            ("exercise", ""),
        )
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                message = required_message()
                message[field] = value
                with self.assertRaises(VisualizerProtocolError):
                    validate_message(message)

    def test_missing_required_field_is_rejected(self):
        message = required_message()
        del message["exercise"]
        with self.assertRaisesRegex(VisualizerProtocolError, "missing required"):
            validate_message(message)


if __name__ == "__main__":
    unittest.main()
