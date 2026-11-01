"""Offline stream lifecycle/backpressure checks, without camera or inference."""
import asyncio
import json
import time
import types
import unittest
from unittest import mock

from frontend_stream import LatestFrontend
from perception_state import PerceptionState, ALGORITHMS
from algorithm_dispatch import AlgorithmFrame
from test_algorithm_dispatch import main


async def until(predicate):
    async def wait():
        while not predicate():
            await asyncio.sleep(0)
    await asyncio.wait_for(wait(), 2)


class Socket:
    def __init__(self):
        self.sent = []
        self.started = asyncio.Event()
        self.release = asyncio.Event()
        self.block = False
        self.closed = False

    async def send_str(self, text):
        self.sent.append(json.loads(text))
        await asyncio.sleep(0)

    async def send_bytes(self, data):
        self.started.set()
        if self.block:
            await self.release.wait()
        self.sent.append(data)

    async def close(self):
        self.closed = True


class StreamTests(unittest.IsolatedAsyncioTestCase):
    async def test_slow_writer_keeps_one_latest_frame_without_blocking_producer(self):
        socket = Socket()
        socket.block = True
        sender = LatestFrontend(socket, lambda: None, send_timeout=1)
        try:
            sender.offer_frame({'type': 'frame_header', 'seq': 0}, b'0', time.monotonic())
            await socket.started.wait()
            for seq in range(1, 101):
                sender.offer_frame({'type': 'frame_header', 'seq': seq}, str(seq).encode(), time.monotonic())
            self.assertEqual(sender.frame[0]['seq'], 100)
            self.assertEqual(len(socket.sent), 1)
            socket.release.set()
            await until(lambda: b'100' in socket.sent)
            self.assertEqual([v['seq'] for v in socket.sent if isinstance(v, dict)], [0, 100])
            self.assertEqual([v for v in socket.sent if isinstance(v, bytes)], [b'0', b'100'])
        finally:
            await sender.close()
        self.assertTrue(sender.task.done())

    async def test_header_and_binary_stay_adjacent_during_overlay_and_control(self):
        socket = Socket()
        sender = LatestFrontend(socket, lambda: None)
        try:
            sender.offer_frame({'type': 'frame_header', 'seq': 4}, b'four', time.monotonic() - .1)
            sender.offer_overlay({'type': 'hand_overlay', 'frame_seq': 3, 'age_ms': 200})
            sender.offer_reply({'type': 'pong'})
            await until(lambda: any(isinstance(v, dict) and v.get('type') == 'hand_overlay' for v in socket.sent))
            index = next(i for i, v in enumerate(socket.sent) if isinstance(v, dict) and v.get('type') == 'frame_header')
            self.assertEqual(socket.sent[index + 1], b'four')
            self.assertGreaterEqual(socket.sent[index]['age_ms'], 100)
            overlay = next(v for v in socket.sent if isinstance(v, dict) and v.get('type') == 'hand_overlay')
            self.assertEqual(overlay['frame_seq'], 3)
        finally:
            await sender.close()

    async def test_stalled_client_times_out_and_cannot_resume_partial_frame(self):
        socket = Socket()
        socket.block = True
        dead = []
        sender = LatestFrontend(socket, lambda: dead.append(True), send_timeout=.01)
        sender.offer_frame({'type': 'frame_header', 'seq': 1}, b'first', time.monotonic())
        await until(lambda: socket.closed)
        sender.offer_frame({'type': 'frame_header', 'seq': 2}, b'second', time.monotonic())
        self.assertTrue(sender.closed)
        self.assertEqual(dead, [True])
        self.assertIsNone(sender.frame)
        self.assertNotIn(b'second', socket.sent)
        await sender.close()

    async def test_disconnect_drops_pending_old_frames_before_reused_sequence(self):
        socket = Socket()
        socket.block = True
        sender = LatestFrontend(socket, lambda: None, send_timeout=1)
        try:
            sender.offer_frame({'type': 'frame_header', 'seq': 1, 'stream_id': 0}, b'old-active', time.monotonic())
            await socket.started.wait()
            sender.offer_frame({'type': 'frame_header', 'seq': 2, 'stream_id': 0}, b'old-pending', time.monotonic())
            sender.camera_status({'type': 'camera_status', 'stream_id': 1, 'connected': False, 'stale': True})
            sender.offer_frame({'type': 'frame_header', 'seq': 1, 'stream_id': 1}, b'new', time.monotonic())
            socket.release.set()
            await until(lambda: b'new' in socket.sent)
            self.assertNotIn(b'old-pending', socket.sent)
            reset = next(i for i, v in enumerate(socket.sent) if isinstance(v, dict) and v.get('type') == 'camera_status')
            fresh = next(i for i, v in enumerate(socket.sent) if isinstance(v, dict) and v.get('stream_id') == 1 and v.get('type') == 'frame_header')
            self.assertLess(reset, fresh)
        finally:
            await sender.close()


