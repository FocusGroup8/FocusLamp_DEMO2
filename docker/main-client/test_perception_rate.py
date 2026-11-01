"""Gesture input sampling configuration, without changing focus or queue semantics."""
import os
import time
import unittest
from unittest import mock

from test_algorithm_dispatch import main, idle


class GestureRateTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.clients = []

    async def asyncTearDown(self):
        for client in self.clients:
            await client.algorithm_dispatcher.close()
            client._preprocess_executor.shutdown(wait=True)

    def client(self, sampling=None):
        with mock.patch.dict(os.environ, {'ENABLED_ALGORITHMS': 'gesture_mantis,focus'}):
            if sampling is None:
                os.environ.pop('GESTURE_SAMPLE_EVERY', None)
            else:
                os.environ['GESTURE_SAMPLE_EVERY'] = sampling
            client = main.MainClient()
        self.clients.append(client)
        return client

    async def submitted(self, client, count):
        seen = []
        async def store(name, image, timestamp, extra_params=None):
            seen.append((name, image, timestamp))
            return time.monotonic()
        client.call_algorithm_and_store = store
        for seq in range(1, count + 1):
            client.last_other_algo_time = 0
            await client._schedule_algorithms(f'image-{seq}', seq, seq, time.monotonic())
            # Use the same bounded dispatcher; each input is allowed to finish
            # here so this test isolates sampling from overload drops.
            for stats in client.algorithm_dispatcher.snapshot().values():
                self.assertLessEqual(stats['worker'], 1)
                self.assertLessEqual(stats['queued'], 1)
            await idle(client.algorithm_dispatcher)
        return seen

    async def test_unset_sampling_preserves_every_third_gesture_input(self):
        client = self.client()
        seen = await self.submitted(client, 9)
        self.assertEqual(client.skip_frames['gesture_mantis'], 3)
        self.assertEqual([timestamp for name, _, timestamp in seen if name == 'gesture_mantis'], [3, 6, 9])
        self.assertEqual([timestamp for name, _, timestamp in seen if name == 'focus'], list(range(1, 10)))

    async def test_sampling_one_submits_each_input_without_changing_focus(self):
        client = self.client('1')
        seen = await self.submitted(client, 6)
        self.assertEqual(client.skip_frames['gesture_mantis'], 1)
        self.assertEqual(client.skip_frames['focus'], 2)  # Existing scheduler still runs focus on every frame.
        for algorithm in ('gesture_mantis', 'focus'):
            self.assertEqual([(image, timestamp) for name, image, timestamp in seen if name == algorithm],
                             [(f'image-{seq}', seq) for seq in range(1, 7)])
            self.assertEqual(client.algorithm_dispatcher.snapshot()[algorithm]['dropped'], 0)

    async def test_invalid_sampling_is_rejected_before_starting_workers(self):
        for value in ('0', '-1', '1.5', 'true', '', 'NaN', '+1', '１'):
            with self.subTest(value=value), mock.patch.dict(os.environ, {
                    'ENABLED_ALGORITHMS': 'gesture_mantis,focus', 'GESTURE_SAMPLE_EVERY': value}):
                with self.assertRaisesRegex(ValueError, 'GESTURE_SAMPLE_EVERY'):
                    main.MainClient()


if __name__ == '__main__':
    unittest.main(verbosity=2)
