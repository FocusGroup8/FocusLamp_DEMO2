"""Offline source guards + response policy model; NOT C or device execution.

The project firmware build checks C integration. These stdlib-only tests model
fragment collection and JSON acceptance, and guard their production wiring.
"""
import json
from pathlib import Path
import re
import unittest


SOURCE = (Path(__file__).resolve().parents[1] / "main/network/tts_inject.c").read_text(encoding="utf-8")
CAPACITY = int(re.search(r"#define TTS_INJECT_RESPONSE_CAPACITY (\d+)", SOURCE).group(1))
SPEAK = SOURCE.split("esp_err_t tts_inject_speak", 1)[1].split("esp_err_t tts_inject_end_chat", 1)[0]
END_CHAT = SOURCE.split("esp_err_t tts_inject_end_chat", 1)[1]


def collect(chunks):
    body = bytearray()
    overflow = False
    for chunk in chunks:
        if overflow:
            continue
        if len(chunk) > CAPACITY - 1 - len(body):
            overflow = True
            continue
        body.extend(chunk)
    return bytes(body), overflow


def accepted(chunks):
    body, overflow = collect(chunks)
    if overflow or not body or b"\x00" in body:
        return False
    try:
        value = json.loads(body)
    except (ValueError, UnicodeError):
        return False
    return isinstance(value, dict) and value.get("ok") is True


class ResponsePolicyModel(unittest.TestCase):
    def test_true_means_accepted(self):
        self.assertTrue(accepted([b'{"ok":true}']))

    def test_false_is_rejected(self):
        self.assertFalse(accepted([b'{"ok":false,"error":"xiaozhi speak failed"}']))

    def test_fragmented_response(self):
        self.assertTrue(accepted([b'{"', b'ok"', b':', b'true', b'}']))

    def test_each_split_point(self):
        response = b'{"ok":true}'
        for split in range(len(response) + 1):
            with self.subTest(split=split):
                self.assertTrue(accepted([response[:split], response[split:]]))

    def test_empty_response(self):
        self.assertFalse(accepted([]))

    def test_missing_ok(self):
        self.assertFalse(accepted([b'{}']))

    def test_non_boolean_ok(self):
        for value in (b'1', b'0', b'"true"', b'null', b'{}', b'[]'):
            with self.subTest(value=value):
                self.assertFalse(accepted([b'{"ok":' + value + b'}']))

    def test_only_json_object(self):
        for body in (b'true', b'[{"ok":true}]', b'null'):
            with self.subTest(body=body):
                self.assertFalse(accepted([body]))

    def test_malformed_or_truncated(self):
        for body in (b'{"ok":true', b'{"ok":}', b'{ok:true}', b'not json'):
            with self.subTest(body=body):
                self.assertFalse(accepted([body]))

    def test_trailing_data(self):
        self.assertFalse(accepted([b'{"ok":true} garbage']))
        self.assertFalse(accepted([b'{"ok":true}{"ok":true}']))

    def test_embedded_nul(self):
        self.assertFalse(accepted([b'{"ok":true}\x00']))
        self.assertFalse(accepted([b'{"ok":true}\x00{"ok":false}']))

    def test_exact_limit_with_whitespace(self):
        body = b'{"ok":true}'
        self.assertTrue(accepted([body + b' ' * (CAPACITY - 1 - len(body))]))

    def test_over_limit_rejects_even_valid_prefix(self):
        self.assertFalse(accepted([b'{"ok":true}', b' ' * CAPACITY]))

    def test_single_oversize_chunk(self):
        body, overflow = collect([b'x' * CAPACITY])
        self.assertTrue(overflow)
        self.assertEqual(body, b'')

    def test_overflow_sticks(self):
        body, overflow = collect([b'{"ok":true}', b'x' * CAPACITY, b' ignored'])
        self.assertTrue(overflow)
        self.assertEqual(body, b'{"ok":true}')


class ProductionSourceGuards(unittest.TestCase):
    def test_per_attempt_collector_is_reset_and_wired(self):
        loop = SPEAK.index("for (int attempt")
        reset = SPEAK.index("tts_inject_response_t response = {0};")
        self.assertLess(loop, reset)
        self.assertIn(".event_handler = tts_inject_http_event", SPEAK)
        self.assertIn(".user_data = &response", SPEAK)

    def test_collector_is_bounded_and_handles_chunked_data(self):
        self.assertIn("event->event_id != HTTP_EVENT_ON_DATA", SOURCE)
        self.assertIn("sizeof(response->body) - 1 - response->length", SOURCE)
        self.assertIn("response->overflow = true;", SOURCE)
        self.assertIn("response->body[response->length] = '\\0';", SOURCE)
        self.assertNotIn("esp_http_client_is_chunked_response", SOURCE)

    def test_strict_parse_and_boolean(self):
        self.assertIn("strlen(response->body) != response->length", SOURCE)
        self.assertRegex(SOURCE, r"cJSON_ParseWithLengthOpts\(response->body,\s*response->length \+ 1, NULL, true\)")
        self.assertIn('cJSON_GetObjectItemCaseSensitive(root, "ok")', SOURCE)
        self.assertIn("cJSON_IsObject(root) && cJSON_IsBool(ok) && cJSON_IsTrue(ok)", SOURCE)
        self.assertIn("cJSON_Delete(root);", SOURCE)

    def test_200_failure_exits_without_business_retry(self):
        success = SPEAK.split("if (status == 200) {", 1)[1].split('ESP_LOGW(TAG, "TTS inject HTTP', 1)[0]
        self.assertIn("tts_inject_response_accepted(&response)", success)
        self.assertIn("err = ESP_FAIL;", success)
        self.assertIn("esp_http_client_cleanup(client);", success)
        self.assertIn("break;", success)
        self.assertNotIn("continue;", success)

    def test_log_means_queued_not_played(self):
        self.assertIn("TTS request accepted (queued, not playback confirmation", SPEAK)
        self.assertNotIn("TTS inject response: HTTP", SPEAK)

    def test_network_parameters_unchanged(self):
        for parameter, value in (("HTTP_TIMEOUT_MS", 2000), ("RETRY_COUNT", 3), ("RETRY_DELAY_MS", 300)):
            self.assertRegex(SOURCE, rf"#define TTS_INJECT_{parameter} {value}\b")

    def test_end_chat_legacy_success_path_preserved(self):
        self.assertIn("if (status == 200)", END_CHAT)
        self.assertIn("return ESP_OK;", END_CHAT)
        self.assertNotIn("tts_inject_response_accepted", END_CHAT)


if __name__ == "__main__":
    unittest.main()
