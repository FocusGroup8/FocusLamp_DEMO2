"""Offline source guards and arithmetic models, NOT C or hardware execution.

Run with Python's stdlib unittest. No device, serial port, Docker, or compiler
is used. Models illustrate the intended maths; source guards tie the important
policy/ownership boundaries to the current files but cannot prove C behavior.
"""
from pathlib import Path
import re
import unittest


HEAD = Path(__file__).resolve().parents[1]
STREAM = (HEAD / "main/camera/camera_stream.c").read_text(encoding="utf-8")
STREAM_HEADER = (HEAD / "main/camera/camera_stream.h").read_text(encoding="utf-8")
WS = (HEAD / "components/websocket_manager/src/websocket_manager.c").read_text(encoding="utf-8")
NETWORK = (HEAD / "main/network/network_manager.c").read_text(encoding="utf-8")


def function(source, name):
    """Extract one definition, counting braces outside C comments/strings."""
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError(f"Missing C function: {name}")
    start = match.end() - 1
    tokens = re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', source[start:])
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:start + token.end()]
    raise AssertionError(f"Unclosed C function: {name}")


def level_table(name, fields):
    table = re.search(r"\b" + name + r"\[BBA_LEVEL_COUNT\]\s*=\s*\{(.*?)\n\};", STREAM, re.S).group(1)
    result = {}
    for level, body in re.findall(r"\[BBA_LEVEL_(LOW|MID|HIGH)\]\s*=\s*\{(.*?)\}", table):
        values=[]
        for field in fields:
            token=re.search(r"\." + field + r"\s*=\s*(\d+|CAMERA_TRIAL_HIGH_FPS)",body).group(1)
            if token == "CAMERA_TRIAL_HIGH_FPS":
                token=re.search(r"#define CAMERA_TRIAL_HIGH_FPS\s+(\d+)",STREAM_HEADER).group(1)
            values.append(int(token))
        result[level]=tuple(values)
    return result


LEVELS = level_table("s_bba_levels", ("quality", "fps"))
RANGES = level_table("s_bba_q_ranges", ("q_min", "q_max"))


class CameraPolicyGuards(unittest.TestCase):
    def test_five_fps_profile_and_bounded_quality(self):
        self.assertEqual(LEVELS, {"LOW": (15, 2), "MID": (16, 5), "HIGH": (18, 5)})
        self.assertEqual(RANGES, {"LOW": (15, 15), "MID": (15, 16), "HIGH": (15, 18)})

    def test_fixed_quality_is_at_encoder_not_only_pi(self):
        task = function(STREAM, "camera_stream_task")
        self.assertIn("frame_fixed_low = frame_fps_limit <= ADAPTIVE_FPS_MIN", task)
        self.assertIn("frame_quality = frame_fixed_low ? 15 :", task)
        self.assertIn("camera_encode_jpeg(s_state.config.camera, frame_quality, &jpeg_size)", task)
        self.assertIn("actualQ=%d mode=%s", task)
        self.assertIn("d.network_us = wire.network_us", function(STREAM, "send_pending_frame"))

    def test_real_window_and_float_ema(self):
        task = function(STREAM, "camera_stream_task")
        update = function(STREAM, "pi_controller_update")
        self.assertIn("uint64_t window_bytes_sent", task)
        self.assertIn("window_bytes_sent * 1000000ULL / (uint64_t)elapsed_us", task)
        self.assertIn("elapsed_us = now - window_start_us", task)
        self.assertIn("ws_manager_server_take_send_stats(&ws_stats)", task)
        self.assertIn("window_start_us = now;", task)
        self.assertIn("float smoothed_quality;", STREAM)
        self.assertIn("error * PI_KI * elapsed_sec", update)
        self.assertNotIn("smoothed_quality = (int)", update)
        self.assertIn("return (int)(pi->smoothed_quality + 0.5f)", update)

    def test_transitions_discard_old_target_window(self):
        task = function(STREAM, "camera_stream_task")
        self.assertIn("curr_level != prev_level || cap_changed || budget_changed", task)
        self.assertNotIn("camera.connected && !motion.is_static", task)
        self.assertIn("camera.connected && !fixed_low", task)

    def test_payload_counter_is_success_only(self):
        pair = function(WS, "ws_manager_server_send_camera_pair")
        self.assertIn("ok ? bin_len : 0", pair)
        record = function(WS, "update_window")
        self.assertLess(record.index("if (ok)"), record.index("jpeg_bytes += jpeg_bytes"))
        self.assertLess(record.index("jpeg_bytes += jpeg_bytes"), record.index("} else {"))
        self.assertIn("ws_stats.jpeg_bytes", function(STREAM, "camera_stream_task"))

    def test_continuous_recovery_and_timeout_before_no_camera(self):
        bba = function(STREAM, "bba_evaluate")
        self.assertLess(bba.index("if (budget_events > 0)"), bba.index("if (!camera.connected)"))
        self.assertRegex(bba, r"if \(!healthy\)\s*\{\s*bba->healthy_since = 0;")
        self.assertRegex(bba, r"bba->current_level\s*= target;\s*bba->healthy_since\s*= 0;")
        self.assertIn("healthy_sec >= BBA_CLIENT_STABLE_TIME_SEC", bba)

    def test_low_mode_api_reports_current_and_requested_quality(self):
        self.assertIn("s_state.fps_limit <= ADAPTIVE_FPS_MIN ? 15", function(STREAM, "camera_stream_get_quality"))
        for name in ("mcp_cb_camera_set_quality", "rest_api_camera_quality_handler"):
            body = function(NETWORK, name)
            self.assertIn("requested_quality", body)
            self.assertIn("camera_stream_get_quality()", body)


