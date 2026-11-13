"""Run the Stage 2 C-peer tests against the actual AI server coordinator.

The intended window_assembler_updated dependency belongs to the Ultra96 setup.
Supply a test-only stand-in while loading this module; no inference is run and
no production import or teammate implementation is changed.
"""
import importlib.util
from pathlib import Path
import sys
from unittest.mock import patch

import test_feedback_delivery as feedback_tests
import window_assembler

spec = importlib.util.spec_from_file_location(
    "_stage2_test_server_ai", Path(__file__).with_name("server_AI.py"))
ai_server = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {
        "window_assembler_updated": window_assembler,
        spec.name: ai_server,
}):
    spec.loader.exec_module(ai_server)


class AIServerFeedbackTests(feedback_tests.FeedbackTests):
    server_module = ai_server
