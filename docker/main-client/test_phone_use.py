"""Offline local-phone integration: exact frames, bounded work, no reminders."""
import asyncio
import copy
import os
import time
import unittest
from unittest import mock

from algorithm_dispatch import AlgorithmFrame
from perception_state import PerceptionState
from phone_use_policy import PhoneUsePolicy
from test_algorithm_dispatch import main, idle


def phone(detected=True, overlap=True):
    return {'phone_detected': detected, 'hand_overlap': overlap,
            'confidence': 0.85 if detected else None, 'detections': []}


def hand(detected=True):
    return {'hand_detected': detected, 'gesture': 'Open_Palm',
            'hand_landmarks': [{'x': 0.2, 'y': 0.4, 'z': 0.0} for _ in range(21)] if detected else []}


class PhonePolicyTests(unittest.TestCase):
    def setUp(self):
        self.policy = PhoneUsePolicy()

    def observe(self, second, result=None, seq=None, delay=0.1):
        return self.policy.observe(phone() if result is None else result,
                                   second if seq is None else seq, second, second + delay)

    def test_three_seconds_requires_distinct_input_samples_not_processing_time(self):
        for second in (100, 101, 102):
            output = self.observe(second)
            self.assertEqual(output['status'], 'detected')
        output = self.observe(103)
        self.assertEqual((output['status'], output['sustained_ms']), ('suspected', 3000))

    def test_missing_hand_absence_and_gap_each_break_continuity(self):
        for interrupt, next_time in ((phone(True, None), 103), (phone(False, False), 103),
                                     (phone(True, False), 103), (None, 106)):
            with self.subTest(interrupt=interrupt, next_time=next_time):
                self.policy.reset()
                self.observe(100)
                self.observe(101)
                if interrupt is not None:
                    self.observe(102, interrupt)
                output = self.observe(next_time)
                self.assertEqual(output['status'], 'detected')
                self.assertEqual(output['sustained_ms'], 0)

    def test_duplicate_out_of_order_and_expired_inputs_cannot_confirm(self):
        for second, seq, delay in ((102, 101, 0.1), (100, 102, 0.1), (103, 103, 3.1)):
            with self.subTest(second=second, seq=seq, delay=delay):
                self.policy.reset()
                self.observe(100)
                self.observe(101)
                with self.assertRaises(ValueError):
                    self.observe(second, seq=seq, delay=delay)
                self.assertEqual(self.observe(104)['sustained_ms'], 0)

    def test_invalid_results_are_errors_and_do_not_become_absence(self):
        for result in ({}, {'phone_detected': 'false', 'hand_overlap': True},
                       {'phone_detected': True, 'hand_overlap': 1},
                       {'phone_detected': True, 'hand_overlap': True, 'error': 'model failed'}):
            with self.assertRaises(ValueError):
                self.observe(100, result)

    def test_phone_with_no_matching_hand_is_detected_but_never_suspected(self):
        for second in range(100, 110):
            output = self.observe(second, phone(overlap=None))
            self.assertEqual((output['status'], output['sustained_ms']), ('detected', 0))


class PhoneCacheTests(unittest.TestCase):
    def test_no_further_result_expires_even_while_camera_keeps_advancing(self):
        cache = PerceptionState()
        cache.commit('phone_use', AlgorithmFrame('input', 0, 1, 100), {'result': phone()}, 100.1)
        cache.update_frame('new-camera-input', 9, 104)
        entry = cache.snapshot({'phone_use': True}, {}, True, 104.1)['algorithms']['phone_use']
        self.assertEqual(entry['state'], 'stale')
        self.assertTrue(entry['result']['phone_detected'])  # Stale is not an absence event.

    def test_error_and_reset_clear_old_confirmation(self):
        cache = PerceptionState()
        for second in range(100, 104):
            cache.commit('phone_use', AlgorithmFrame('input', 0, second, second), {'result': phone()}, second + 0.1)
        cache.failed('phone_use')
        self.assertEqual(cache.snapshot({'phone_use': True}, {}, True, 103.2)['algorithms']['phone_use']['state'], 'error')
        output = cache.commit('phone_use', AlgorithmFrame('input', 0, 104, 104), {'result': phone()}, 104.1)
        self.assertEqual(output['sustained_ms'], 0)
        cache.reset_phone()
        entry = cache.snapshot({'phone_use': True}, {}, True, 104.2)['algorithms']['phone_use']
        self.assertEqual(entry['state'], 'waiting')
        self.assertIsNone(entry['result'])


class PhoneWiringTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        with mock.patch.dict(os.environ, {'ENABLED_ALGORITHMS': 'gesture_mantis,emotion,focus,phone_use'}):
            self.client = main.MainClient()
        self.client.camera_connected.set()

    async def asyncTearDown(self):
        await self.client.algorithm_dispatcher.close()
        self.client._preprocess_executor.shutdown(wait=True)

    async def test_phone_requires_existing_hand_service_and_does_not_enable_vlm(self):
        self.assertTrue(self.client.enabled_algorithms['phone_use'])
        self.assertFalse(self.client.enabled_algorithms['vlm_game_detector'])
        self.assertFalse(self.client.enabled_algorithms['vlm_visual_qa'])
        self.assertTrue(self.client._build_enabled_algorithms([])['phone_use'])
        with mock.patch.dict(os.environ, {'ENABLED_ALGORITHMS': 'phone_use'}):
            with self.assertRaises(ValueError):
                main.MainClient()

    async def test_same_frame_reuses_mirrored_hand_and_one_hz_upper_bound(self):
        asyncio.get_running_loop().set_debug(False)  # This test controls the monotonic sample clock.
        calls = []
        async def algorithm(name, image, timestamp, extra_params=None, **kwargs):
            calls.append((name, image, timestamp, copy.deepcopy(extra_params)))
            return {'success': True, 'result': hand() if name == 'gesture_mantis' else phone()}
        self.client.call_algorithm = algorithm
        with mock.patch.object(main.time, 'monotonic', return_value=100.2):
            await self.client._process_continuous_algorithm('gesture_mantis', AlgorithmFrame('exact-input', 55, 5, 100))
            await idle(self.client.algorithm_dispatcher)
        phone_calls = [call for call in calls if call[0] == 'phone_use']
        self.assertEqual(len(phone_calls), 1)
        _, image, timestamp, payload = phone_calls[0]
        self.assertEqual((image, timestamp, payload['frame_seq'], payload['hand_frame_seq']), ('exact-input', 55, 5, 5))
        self.assertAlmostEqual(payload['hand_landmarks'][0]['x'], 0.8)
        self.assertNotIn('_phone_generation', payload)
        self.assertNotIn('_phone_hand_revision', payload)
        with mock.patch.object(main.time, 'monotonic', return_value=100.9):
            await self.client._process_continuous_algorithm('gesture_mantis', AlgorithmFrame('next', 56, 6, 100.8))
            await idle(self.client.algorithm_dispatcher)
        self.assertEqual(len([call for call in calls if call[0] == 'phone_use']), 1)

    async def test_hand_interruption_skipped_by_one_hz_gate_still_breaks_confirmation(self):
        asyncio.get_running_loop().set_debug(False)
        invalid = hand()
        invalid['hand_landmarks'][0]['x'] = float('nan')
        async def algorithm(*args, **kwargs):
            return {'success': True, 'result': phone()}
        self.client.call_algorithm = algorithm
        for interruption in (hand(False), invalid):
            with self.subTest(interruption=interruption['hand_detected']):
                self.client._invalidate_phone_use()
                for second in range(97, 101):
                    self.client.perception.commit('phone_use', AlgorithmFrame('input', 0, second, second),
                                                  {'result': phone()}, second + 0.1)
                self.client._phone_last_sample_at = 100.0
                self.client._schedule_phone_use(AlgorithmFrame('interrupted', 0, 101, 100.6),
                                               {'result': interruption}, 100.7)
                self.assertEqual(self.client._phone_last_sample_at, 100.0)
                self.assertFalse(any(item['worker'] for item in self.client.algorithm_dispatcher.snapshot().values()))
                with mock.patch.object(main.time, 'monotonic', return_value=101.3):
                    self.client._schedule_phone_use(AlgorithmFrame('next-positive', 0, 102, 101.2),
                                                   {'result': hand()}, 101.3)
                    await idle(self.client.algorithm_dispatcher)
                output = self.client.latest_results['phone_use']['result']
                self.assertEqual((output['status'], output['sustained_ms']), ('detected', 0))

    async def test_inflight_positive_cannot_restore_overlap_after_a_newer_missing_hand(self):
        asyncio.get_running_loop().set_debug(False)
        started, release = asyncio.Event(), asyncio.Event()
        async def algorithm(*args, **kwargs):
            started.set()
            await release.wait()
            return {'success': True, 'result': phone()}
        self.client.call_algorithm = algorithm
        with mock.patch.object(main.time, 'monotonic', return_value=100.2):
            self.client._schedule_phone_use(AlgorithmFrame('inflight', 0, 100, 100), {'result': hand()}, 100.2)
            await started.wait()
            self.client._schedule_phone_use(AlgorithmFrame('no-hand', 0, 101, 100.6),
                                           {'result': hand(False)}, 100.7)
            release.set()
            await idle(self.client.algorithm_dispatcher)
        output = self.client.latest_results['phone_use']['result']
        self.assertTrue(output['phone_detected'])
        self.assertIsNone(output['hand_overlap'])
        self.assertEqual((output['status'], output['sustained_ms']), ('detected', 0))

    async def test_camera_frame_loop_never_starts_an_unpaired_phone_request(self):
        calls = []
        async def store(name, *args, **kwargs):
            calls.append(name)
            return None
        self.client.call_algorithm_and_store = store
        for seq in range(1, 4):
            self.client.last_other_algo_time = 0
            await self.client._schedule_algorithms('image', seq, seq, time.monotonic())
        await idle(self.client.algorithm_dispatcher)
        self.assertNotIn('phone_use', calls)

    async def test_late_phone_and_gesture_results_cannot_cross_camera_generation(self):
        started, release = asyncio.Event(), asyncio.Event()
        async def algorithm(*args, **kwargs):
            started.set()
            await release.wait()
            return {'success': True, 'result': phone()}
        self.client.call_algorithm = algorithm
        before = time.monotonic()
        payload = AlgorithmFrame('old', 1, 1, before, {'_phone_generation': self.client._phone_generation})
        task = asyncio.create_task(self.client._process_phone_use(payload))
        await started.wait()
        self.client._phone_input_epoch_at = before + 0.1
        self.client._invalidate_phone_use()
        release.set()
        self.assertIsNone(await task)
        self.assertNotIn('phone_use', self.client.latest_results)
        self.client._schedule_phone_use(AlgorithmFrame('old-gesture', 1, 1, before), {'result': hand()}, before + 0.2)
        self.assertNotIn('phone_use', self.client.algorithm_dispatcher.snapshot())

    async def test_unhealthy_or_failed_phone_is_error_and_phone_never_enters_head_message(self):
        self.client.algorithm_healthy['phone_use'] = False
        now = time.monotonic()
        frame = AlgorithmFrame('image', 1, 1, now, {'_phone_generation': self.client._phone_generation})
        await self.client._process_phone_use(frame)
        self.assertEqual(self.client.perception.snapshot(self.client.enabled_algorithms, {}, True)['algorithms']['phone_use']['state'], 'error')
        self.client.latest_results = {'phone_use': {'success': True, 'result': {**phone(), 'status': 'suspected'}},
                                      'gesture_mantis': {'result': {'gesture': 'Thumb_Up'}}}
        arguments = self.client._build_algo_arguments()
        self.assertNotIn('phone_use', arguments)
        self.assertNotIn('vlm_game_detector', arguments)
        self.assertEqual(arguments['gesture'], 'Thumb_Up')

    async def test_phone_http_is_json_only_with_existing_timeout(self):
        calls = []
        class Response:
            status = 200
            async def __aenter__(self): return self
            async def __aexit__(self, *args): pass
            async def json(self): return {'success': True, 'result': phone()}
        class Session:
            def post(self, url, **kwargs):
                calls.append((url, kwargs))
                return Response()
        self.client.session = Session()
        await self.client.call_algorithm('phone_use', 'image', 1)
        self.assertTrue(calls[0][0].endswith('/detect'))
        self.assertEqual(calls[0][1]['timeout'].total, main.ALGORITHM_TIMEOUT_DEFAULT)


if __name__ == '__main__':
    unittest.main(verbosity=2)