class WebsocketOwnershipGuards(unittest.TestCase):
    def test_plain_http_rejected_before_slot_logic(self):
        handler = function(WS, "ws_server_handler")
        self.assertLess(handler.index("httpd_ws_get_fd_info"), handler.index("bool is_camera"))
        self.assertLess(handler.index("HTTPD_400_BAD_REQUEST"), handler.index("camera_client_kick"))
        self.assertIn('ws_uri_is(req->uri, "/camera")', handler)

    def test_publish_and_capture_share_mutex(self):
        admit = function(WS, "track_client_add")
        self.assertLess(admit.index("xSemaphoreTake"), admit.index("camera_client_set_fd(fd)"))
        self.assertLess(admit.index("camera_client_set_fd(fd)"), admit.rindex("xSemaphoreGive"))
        pair = function(WS, "ws_manager_server_send_camera_pair")
        self.assertIn("xSemaphoreTake(s_camera_send_mutex, portMAX_DELAY)", pair)
        self.assertNotIn("s_server_mutex", pair)
        self.assertLess(pair.index("xSemaphoreTake"), pair.index("const int client_fd = s_camera_client_fd"))
        self.assertIn("if (client_fd < 0)", pair)
        self.assertNotIn("ws_send_ws_frame(s_camera_client_fd", pair)
        self.assertNotIn("shutdown(s_camera_client_fd", pair)
        for name in ("server_send_camera_text", "ws_manager_server_send_binary"):
            body = function(WS, name)
            self.assertIn("camera_client_same_session(client_fd, session_id)", body)
        broadcast = function(WS, "ws_manager_server_broadcast_text")
        self.assertLess(broadcast.index("xSemaphoreGive(s_server_mutex)"),
                        broadcast.index("server_send_camera_text(camera_fd, camera_session"))

    def test_partial_frame_shutdown_and_no_duplicate_app_queue(self):
        pair = function(WS, "ws_manager_server_send_camera_pair")
        self.assertIn("group.reason == WS_SEND_BUDGET_TIMEOUT && group.bytes == 0", pair)
        self.assertIn("shutdown(client_fd, SHUT_RDWR)", pair)
        self.assertNotIn("camera_client_kick(", pair)
        self.assertNotIn("httpd_sess_trigger_close(", pair)
        self.assertIn("(1500 * 1000)", WS)

    def test_legacy_binary_send_dropped_pool_direct_send(self):
        body = function(WS, "server_send_binary")
        self.assertNotIn("ws_frame_pool_acquire", body)
        self.assertNotIn("ws_frame_buf_t", body)
        self.assertNotIn("memcpy(buf->data", body)
        self.assertNotIn("atomic_store(&buf->in_use", body)
        self.assertIn("ws_send_ws_frame(client_fd, HTTPD_WS_TYPE_BINARY, (const uint8_t *)data,", body)
        self.assertIn("int64_t dur_us = esp_timer_get_time() - send_start_us;", body)
        self.assertIn("if (len > WS_FRAME_BUF_SIZE)", body)

    def test_legacy_binary_send_keeps_failure_close(self):
        body = function(WS, "server_send_binary")
        self.assertIn("record_send(false, true, dur_us, 0, 0, false)", body)
        self.assertIn("record_send(false, false, dur_us, 0, errno, false)", body)
        self.assertIn("httpd_sess_update_lru_counter(s_server, client_fd)", body)
        self.assertIn("camera_client_set_fd(-1)", body)
        self.assertIn("camera_client_reset_failures()", body)

    def test_close_cleanup_owns_success_and_failure_paths(self):
        handler = function(WS, "ws_server_handler")
        self.assertEqual(handler.count("ws_finish_close(req)"), 3)
        self.assertIn("ws_pkt.type == HTTPD_WS_TYPE_CLOSE ? ESP_OK : ret", handler)
        self.assertIn("return ESP_OK", function(WS, "ws_finish_close"))
        self.assertIn("close(sockfd)", function(WS, "ws_session_close_cb"))


class ArithmeticModels(unittest.TestCase):
    def test_stretched_window_normalization(self):
        # 32KiB over1.6s ->20KiB/s, not32KiB/s.
        self.assertEqual(32768 * 1000000 // 1600000, 20480)

    def test_ceiling_scaled_target_model(self):
        target = {"LOW": 20 * 1024, "MID": 50 * 1024, "HIGH": 60 * 1024}
        def scaled(level, cap):
            fps = min(LEVELS[level][1], cap)
            return 10 * 1024 * fps if cap <= 2 else target[level] * fps // LEVELS[level][1]
        self.assertEqual([scaled(level, 2) for level in LEVELS], [20480] * 3)
        self.assertEqual(scaled("LOW", 1), 10240)
        self.assertEqual(scaled("MID", 4), 40960)
        self.assertEqual(scaled("HIGH", 4), 49152)
        source = function(STREAM, "stream_target_bitrate")
        self.assertIn("10 * 1024 * applied_fps", source)
        self.assertIn("* applied_fps / s_bba_levels[level].fps", source)

    def test_float_ema_can_reach_both_bounds_without_old_integer_bias(self):
        for level, (minimum, maximum) in RANGES.items():
            value = float(maximum)
            for _ in range(200):
                value = 0.15 * minimum + 0.85 * value
            self.assertEqual(int(value + 0.5), minimum, level)
            for _ in range(200):
                value = 0.15 * maximum + 0.85 * value
                self.assertLessEqual(value, maximum + 1e-9)
            self.assertEqual(int(value + 0.5), maximum, level)

    def test_old_high_q29_claim_is_not_q35(self):
        value = 18
        for _ in range(100):
            value = int(0.15 * 35 + 0.85 * value)
        self.assertEqual(value, 29)


if __name__ == "__main__":
    unittest.main(verbosity=2)
