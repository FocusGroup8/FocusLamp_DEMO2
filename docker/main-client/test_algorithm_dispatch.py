"""Offline behavioral tests. No board, containers, camera or real HTTP needed.

Third-party imports in main.py are stubbed only while loading that module.
These tests verify scheduling/integration, not inference speed or video FPS.
"""
import asyncio
import importlib.util
import logging
import os
from pathlib import Path
import sys
import time
import types
import unittest
from unittest import mock

from algorithm_dispatch import AlgorithmFrame, LatestAlgorithmDispatcher


def load_main():
    web = types.SimpleNamespace(WebSocketResponse=object, json_response=lambda value: value)
    aiohttp = types.ModuleType('aiohttp')
    aiohttp.web = web
    aiohttp.ClientTimeout = lambda **kwargs: types.SimpleNamespace(**kwargs)
    stubs = {name: types.ModuleType(name) for name in ('cv2', 'numpy', 'websockets')}
    stubs['aiohttp'] = aiohttp
    spec = importlib.util.spec_from_file_location('main_offline_test', Path(__file__).with_name('main.py'))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, stubs):
        spec.loader.exec_module(module)
    return module


main = load_main()


def frame(seq, extra=None):
    return AlgorithmFrame(f'image-{seq}', seq * 1000, seq, time.monotonic(), extra)


async def idle(dispatcher):
    async def wait():
        while any(s['worker'] for s in dispatcher.snapshot().values()):
            await asyncio.sleep(0)
    await asyncio.wait_for(wait(), timeout=3)


class DispatchTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.dispatchers = []

    async def asyncTearDown(self):
        for dispatcher in self.dispatchers:
            await dispatcher.close()

    def dispatcher(self, process):
        value = LatestAlgorithmDispatcher(process)
        self.dispatchers.append(value)
        return value

    async def test_normal_rate_has_no_drops(self):
        seen = []
        async def process(name, payload):
            seen.append(payload.frame_seq)
            return time.monotonic()
        d = self.dispatcher(process)
        for seq in range(10):
            d.submit('gesture', frame(seq))
            await idle(d)
        self.assertEqual(seen, list(range(10)))
        self.assertEqual(d.snapshot()['gesture']['dropped'], 0)
        self.assertEqual(d.snapshot()['gesture']['queued'], 0)

    async def test_slow_request_keeps_only_latest_and_matching_params(self):
        started, release = asyncio.Event(), asyncio.Event()
        seen = []
        async def process(name, payload):
            seen.append(payload)
            if payload.frame_seq == 0:
                started.set()
                await release.wait()
            return time.monotonic()
        d = self.dispatcher(process)
        d.submit('gesture', frame(0))
        await started.wait()
        for seq in range(1, 101):
            extra = {'emotion': str(seq), 'pose': [seq]}
            d.submit('gesture', frame(seq, extra))
            extra['pose'][0] = -1  # Must not mutate the pending snapshot.
        stats = d.snapshot()['gesture']
        self.assertEqual((stats['worker'], stats['inflight'], stats['queued']), (1, 1, 1))
        self.assertEqual(stats['dropped'], 99)
        self.assertEqual(sum(t.get_name() == 'algorithm:gesture' for t in asyncio.all_tasks()), 1)
        release.set()
        await idle(d)
        self.assertEqual([p.frame_seq for p in seen], [0, 100])
        self.assertEqual(seen[-1].extra_params, {'emotion': '100', 'pose': [100]})
        self.assertEqual((seen[-1].image_base64, seen[-1].timestamp), ('image-100', 100000))

    async def test_global_semaphore_wait_does_not_create_more_workers(self):
        sem = asyncio.Semaphore(1)
        await sem.acquire()
        seen = []
        async def process(name, payload):
            async with sem:
                seen.append((name, payload.frame_seq))
                return time.monotonic()
        d = self.dispatcher(process)
        for name in ('gesture', 'focus'):
            d.submit(name, frame(0))
        await asyncio.sleep(0)
        for seq in range(1, 51):
            for name in ('gesture', 'focus'):
                d.submit(name, frame(seq))
        for stats in d.snapshot().values():
            self.assertEqual((stats['inflight'], stats['queued'], stats['dropped']), (1, 1, 49))
        sem.release()
        await idle(d)
        for name in ('gesture', 'focus'):
            self.assertEqual([seq for n, seq in seen if n == name], [0, 50])

    async def test_exception_continues_with_latest_pending(self):
        started, release = asyncio.Event(), asyncio.Event()
        seen = []
        async def process(name, payload):
            seen.append(payload.frame_seq)
            if payload.frame_seq == 0:
                started.set()
                await release.wait()
                raise RuntimeError('controlled failure')
            return time.monotonic()
        d = self.dispatcher(process)
        d.submit('gesture', frame(0))
        await started.wait()
        d.submit('gesture', frame(1))
        release.set()
        with self.assertLogs('main_client', level='ERROR'):
            await idle(d)
        self.assertEqual(seen, [0, 1])
        self.assertEqual(d.snapshot()['gesture']['errors'], 1)

    async def test_cancellation_clears_pending_and_allows_new_submission(self):
        started = asyncio.Event()
        async def process(name, payload):
            if payload.frame_seq == 0:
                started.set()
                await asyncio.Event().wait()
            return time.monotonic()
        d = self.dispatcher(process)
        d.submit('gesture', frame(0))
        await started.wait()
        d.submit('gesture', frame(1))
        task = d._slots['gesture'].worker
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        self.assertEqual(d.snapshot()['gesture']['queued'], 0)
        d.submit('gesture', frame(2))
        await idle(d)
        self.assertEqual(d.snapshot()['gesture']['result_frame_seq'], 2)

    async def test_cancel_before_worker_starts_and_done_callback_identity(self):
        async def process(name, payload):
            return time.monotonic()
        d = self.dispatcher(process)
        d.submit('gesture', frame(0))
        old = d._slots['gesture'].worker
        old.cancel()
        # Await with a separate shield so the old done callback can run.
        await asyncio.gather(old, return_exceptions=True)
        self.assertEqual(d.snapshot()['gesture']['queued'], 0)
        d.submit('gesture', frame(1))
        replacement = d._slots['gesture'].worker
        d._worker_done(d._slots['gesture'], old)
        self.assertIs(d._slots['gesture'].worker, replacement)
        await idle(d)
        self.assertEqual(d.snapshot()['gesture']['result_frame_seq'], 1)

    async def test_result_age_uses_payload_monotonic_time(self):
        async def process(name, payload):
            return payload.received_at + 0.125
        d = self.dispatcher(process)
        d.submit('gesture', AlgorithmFrame('image', -999999999, 7, 100.0))
        await idle(d)
        self.assertAlmostEqual(d.snapshot()['gesture']['result_age_ms'], 125.0)

    async def test_slow_algorithm_does_not_hold_another_algorithms_pending(self):
        started, release, fast_done = asyncio.Event(), asyncio.Event(), asyncio.Event()
        async def process(name, payload):
            if name == 'slow':
                started.set()
                await release.wait()
            else:
                fast_done.set()
            return time.monotonic()
        d = self.dispatcher(process)
        d.submit('slow', frame(0))
        await started.wait()
        d.submit('slow', frame(1))
        d.submit('fast', frame(10))
        await asyncio.wait_for(fast_done.wait(), 3)
        self.assertEqual(d.snapshot()['slow']['inflight'], 1)
        self.assertEqual(d.snapshot()['slow']['queued'], 1)
        release.set()
        await idle(d)
        self.assertEqual(d.snapshot()['fast']['result_frame_seq'], 10)

    async def test_no_result_does_not_stick_worker_or_overwrite_age(self):
        async def process(name, payload):
            return None
        d = self.dispatcher(process)
        for seq in range(2):
            d.submit('gesture', frame(seq))
            await idle(d)
        stats = d.snapshot()['gesture']
        self.assertEqual((stats['worker'], stats['inflight'], stats['queued']), (0, 0, 0))
        self.assertIsNone(stats['result_age_ms'])

    async def test_close_clears_unstarted_workers_and_rejects_new_work(self):
        async def process(name, payload):
            self.fail('Unstarted work should have been cancelled')
        d = self.dispatcher(process)
        d.submit('gesture', frame(0))
        await d.close()
        d.submit('gesture', frame(1))
        stats = d.snapshot()['gesture']
        self.assertEqual((stats['worker'], stats['inflight'], stats['queued']), (0, 0, 0))

    async def test_sustained_overload_is_bounded_and_last_frame_finishes(self):
        ages, task_counts = [], []
        started, permits = asyncio.Queue(), asyncio.Queue()
        async def process(name, payload):
            started.put_nowait(payload.frame_seq)
            await permits.get()  # Controlled slow response, independent of OS timer granularity.
            stored = time.monotonic()
            ages.append((stored - payload.received_at) * 1000)
            return stored
        d = self.dispatcher(process)
        d.submit('gesture', frame(0))
        self.assertEqual(await started.get(), 0)
        for batch in range(12):
            for offset in range(1, 26):
                seq = batch * 25 + offset
                d.submit('gesture', frame(seq))
                stats = d.snapshot()['gesture']
                self.assertLessEqual(stats['inflight'], 1)
                self.assertLessEqual(stats['queued'], 1)
                task_counts.append(sum(t.get_name().startswith('algorithm:') for t in asyncio.all_tasks()))
            permits.put_nowait(None)
            self.assertEqual(await asyncio.wait_for(started.get(), 3), (batch + 1) * 25)
        permits.put_nowait(None)
        await idle(d)
        self.assertEqual(max(task_counts), 1)
        self.assertEqual(d.snapshot()['gesture']['dropped'], 12 * 24)
        self.assertEqual(d.snapshot()['gesture']['result_frame_seq'], 300)
        self.assertLess(max(ages), 1000)  # Loose scheduling tolerance, not a performance target.
        print(f'Overload simulation: input=301 completed={len(ages)} dropped={d.snapshot()["gesture"]["dropped"]} '
              f'peak_workers={max(task_counts)} max_receive_to_result_ms={max(ages):.1f}')


class MainIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        with mock.patch.dict(os.environ, {'ENABLED_ALGORITHMS': 'gesture_mantis'}):
            self.client = main.MainClient()

    async def asyncTearDown(self):
        await self.client.algorithm_dispatcher.close()
        self.client._preprocess_executor.shutdown(wait=True)

    async def test_original_skip_and_dependent_parameters(self):
        c = self.client
        c.skip_frames['face_position'] = 3
        c.enabled_algorithms['face_position'] = True
        c.latest_results = {'emotion': {'result': {'type': 'Happiness'}},
                            'pose': {'result': {'pose_landmarks': [1, 2, 3]}}}
        calls = []
        async def store(name, image, timestamp, extra_params=None):
            calls.append((name, image, timestamp, extra_params))
            return time.monotonic()
        c.call_algorithm_and_store = store
        for seq in range(1, 4):
            c.last_other_algo_time = 0
            await c._schedule_algorithms(f'frame-{seq}', seq, seq, time.monotonic())
        await idle(c.algorithm_dispatcher)
        self.assertEqual(c.skip_frames['gesture_mantis'], 3)
        self.assertEqual(calls, [('gesture_mantis', 'frame-3', 3, {'current_emotion': 'Happiness'}),
                                 ('face_position', 'frame-3', 3, {'pose_landmarks': [1, 2, 3]})])

    async def test_vlm_keeps_original_path_without_real_service(self):
        c = self.client
        c.enabled_algorithms = {'vlm_game_detector': True}
        calls = []
        async def store(*args, **kwargs):
            calls.append(args[0])
        c.call_algorithm_and_store = store
        for seq in range(3):
            c.last_other_algo_time = 0
            await c._schedule_algorithms('image', seq, seq)
        await asyncio.sleep(0)
        self.assertEqual(calls, ['vlm_game_detector'])
        self.assertEqual(c.algorithm_dispatcher.snapshot(), {})

    async def test_actual_http_path_retains_global_semaphore_and_timeout(self):
        c = self.client
        c.algorithm_semaphore = asyncio.Semaphore(1)
        await c.algorithm_semaphore.acquire()
        calls = []
        class Request:
            status = 200
            async def __aenter__(self):
                return self
            async def __aexit__(self, *args):
                pass
            async def json(self):
                return {'success': True, 'result': {'gesture': 'Thumb_Up'}}
        class Session:
            def post(self, url, **kwargs):
                calls.append(kwargs)
                return Request()
        c.session = Session()
        received = time.monotonic() - 0.05
        c.algorithm_dispatcher.submit('gesture_mantis', AlgorithmFrame('image', 99, 9, received))
        await asyncio.sleep(0)
        self.assertEqual(calls, [])
        c.algorithm_semaphore.release()
        await idle(c.algorithm_dispatcher)
        self.assertEqual(calls[0]['timeout'].total, main.ALGORITHM_TIMEOUT_DEFAULT)
        self.assertEqual(calls[0]['json'], {'image': 'image', 'timestamp': 99})
        self.assertEqual(c.latest_results['gesture_mantis']['result']['gesture'], 'Thumb_Up')
        self.assertGreaterEqual(c.algorithm_dispatcher.snapshot()['gesture_mantis']['result_age_ms'], 49.9)
        self.assertTrue(main.ALGORITHM_TIMEOUTS == {'focus': 8.0, 'vlm_visual_qa': 30.0, 'vlm_game_detector': 8.0})

    async def test_receive_timestamp_survives_preprocessing_and_delivery(self):
        c = self.client
        received = time.monotonic() - 0.2
        delivered = asyncio.Event()
        async def preprocess(data):
            await asyncio.sleep(0)
            return data
        async def broadcast(*args):
            delivered.set()
        c._preprocess_frame = preprocess
        c.broadcast_to_frontends = broadcast
        c.enabled_algorithms = {}
        c.pending_delivery_frame = (b'jpeg-test-bytes', 9, 999, received)
        c.delivery_frame_ready.set()
        task = asyncio.create_task(c.deliver_latest_frames())
        try:
            await delivered.wait()
            await asyncio.sleep(0)
            self.assertEqual(c.latest_frame_received_at, received)
            self.assertEqual(c.latest_frame_seq, 9)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def test_results_endpoint_adds_diagnostics_without_changing_results(self):
        self.client.latest_results = {'gesture_mantis': {'success': True}}
        request = types.SimpleNamespace(app={'client': self.client})
        result = await main.handle_results(request)
        self.assertEqual(result['results'], self.client.latest_results)
        self.assertEqual(result['algorithm_diagnostics'], {})


if __name__ == '__main__':
    logging.basicConfig(level=logging.WARNING)
    unittest.main(verbosity=2)
