"""Offline source guards and policy arithmetic, using only Python's stdlib.

Run: python -m unittest discover -s controller/tests -p test_ambient_breathing_policy.py -v

These tests DO NOT compile, import, or execute controller C, run firmware, read
ADC, or control a board. Source guards check selected integration statements;
the separately named ReferenceModelTests calculate the intended policy in
Python. Passing them is not evidence of C/runtime or physical-light behavior.
"""

import ast
import json
import math
from pathlib import Path
import re
import unittest


CONTROLLER = Path(__file__).resolve().parents[1]
PROFILES = ((4, 12), (5, 16), (6, 20), (8, 25), (10, 30))


def code_only(source):
    """Blank C comments/strings, preserving offsets for the small brace reader."""
    return re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), source, flags=re.S,
    )


def function_body(source, name):
    """Read a named definition for source assertions, without executing C."""
    masked = code_only(source)
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", masked)
    if match is None:
        raise AssertionError("Missing C definition: " + name)
    start = match.end()
    depth = 1
    for offset in range(start, len(masked)):
        depth += (masked[offset] == "{") - (masked[offset] == "}")
        if depth == 0:
            return source[start:offset]
    raise AssertionError("Unclosed C definition: " + name)


class AmbientReference:
    """Policy model, not a binding to C. Input is an 8-ADC-read batch mean.

    Assumptions: batch completion times are monotonic milliseconds; each call
    represents the sampler's scheduled batch, usually 1 second apart. None,
    negative, nonfinite, or error results are unavailable. The successful batch
    at a window boundary is included before that actual-time window closes.
    """

    def __init__(self):
        self.lux = None
        self.last_ok_ms = None
        self.block_start_ms = 0
        self.pending = []

    def snapshot(self, now_ms):
        age = -1 if self.last_ok_ms is None else now_ms - self.last_ok_ms
        return {"valid": self.lux is not None,
                "fresh": self.lux is not None and 0 <= age <= 10000,
                "lux": self.lux, "last_ok_ms": self.last_ok_ms, "age_ms": age}

    def batch(self, now_ms, lux, ok=True):
        fresh = self.snapshot(now_ms)["fresh"]
        good = ok and lux is not None and math.isfinite(lux) and lux >= 0
        if good and not fresh:
            self.lux, self.last_ok_ms = lux, now_ms
            self.pending = []
            self.block_start_ms = now_ms
            return
        if good:
            self.pending.append((now_ms, lux))
        if now_ms - self.block_start_ms >= 2000:
            if self.pending:
                mean = sum(value for _, value in self.pending) / len(self.pending)
                self.lux = 0.5 * mean + 0.5 * self.lux if fresh else mean
                self.last_ok_ms = self.pending[-1][0]
            self.pending = []
            self.block_start_ms = now_ms


def reference_envelope(previous, target, dt_ms, manual_peak=None):
    """Float arithmetic policy: shared ~400 ms EMA and immediate manual cap.

    Targets are raw brightness units, capped at 30, with valley <= peak.
    A manual peak 0..3 is allowed. Automatic target profiles have amplitude >=4;
    returning from a zero manual cap may temporarily have a smaller amplitude.
    Like the selected implementation, an elapsed gap advances at most 100 ms.
    """
    valley, peak = target
    peak = min(30, max(0, peak))
    if manual_peak is not None:
        peak = min(30, max(0, manual_peak))
    valley = min(peak, max(0, valley))
    alpha = 1 - math.exp(-min(100, max(0, dt_ms)) / 400)
    applied = [old + alpha * (new - old)
               for old, new in zip(previous, (valley, peak))]
    if manual_peak is not None:
        applied[1] = min(applied[1], peak)
        applied[0] = min(applied[0], applied[1])
    return tuple(applied)


