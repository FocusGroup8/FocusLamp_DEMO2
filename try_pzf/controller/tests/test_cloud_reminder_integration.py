"""Cloud radar source-contract regression checks; no RTOS/audio execution."""
from pathlib import Path
import re
import unittest
from test_welcome_tts_lifecycle import function_body

ROOT = Path(__file__).resolve().parents[2]


class CloudReminderIntegration(unittest.TestCase):
    def test_short_cloud_input_and_no_local_submit(self):
        manager = (ROOT / 'controller/components/xiaozhi_manager/src/xiaozhi_manager.c').read_text(encoding='utf-8')
        body = function_body(manager, 'xiaozhi_manager_speak_reminder')
        text = re.search(r'"(请只朗读：[^\"]+)"', body).group(1)
        self.assertLess(len(text), 15)
        self.assertIn('true, cookie, allowed, result', body)
        demo = (ROOT / 'controller/main/demo_behaviors.c').read_text(encoding='utf-8')
        self.assertIn('SEDENTARY_US (60LL * 1000000)', demo)
        self.assertIn('xiaozhi_manager_speak_reminder(cycle, cloud_reminder_allowed, cloud_reminder_result', demo)
        self.assertNotIn('local_reminder_submit(', demo)

    def test_presence_validation_does_not_reject_its_own_cloud_voice(self):
        demo = (ROOT / 'controller/main/demo_behaviors.c').read_text(encoding='utf-8')
        allowed = function_body(demo, 'cloud_reminder_allowed')
        for condition in ['s_reminder.cycle == cycle', 'mode.revision == revision',
                          'mode.mode != APP_STATE_COMPANION', 'radar.person_present', 'RADAR_FRESH_MS']:
            self.assertIn(condition, allowed)
        self.assertNotIn('voice_active', allowed)
        self.assertNotIn('XIAOZHI_MANAGER_STATE_SPEAKING', allowed)
        reset = function_body(demo, 'reset_reminder')
        self.assertIn('xiaozhi_manager_cancel_speak(request_id)', reset)
        manager = (ROOT / 'controller/components/xiaozhi_manager/src/xiaozhi_manager.c').read_text(encoding='utf-8')
        cancel = function_body(manager, 'xiaozhi_manager_cancel_speak')
        self.assertIn('s_tracked.id == id', cancel)
        process = function_body(manager, 'speak_process_request')
        self.assertGreaterEqual(process.count('req->allowed(req->cookie)'), 2)

    def test_head_uses_new_trigger_and_no_welcome_transport_retry(self):
        head = (ROOT / 'head/main/touch_handler.c').read_text(encoding='utf-8')
        self.assertIn('WELCOME_TRIGGER = "欢迎播报"', head)
        self.assertNotIn('s_welcome_messages', head)
        transport = (ROOT / 'head/main/network/tts_inject.c').read_text(encoding='utf-8')
        body = function_body(transport, 'tts_inject_speak')
        self.assertIn('strcmp(text, "欢迎播报") == 0 ? 1', body)
        self.assertIn('tts_inject_response_skipped(&response)', body)


if __name__ == '__main__':
    unittest.main()
