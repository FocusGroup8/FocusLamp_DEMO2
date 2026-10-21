"""Source guards and an independent Python model for nine local welcomes.

Run: py -m unittest discover -s controller/tests -p test_local_welcome_policy.py -v

These tests do NOT compile, import, or execute controller C, ESP-IDF, FreeRTOS,
or board audio. Source guards inspect the small header's selected contracts.
ReferenceModelTests exercise intended transitions in Python and do not prove
C runtime behavior, worker scheduling, or the time actual audio starts.
"""

from dataclasses import dataclass
from pathlib import Path
import re
import unittest


HEADER = Path(__file__).resolve().parents[1] / "main/local_welcome_policy.h"
WELCOME_COUNT = 9


def code_only(source):
    return re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), source, flags=re.S,
    )


def function_body(source, name):
    masked = code_only(source)
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", masked)
    if match is None:
        raise AssertionError("Missing C definition: " + name)
    depth = 1
    for offset in range(match.end(), len(masked)):
        depth += (masked[offset] == "{") - (masked[offset] == "}")
        if depth == 0:
            return code_only(source[match.end():offset])
    raise AssertionError("Unclosed C definition: " + name)


@dataclass
class LocalWelcomeReference:
    """Intended policy model, not a binding to or execution of the C header."""

    next_index: int = 0
    active_index: int = 0
    pending: bool = False
    active: bool = False
    started: bool = False
    generation: int = 0
    active_generation: int = 0

    def request(self):
        self.pending = True

    def take(self):
        if self.active or not self.pending:
            return None
        self.generation = (self.generation + 1) & 0xFFFFFFFF
        self.active_generation = self.generation
        self.active_index = self.next_index
        self.active = True
        self.pending = False
        self.started = False
        return self.active_generation, self.active_index

    def cancelled(self, generation):
        return (not self.active or self.active_generation != generation or
                self.generation != generation)

    def start(self, generation):
        if self.cancelled(generation):
            return False
        return self.output_started(generation)

    def output_started(self, generation):
        if not self.active or self.active_generation != generation or self.started:
            return False
        self.started = True
        self.next_index = (self.active_index + 1) % WELCOME_COUNT
        return True

    def cancel(self):
        self.generation = (self.generation + 1) & 0xFFFFFFFF
        self.pending = False

    def finish(self, generation):
        if self.active_generation == generation:
            self.active = False
            self.started = False


