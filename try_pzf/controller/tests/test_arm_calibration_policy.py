"""Offline source guards and independent reference arithmetic for trial jogging.

These Python tests do not execute C, schedule FreeRTOS or control a board.
The separate fixtures/arm_calibration_policy_fixture.c exercises the actual
policy header when compiled and run with a host C compiler. Cross-compilation
alone is reported separately and does not constitute a C runtime test.
"""

from pathlib import Path
import unittest

from test_arm_pose_http import code_only, function_body


CONTROLLER = Path(__file__).resolve().parents[1]
COMPONENT = CONTROLLER / "components/motion_controller"


class CalibrationSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.motion = (COMPONENT / "src/motion_controller.c").read_text(encoding="utf-8")
        cls.header = (COMPONENT / "include/motion_controller.h").read_text(encoding="utf-8")
        cls.policy = (COMPONENT / "include/motion_calibration_policy.h").read_text(encoding="utf-8")

    def body(self, name):
        return function_body(self.motion, name)

    def test_public_apis_distinguish_session_begin_jog_end_and_cached_status(self):
        for name in ("motion_calibration_begin_async", "motion_calibration_jog_async",
                     "motion_calibration_end", "motion_calibration_get_snapshot"):
            self.assertIn(name, self.header)
            self.body(name)
        for field in ("reference_valid", "has_observations", "has_target", "feedback",
                      "action_status", "last_error", "reason"):
            self.assertIn(field, self.header)

    def test_trial_policy_has_no_driver_or_scheduler_dependency(self):
        self.assertNotRegex(code_only(self.policy), r"freertos|servo_driver|esp_err|xSemaphore|vTask")
        self.assertIn("config->step<=0", self.policy)
        self.assertIn("config->min_position>=config->max_position", self.policy)
        self.assertIn("config->time_ms==0", self.policy)
        self.assertIn("config->speed==0", self.policy)
        self.assertIn("(int64_t)direction*config->step", self.policy)
        self.assertIn("MOTION_CALIBRATION_TRIAL_BOUNDARY", self.policy)

    def test_begin_checks_configuration_and_idle_owner_before_session_registration(self):
        body = self.body("motion_calibration_begin_async")
        self.assertLess(body.index("motion_calibration_config_valid"), body.index("calloc("))
        self.assertIn("calibration.active || busy || pending_count", body)
        self.assertLess(body.index(".active=true"), body.index("submit(j)"))
        self.assertIn("JOB_CALIBRATION_BEGIN", body)
        self.assertNotRegex(code_only(body), r"\b(?:read_axis|move_axis|servo_\w+)\s*\(")
        self.assertIn("calibration.active=false", body)

    def test_jog_admission_checks_identity_ready_and_busy_and_never_queues(self):
        body = self.body("motion_calibration_jog_async")
        self.assertIn("direction!=-1 && direction!=1", body)
        self.assertIn("calibration.session_id!=session_id", body)
        self.assertIn("!calibration.ready || busy || pending_count", body)
        self.assertIn("ESP_ERR_NOT_FOUND", body)
        self.assertIn("xSemaphoreTake(admission_lock,0)", body)
        self.assertIn("JOB_CALIBRATION_JOG", body)
        self.assertNotRegex(code_only(body), r"\b(?:read_axis|move_axis|servo_\w+)\s*\(")

    def test_ordinary_entries_are_rejected_before_preemption_and_queueing(self):
        dispatch = self.body("dispatch")
        self.assertLess(dispatch.index("if (calibration_locked)"), dispatch.index("preempt ? stop_owned()"))
        submit = self.body("submit")
        self.assertLess(submit.index("calibration.active && !calibration_job"), submit.index("if (busy)"))
        self.assertIn("!calibration.active || calibration.session_id!=j->session_id", submit)

    def test_worker_uses_fresh_read_then_boundary_decision_before_torque_or_position(self):
        body = self.body("run_calibration_job")
        self.assertLess(body.index("read_axis(j->axis)"), body.index("motion_calibration_target("))
        self.assertLess(body.index("motion_calibration_target("), body.index("move_axis("))
        self.assertIn("MOTION_CALIBRATION_CURRENT_OUTSIDE", body)
        self.assertIn('"trial_boundary",true', body)
        self.assertIn('"read_failed",false', body)
        self.assertGreaterEqual(body.count("calibration_current(j,NULL)"), 3)
        self.assertNotRegex(code_only(body), r"\bservo_\w+\s*\(")

    def test_begin_branch_only_records_reference_without_moving(self):
        body = self.body("run_calibration_job")
        begin = body.split("if (j->kind==JOB_CALIBRATION_BEGIN)", 1)[1].split("int target;", 1)[0]
        self.assertIn("calibration.reference_raw=actual", begin)
        self.assertIn("actual<config.min_position || actual>config.max_position", begin)
        self.assertNotIn("move_axis", code_only(begin))

    def test_exact_step_verification_covers_both_protocols_and_records_actual_reads(self):
        body = self.body("run_calibration_job")
        self.assertIn("j->axis==0 ? 300U : (uint32_t)config.time_ms+300U", body)
        self.assertIn("if (actual==target)", body)
        self.assertNotIn("tolerance", code_only(body))
        self.assertIn("esp_timer_get_time()<deadline", body)
        self.assertGreaterEqual(body.count("calibration_observe(j,actual)"), 2)
        self.assertIn('"position_unconfirmed",false', body)
        observe = self.body("calibration_observe")
        self.assertIn("cache_position(j->axis,actual)", observe)
        self.assertNotIn("target_raw", code_only(observe))

    def test_stop_invalidation_wins_over_late_worker_results(self):
        stop = self.body("stop_owned")
        self.assertIn("calibration.active=false; calibration.ready=false", stop)
        self.assertLess(stop.index("calibration.active=false"), stop.index("xEventGroupWaitBits"))
        result = self.body("calibration_result")
        self.assertIn("!calibration.active || cancelled()", result)
        self.assertIn("ACTION_STATUS_INTERRUPTED", result)
        end = self.body("motion_calibration_end")
        self.assertIn("!matches ? ESP_ERR_NOT_FOUND : active ? stop_owned() : ESP_OK", end)

    def test_status_is_cache_only_and_session_ids_are_not_boot_counter_one(self):
        body = self.body("motion_calibration_get_snapshot")
        self.assertIn("motion_position_cache_project", body)
        self.assertIn("out->motion_busy=busy", body)
        self.assertNotRegex(code_only(body), r"\b(?:read_axis|move_axis|servo_\w+|vTaskDelay)\s*\(")
        self.assertIn("calibration_sequence=esp_random()", self.body("motion_controller_init"))
        self.assertIn("if (++calibration_sequence==0) ++calibration_sequence", self.body("motion_calibration_begin_async"))

    def test_no_formal_limit_persistence_unload_or_automatic_home_was_added(self):
        for name in ("motion_calibration_begin_async", "motion_calibration_jog_async",
                     "motion_calibration_end", "run_calibration_job"):
            self.assertNotRegex(code_only(self.body(name)), r"\b(?:nvs_\w+|servo_\w*unload\w*|motion_home_async)\s*\(")
            self.assertNotIn("MOTION_MANUAL_SERVO_", code_only(self.body(name)))