class SourceGuardTests(unittest.TestCase):
    """Selected source contracts only; these assertions do not prove runtime behavior."""

    @classmethod
    def setUpClass(cls):
        def read(path):
            return (CONTROLLER / path).read_text(encoding="utf-8")
        cls.ambient = read("components/ambient_service/src/ambient_service.c")
        cls.header = read("components/ambient_service/include/ambient_service.h")
        cls.led = read("components/led_service/src/led_service.c")
        cls.api = read("main/tts_http_api.c")
        cls.startup = read("main/hello_world_main.c")
        cls.types = read("components/common/include/led_types.h")
        cls.config = read("components/common/include/system_config.h")

    def test_sampler_cadence_batch_size_and_filter_constants(self):
        for name, value in (("SAMPLE_INTERVAL", 1000), ("BLOCK_INTERVAL", 2000),
                            ("STALE_TIMEOUT", 10000)):
            self.assertRegex(self.header, rf"#define\s+AMBIENT_{name}_MS\s+{value}\b")
        self.assertRegex(self.ambient, r"#define\s+AMBIENT_BATCH_SAMPLES\s+8\b")
        self.assertRegex(self.ambient, r"#define\s+AMBIENT_EMA_ALPHA\s+0\.5f\b")
        task = code_only(function_body(self.ambient, "ambient_task"))
        self.assertEqual(task.count("light_sensor_driver_read_averaged("), 1)
        self.assertIn("light_sensor_driver_read_averaged(&lux, AMBIENT_BATCH_SAMPLES)", task)
        self.assertIn("vTaskDelayUntil(&last_wake, period)", task)
        self.assertIn("ret == ESP_OK && isfinite(lux) && lux >= 0.0f", task)
        self.assertIn("now_ms - block_start_ms >= AMBIENT_BLOCK_INTERVAL_MS", task)
        self.assertIn("block_sum / (float)block_count", task)
        self.assertIn("publish_sample(filtered, average, last_batch_ms)", task)

    def test_snapshot_is_cache_only_and_does_not_refresh_sample_timestamp(self):
        getter = code_only(function_body(self.ambient, "ambient_service_get_snapshot"))
        self.assertIn("*out = s_cache", getter)
        self.assertIn("esp_timer_get_time() / 1000", getter)
        self.assertIn("out->age_ms <= AMBIENT_STALE_TIMEOUT_MS", getter)
        self.assertIn("out->age_ms >= 0", getter)
        self.assertNotRegex(getter, r"s_cache(?:\.\w+)?\s*=")
        self.assertNotRegex(getter, r"out->last_ok_ms\s*=")
        self.assertNotRegex(getter, r"light_sensor_driver_|adc_|device_state_set_|xSemaphoreTake")
        publisher = code_only(function_body(self.ambient, "publish_sample"))
        self.assertIn("s_cache.last_ok_ms = sample_ms", publisher)

    def test_http_uses_cache_and_omits_level_from_503_body(self):
        handler = function_body(self.api, "ambient_get_handler")
        self.assertIn("ambient_service_get_snapshot(&sample)", handler)
        self.assertIn("!sample.fresh", handler)
        self.assertIn('"503 Service Unavailable"', handler)
        failure = re.search(r"return\s+httpd_resp_sendstr\(req,\s*(\"(?:\\.|[^\"\\])*\")\)", handler)
        self.assertIsNotNone(failure)
        payload = json.loads(ast.literal_eval(failure.group(1)))
        self.assertFalse(payload["ok"])
        self.assertNotIn("level", payload)
        self.assertNotRegex(code_only(handler), r"light_sensor_driver_|adc_")
        self.assertIn('(unsigned)sample.level, (double)sample.lux', handler)
        self.assertNotRegex(code_only(handler), r"sample\.level\s*(?:>|!=)\s*0")
        self.assertIn(r'\"sampled_at_ms\":%lld', handler)
        self.assertIn(r'\"age_ms\":%lld', handler)
        self.assertRegex(self.api, r'\.uri\s*=\s*"/api/ambient"\s*,\s*\.method\s*=\s*HTTP_GET')

    def test_ambient_starts_before_network_and_failure_is_nonfatal(self):
        init = code_only(function_body(self.startup, "init_local_light"))
        self.assertIn("ambient_service_start()", init)
        self.assertNotIn("ESP_ERROR_CHECK", init)
        main = code_only(function_body(self.startup, "app_main"))
        self.assertLess(main.index("init_local_light()"), main.index("wifi_manager_init()"))

    def test_led_profile_table_and_raw_cap_match_policy(self):
        table = re.search(r"s_ambient_breath_profiles\[\]\s*=\s*\{(.*?)\};", self.led, re.S)
        self.assertIsNotNone(table)
        pairs = re.findall(r"\{\s*(\d+)\.0f\s*,\s*(\d+)\.0f\s*\}", table.group(1))
        self.assertEqual(tuple((int(a), int(b)) for a, b in pairs), PROFILES)
        self.assertRegex(self.config, r"#define\s+LED_BRIGHTNESS_MAX\s+\(?30\)?")
        self.assertRegex(self.types, r"#define\s+LED_BREATH_MAX_BRIGHTNESS\s+30\b")

    def test_led_uses_fresh_cache_and_shared_envelope_smoothing(self):
        envelope = code_only(function_body(self.led, "led_service_update_breath_envelope"))
        self.assertIn("ambient_service_get_snapshot(&ambient)", envelope)
        self.assertIn("ambient.valid && ambient.fresh", envelope)
        self.assertIn("target = s_ambient_breath_profiles[ambient.level]", envelope)
        self.assertRegex(self.led, r"#define\s+BREATH_ENVELOPE_TIME_CONSTANT_US\s+400000\.0f")
        self.assertIn("expf(-(float)elapsed_us / BREATH_ENVELOPE_TIME_CONSTANT_US)", envelope)
        for endpoint in ("valley", "peak"):
            self.assertIn(f"s_breath_envelope.{endpoint} += alpha *", envelope)
        self.assertIn("s_breath_envelope.peak = fminf(s_breath_envelope.peak, target.peak)", envelope)
        self.assertIn("target.valley = fminf(target.valley, target.peak)", envelope)
        self.assertNotRegex(code_only(self.led), r"light_sensor_driver_|adc_oneshot_read")

    def test_manual_commands_and_endpoints_route_through_manual_setter(self):
        setter = code_only(function_body(self.led, "led_service_set_brightness"))
        self.assertRegex(setter, r"if\s*\(s_current_mode == LED_MODULE_MODE_BREATH\)\s*\{\s*s_breath_manual_override = true")
        self.assertIn("led_service_show_breath(s_elapsed)", setter)
        level = code_only(function_body(self.led, "led_service_set_brightness_level"))
        self.assertIn("led_service_set_brightness(0)", level)
        self.assertIn("led_service_set_brightness(s_current_brightness)", level)
        up = code_only(function_body(self.led, "led_service_brightness_up"))
        down = code_only(function_body(self.led, "led_service_brightness_down"))
        self.assertIn("led_service_set_brightness(s_current_brightness)", up)
        self.assertIn("led_service_set_brightness_level(0)", down)
        for body in (up, down):
            self.assertIn("LED_MODULE_MODE_BREATH", body)

    def test_breath_reentry_restores_auto_without_resetting_phase(self):
        effect = code_only(function_body(self.led, "led_service_set_effect"))
        breath_case = effect.split("case LED_EFFECT_BREATHING:", 1)[1].split("break;", 1)[0]
        self.assertIn("s_breath_manual_override = false", breath_case)
        self.assertNotRegex(breath_case, r"s_elapsed\s*=")
        turn_on = code_only(function_body(self.led, "led_service_turn_on"))
        self.assertNotIn("s_breath_manual_override", turn_on)
        self.assertNotIn("led_service_set_brightness(", turn_on)
        update = code_only(function_body(self.led, "led_service_update"))
        self.assertIn("s_elapsed += dt_ms", update)
        self.assertIn("case LED_MODULE_MODE_BREATH:", update)
        self.assertNotIn("case LED_MODULE_MODE_OFF:", update)
        self.assertNotIn("ambient_service", update)
        renderer = code_only(function_body(self.led, "led_service_show_breath"))
        self.assertIn("elapsed_ms % period_ms", renderer)
        self.assertRegex(renderer, r"if\s*\(s_current_mode == LED_MODULE_MODE_BREATH\)\s*\{\s*envelope = led_service_update_breath_envelope\(\)")
        self.assertRegex(self.types, r"LED_BREATH_DEFAULT_PERIOD_MS\s+3000\b")