class LocalWelcomeSourceGuardTests(unittest.TestCase):
    """Selected C source contracts only; no C runtime is exercised here."""

    @classmethod
    def setUpClass(cls):
        cls.source = HEADER.read_text(encoding="utf-8")
        cls.code = code_only(cls.source)

    def body(self, suffix):
        return function_body(self.source, "local_welcome_policy_" + suffix)

    def test_header_is_standalone_with_zero_initializable_state(self):
        self.assertRegex(self.code, r"#define\s+LOCAL_WELCOME_COUNT\s+9\b")
        self.assertEqual(re.findall(r"#include\s+[<\"]([^>\"]+)", self.code),
                         ["stdbool.h", "stdint.h"])
        for field in ("next_index", "active_index"):
            self.assertRegex(self.code, rf"\buint8_t\s+{field}\s*;")
        for field in ("pending", "active", "started"):
            self.assertRegex(self.code, rf"\bbool\s+{field}\s*;")
        for field in ("generation", "active_generation"):
            self.assertRegex(self.code, rf"\buint32_t\s+{field}\s*;")
        self.assertNotRegex(self.code, r"freertos|portENTER|Semaphore|TaskHandle")
        for suffix in ("request", "take", "cancelled", "start", "output_started",
                       "cancel", "finish"):
            self.assertRegex(self.code, rf"static\s+inline\s+(?:bool|void)\s+"
                             rf"local_welcome_policy_{suffix}\s*\(")

    def test_requests_coalesce_without_advancing_or_taking_ownership(self):
        body = self.body("request")
        self.assertIn("s->pending = true;", body)
        self.assertEqual(re.findall(r"s->(\w+)\s*=", body), ["pending"])

    def test_take_requires_pending_and_free_slot_then_returns_current_item(self):
        body = self.body("take")
        self.assertRegex(body, r"if\s*\(!s\s*\|\|\s*s->active\s*\|\|\s*!s->pending\)")
        ordered = ("++s->generation;", "s->active_generation = s->generation;",
                   "s->active_index = s->next_index;", "s->active = true;",
                   "s->pending = false;", "s->started = false;")
        offsets = [body.index(statement) for statement in ordered]
        self.assertEqual(offsets, sorted(offsets))
        self.assertIn("*generation_out = s->active_generation;", body)
        self.assertIn("*index_out = s->active_index;", body)
        self.assertNotRegex(body, r"s->next_index\s*=")

    def test_cancelled_checks_both_slot_ownership_and_cancellation_generation(self):
        body = self.body("cancelled")
        for check in ("!s", "!s->active", "s->active_generation != generation",
                      "s->generation != generation"):
            self.assertIn(check, body)

    def test_output_report_commits_once_despite_cancellation_and_owns_index_write(self):
        body = self.body("output_started")
        self.assertIn("!s || !s->active || s->active_generation != generation || s->started", body)
        self.assertNotRegex(body, r"s->generation\b|local_welcome_policy_cancelled")
        self.assertIn("s->started = true;", body)
        self.assertRegex(body, r"s->next_index\s*=\s*\(uint8_t\)\(\(s->active_index"
                         r"\s*\+\s*1U\)\s*%\s*LOCAL_WELCOME_COUNT\)")
        self.assertEqual(len(re.findall(r"s->next_index\s*=", self.code)), 1)

    def test_start_validates_cancellation_then_delegates_output_commit(self):
        body = self.body("start")
        self.assertIn("local_welcome_policy_cancelled(s, generation)", body)
        self.assertIn("return local_welcome_policy_output_started(s, generation);", body)
        self.assertLess(body.index("local_welcome_policy_cancelled"),
                        body.index("local_welcome_policy_output_started"))

    def test_cancel_invalidates_generation_and_drops_only_pending(self):
        body = self.body("cancel")
        self.assertIn("++s->generation;", body)
        self.assertIn("s->pending = false;", body)
        self.assertNotRegex(body, r"s->(?:active|started|next_index|active_generation)\s*=")

    def test_finish_accepts_cancelled_owner_preserves_pending_and_rejects_stale_owner(self):
        body = self.body("finish")
        self.assertIn("!s || s->active_generation != generation", body)
        self.assertNotRegex(body, r"s->generation\b|s->pending\s*=|s->next_index\s*=")
        self.assertIn("s->active = false;", body)
        self.assertIn("s->started = false;", body)


