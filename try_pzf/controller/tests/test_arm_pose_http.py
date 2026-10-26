"""Offline source integration guards for GET /api/arm/pose.

These checks inspect the production HTTP handler and its registration. They do
not execute C, run firmware, query a servo, or prove board/RTOS behavior.
Run: python -m unittest discover -s controller/tests -p test_arm_pose_http.py -v
"""

from pathlib import Path
import re
import unittest


CONTROLLER = Path(__file__).resolve().parents[1]


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
    start, depth = match.end(), 1
    for offset in range(start, len(masked)):
        depth += (masked[offset] == "{") - (masked[offset] == "}")
        if depth == 0:
            return source[start:offset]
    raise AssertionError("Unclosed C definition: " + name)


class ArmPoseHttpSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api = (CONTROLLER / "main/tts_http_api.c").read_text(encoding="utf-8")
        cls.handler = function_body(cls.api, "arm_pose_handler")

    def test_route_is_get_and_registered_with_existing_routes(self):
        route = re.search(r"arm_pose_uri\s*=\s*\{(.*?)\};", self.api, re.S)
        self.assertIsNotNone(route)
        self.assertIn('.uri = "/api/arm/pose"', route.group(1))
        self.assertRegex(route.group(1), r"\.method\s*=\s*HTTP_GET\b")
        self.assertRegex(route.group(1), r"\.handler\s*=\s*arm_pose_handler\b")
        init = function_body(self.api, "tts_http_api_init")
        for name in ("tts_speak", "chat_end", "detect_phone", "ambient_get",
                     "arm_gesture", "arm_stop", "arm_pose"):
            self.assertIn(f"ws_manager_server_register_uri(&{name}_uri)", init)

    def test_http_reads_one_snapshot_and_never_queries_or_controls_the_bus(self):
        code = code_only(self.handler)
        motion_calls = re.findall(r"\b(motion_\w+)\s*\(", code)
        self.assertEqual(motion_calls, ["motion_controller_get_position_snapshot"])
        self.assertNotRegex(code, r"\b(?:servo_\w*|em3_\w*|lx_\w*|uart_\w*)\s*\(")
        self.assertNotRegex(code, r"\b(?:xTaskCreate\w*|vTaskDelay\w*|xSemaphoreTake)\s*\(")
        self.assertNotIn("arm_read_body", code)
        self.assertIn('httpd_resp_set_hdr(req, "Cache-Control", "no-store")', self.handler)
        self.assertIn('httpd_resp_set_type(req, "application/json")', self.handler)

    def test_unstarted_sampler_is_unavailable_but_missing_axis_values_are_not(self):
        before_json = self.handler.split("cJSON *root", 1)[0]
        self.assertRegex(before_json, r"motion_controller_get_position_snapshot\(&snapshot\)\s*!=\s*ESP_OK")
        self.assertIn("!snapshot.sampling_started", before_json)
        self.assertIn('"503 Service Unavailable"', before_json)
        self.assertNotRegex(before_json, r"axes|has_value|\.valid|\.stale")
        after_json = self.handler.split("cJSON *root", 1)[1]
        self.assertNotIn('"503 Service Unavailable"', after_json)
        self.assertIn('"200 OK"', after_json)

    def test_response_identifies_idle_cache_and_keeps_all_five_axis_slots(self):
        root_fields = set(re.findall(r'cJSON_Add\w+ToObject\(root,\s*"([^"\\]+)"', self.handler))
        self.assertEqual(root_fields, {
            "ok", "read_only", "source", "sampling_mode", "sampling_started",
            "motion_busy", "captured_at_ms", "stale_after_ms", "axes",
        })
        for expression in (
            'cJSON_AddBoolToObject(root, "ok", true)',
            'cJSON_AddBoolToObject(root, "read_only", true)',
            'cJSON_AddStringToObject(root, "source", "servo-feedback-cache")',
            'cJSON_AddStringToObject(root, "sampling_mode", "idle_only")',
        ):
            self.assertIn(expression, self.handler)
        self.assertRegex(self.handler, r"for\s*\([^;]+;\s*i\s*<\s*MOTION_POSITION_AXIS_COUNT\s*;")
        self.assertIn("&snapshot.axes[i]", self.handler)
        self.assertIn("cJSON_AddItemToArray(axes, item)", self.handler)
        self.assertIn('axis->axis == 0 ? "em3" : "lx"', self.handler)
        axis_fields = set(re.findall(r'cJSON_Add\w+ToObject\(item,\s*"([^"\\]+)"', self.handler))
        self.assertEqual(axis_fields, {
            "axis", "bus_id", "protocol", "raw", "valid", "stale", "age_ms",
            "last_attempt_at_ms", "last_ok_at_ms", "attempts", "successes", "failures",
        })

    def test_unknown_raw_and_timestamps_are_null_and_raw_zero_is_not_rejected(self):
        for field, condition, member in (
            ("raw", r"axis->has_value", "raw"),
            ("age_ms", r"axis->has_value\s*&&\s*axis->age_ms\s*>=\s*0", "age_ms"),
            ("last_attempt_at_ms", r"axis->attempted", "last_attempt_at_ms"),
            ("last_ok_at_ms", r"axis->has_value", "last_ok_at_ms"),
        ):
            self.assertRegex(
                self.handler,
                condition + r'\s*\?\s*cJSON_AddNumberToObject\(item,\s*"' + field +
                r'",\s*axis->' + member + r'\)\s*:\s*cJSON_AddNullToObject\(item,\s*"' + field + r'"\)',
            )
        self.assertIn('cJSON_AddBoolToObject(item, "valid", axis->valid)', self.handler)
        self.assertIn('cJSON_AddBoolToObject(item, "stale", axis->stale)', self.handler)
        self.assertNotRegex(code_only(self.handler), r"axis->raw\s*(?:==|!=|>|<)")

    def test_json_allocation_failure_releases_tree_and_success_releases_output(self):
        self.assertIn("cJSON_PrintUnformatted(root)", self.handler)
        self.assertIn("if (!response) goto no_memory", self.handler)
        self.assertRegex(self.handler, r"if\s*\(!cJSON_AddItemToArray\(axes, item\)\)\s*\{\s*cJSON_Delete\(item\);")
        failure = self.handler.split("no_memory:", 1)[1]
        self.assertIn("cJSON_Delete(root)", failure)
        self.assertIn('"500 Internal Server Error"', failure)
        success = self.handler.split("no_memory:", 1)[0]
        self.assertIn("cJSON_Delete(root)", success)
        self.assertIn("cJSON_free(response)", success)


if __name__ == "__main__":
    unittest.main()
