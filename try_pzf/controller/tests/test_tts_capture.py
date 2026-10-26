"""Host format/HTTP integration tests + firmware source guards, not board tests."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import capture_xiaozhi_tts as capture
import tts_capture_format as formats


def exported(packets):
    payload = b"".join(struct.pack("<H", len(packet)) + packet for packet in packets)
    return struct.pack("<4sIII", b"FOP1", 16000, len(packets), len(payload)) + payload


class FormatTests(unittest.TestCase):
    def test_export_roundtrip(self):
        packets = [b"\xf8\xff\xfe", b"\xf8\xff\xfe"]
        self.assertEqual(formats.parse_capture(exported(packets)), (16000, packets))

    def test_truncated_download_rejected(self):
        for data in (b"", b"FOP1", exported([b"\xf8\xff\xfe"])[:-1]):
            with self.subTest(data=data), self.assertRaises(ValueError):
                formats.parse_capture(data)

    def test_wrong_magic_count_and_empty_packet_rejected(self):
        for data in (b"FAIL" + exported([b"\xf8"])[4:],
                     struct.pack("<4sIII", b"FOP1", 16000, 2, 3) + b"\x01\x00\xf8",
                     struct.pack("<4sIII", b"FOP1", 16000, 1, 2) + b"\x00\x00"):
            with self.assertRaises(ValueError):
                formats.parse_capture(data)

    def test_toc_durations_are_not_fixed_sixty_ms(self):
        self.assertEqual(formats.opus_samples_48k(b"\xf8\xff\xfe"), 960)
        self.assertEqual(formats.opus_samples_48k(b"\xf0\xff"), 480)
        self.assertEqual(formats.opus_samples_48k(b"\x18\x01"), 2880)
        self.assertEqual(formats.opus_samples_48k(b"\xfb\x03\xff"), 2880)
        for data in (b"", b"\xfb", b"\xfb\x00", b"\xfb\x07"):
            with self.assertRaises(ValueError):
                formats.opus_samples_48k(data)

    def test_ogg_headers_granules_and_crc(self):
        data = formats.make_ogg([b"\xf8\xff\xfe"] * 3)
        pos, pages = 0, []
        while pos < len(data):
            self.assertEqual(data[pos:pos+4], b"OggS")
            count = data[pos+26]
            length = 27 + count + sum(data[pos+27:pos+27+count])
            page = bytearray(data[pos:pos+length])
            crc = struct.unpack_from("<I", page, 22)[0]
            page[22:26] = bytes(4)
            self.assertEqual(formats.ogg_crc(page), crc)
            pages.append(bytes(page))
            pos += length
        self.assertEqual(len(pages), 5)
        self.assertEqual(pages[0][5], 2)
        self.assertIn(b"OpusHead", pages[0])
        self.assertEqual(struct.unpack_from("<Q", pages[-1], 6)[0], 2880)
        self.assertEqual(pages[-1][5], 4)

    def test_exact_255_bytes_has_terminating_zero_lace(self):
        page = formats.ogg_page(bytes(255), 0, 0, 0, 1)
        self.assertEqual(page[26:29], b"\x02\xff\x00")

    def test_subtitle_matching_rejects_greetings_and_added_explanation(self):
        self.assertTrue(capture.text_matches({"cloud_text": "嗨，Focus！"}, "嗨 focus。"))
        self.assertFalse(capture.text_matches({"cloud_text": "我在呢"}, "嗨，Focus"))
        self.assertFalse(capture.text_matches({"cloud_text": "好的。嗨，Focus！"}, "嗨，Focus"))
        self.assertFalse(capture.text_matches({"cloud_text": "嗨，Focus！", "text_truncated": True}, "嗨，Focus"))

    def test_normalization_preserves_exact_matching_for_punctuation_case_and_focus_alias(self):
        self.assertEqual(capture.normalized(" \t嗨，福克斯！\n"), "嗨focus")
        self.assertEqual(capture.match_kind({"cloud_text": "嗨，福克斯！"}, "嗨 FOCUS。"), "exact")

    def test_approved_second_and_fourth_phrase_variants_are_equivalent(self):
        targets = dict(capture.batch_targets())
        variants = [
            ("welcome_02.wav", "哈哈，摸得我脑袋痒痒的，开心值又加一了。"),
            ("welcome_04.wav", "今天也辛苦啦，先做好，让focus陪着你。"),
            ("welcome_04.wav", "今天也辛苦了，先做好，让focus陪着你。"),
            ("welcome_04.wav", "今天也辛苦啦，先坐好，让Focus陪着你。"),
        ]
        for name, actual in variants:
            with self.subTest(name=name, actual=actual):
                status = {"cloud_text": actual, "text_truncated": False}
                self.assertIs(capture.text_matches(status, targets[name]), True)
                self.assertEqual(capture.match_kind(status, targets[name]), "equivalent")

    def test_unapproved_greetings_farewells_and_fragments_never_match_batch(self):
        rejected = ["你好", "辛苦了亲", "今天", "我在欸有什么事吗", "好啦那我先退下啰拜拜",
                    "天也辛苦了，先做好，让focus陪着你。"]
        for actual in rejected:
            for name, expected in capture.batch_targets():
                with self.subTest(actual=actual, name=name):
                    status = {"cloud_text": actual}
                    self.assertIs(capture.text_matches(status, expected), False)
                    self.assertEqual(capture.match_kind(status, expected), "none")

    def test_empty_and_truncated_subtitles_do_not_match(self):
        expected = "今天辛苦了，休息一下，我会一直陪着你。"
        cases = [({}, expected), ({"cloud_text": "。 \n"}, expected),
                 ({"cloud_text": expected}, ""), ({"cloud_text": expected}, "。"),
                 ({"cloud_text": expected, "text_truncated": True}, expected),
                 ({"cloud_text": "今天也辛苦啦，先做好，让focus陪着你。",
                   "text_truncated": True}, expected)]
        for status, target in cases:
            with self.subTest(status=status, target=target):
                self.assertIs(capture.text_matches(status, target), False)
                self.assertEqual(capture.match_kind(status, target), "none")

    def test_batch_has_ten_existing_names(self):
        targets = capture.batch_targets()
        self.assertEqual(len(targets), 10)
        self.assertEqual(targets[0][0], "welcome_01.wav")
        self.assertEqual(targets[-1][0], "sedentary_reminder.wav")

    def test_batch_uses_new_fourth_prompt_and_preserves_other_prompts(self):
        self.assertEqual(capture.batch_targets(), [
            ("welcome_01.wav", "嗨，主人，想我了吗？Focus一直都在哦！"),
            ("welcome_02.wav", "哈哈，摸得我脑袋痒痒的，开心值又加一啦！"),
            ("welcome_03.wav", "欢迎回来！小灯已经就位，随时陪你出发。"),
            ("welcome_04.wav", "今天辛苦了，休息一下，我会一直陪着你。"),
            ("welcome_05.wav", "给你一点光，也给你一点好心情。"),
            ("welcome_06.wav", "准备好了吗？我们一起专注，把事情做好。"),
            ("welcome_07.wav", "收到摸摸头！今天也要元气满满哦。"),
            ("welcome_08.wav", "别着急，我们一件一件慢慢来。"),
            ("welcome_09.wav", "有你在，我的小世界又亮起来啦！"),
            ("sedentary_reminder.wav", "你已经坐了很久，请站起来活动活动吧。"),
        ])
        expected = capture.batch_targets()[3][1]
        self.assertEqual(capture.match_kind({"cloud_text": expected}, expected), "exact")

    def test_saved_metadata_preserves_actual_expected_and_match_type(self):
        expected = "今天辛苦了，休息一下，我会一直陪着你。"
        cases = [(expected, "exact"), ("今天也辛苦了，先做好，让focus陪着你。", "equivalent"),
                 ("天也辛苦了，先做好，让focus陪着你。", "none")]
        blob = exported([b"\xf8\xff\xfe"] * 2)
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            for actual, kind in cases:
                with self.subTest(kind=kind):
                    status = {"cloud_text": actual, "text_truncated": False, "frames": 2}
                    capture.save_reply(folder, kind + ".wav", status, blob, None, expected)
                    metadata = json.loads((folder / (kind + ".json")).read_text(encoding="utf-8"))
                    self.assertEqual(metadata["cloud_text"], actual)
                    self.assertEqual(metadata["expected_text"], expected)
                    self.assertIs(metadata["transcript_matches"], kind != "none")
                    self.assertEqual(metadata["match_type"], kind)
                    self.assertIs(metadata["audible_verified"], False)

    def test_ffmpeg_decodes_real_opus_silence_to_required_wav(self):
        ffmpeg = formats.find_ffmpeg()
        if not ffmpeg:
            self.skipTest("Optional FFmpeg dependency not installed")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            opus = folder / "silence.opus"
            opus.write_bytes(formats.make_ogg([b"\xf8\xff\xfe"] * 10))
            duration = formats.convert_wav(ffmpeg, opus, folder / "silence.wav")
            self.assertAlmostEqual(duration, 0.2, places=3)
            with self.assertRaises(FileExistsError):
                formats.convert_wav(ffmpeg, opus, folder / "silence.wav")

    def test_ffmpeg_must_not_silently_drop_a_corrupt_packet(self):
        ffmpeg = formats.find_ffmpeg()
        if not ffmpeg:
            self.skipTest("Optional FFmpeg dependency not installed")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            opus = folder / "corrupt.opus"
            opus.write_bytes(formats.make_ogg([b"\xf8\xff\xfe", b"\xfb\x41", b"\xf8\xff\xfe"]))
            with self.assertRaises((subprocess.CalledProcessError, ValueError)):
                formats.convert_wav(ffmpeg, opus, folder / "corrupt.wav", 960)
            self.assertFalse((folder / "corrupt.wav").exists())

    def test_ffmpeg_rejects_wrong_decoded_duration(self):
        ffmpeg = formats.find_ffmpeg()
        if not ffmpeg:
            self.skipTest("Optional FFmpeg dependency not installed")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            opus = folder / "silence.opus"
            opus.write_bytes(formats.make_ogg([b"\xf8\xff\xfe"] * 10))
            with self.assertRaisesRegex(ValueError, "duration"):
                formats.convert_wav(ffmpeg, opus, folder / "bad_duration.wav", 3201)
            self.assertFalse((folder / "bad_duration.wav").exists())


class HttpIntegrationTests(unittest.TestCase):
    def test_guided_batch_downloads_and_sorts_unmatched_without_overwriting_assets(self):
        self.run_batch_case(1)

    def test_resume_at_second_keeps_original_names_and_skips_first(self):
        self.run_batch_case(2)

    def test_guided_batch_accepts_approved_equivalents_and_preserves_transcripts(self):
        self.run_batch_case(1, {
            "welcome_02.wav": "哈哈，摸得我脑袋痒痒的，开心值又加一了。",
            "welcome_04.wav": "今天也辛苦啦，先做好，让focus陪着你。",
        })

    def run_batch_case(self, start_at, equivalents=None):
        targets = capture.batch_targets()[start_at - 1:]
        equivalents = equivalents or {}
        texts = ["我在呢"] + [equivalents.get(name, text) for name, text in targets]
        blob = exported([b"\xf8\xff\xfe"] * 2)
        state = {"id": 0, "index": -1, "armed": False, "cancelled": False}

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send(self, data):
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                state["id"] += 1
                if body["action"] == "arm":
                    state["index"] += 1
                    state["armed"] = True
                else:
                    state["cancelled"] = True
                self.send(json.dumps({"ok": True, "id": state["id"]}).encode())

            def do_GET(self):
                if "download=1" in self.path:
                    self.send(blob)
                else:
                    self.send(json.dumps({"ok": True, "id": state["id"], "state": "complete",
                        "frames": 2, "cloud_text": texts[state["index"]], "text_truncated": False}).encode())

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
                folder = Path(temporary) / "new_capture"
                args = argparse.Namespace(host=f"127.0.0.1:{server.server_port}", ffmpeg=None,
                    opus_only=True, output=str(folder), batch=True, count=1, timeout=10, start_at=start_at)
                with contextlib.redirect_stdout(io.StringIO()):
                    capture.run(args)
                self.assertEqual(len(list(folder.glob("*.opus"))), len(targets))
                if start_at == 2:
                    self.assertFalse((folder / "welcome_01.opus").exists())
                self.assertEqual(len(list((folder / "unmatched").glob("*.opus"))), 1)
                for name, expected in targets:
                    metadata = json.loads((folder / (Path(name).stem + ".json")).read_text(encoding="utf-8"))
                    self.assertEqual(metadata["cloud_text"], equivalents.get(name, expected))
                    self.assertEqual(metadata["expected_text"], expected)
                    self.assertTrue(metadata["transcript_matches"])
                    self.assertEqual(metadata["match_type"], "equivalent" if name in equivalents else "exact")
                    self.assertFalse(metadata["audible_verified"])
                unmatched_path = next((folder / "unmatched").glob("*.json"))
                unmatched = json.loads(unmatched_path.read_text(encoding="utf-8"))
                self.assertEqual(unmatched["cloud_text"], "我在呢")
                self.assertEqual(unmatched["expected_text"], targets[0][1])
                self.assertFalse(unmatched["transcript_matches"])
                self.assertEqual(unmatched["match_type"], "none")
                self.assertFalse(unmatched["audible_verified"])
                self.assertTrue(state["cancelled"])
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)

    def test_changed_capture_id_does_not_download_or_cancel_someone_else(self):
        client = capture.CaptureClient("127.0.0.1")
        client.request = lambda **kwargs: {"id": 2, "state": "complete"}
        with self.assertRaisesRegex(RuntimeError, "ID changed"):
            client.reply(1, time.monotonic() + 1)

    def test_sealing_waits_for_final_writer_before_download(self):
        client = capture.CaptureClient("127.0.0.1")
        blob = exported([b"\xf8\xff\xfe"])
        statuses = iter([
            {"id": 1, "state": "sealing"},
            {"id": 1, "state": "complete", "frames": 1},
            {"id": 1, "state": "complete", "frames": 1},
        ])
        def request(**kwargs):
            return blob if kwargs.get("binary") else next(statuses)
        client.request = request
        status, data = client.reply(1, time.monotonic() + 2)
        self.assertEqual(status["state"], "complete")
        self.assertEqual(data, blob)

    def test_rejects_production_asset_output(self):
        args = argparse.Namespace(host="127.0.0.1", ffmpeg=None, opus_only=True,
            output=str(ROOT / "main/audio"), batch=False, count=1, timeout=1)
        with self.assertRaisesRegex(ValueError, "production"):
            capture.run(args)


class FirmwareSourceGuards(unittest.TestCase):
    def test_passive_capture_does_not_change_voice_pipeline_or_depend_on_flash_io(self):
        source = (ROOT / "main/tts_capture.c").read_text(encoding="utf-8")
        for forbidden in ("vTaskDelete", "xTaskCreate", "esp_xiaozhi_chat_send", "fwrite(", "nvs_set", "i2s_"):
            self.assertNotIn(forbidden, source)
        self.assertNotIn("xSemaphoreTake", source)
        self.assertNotIn("capture_callback_busy", source)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", source)
        self.assertIn("CAP_ERROR", source)
        self.assertIn("s_capture.phase == CAP_SEALING && !s_writers", source)

    def test_packet_payload_copy_is_outside_critical_and_pinned_until_done(self):
        from test_welcome_tts_lifecycle import function_body
        source = (ROOT / "main/tts_capture.c").read_text(encoding="utf-8")
        packet = function_body(source, "tts_capture_packet")
        self.assertLess(packet.index("++s_writers"), packet.index("portEXIT_CRITICAL"))
        self.assertLess(packet.index("portEXIT_CRITICAL"), packet.index("memcpy("))
        self.assertLess(packet.index("memcpy("), packet.index("writer_done();"))
        control = function_body(source, "control_handler")
        self.assertIn("s_downloading || s_writers", control)
        self.assertLess(control.rindex("portEXIT_CRITICAL"), control.index("free(released)"))
        stop = function_body(source, "tts_capture_stop")
        self.assertIn("CAP_SEALING", stop)
        self.assertNotIn("CAP_COMPLETE", stop)

    def test_hook_and_router_are_present(self):
        main = (ROOT / "main/hello_world_main.c").read_text(encoding="utf-8")
        for hook in ("tts_capture_start();", "tts_capture_stop();", "tts_capture_abort();",
                     "tts_capture_packet(data, (size_t)len)"):
            self.assertIn(hook, main)
        self.assertIn('"tts_capture.c"', (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertIn("tts_capture_http_init()", (ROOT / "main/tts_http_api.c").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