class LocalWelcomeReferenceModelTests(unittest.TestCase):
    """Python policy examples only; not C, RTOS, or board playback tests."""

    def test_zero_initialized_model_is_idle_at_item_one(self):
        model = LocalWelcomeReference()
        self.assertEqual(model.next_index, 0)
        self.assertIsNone(model.take())
        self.assertTrue(model.cancelled(0))
        self.assertFalse(model.start(0))

    def test_items_one_to_nine_wrap_in_order_only_after_start(self):
        model = LocalWelcomeReference()
        played = []
        for _ in range(2 * WELCOME_COUNT):
            model.request()
            generation, index = model.take()
            self.assertEqual(model.next_index, index)
            self.assertTrue(model.start(generation))
            played.append(index + 1)
            model.finish(generation)
        self.assertEqual(played, list(range(1, 10)) * 2)
        self.assertEqual(model.next_index, 0)

    def test_burst_retains_only_one_pending_playback_while_active(self):
        model = LocalWelcomeReference()
        for _ in range(20):
            model.request()
        first, _ = model.take()
        self.assertFalse(model.pending)
        for _ in range(20):
            model.request()
        self.assertIsNone(model.take())
        self.assertTrue(model.start(first))
        model.finish(first)
        second, index = model.take()
        self.assertEqual(index, 1)
        self.assertFalse(model.pending)
        self.assertTrue(model.start(second))
        model.finish(second)
        self.assertIsNone(model.take())

    def test_cancel_pending_does_not_skip_an_item(self):
        model = LocalWelcomeReference()
        model.request()
        model.cancel()
        self.assertFalse(model.pending)
        self.assertIsNone(model.take())
        self.assertEqual(model.next_index, 0)
        model.request()
        generation, index = model.take()
        self.assertEqual(index, 0)
        self.assertTrue(model.start(generation))

    def test_cancel_unstarted_active_keeps_new_tap_and_retries_same_item(self):
        model = LocalWelcomeReference()
        model.request()
        cancelled, _ = model.take()
        model.cancel()
        self.assertTrue(model.active)
        self.assertTrue(model.cancelled(cancelled))
        self.assertFalse(model.start(cancelled))
        model.request()
        self.assertIsNone(model.take())  # Old worker must complete its cleanup.
        model.finish(cancelled)
        self.assertTrue(model.pending)
        replacement, index = model.take()
        self.assertNotEqual(replacement, cancelled)
        self.assertEqual(index, 0)
        self.assertTrue(model.start(replacement))

    def test_cancel_started_active_keeps_new_tap_and_uses_next_item(self):
        model = LocalWelcomeReference()
        model.request()
        cancelled, _ = model.take()
        self.assertTrue(model.start(cancelled))
        model.cancel()
        self.assertTrue(model.started)
        model.request()
        model.finish(cancelled)
        self.assertTrue(model.pending)
        self.assertFalse(model.started)
        _, index = model.take()
        self.assertEqual(index, 1)

    def test_late_finish_cannot_release_newer_active_playback(self):
        model = LocalWelcomeReference()
        model.request()
        old, _ = model.take()
        model.start(old)
        model.finish(old)
        model.request()
        current, _ = model.take()
        model.start(current)
        model.finish(old)
        self.assertTrue(model.active)
        self.assertTrue(model.started)
        self.assertFalse(model.cancelled(current))
        self.assertIsNone(model.take())

    def test_start_advances_once_and_rejects_stale_or_cancelled_token(self):
        model = LocalWelcomeReference()
        model.request()
        generation, _ = model.take()
        self.assertFalse(model.start(generation + 1))
        self.assertEqual(model.next_index, 0)
        self.assertTrue(model.start(generation))
        self.assertEqual(model.next_index, 1)
        self.assertFalse(model.start(generation))
        self.assertEqual(model.next_index, 1)
        model.cancel()
        self.assertFalse(model.start(generation))
        self.assertEqual(model.next_index, 1)

    def test_cancel_before_first_write_without_output_does_not_advance(self):
        model = LocalWelcomeReference()
        model.request()
        generation, _ = model.take()
        model.cancel()
        output_started = False  # Blocking bridge returned without a PCM write.
        if output_started:
            model.output_started(generation)
        model.finish(generation)
        self.assertEqual(model.next_index, 0)
        model.request()
        self.assertEqual(model.take()[1], 0)

    def test_pcm_output_then_cancel_still_commits_report_exactly_once(self):
        model = LocalWelcomeReference()
        model.request()
        generation, _ = model.take()
        # PCM was written inside the bridge; cancellation arrived before return.
        model.cancel()
        model.request()
        self.assertTrue(model.cancelled(generation))
        self.assertFalse(model.start(generation))
        self.assertTrue(model.output_started(generation))
        self.assertEqual(model.next_index, 1)
        self.assertFalse(model.output_started(generation))
        self.assertEqual(model.next_index, 1)
        model.finish(generation)
        self.assertTrue(model.pending)
        self.assertEqual(model.take()[1], 1)

    def test_stale_output_report_cannot_commit_newer_active_item(self):
        model = LocalWelcomeReference()
        model.request()
        old, _ = model.take()
        model.cancel()
        model.finish(old)
        model.request()
        current, _ = model.take()
        self.assertFalse(model.output_started(old))
        self.assertFalse(model.started)
        self.assertEqual(model.next_index, 0)
        self.assertTrue(model.output_started(current))
        self.assertEqual(model.next_index, 1)

    def test_generation_wrap_still_uses_active_slot_and_cancel_fence(self):
        model = LocalWelcomeReference(generation=0xFFFFFFFF)
        model.request()
        generation, _ = model.take()
        self.assertEqual(generation, 0)
        self.assertFalse(model.cancelled(generation))
        model.cancel()
        self.assertTrue(model.cancelled(generation))
        model.request()
        model.finish(generation)
        self.assertTrue(model.pending)
        self.assertEqual(model.take()[0], 2)


if __name__ == "__main__":
    unittest.main()