class MetadataTests(unittest.TestCase):
    def test_metadata_only_omits_jpeg_and_retains_analyzed_frame_identity(self):
        state = PerceptionState()
        state.update_frame('latest', 9, 100.8)
        points = {'hand_detected': True, 'hand_landmarks': [{'x': .2, 'y': .3} for _ in range(21)]}
        state.commit('gesture_mantis', AlgorithmFrame('older', 8, 8, 100), {'result': points}, 100.5)
        value = state.snapshot({name: True for name in ALGORITHMS}, {}, True, 101, include_image=False)
        self.assertNotIn('image', value['frame'])
        self.assertEqual(value['frame']['frame_seq'], 9)
        self.assertEqual(value['hand_overlay']['frame_seq'], 8)
        self.assertAlmostEqual(value['hand_overlay']['hand_landmarks'][0]['x'], .8)
        state.reset_video(1)
        reset = state.hand_overlay({'gesture_mantis': True}, {}, True, 101)
        self.assertEqual(reset['stream_id'], 1)
        self.assertFalse(reset['overlay_valid'])
        self.assertIsNone(reset['frame_seq'])


class IntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        with mock.patch.dict('os.environ', {'ENABLED_ALGORITHMS': 'gesture_mantis,emotion,focus'}):
            self.client = main.MainClient()

    async def asyncTearDown(self):
        for ws in tuple(self.client.frontend_senders):
            await self.client.unregister_frontend(ws)
        await self.client.algorithm_dispatcher.close()
        self.client._preprocess_executor.shutdown(wait=True)

    async def test_frame_broadcast_returns_while_client_write_is_stalled(self):
        socket = Socket()
        socket.block = True
        self.client.register_frontend(socket)
        await asyncio.wait_for(self.client.broadcast_to_frontends(b'jpeg', 2, 1, time.monotonic()), .05)
        await socket.started.wait()
        self.assertIn(socket, self.client.frontend_clients)

    async def test_gesture_completion_pushes_its_input_identity_without_waiting_for_next_frame(self):
        socket = Socket()
        self.client.camera_connected.set()
        self.client.register_frontend(socket)
        self.client.call_algorithm = mock.AsyncMock(return_value={'result': {'hand_detected': False}})
        await self.client._process_continuous_algorithm('gesture_mantis', AlgorithmFrame('input', 7, 7, time.monotonic()))
        await until(lambda: any(isinstance(v, dict) and v.get('type') == 'hand_overlay' and v.get('frame_seq') == 7 for v in socket.sent))
        self.assertFalse(next(v for v in socket.sent if isinstance(v, dict) and v.get('frame_seq') == 7)['overlay_valid'])

    async def test_late_old_connection_gesture_does_not_restore_overlay(self):
        started, release = asyncio.Event(), asyncio.Event()
        async def detect(*args, **kwargs):
            started.set()
            await release.wait()
            return {'result': {'hand_detected': False}}
        self.client.call_algorithm = detect
        task = asyncio.create_task(self.client._process_continuous_algorithm('gesture_mantis', AlgorithmFrame('old', 1, 1, time.monotonic())))
        await started.wait()
        self.client._camera_generation += 1
        self.client.perception.reset_video(1)
        release.set()
        await task
        self.assertIsNone(self.client.perception.hand_overlay({'gesture_mantis': True}, {}, True)['frame_seq'])

    async def test_metadata_http_option_is_forwarded_without_image(self):
        self.client.camera_connected.set()
        self.client.perception.update_frame('jpeg', 4, time.monotonic())
        response = await main.handle_perception(types.SimpleNamespace(app={'client': self.client}, query={'include_image': 'false'}))
        self.assertNotIn('image', response['frame'])


if __name__ == '__main__':
    unittest.main()
