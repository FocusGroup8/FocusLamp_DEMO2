"""Source guards + independent Python reference policy for queued TTS.

Run: python -m unittest discover -s controller/tests -p test_welcome_tts_lifecycle.py -v

These checks do NOT execute controller C, ESP-IDF, FreeRTOS, SDK callbacks,
network I/O, or board audio. Source guards inspect selected integration points.
The separate policy model calculates intended transitions in Python; it does
not prove runtime scheduling or transport/event-fence behavior.
"""

from collections import deque
from pathlib import Path
import re
import unittest


CONTROLLER = Path(__file__).resolve().parents[1]
MANAGER = CONTROLLER / "components/xiaozhi_manager/src/xiaozhi_manager.c"
FIRST_RESPONSE_US = 30_000_000


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
            return source[match.end():offset]
    raise AssertionError("Unclosed C definition: " + name)


class WelcomeLifecycleSourceGuards(unittest.TestCase):
    """Selected source statements only, not execution of the C implementation."""

    @classmethod
    def setUpClass(cls):
        cls.source = MANAGER.read_text(encoding="utf-8")

    def test_waiting_is_registered_before_sdk_send(self):
        body = function_body(self.source, "speak_process_request")
        self.assertLess(body.index("speak_cycle_wait("), body.index("speak_send("))
        self.assertIn("esp_timer_get_time()", body)

    def test_diagnostic_first_response_window_is_thirty_seconds(self):
        self.assertRegex(self.source, r"#define\s+SPEAK_FIRST_TTS_WAIT_US\s+"
                         r"\(30LL\s*\*\s*1000000\)")
        self.assertEqual(self.source.count(
            "esp_timer_get_time(), SPEAK_FIRST_TTS_WAIT_US)"), 2)

    def test_public_queue_worker_waits_for_complete_cycle(self):
        api = function_body(self.source, "xiaozhi_manager_speak")
        worker = function_body(self.source, "speak_task_func")
        process = function_body(self.source, "speak_process_request")
        self.assertIn("xQueueSend(s_speak_queue", api)
        self.assertIn("xQueueReceive(s_speak_queue", worker)
        self.assertIn("speak_process_request(&req)", worker)
        self.assertLess(process.index("bool closing ="), process.index("speak_cycle_release("))
        self.assertLess(process.index("if (closing) break;"), process.index("speak_cycle_release("))
        self.assertLess(process.rindex("speak_transport_reset("), process.index("speak_cycle_release("))

    def test_ordinary_dialogue_and_both_upload_gates_exclude_injection(self):
        for name in ("dialogue_pin", "uplink_enqueue", "uplink_task_func", "uplink_resume"):
            with self.subTest(function=name):
                self.assertIn("!speak_cycle_busy(&s_speak_cycle)",
                              function_body(self.source, name))
        process = function_body(self.source, "speak_process_request")
        self.assertIn("!s_normal_dialogue_active", process)
        self.assertIn("!s_normal_tts_active", process)

    def test_injection_stop_is_local_cleanup_only(self):
        callback = function_body(self.source, "xiaozhi_event_process")
        stop = callback.split("case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:", 1)[1].split(
            "case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:", 1)[0]
        self.assertNotIn("chat_close_channel(", code_only(stop))
        self.assertIn("if (finished) s_mic_paused_for_tts = false;", stop)
        normal_branch = stop.split("} else {", 1)
        self.assertEqual(len(normal_branch), 2)
        self.assertNotIn("mic_resume_after_tts(", code_only(normal_branch[0]))
        self.assertIn("mic_resume_after_tts()", normal_branch[1])
        self.assertIn("s_normal_tts_active", stop)
        self.assertIn("if (!finished && !normal)", stop)

    def test_retired_transport_is_destroyed_and_events_drained_before_new_init(self):
        body = function_body(self.source, "chat_rebuild_runtime")
        operations = [body.index(name) for name in (
            "esp_xiaozhi_chat_stop(", "esp_xiaozhi_chat_deinit(",
            "chat_drain_events(", "esp_xiaozhi_chat_init(")]
        self.assertEqual(operations, sorted(operations))
        self.assertIn("if (drained != ESP_OK)", body)
        self.assertIn("s_mic_paused_for_tts = false;", body)

    def test_closing_audio_is_rejected_and_barrier_is_registered(self):
        audio = function_body(self.source, "xiaozhi_audio_callback")
        self.assertIn("s_speak_cycle.phase == SPEAK_PLAYING", audio)
        self.assertIn("s_speak_cycle.phase == SPEAK_IDLE && s_normal_tts_active", audio)
        self.assertIn("if (!current) return;", audio)
        self.assertRegex(self.source, r"esp_event_handler_register\(\s*"
                         r"XIAOZHI_MANAGER_PRIVATE_EVENTS,\s*0,\s*chat_barrier_handler")
        self.assertIn("esp_event_post(XIAOZHI_MANAGER_PRIVATE_EVENTS", self.source)


