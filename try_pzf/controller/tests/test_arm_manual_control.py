"""Offline source integration guards for the manual servo path.

These check production wiring and safety-gate ordering. They do not execute C,
simulate FreeRTOS, validate UART replies or prove physical motion. Firmware is
built separately; no board, HTTP command, flash or actuation is used here.
"""

from pathlib import Path
import re
import unittest

from test_arm_pose_http import code_only, function_body


CONTROLLER = Path(__file__).resolve().parents[1]


class ManualServoSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.motion = (CONTROLLER / "components/motion_controller/src/motion_controller.c").read_text(encoding="utf-8")
        cls.api = (CONTROLLER / "main/tts_http_api.c").read_text(encoding="utf-8")
        cls.config = (CONTROLLER / "components/motion_controller/include/motion_manual_limits.h").read_text(encoding="utf-8")

    def test_confirmed_intervals_are_enabled_and_inside_protocol_ranges(self):
        for servo_id in range(1, 6):
            self.assertRegex(self.config, rf"#define MOTION_MANUAL_SERVO_{servo_id}_ENABLED\s+1\b")
            bounds = [int(re.search(rf"#define MOTION_MANUAL_SERVO_{servo_id}_{bound}_RAW\s+(\d+)\b", self.config).group(1))
                      for bound in ("MIN", "MAX")]
            self.assertGreaterEqual(bounds[0], 0)
            self.assertLess(bounds[0], bounds[1])
            self.assertLessEqual(bounds[1], 3000 if servo_id == 4 else 1000)
        self.assertIn("User-confirmed usable boundaries", self.config)
        self.assertIn("BOTTOM to TOP", self.config)

    def test_compile_time_limits_use_bottom_to_top_mapping_and_checked_narrowing(self):
        body = function_body(self.motion, "motion_manual_servo_get_limits")
        for servo_id, axis in ((1, 1), (2, 2), (3, 3), (4, 0), (5, 4)):
            self.assertIn(f"{{{axis},MOTION_MANUAL_SERVO_{servo_id}_ENABLED,", body)
        for condition in ("min>=protocol_min", "max<=protocol_max", "min>=0",
                          "max<=INT16_MAX", "min<=max", "min==(int)min", "max==(int)max"):
            self.assertIn(condition, body)
        self.assertLess(body.index("min>=0"), body.index("min==(int)min"))
        self.assertLess(body.index("max<=INT16_MAX"), body.index("max==(int)max"))
        self.assertNotRegex(code_only(body), r"\b(?:read_axis|servo_\w+|xSemaphoreTake)\s*\(")

    def test_manual_admission_checks_config_and_parameters_before_submit(self):
        body = function_body(self.motion, "motion_manual_servo_async")
        self.assertLess(body.index("!limits.configured"), body.index("calloc("))
        self.assertIn("pos<limits.min_position || pos>limits.max_position", body)
        self.assertIn("j->kind=JOB_MANUAL", body)
        self.assertIn("xSemaphoreTake(admission_lock,0)", body)
        self.assertLess(body.index("xSemaphoreTake(admission_lock,0)"), body.index("submit(j)"))
        self.assertLess(body.index("submit(j)"), body.index("xSemaphoreGive(admission_lock)"))
        self.assertNotRegex(code_only(body), r"\b(?:stop_owned|read_axis|move_axis|motion_send_pose)\s*\(")
        self.assertIn("if (time_ms!=0) return ESP_ERR_INVALID_ARG", body)
        self.assertIn("if (speed!=0) return ESP_ERR_INVALID_ARG", body)

    def test_busy_submit_queue_is_restricted_to_old_single_jobs(self):
        body = function_body(self.motion, "submit")
        busy = body.split("if (busy)", 1)[1]
        self.assertLess(busy.index("j->kind!=JOB_SINGLE"), busy.index("pending[depth-1]=j"))
        self.assertNotIn("JOB_MANUAL", busy)
        self.assertIn("free(j); return ESP_ERR_INVALID_STATE", busy)

    def test_worker_checks_config_and_fresh_current_interval_before_move(self):
        body = function_body(self.motion, "run_job")
        # The inner out-of-range branch contains an else-if, so inspect through
        # the legacy single-axis branch boundary for the whole preflight.
        manual = body.split("if (j->kind==JOB_MANUAL)", 1)[1].split("} else if (j->kind==JOB_SINGLE", 1)[0]
        self.assertLess(manual.index("manual_target_limits"), manual.index("read_axis(j->axis)"))
        self.assertIn("worker_error==ESP_OK", manual)
        self.assertIn("actual==INT16_MIN", manual)
        self.assertIn("fail(ESP_ERR_TIMEOUT)", manual)
        self.assertIn("actual<limits.min_position || actual>limits.max_position", manual)
        self.assertIn("fail(ESP_ERR_INVALID_STATE)", manual)
        self.assertNotIn("position_cache", code_only(manual))
        self.assertLess(body.index("actual<limits.min_position"), body.index("switch(j->kind)"))
        self.assertIn("if (!cancelled() && worker_error==ESP_OK)", body)

    def test_manual_worker_sends_only_its_axis_and_reuses_verification(self):
        body = function_body(self.motion, "run_job")
        case = body.split("case JOB_MANUAL:", 1)[1].split("case JOB_SINGLE:", 1)[0]
        self.assertEqual(re.findall(r"\bmove_axis\s*\(", code_only(case)), ["move_axis("])
        self.assertIn("move_axis(j->axis,j->position,j->time_ms,j->speed)", case)
        self.assertIn("if (j->axis!=0) delay_with_stop_check(j->time_ms+300)", case)
        self.assertNotIn("motion_send_pose", code_only(case))
        self.assertNotRegex(code_only(case), r"\bservo_\w+\s*\(")
        self.assertIn("verify_targets()", body)
        move = function_body(self.motion, "move_axis")
        self.assertIn("cancelled() || worker_error!=ESP_OK", move)
        verify = function_body(self.motion, "verify_targets")
        self.assertIn("if (!target_set[i]) continue", verify)

    def test_limits_http_returns_configured_intervals_or_null_without_bus_access(self):
        body = function_body(self.api, "arm_limits_handler")
        self.assertIn('"manual-servo-limits"', body)
        for field in ("time_ms_min", "time_ms_max", "time_ms_default", "speed_min", "speed_max", "speed_default"):
            self.assertIn(f'"{field}"', body)
        self.assertIn("servo_id<=MOTION_MANUAL_SERVO_COUNT", body)
        self.assertIn('limits.axis==0 ? "em3" : "lx"', body)
        self.assertIn('cJSON_AddNullToObject(item,"min")', body)
        self.assertIn('cJSON_AddNullToObject(item,"max")', body)
        self.assertNotRegex(code_only(body), r"\b(?:servo_\w+|read_axis|motion_manual_servo_async)\s*\(")

    def test_parser_requires_flat_object_finite_integer_fields_and_rejects_duplicates(self):
        body = function_body(self.api, "arm_manual_parse")
        self.assertIn("cJSON_ParseWithOpts(body,NULL,true)", body)
        self.assertIn("!cJSON_IsObject(root)", body)
        self.assertIn("cJSON_ArrayForEach(item,root)", body)
        self.assertIn("out->fields & field", body)
        self.assertIn("!cJSON_IsNumber(item)", body)
        self.assertIn("!isfinite(item->valuedouble)", body)
        self.assertIn("item->valuedouble>INT_MAX", body)
        self.assertIn("item->valuedouble!=(int)item->valuedouble", body)
        self.assertLess(body.index("item->valuedouble>INT_MAX"), body.index("item->valuedouble!=(int)item->valuedouble"))
        self.assertIn("else { valid=false; break; }", body)
        self.assertIn("cJSON_Delete(root)", body)
        self.assertIn("ARM_MANUAL_SERVO_FIELD|ARM_MANUAL_POSITION_FIELD", body)

    def test_body_is_bounded_and_embedded_nul_is_rejected(self):
        body = function_body(self.api, "arm_servo_handler")
        self.assertIn("char body[129]", body)
        self.assertIn("req->content_len>sizeof(body)-1", body)
        self.assertIn("req->content_len==0", body)
        self.assertIn("n<=0", body)
        self.assertIn("memchr(body,'\\0',received)", body)
        self.assertNotIn("cJSON_Parse(body)", body)

    def test_handler_does_not_mix_em3_speed_with_lx_duration(self):
        body = function_body(self.api, "arm_servo_handler")
        self.assertIn("command.servo_id==4", body)
        self.assertIn("command.fields & ARM_MANUAL_TIME_FIELD", body)
        self.assertIn("command.fields & ARM_MANUAL_SPEED_FIELD", body)
        self.assertIn("command.speed=MOTION_MANUAL_SPEED_DEFAULT", body)
        self.assertIn("command.time_ms=MOTION_MANUAL_TIME_MS_DEFAULT", body)
        for name, value in (("TIME_MS_MIN", 500), ("TIME_MS_MAX", 10000), ("TIME_MS_DEFAULT", 1500),
                            ("SPEED_MIN", 1), ("SPEED_MAX", 500), ("SPEED_DEFAULT", 100)):
            self.assertRegex(self.config, rf"#define MOTION_MANUAL_{name}\s+{value}\b")

    def test_http_only_enqueues_new_manual_api_and_explains_acceptance(self):
        body = function_body(self.api, "arm_servo_handler")
        self.assertEqual(re.findall(r"\b(motion_\w+)\s*\(", code_only(body)), [
            "motion_controller_get_status", "motion_manual_servo_get_limits", "motion_manual_servo_async",
        ])
        self.assertNotRegex(code_only(body), r"\b(?:servo_\w+|motion_send_pose|move_axis)\s*\(")
        self.assertIn('"202 Accepted"', body)
        for fragment in ('\\"accepted\\":true', '\\"completed\\":false', '\\"status\\":\\"accepted\\"'):
            self.assertIn(fragment, body)
        for code in ("unready", "busy", "limits_unconfigured", "position_out_of_range"):
            self.assertIn(f'"{code}"', body)
        error = function_body(self.api, "arm_manual_error")
        self.assertIn('\\"error\\":{\\"code\\":', error)
        self.assertIn('\\"message\\":', error)

    def test_both_new_routes_and_optional_capture_fit_existing_server(self):
        init = function_body(self.api, "tts_http_api_init")
        for route, method in (("arm_limits", "GET"), ("arm_servo", "POST")):
            self.assertIn(f"ws_manager_server_register_uri(&{route}_uri)", init)
            declaration = re.search(route + r"_uri\s*=\s*\{(.*?)\};", self.api, re.S).group(1)
            self.assertIn(f"HTTP_{method}", declaration)
        self.assertIn("tts_capture_http_init()", init)
        paths = ["main/tts_http_api.c", "main/tts_capture.c", "main/device_info_api.c",
                 "main/mode_http_api.c", "components/status_receiver/src/status_receiver.c"]
        routes = 2  # Existing / and /ws registrations use HTTPD directly.
        for path in paths:
            routes += len(re.findall(r"ws_manager_server_register_uri\(&\w+_uri\)",
                                     (CONTROLLER / path).read_text(encoding="utf-8")))
        server = (CONTROLLER / "components/websocket_manager/src/websocket_manager.c").read_text(encoding="utf-8")
        capacity = int(re.search(r"config.max_uri_handlers\s*=\s*(\d+)", server).group(1))
        self.assertEqual(routes, 20)  # Three calibration/jog handlers share this server.
        self.assertLessEqual(routes, capacity)


if __name__ == "__main__":
    unittest.main()
