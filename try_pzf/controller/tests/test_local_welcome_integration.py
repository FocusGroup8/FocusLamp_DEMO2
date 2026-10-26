"""Static integration/asset checks, not board/RTOS or audible-playback tests."""

import json
import hashlib
from pathlib import Path
import struct
import unittest
import wave

from test_welcome_tts_lifecycle import function_body


ROOT = Path(__file__).resolve().parents[2]
CONTROLLER = ROOT / "controller"


class LocalWelcomeIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.local = (CONTROLLER / "main/local_reminder.c").read_text(encoding="utf-8")
        cls.api = (CONTROLLER / "main/tts_http_api.c").read_text(encoding="utf-8")
        cls.manager = (CONTROLLER / "components/xiaozhi_manager/src/xiaozhi_manager.c").read_text(encoding="utf-8")
        cls.audio = (CONTROLLER / "components/audio_bridge/src/audio_bridge.c").read_text(encoding="utf-8")

    def test_head_aliases_remain_exact_and_other_text_stays_on_cloud(self):
        head = (ROOT / "head/main/touch_handler.c").read_text(encoding="utf-8")
        aliases = function_body(self.local, "local_welcome_matches")
        for text in (
            "请只说：欢迎回来，我是小智，今天也一起专注学习吧！",
            "请只说：你好呀，准备好了吗？让我们开始今天的学习吧！",
            "请只说：很高兴见到你，记得保持坐姿，也别忘了适时休息！",
        ):
            self.assertIn('"' + text + '"', head)
            self.assertIn('"' + text + '"', aliases)
        self.assertIn("!strcmp(text, aliases[i])", aliases)
        route = function_body(self.api, "tts_speak_handler")
        self.assertIn("local_welcome_matches(text)", route)
        self.assertIn("local_welcome_submit()", route)
        self.assertIn("xiaozhi_manager_speak(text, priority)", route)

    def test_long_press_cancels_before_cloud_close(self):
        body = function_body(self.api, "chat_end_handler")
        self.assertLess(body.index("local_welcome_cancel()"),
                        body.index("xiaozhi_manager_close_audio_channel()"))
        cancel = function_body(self.local, "local_welcome_cancel")
        self.assertIn("local_welcome_policy_cancel(&s_welcome)", cancel)
        self.assertNotIn("xSemaphoreTake", cancel)
        self.assertNotIn("vTaskDelay", cancel)

    def test_one_shared_worker_and_nonblocking_admission(self):
        self.assertEqual(self.local.count("xTaskCreate("), 1)
        self.assertIn("xQueueCreateStatic(1, sizeof(request_t)", self.local)
        submit = function_body(self.local, "local_welcome_submit")
        self.assertIn("local_welcome_policy_request(&s_welcome)", submit)
        self.assertIn("xTaskNotifyGive(s_task)", submit)
        self.assertNotIn("xQueueSend", submit)
        self.assertNotIn("vTaskDelay", submit)
        play = function_body(self.local, "play_welcome")
        self.assertNotIn("xiaozhi_manager_speak", play)
        self.assertNotIn("voice_active", play)
        self.assertNotIn("XIAOZHI_MANAGER_STATE_CONNECTED", play)
        self.assertIn("audio_bridge_play_local_pcm16_cancellable", play)
        self.assertIn("welcome_cancelled, &generation, &output_started", play)
        self.assertIn("if (output_started) local_welcome_policy_output_started", play)
        self.assertLess(play.index("audio_bridge_play_local_pcm16_cancellable"),
                        play.index("local_welcome_policy_output_started"))
        self.assertIn("if (paused) resume_local_recognition()", play)
        self.assertIn("if (admitted) xiaozhi_manager_local_audio_end()", play)

    def test_local_gate_blocks_upload_cloud_audio_and_new_cloud_operations(self):
        for name in ("dialogue_pin", "uplink_resume", "uplink_enqueue", "uplink_task_func",
                     "xiaozhi_audio_callback", "speak_process_request"):
            with self.subTest(function=name):
                self.assertIn("s_local_audio_active", function_body(self.manager, name))
        begin = function_body(self.manager, "xiaozhi_manager_local_audio_begin")
        end = function_body(self.manager, "xiaozhi_manager_local_audio_end")
        self.assertIn("uplink_clear_locked()", begin)
        self.assertIn("uplink_resume()", end)
        for body in (begin, end):
            self.assertNotIn("chat_listen(", body)
            self.assertNotIn("chat_close_channel(", body)
            self.assertNotIn("s_mic_paused_for_tts =", body)

    def test_radar_trigger_and_local_playback_api_remain(self):
        reminder = function_body(self.local, "play_reminder")
        self.assertIn("req->allowed(req->cookie)", reminder)
        self.assertIn("req->result(req->cookie, ret)", reminder)
        self.assertIn("audio_bridge_play_local_pcm16(s_reminder.pcm, s_reminder.pcm_size)", reminder)
        self.assertIn("audio_bridge_play_local_pcm16_cancellable", self.audio)

    def test_cloud_voice_assets_match_installed_hashes_and_sample_counts(self):
        audio_dir = CONTROLLER / "main/audio"
        provenance = json.loads((audio_dir / "cloud_voice_assets.json").read_text(encoding="utf-8"))
        self.assertEqual(provenance["source"], "cloud_tts_capture")
        self.assertFalse(provenance["provider_id_known"])
        clips = provenance["clips"]
        self.assertEqual([clip["file"] for clip in clips],
                         [f"welcome_{index:02d}.wav" for index in range(1, 10)] + ["sedentary_reminder.wav"])
        for clip in clips:
            with self.subTest(file=clip["file"]):
                path = audio_dir / clip["file"]
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), clip["sha256"])
                self.assertEqual(path.stat().st_size, clip["wav_bytes"])
                with wave.open(str(path), "rb") as wav:
                    self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()),
                                     (1, 2, 16000, "NONE"))
                    self.assertEqual(wav.getnframes(), clip["sample_count"])
                    self.assertAlmostEqual(wav.getnframes() / 16000, clip["duration_seconds"])

    def test_current_manifest_uses_actual_cloud_subtitles_not_local_sapi_voice(self):
        audio_dir = CONTROLLER / "main/audio"
        manifest = json.loads((audio_dir / "welcome_messages.json").read_text(encoding="utf-8-sig"))
        provenance = json.loads((audio_dir / "cloud_voice_assets.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["source"], "cloud_tts_capture")
        self.assertEqual(manifest["voice"], provenance["voice"])
        self.assertNotIn("Huihui", manifest["voice"])
        self.assertEqual([message["text"] for message in manifest["messages"]],
                         [clip["text"] for clip in provenance["clips"][:9]])

    def test_embedded_wavs_and_spoken_texts_match_manifest(self):
        audio_dir = CONTROLLER / "main/audio"
        manifest = json.loads((audio_dir / "welcome_messages.json").read_text(encoding="utf-8-sig"))
        messages = manifest["messages"]
        self.assertEqual([m["id"] for m in messages], list(range(1, 10)))
        cmake = (CONTROLLER / "main/CMakeLists.txt").read_text(encoding="utf-8")
        for item in messages:
            with self.subTest(welcome=item["id"]):
                self.assertIn('"' + item["text"] + '"', self.local)
                self.assertIn('"audio/' + item["file"] + '"', cmake)
                with wave.open(str(audio_dir / item["file"]), "rb") as wav:
                    self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate(),
                                      wav.getcomptype()), (1, 2, 16000, "NONE"))
                    self.assertGreater(wav.getnframes(), 16000)
                    data = wav.readframes(wav.getnframes())
                peak = max(abs(sample[0]) for sample in struct.iter_unpack("<h", data))
                self.assertGreater(peak, 100)


if __name__ == "__main__":
    unittest.main()
