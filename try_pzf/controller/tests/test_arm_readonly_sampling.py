"""Offline integration guards, not FreeRTOS scheduling or physical servo tests.

The separate tools/test_motion_position_cache.py fixture tests actual C policy
when a host compiler is available. These assertions only inspect wiring.
"""

from pathlib import Path
import re
import unittest

from test_arm_pose_http import code_only, function_body


CONTROLLER = Path(__file__).resolve().parents[1]


class ArmReadOnlySamplingSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.motion = (CONTROLLER / "components/motion_controller/src/motion_controller.c").read_text(encoding="utf-8")
        cls.main = (CONTROLLER / "main/hello_world_main.c").read_text(encoding="utf-8")

    def test_sampler_has_no_movement_torque_home_or_driver_initialization(self):
        body = code_only(function_body(self.motion, "position_sampler"))
        self.assertEqual(re.findall(r"\b(read_axis)\s*\(", body), ["read_axis"])
        self.assertNotRegex(body, r"\b(?:servo_\w+|move_axis|motion_send_pose|dispatch|submit|motion_home_async)\s*\(")
        self.assertNotRegex(body, r"\b(?:targets|target_set|worker_error|s_stop_requested)\s*\[")

    def test_idle_check_and_one_read_are_reserved_against_new_actions(self):
        body = function_body(self.motion, "position_sampler")
        self.assertIn("xSemaphoreTake(admission_lock,0)==pdTRUE", body)
        self.assertIn("bool can_sample=!busy", body)
        state_taken = body.index("xSemaphoreTake(state_lock")
        state_given = body.index("xSemaphoreGive(state_lock)")
        read = body.index("read_axis((int)axis)")
        admission_given = body.index("xSemaphoreGive(admission_lock)")
        self.assertLess(state_taken, state_given)
        self.assertLess(state_given, read)
        self.assertLess(read, admission_given)
        self.assertIn("if (can_sample)", body[state_given:read])
        dispatch = function_body(self.motion, "dispatch")
        self.assertLess(dispatch.index("xSemaphoreTake(admission_lock"), dispatch.index("submit(j)"))

    def test_cursor_advances_only_after_attempt_and_cooldown_is_after_unlock(self):
        body = function_body(self.motion, "position_sampler")
        self.assertIn("unsigned axis=0", body)
        self.assertLess(body.index("cache_position((int)axis,raw)"),
                        body.index("axis=(axis+1U)%MOTION_POSITION_AXIS_COUNT"))
        self.assertLess(body.index("xSemaphoreGive(admission_lock)"), body.index("vTaskDelay("))
        self.assertNotIn("vTaskDelayUntil", body)
        self.assertEqual(code_only(body).count("read_axis("), 1)

    def test_snapshot_getter_never_reads_or_changes_the_bus(self):
        body = code_only(function_body(self.motion, "motion_controller_get_position_snapshot"))
        self.assertNotRegex(body, r"\b(?:read_axis|servo_\w+|dispatch|submit|motion_home_async|xTaskCreate|vTaskDelay)\s*\(")
        self.assertIn("motion_position_cache_project", body)
        self.assertIn("xSemaphoreTake(state_lock", body)
        self.assertIn("esp_timer_get_time()/1000", body)
        self.assertIn("out->motion_busy=busy", body)
        self.assertIn("MOTION_SERVO_EM3_ID : lx_ids[axis-1]", body)

    def test_cache_updates_use_actual_completed_read_time_not_home_or_targets(self):
        body = code_only(function_body(self.motion, "cache_position"))
        self.assertIn("esp_timer_get_time()/1000", body)
        self.assertIn("motion_position_cache_record", body)
        self.assertNotRegex(body, r"\b(?:home|targets|target_set|saved_pose)\b")
        self.assertIn("xSemaphoreTake(state_lock", body)

    def test_sampling_start_is_idempotent_and_rolls_back_creation_failure(self):
        body = function_body(self.motion, "motion_controller_start_position_sampling")
        self.assertIn("if (position_sampling_started)", body)
        self.assertIn("position_sampling_started=true", body)
        self.assertIn("position_sampling_started=false", body)
        self.assertIn("return ESP_ERR_NO_MEM", body)
        self.assertIn("MOTION_TASK_PRIORITY-1", body)
        self.assertNotRegex(code_only(body), r"\b(?:read_axis|move_axis|servo_\w+|dispatch|motion_home_async)\s*\(")

    def test_boot_logs_are_read_before_background_observer_is_started(self):
        body = function_body(self.main, "init_local_motion")
        self.assertLess(body.index("motion_controller_init()"), body.index("motion_log_positions()"))
        self.assertLess(body.index("motion_log_positions()"), body.index("motion_controller_start_position_sampling()"))
        self.assertNotIn("motion_home_async", code_only(body))
        startup = function_body(self.motion, "motion_log_positions")
        self.assertIn("cache_position(i,p)", startup)
        self.assertNotRegex(code_only(startup), r"\b(?:move_axis|servo_\w+_move|motion_send_pose)\s*\(")

    def test_existing_preflight_still_uses_live_read_not_telemetry_cache(self):
        preflight = function_body(self.motion, "read_pose")
        self.assertIn("out[i]=read_axis(i)", preflight)
        self.assertIn("return ESP_ERR_TIMEOUT", preflight)
        self.assertNotIn("position_cache", code_only(preflight))
        job = function_body(self.motion, "run_job")
        self.assertIn("read_pose(before)", job)
        self.assertIn("if (!cancelled()) fail(ret)", job)


if __name__ == "__main__":
    unittest.main()