class WelcomePolicyModel:
    """Independent intended-policy model, not a translation/binding of C.

    Old SDK events have no origin token here either. The pending event list
    models events drained while admission is closed, after stopping/deinitializing
    their producer. That fence is an assumption tested only structurally above.
    """

    def __init__(self):
        self.phase = "idle"
        self.outcome = None
        self.queue = deque()
        self.current = None
        self.wait_us = 0
        self.normal_dialogue = False
        self.normal_tts = False
        self.mic_paused = False
        self.events = deque()
        self.trace = []

    def enqueue(self, text):
        self.queue.append(text)

    def worker_step(self, now_us):
        if self.phase != "idle" or self.normal_dialogue or not self.queue:
            return False
        self.current = self.queue.popleft()
        self.phase = "waiting"
        self.wait_us = now_us
        self.outcome = None
        self.trace.append(("begin", self.current))
        return True

    def begin_normal(self):
        if self.phase != "idle":
            return False
        self.normal_dialogue = True
        return True

    def end_normal(self):
        self.normal_dialogue = False
        self.normal_tts = False
        self.mic_paused = False

    def start(self):
        if self.phase in ("waiting", "playing"):
            self.phase = "playing"
            self.mic_paused = True
            return True
        if self.phase == "idle" and self.normal_dialogue:
            self.normal_tts = True
            self.mic_paused = True
            return True
        return False

    def stop(self):
        if self.phase in ("waiting", "playing"):
            self.outcome = "done" if self.phase == "playing" else "error"
            self.phase = "closing"
            self.mic_paused = False
            return True
        if self.phase == "idle" and self.normal_dialogue and self.normal_tts:
            self.normal_tts = False
            self.mic_paused = False
            return True
        return False

    def tick(self, now_us, first_response_us=FIRST_RESPONSE_US):
        if self.phase == "waiting" and now_us - self.wait_us >= first_response_us:
            self.phase = "closing"
            self.outcome = "timeout"
            return True
        return False

    def cleanup(self):
        if self.phase != "closing":
            return False
        while self.events:
            event = self.events.popleft()
            if event == "start":
                self.start()
            elif event == "stop":
                self.stop()
        self.trace.append(("release", self.current))
        self.current = None
        self.mic_paused = False
        self.phase = "idle"
        return True


class WelcomeReferenceModelTests(unittest.TestCase):
    """Python policy examples only; not C, SDK, or RTOS runtime tests."""

    def test_welcome_completion_then_ordinary_dialogue(self):
        model = WelcomePolicyModel()
        model.enqueue("welcome")
        self.assertTrue(model.worker_step(0))
        self.assertFalse(model.begin_normal())
        self.assertTrue(model.start())
        self.assertTrue(model.stop())
        self.assertEqual(model.outcome, "done")
        self.assertFalse(model.begin_normal())
        self.assertTrue(model.cleanup())
        self.assertFalse(model.mic_paused)
        self.assertTrue(model.begin_normal())
        self.assertTrue(model.start())
        self.assertTrue(model.stop())
        self.assertTrue(model.normal_dialogue)
        self.assertFalse(model.normal_tts)
        self.assertFalse(model.mic_paused)

    def test_first_response_timeout_drains_late_stop_without_resuming_dialogue(self):
        model = WelcomePolicyModel()
        model.enqueue("welcome")
        model.worker_step(100)
        self.assertTrue(model.tick(FIRST_RESPONSE_US + 100))
        model.events.extend(("start", "stop"))
        self.assertFalse(model.stop())
        self.assertTrue(model.cleanup())
        self.assertEqual(model.outcome, "timeout")
        self.assertFalse(model.normal_dialogue)
        self.assertFalse(model.stop())
        self.assertTrue(model.begin_normal())
        self.assertFalse(model.stop())  # no normal START, so no normal resume
        self.assertTrue(model.start())
        self.assertTrue(model.stop())

    def test_first_response_timeout_never_truncates_playing(self):
        model = WelcomePolicyModel()
        model.enqueue("long welcome")
        model.worker_step(0)
        model.start()
        self.assertFalse(model.tick(6000000))
        self.assertFalse(model.tick(60000000))
        self.assertEqual(model.phase, "playing")
        self.assertTrue(model.mic_paused)
        self.assertTrue(model.stop())
        self.assertEqual(model.outcome, "done")

    def test_diagnostic_window_accepts_start_after_old_five_second_limit(self):
        model = WelcomePolicyModel()
        model.enqueue("delayed welcome")
        model.worker_step(100)
        self.assertFalse(model.tick(5_000_100))
        self.assertFalse(model.tick(FIRST_RESPONSE_US - 1 + 100))
        self.assertEqual(model.phase, "waiting")
        self.assertTrue(model.start())
        self.assertFalse(model.tick(60_000_100))
        self.assertTrue(model.stop())
        self.assertEqual(model.outcome, "done")

    def test_welcome_defers_during_ordinary_dialogue(self):
        model = WelcomePolicyModel()
        model.begin_normal()
        model.enqueue("welcome")
        self.assertFalse(model.worker_step(0))
        model.start()
        self.assertFalse(model.worker_step(100))
        model.stop()
        self.assertFalse(model.worker_step(200))  # dialogue still listening
        self.assertEqual(list(model.queue), ["welcome"])
        model.end_normal()
        self.assertTrue(model.worker_step(300))

    def test_two_injections_are_serialized_through_cleanup(self):
        model = WelcomePolicyModel()
        model.enqueue("first")
        model.enqueue("second")
        self.assertTrue(model.worker_step(0))
        self.assertFalse(model.worker_step(1))
        model.start()
        model.stop()
        self.assertFalse(model.worker_step(2))
        self.assertEqual(list(model.queue), ["second"])
        model.cleanup()
        self.assertTrue(model.worker_step(3))
        model.start()
        model.stop()
        model.cleanup()
        self.assertEqual(model.trace, [
            ("begin", "first"), ("release", "first"),
            ("begin", "second"), ("release", "second"),
        ])


if __name__ == "__main__":
    unittest.main()
