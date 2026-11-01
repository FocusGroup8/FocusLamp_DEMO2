"""Offline presentation contract checks; no model, board, cloud or HTTP calls."""
import ast
import asyncio
from enum import Enum
import importlib.util
import math
import os
from pathlib import Path
import sys
import threading
import types
import unittest
from unittest import mock

from algorithm_dispatch import AlgorithmFrame
from perception_state import ALGORITHMS, PerceptionState
from test_algorithm_dispatch import main


def hand(x=0.2):
    return {'hand_detected': True, 'gesture': 'Thumb_Up', 'confidence': 0.9,
            'hand_landmarks': [{'x': x, 'y': 0.4, 'z': 0.0} for _ in range(21)]}


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.cache = PerceptionState()
        self.enabled = {name: True for name in ALGORITHMS}

    def snapshot(self, now=101.0, connected=True):
        return self.cache.snapshot(self.enabled, {}, connected, now)

    def test_waiting_disabled_and_no_frame_are_explicit(self):
        self.enabled['emotion'] = False
        snapshot = self.snapshot()
        self.assertIsNone(snapshot['frame'])
        self.assertTrue(snapshot['camera']['stale'])
        self.assertEqual(snapshot['algorithms']['emotion']['state'], 'disabled')
        self.assertEqual(snapshot['algorithms']['focus']['state'], 'waiting')

    def test_latest_image_never_waits_for_hand_pair_and_overlay_maps_mirror_once(self):
        payload = AlgorithmFrame('exact-image-3', 3, 3, 100.0)
        result = hand()
        self.cache.commit('gesture_mantis', payload, {'result': result}, 100.5)
        self.cache.update_frame('newer-image-4', 4, 100.8)
        result['hand_landmarks'][0]['x'] = 0.9  # Caller mutation must not leak.
        snapshot = self.snapshot()
        frame = snapshot['frame']
        self.assertEqual((frame['frame_seq'], frame['image']), (4, 'newer-image-4'))
        self.assertEqual(snapshot['camera']['frame_seq'], 4)
        self.assertFalse(frame['overlay_valid'])
        self.assertIsNone(frame['hand_landmarks'])
        self.assertEqual(snapshot['hand_overlay']['frame_seq'], 3)
        self.assertAlmostEqual(snapshot['hand_overlay']['hand_landmarks'][0]['x'], 0.8)
        self.assertEqual(snapshot['hand_overlay']['age_ms'], 1000)
        self.assertEqual(snapshot['algorithms']['gesture_mantis']['result']['hand_landmarks'][0]['x'], 0.2)
        self.assertEqual(snapshot['algorithms']['gesture_mantis']['age_ms'], 500)
        self.assertEqual(self.snapshot(102)['algorithms']['gesture_mantis']['age_ms'], 1500)

    def test_old_pair_falls_back_to_latest_without_stale_skeleton(self):
        self.cache.commit('gesture_mantis', AlgorithmFrame('old', 1, 1, 90), {'result': hand()}, 90.1)
        self.cache.update_frame('latest', 2, 100.9)
        snapshot = self.snapshot()
        self.assertEqual(snapshot['frame']['image'], 'latest')
        self.assertFalse(snapshot['frame']['overlay_valid'])
        self.assertIsNone(snapshot['frame']['hand_landmarks'])
        self.assertEqual(snapshot['algorithms']['gesture_mantis']['state'], 'stale')
        self.assertIsNone(self.snapshot(101, connected=False)['frame'])

    def test_no_hand_and_invalid_points_never_draw(self):
        self.cache.update_frame('frame', 1, 100)
        for data in ({'hand_detected': False}, hand(float('nan')), hand(1.5)):
            self.cache.commit('gesture_mantis', AlgorithmFrame('same', 1, 1, 100), {'result': data}, 100.1)
            self.assertFalse(self.snapshot()['frame']['overlay_valid'])

    def test_nonfinite_optional_depth_does_not_produce_invalid_json(self):
        points = hand()
        points['hand_landmarks'][0]['z'] = float('nan')
        points['hand_landmarks'][1]['z'] = float('inf')
        points['hand_landmarks'][2]['z'] = True
        self.cache.update_frame('frame', 1, 100)
        self.cache.commit('gesture_mantis', AlgorithmFrame('same', 1, 1, 100), {'result': points}, 100.1)
        normalized = self.snapshot()['frame']['hand_landmarks']
        self.assertEqual([point['z'] for point in normalized[:3]], [0, 0, 0])

    def test_error_no_face_no_model_and_failure_are_not_confident_results(self):
        self.cache.update_frame('frame', 1, 100)
        for state in ('no_face', 'no_model', 'error'):
            self.cache.commit('emotion', AlgorithmFrame('frame', 1, 1, 100), {'result': {'type': state, 'confidence': None}}, 100.1)
            algorithm = self.snapshot()['algorithms']['emotion']
            self.assertEqual(algorithm['state'], state)
            self.assertIsNone(algorithm['result']['confidence'])
        self.cache.failed('focus')
        self.assertEqual(self.snapshot()['algorithms']['focus']['state'], 'error')

    def test_focus_window_age_advances_independently_from_result_age(self):
        self.cache.update_frame('frame', 1, 100)
        result = {'focus_score': math.log(6), 'window': {'state': 'ready', 'completed_age_ms': 30000}}
        self.cache.commit('focus', AlgorithmFrame('frame', 1, 1, 100), {'result': result}, 100)
        first = self.snapshot()['algorithms']['focus']
        self.assertEqual(first['result']['window']['completed_age_ms'], 31000)
        self.assertEqual(self.snapshot()['algorithms']['focus']['result']['window']['completed_age_ms'], 31000)
        self.assertAlmostEqual(first['result']['focus_score'], math.log(6))


class WiringTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        with mock.patch.dict(os.environ, {'ENABLED_ALGORITHMS': 'gesture_mantis,emotion,focus', 'FORWARD_EMOTION_TO_HEAD': 'false'}):
            self.client = main.MainClient()

    async def asyncTearDown(self):
        await self.client.algorithm_dispatcher.close()
        self.client._preprocess_executor.shutdown(wait=True)

    async def test_three_algorithms_request_json_only_without_extra_frames(self):
        urls = []
        class Response:
            status = 200
            async def __aenter__(self): return self
            async def __aexit__(self, *args): pass
            async def json(self): return {'success': True, 'result': {}}
        class Session:
            def post(self, url, **kwargs):
                urls.append(url)
                return Response()
        self.client.session = Session()
        for name in ('gesture_mantis', 'emotion', 'focus'):
            await self.client.call_algorithm(name, 'image', 1)
        self.assertEqual([url.rsplit('/', 1)[1] for url in urls], ['detect'] * 3)

    async def test_emotion_is_not_forwarded_but_original_gesture_is(self):
        self.client.latest_results = {
            'emotion': {'result': {'type': 'happiness', 'confidence': None}},
            'gesture_mantis': {'result': {'gesture': 'Thumb_Up'}}}
        arguments = self.client._build_algo_arguments()
        self.assertNotIn('emotion', arguments)
        self.assertEqual(arguments['gesture'], 'Thumb_Up')
        self.client.forward_emotion_to_head = True
        self.assertEqual(self.client._build_algo_arguments()['emotion'], 'happiness')

    async def test_continuous_commit_captures_matching_frame_and_result(self):
        async def result(*args, **kwargs): return {'success': True, 'result': hand()}
        self.client.call_algorithm = result
        self.client.camera_connected.set()
        self.client.perception.update_frame('new', 9, 100)
        await self.client._process_continuous_algorithm('gesture_mantis', AlgorithmFrame('exact', 8, 8, 100))
        snapshot = self.client.perception.snapshot(self.client.enabled_algorithms, {}, True, 100.1)
        self.assertEqual(snapshot['frame']['image'], 'new')
        self.assertEqual(snapshot['frame']['frame_seq'], 9)
        self.assertEqual(snapshot['hand_overlay']['frame_seq'], 8)
        response = await main.handle_perception(types.SimpleNamespace(app={'client': self.client}))
        self.assertEqual(response['schema_version'], 1)


