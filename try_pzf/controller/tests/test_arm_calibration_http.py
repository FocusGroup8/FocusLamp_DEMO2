"""Offline source integration guards for calibration HTTP admission.

These inspect production request validation and API wiring. They do not execute
the C parser, schedule FreeRTOS jobs, access UART or prove physical motion.
"""

from pathlib import Path
import re
import unittest

from test_arm_pose_http import code_only, function_body


CONTROLLER = Path(__file__).resolve().parents[1]


class ArmCalibrationHttpSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api = (CONTROLLER / "main/tts_http_api.c").read_text(encoding="utf-8")

    def test_three_routes_are_registered_with_correct_methods(self):
        init = function_body(self.api, "tts_http_api_init")
        for name, path, method, handler in (
            ("arm_calibration_status", "/api/arm/calibration", "GET", "arm_calibration_status_handler"),
            ("arm_calibration", "/api/arm/calibration", "POST", "arm_calibration_handler"),
            ("arm_jog", "/api/arm/jog", "POST", "arm_jog_handler"),
        ):
            declaration = re.search(r"\b" + name + r"_uri\s*=\s*\{(.*?)\};", self.api, re.S)
            self.assertIsNotNone(declaration)
            self.assertRegex(declaration.group(1), r'\.uri\s*=\s*"' + re.escape(path) + '"')
            self.assertRegex(declaration.group(1), r"\.method\s*=\s*HTTP_" + method + r"\b")
            self.assertRegex(declaration.group(1), r"\.handler\s*=\s*" + handler + r"\b")
            self.assertIn(f"ws_manager_server_register_uri(&{name}_uri)", init)
        server = (CONTROLLER / "components/websocket_manager/src/websocket_manager.c").read_text(encoding="utf-8")
        self.assertRegex(server, r"config.max_uri_handlers\s*=\s*20\b")

    def test_body_is_bounded_fully_received_and_raw_nul_is_rejected(self):
        body = function_body(self.api, "arm_calibration_read_body")
        for expression in (
            "req->content_len==0 || req->content_len>256",
            "while (done<req->content_len)",
            "httpd_req_recv(req,body+done,req->content_len-done)",
            "if (n<=0)", "done+=(size_t)n", "body[done]='\\0'",
            "memchr(body,'\\0',done)",
        ):
            self.assertIn(expression, body)
        self.assertIn('"413 Payload Too Large"', body)
        self.assertIn('"408 Request Timeout"', body)
        self.assertIn('httpd_resp_set_hdr(req,"Connection","close")', body)
        for name in ("arm_calibration_handler", "arm_jog_handler"):
            handler = function_body(self.api, name)
            self.assertIn("char body[257]", handler)
            self.assertLess(handler.index("arm_calibration_read_body"), handler.index("arm_calibration_parse"))

    def test_parser_requires_complete_flat_object_known_unique_fields(self):
        body = function_body(self.api, "arm_calibration_parse")
        self.assertIn("cJSON_ParseWithOpts(body,NULL,true)", body)
        self.assertIn("!cJSON_IsObject(root)", body)
        self.assertIn("cJSON_ArrayForEach(item,root)", body)
        self.assertIn("if (out->fields & field)", body)
        self.assertIn("else { valid=false; break; }", body)
        names = re.findall(r'strcmp\(item->string,"([^"]+)"\)', body)
        self.assertEqual(set(names), {
            "action", "confirmed", "session_id", "servo_id", "min", "max",
            "step", "time_ms", "speed", "direction",
        })
        # Raw NUL validation alone cannot prevent cJSON decoding a hidden suffix.
        self.assertIn('strstr(body,"\\\\u0000")', body)
        self.assertLess(body.index("strstr("), body.index("cJSON_ParseWithOpts("))
        self.assertIn("cJSON_Delete(root)", body)

    def test_parser_checks_types_finiteness_and_bounds_before_integer_narrowing(self):
        body = function_body(self.api, "arm_calibration_parse")
        self.assertIn("!cJSON_IsString(item)", body)
        self.assertIn("!cJSON_IsTrue(item)", body)
        self.assertIn('strcmp(item->valuestring,"begin")', body)
        self.assertIn('strcmp(item->valuestring,"end")', body)
        session = body.split("else if (field==CAL_SESSION)", 1)[1].split("} else {", 1)[0]
        for condition in ("!cJSON_IsNumber(item)", "!isfinite(number)", "number<1",
                          "number>UINT32_MAX", "number!=(double)(uint32_t)number"):
            self.assertIn(condition, session)
        self.assertLess(session.index("number>UINT32_MAX"), session.index("(uint32_t)number"))
        integer = body.split("} else {\n            double number=item->valuedouble;", 1)[1]
        for condition in ("!cJSON_IsNumber(item)", "!isfinite(number)",
                          "number<(field==CAL_DIRECTION ? -1 : 0)", "number>INT_MAX", "number!=(int)number"):
            self.assertIn(condition, integer)
        self.assertLess(integer.index("number>INT_MAX"), integer.index("(int)number"))

    def test_begin_requires_explicit_confirmation_interval_step_and_protocol_rate(self):
        body = function_body(self.api, "arm_calibration_handler")
        required = re.search(r"unsigned required=(.*?);", body, re.S).group(1)
        for field in ("CAL_ACTION", "CAL_SERVO", "CAL_MIN", "CAL_MAX", "CAL_STEP", "CAL_CONFIRMED"):
            self.assertIn(field, required)
        self.assertIn("command.servo_id==4 ? CAL_SPEED : CAL_TIME", required)
        self.assertIn("command.action!=1 || command.fields!=required", body)
        self.assertNotIn("DEFAULT", body)
        self.assertLess(body.index("command.fields!=required"), body.index("motion_calibration_begin_async"))
        self.assertLess(body.index("if (state.active)"), body.index("motion_calibration_begin_async"))
        for assignment in (".servo_id=command.servo_id", ".min_position=command.min",
                           ".max_position=command.max", ".step=command.step",
                           ".time_ms=command.time_ms", ".speed=command.speed"):
            self.assertIn(assignment, body)

    def test_end_and_jog_require_exact_token_fields_and_jog_direction(self):
        begin_end = function_body(self.api, "arm_calibration_handler")
        self.assertIn("command.action==2 && command.fields==(CAL_ACTION|CAL_SESSION)", begin_end)
        self.assertIn("motion_calibration_end(command.session_id)", begin_end)
        jog = function_body(self.api, "arm_jog_handler")
        self.assertIn("command.fields!=(CAL_SESSION|CAL_DIRECTION)", jog)
        self.assertIn("command.direction!=-1 && command.direction!=1", jog)
        self.assertIn("!state.active || state.session_id!=command.session_id", jog)
        self.assertIn("if (!state.ready)", jog)
        self.assertLess(jog.index("!state.ready"), jog.index("motion_calibration_jog_async"))
        self.assertIn("motion_calibration_jog_async(command.session_id,command.direction)", jog)
        self.assertNotRegex(code_only(jog), r"\b(?:raw|position|step)\s*[+\-]=")

    def test_http_uses_public_calibration_apis_without_bus_or_torque_access(self):
        expected = {
            "arm_calibration_status_handler": ["motion_calibration_get_snapshot"],
            "arm_calibration_handler": ["motion_calibration_get_snapshot", "motion_calibration_end",
                                        "motion_calibration_begin_async"],
            "arm_jog_handler": ["motion_calibration_get_snapshot", "motion_calibration_jog_async"],
        }
        for name, calls in expected.items():
            body = function_body(self.api, name)
            code = code_only(body)
            self.assertEqual(re.findall(r"\b(motion_\w+)\s*\(", code), calls)
            self.assertNotRegex(code, r"\b(?:servo_\w+|uart_\w+|read_axis|move_axis|motion_send_pose|nvs_\w+)\s*\(")
            self.assertNotRegex(code, r"\b(?:xSemaphoreTake|vTaskDelay\w*|xTaskCreate\w*)\s*\(")
            self.assertIn('"Cache-Control","no-store"', function_body(self.api, "arm_calibration_read_body")
                          if name != "arm_calibration_status_handler" else body)

    def test_async_response_reports_acceptance_without_claiming_completion(self):
        for name in ("arm_calibration_handler", "arm_jog_handler"):
            body = function_body(self.api, name)
            self.assertIn('"202 Accepted"', body)
            for fragment in ('\\"accepted\\":true', '\\"completed\\":false', '\\"session_id\\":'):
                self.assertIn(fragment, body)
            self.assertNotIn('\\"completed\\":true', body)
            self.assertLess(body.index("if (ret!=ESP_OK)"), body.index('"202 Accepted"'))

    def test_snapshot_separates_reference_target_observations_and_cached_feedback(self):
        body = function_body(self.api, "arm_calibration_status_handler")
        for flag, field in (("reference_valid", "reference_raw"), ("has_observations", "observed_min"),
                            ("has_observations", "observed_max"), ("has_target", "target_raw")):
            self.assertIn(f'state.{flag} ? cJSON_AddNumberToObject(root,"{field}",', body)
            self.assertIn(f'cJSON_AddNullToObject(root,"{field}")', body)
        for field in ("active", "ready", "motion_busy", "session_id", "action_status", "last_error",
                      "reason", "config", "feedback", "valid", "stale", "age_ms", "last_attempt_at_ms",
                      "last_ok_at_ms", "attempts", "successes", "failures"):
            self.assertIn(f'"{field}"', body)
        self.assertIn('p->has_value ? cJSON_AddNumberToObject(feedback,"raw",p->raw)', body)
        self.assertIn('cJSON_AddNullToObject(feedback,"raw")', body)
        self.assertIn('cJSON_AddNullToObject(root,"config")', body)
        self.assertIn('cJSON_AddNullToObject(root,"feedback")', body)
        self.assertNotIn("configured", body)
        self.assertIn("cJSON_Delete(root)", body)
        self.assertIn("cJSON_free(response)", body)

    def test_submission_errors_preserve_business_rejection_codes(self):
        body = function_body(self.api, "arm_calibration_submit_error")
        self.assertIn('"invalid_session"', body)
        self.assertIn('"invalid_config"', body)
        self.assertIn('"busy"', body)
        self.assertIn('"submit_failed"', body)
        self.assertIn('"409 Conflict"', body)
        self.assertIn('"400 Bad Request"', body)
        self.assertIn("ESP_ERR_INVALID_STATE || ret==ESP_ERR_TIMEOUT", body)
        self.assertNotIn("202 Accepted", body)


if __name__ == "__main__":
    unittest.main()