class ReferenceModelTests(unittest.TestCase):
    """Numerical acceptance examples for Python models, not executed C tests."""

    def test_first_good_batch_and_zero_lux_are_valid_immediately(self):
        model = AmbientReference()
        self.assertFalse(model.snapshot(0)["valid"])
        model.batch(0, 0.0)
        self.assertEqual(model.snapshot(0)["lux"], 0.0)
        self.assertTrue(model.snapshot(0)["fresh"])

    def test_two_successful_batches_are_averaged_then_ema_filtered(self):
        model = AmbientReference()
        model.batch(0, 100.0)
        model.batch(1000, 200.0)
        self.assertEqual(model.lux, 100.0)
        model.batch(2000, 400.0)
        self.assertEqual(model.lux, 200.0)  # .5 * mean(200,400) + .5 * 100
        self.assertEqual(model.last_ok_ms, 2000)

    def test_failures_do_not_become_zero_or_refresh_an_empty_window(self):
        model = AmbientReference()
        model.batch(0, 100.0)
        model.batch(1000, 300.0)
        model.batch(2000, None)
        self.assertEqual(model.lux, 200.0)
        self.assertEqual(model.last_ok_ms, 1000)  # Incorporated batch time, not 2000.
        for now, value, ok in ((3000, float("nan"), True), (4000, -1, True),
                               (5000, float("inf"), True), (6000, 0, False)):
            model.batch(now, value, ok)
        self.assertEqual((model.lux, model.last_ok_ms), (200.0, 1000))

    def test_actual_time_window_closes_despite_missed_sampling_periods(self):
        model = AmbientReference()
        model.batch(0, 100.0)
        model.batch(1100, 200.0)
        model.batch(3500, None)
        self.assertEqual((model.lux, model.last_ok_ms), (150.0, 1100))

    def test_reads_never_extend_freshness_and_boundary_is_inclusive(self):
        model = AmbientReference()
        model.batch(123, 50.0)
        for now in (123, 1123, 9000, 10123):
            self.assertTrue(model.snapshot(now)["fresh"])
            self.assertEqual(model.last_ok_ms, 123)
        self.assertFalse(model.snapshot(10124)["fresh"])
        self.assertEqual(model.snapshot(10124)["age_ms"], 10001)

    def test_stale_recovery_starts_from_new_environment(self):
        model = AmbientReference()
        model.batch(0, 1000.0)
        model.batch(11000, 10.0)
        self.assertEqual((model.lux, model.last_ok_ms), (10.0, 11000))
        self.assertEqual(model.pending, [])

    def test_auto_targets_have_amplitude_and_shared_smoothing_preserves_bounds(self):
        for previous in PROFILES:
            for target in PROFILES:
                self.assertGreaterEqual(target[1] - target[0], 4)
                valley, peak = reference_envelope(previous, target, 20)
                self.assertTrue(0 <= valley <= peak <= 30)
                self.assertGreaterEqual(peak - valley, 4)
                self.assertLessEqual(abs(peak - previous[1]), 1.0)

    def test_manual_zero_to_three_caps_immediately_even_with_zero_elapsed(self):
        for manual in range(4):
            for dt in (0, 20, 5000):
                valley, peak = reference_envelope((10, 30), (10, 30), dt, manual)
                self.assertTrue(0 <= valley <= peak <= manual)

    def test_inverted_manual_interval_is_clamped_and_does_not_force_amplitude(self):
        self.assertEqual(reference_envelope((2, 2), (10, 30), 20, 2), (2.0, 2.0))
        valley, peak = reference_envelope((0, 0), PROFILES[0], 20)
        self.assertTrue(0 < peak - valley < 4)  # Smooth auto recovery is intentional.


if __name__ == "__main__":
    unittest.main()