def reference_target(low, high, current, step, direction):
    """Independent Python arithmetic; not execution of the production C."""
    if direction not in (-1, 1) or step <= 0:
        return "bad_step", None
    if not low <= current <= high:
        return "current_outside", None
    target = current + direction * step
    return ("ok", target) if low <= target <= high else ("trial_boundary", None)


class CalibrationReferenceArithmeticTests(unittest.TestCase):
    def test_trial_edges_refuse_instead_of_clamping(self):
        self.assertEqual(reference_target(100, 200, 199, 2, 1), ("trial_boundary", None))
        self.assertEqual(reference_target(100, 200, 100, 2, -1), ("trial_boundary", None))
        self.assertEqual(reference_target(100, 200, 198, 2, 1), ("ok", 200))

    def test_current_must_be_inside_before_any_candidate_is_produced(self):
        self.assertEqual(reference_target(100, 200, 99, 2, 1), ("current_outside", None))
        self.assertEqual(reference_target(100, 200, 201, 2, -1), ("current_outside", None))

    def test_each_step_is_relative_to_new_actual_position_not_last_target(self):
        self.assertEqual(reference_target(100, 200, 151, 2, 1), ("ok", 153))
        self.assertEqual(reference_target(100, 200, 149, 2, -1), ("ok", 147))

    def test_overflow_sized_requests_never_wrap_into_an_allowed_target(self):
        self.assertEqual(reference_target(0, 2**31-1, 2**31-1, 2**31-1, 1), ("trial_boundary", None))
        self.assertEqual(reference_target(0, 1000, 0, 0, 1), ("bad_step", None))
        self.assertEqual(reference_target(0, 1000, 500, 1, 0), ("bad_step", None))


if __name__ == "__main__":
    unittest.main()