def load_window_manager():
    class EngageLevel(Enum):
        HighlyEngaged = 0
        Engaged = 1
        BarelyEngaged = 2
        NotEngaged = 3
    class FocusLevel(Enum):
        HighFocused = 3
        MediumFocused = 2
        LowFocused = 1
        Neutral = 0
        LowDistracted = -1
        MediumDistracted = -2
        HighDistracted = -3
    class Threshold(Enum):
        HighFocused = 2.277722765
        MediumFocused = 1.721814093
        LowFocused = 1.175777189
        LowDistracted = 0.818128286
        MediumDistracted = 0.43668397
        HighDistracted = 0.134989059
    class Weight(Enum):
        PrevWindowWeight = 0.4
        CurrentWindowWeight = 0.6
    modules = {'numpy': types.SimpleNamespace(log=math.log),
               'core.utils.logger': types.SimpleNamespace(setup_logging=lambda: mock.Mock()),
               'core.types.engage': types.SimpleNamespace(EngageLevel=EngageLevel),
               'core.types.focus_corr_state': types.SimpleNamespace(FocusLevel=FocusLevel, FocusLevelThreshold=Threshold, FocusScoreWeight=Weight)}
    path = Path(__file__).parents[1] / 'algorithms/focus/core/manager/pomodoro_manager.py'
    spec = importlib.util.spec_from_file_location('window_test', path)
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, modules): spec.loader.exec_module(module)
    return module


class WindowTests(unittest.TestCase):
    def test_json_only_focus_handler_shares_lock_and_always_releases_owned_lock(self):
        path = Path(__file__).parents[1] / 'algorithms/focus/server.py'
        tree = ast.parse(path.read_text(encoding='utf-8'))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'detect')
        function.decorator_list = []
        lock = threading.Lock()
        process = mock.Mock(return_value={'engage_detect_result': {}})
        namespace = {'processing_lock': lock, 'jsonify': lambda value: value,
                     'time': types.SimpleNamespace(time=lambda: 100),
                     'request': types.SimpleNamespace(json={'image': 'test-image'}),
                     'decode_image': lambda value: 'decoded', 'process_video_frame': process}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), 'exec'), namespace)
        lock.acquire()
        self.assertEqual(namespace['detect']()[1], 503)
        self.assertTrue(lock.locked())
        process.assert_not_called()
        lock.release()
        self.assertTrue(namespace['detect']()['success'])
        self.assertFalse(lock.locked())
        namespace['request'].json = {}
        self.assertEqual(namespace['detect']()[1], 400)
        self.assertFalse(lock.locked())
        namespace['request'].json = {'image': 'test-image'}
        process.side_effect = RuntimeError('inference failure')
        self.assertEqual(namespace['detect']()[1], 500)
        self.assertFalse(lock.locked())

    def test_first_sample_starts_window_and_score_formula_is_unchanged(self):
        module = load_window_manager()
        with mock.patch.object(module.time, 'monotonic', return_value=100), mock.patch.object(module.time, 'time', return_value=1000):
            manager = module.PomodoroManager()
            self.assertEqual(manager.window_metadata()['state'], 'waiting')
            self.assertIsNone(manager.last_focus_result)
            manager.update(0)
            self.assertEqual(manager.window_metadata()['state'], 'collecting')
            self.assertEqual(manager.window_metadata()['sample_count'], 1)
            self.assertAlmostEqual(manager.get_focus_score(), math.log(6))
        with mock.patch.object(module.time, 'monotonic', return_value=190), mock.patch.object(module.time, 'time', return_value=1090):
            manager.update(1)
            self.assertAlmostEqual(manager.last_focus_result['focus_score'], round(math.log(9), 4))
            meta = manager.window_metadata()
            self.assertEqual(meta['state'], 'ready')
            self.assertEqual(meta['completed_at_ms'], 1090000)
            self.assertEqual(meta['duration_s'], 90)
            self.assertEqual(meta['sample_count'], 0)

    def test_reset_drops_completed_window_metadata(self):
        module = load_window_manager()
        manager = module.PomodoroManager()
        manager.update(0)
        manager.calculate_focus_level()
        manager.full_reset()
        self.assertEqual(manager.window_metadata()['state'], 'waiting')
        self.assertIsNone(manager.window_metadata()['completed_at_ms'])

    def test_emotion_missing_provider_confidence_is_null(self):
        path = Path(__file__).parents[1] / 'algorithms/emotion/server.py'
        tree = ast.parse(path.read_text(encoding='utf-8'))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'detect_emotion')
        namespace = {'emotion_detector': types.SimpleNamespace(detect=lambda image: {'emotion': 'happiness'}), 'EMOTION_TO_RATING': {'happiness': 90}}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), 'exec'), namespace)
        self.assertIsNone(namespace['detect_emotion'](None)['confidence'])
        namespace['emotion_detector'] = None
        self.assertIsNone(namespace['detect_emotion'](None)['confidence'])


if __name__ == '__main__':
    unittest.main()
