"""Handler tests use no model. Run in the detector image with Flask installed."""
import importlib.util
import os
import unittest
from unittest.mock import patch


class Image:
    shape = (480, 640, 3)


@unittest.skipUnless(importlib.util.find_spec('flask'), 'Flask is installed in the detector image')
class PhoneServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Importing this test must never load weights, Torch or a remote asset.
        with patch.dict(os.environ, {'YOLO_MODEL': '/nonexistent/phone-test-model.pt'}):
            import server
        cls.server = server

    def client(self, predictor=lambda image: []):
        runtime = self.server.PhoneRuntime(predictor=predictor)
        app = self.server.create_app(runtime, decoder=lambda value: Image())
        app.config['TESTING'] = True
        return app.test_client(), runtime

    def test_success_is_data_only(self):
        client, _ = self.client(lambda image: [[200, 150, 400, 350, .8, 67]])
        response = client.post('/detect', json={'image': 'fixture', 'frame_seq': 1,
                               'hand_frame_seq': 1, 'hand_detected': False, 'hand_landmarks': []})
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json['success'])
        self.assertTrue(response.json['result']['phone_detected'])
        self.assertNotIn('visualized_image', response.json)
        self.assertIs(response.json['result']['hand_overlap'], False)

    def test_busy_does_not_queue_inference(self):
        client, runtime = self.client()
        runtime.lock.acquire()
        try:
            self.assertEqual(client.post('/detect', json={'image': 'fixture'}).status_code, 503)
        finally:
            runtime.lock.release()

    def test_inference_error_does_not_report_no_phone_or_keep_lock(self):
        def fail(image):
            raise RuntimeError('fixture failure')
        client, runtime = self.client(fail)
        response = client.post('/detect', json={'image': 'fixture'})
        self.assertEqual(response.status_code, 500)
        self.assertFalse(response.json['success'])
        self.assertNotIn('result', response.json)
        self.assertFalse(runtime.lock.locked())

    def test_invalid_json_is_rejected(self):
        client, _ = self.client()
        self.assertEqual(client.post('/detect', json=[]).status_code, 400)

    def test_missing_model_is_not_a_healthy_empty_detection(self):
        with patch.dict(os.environ, {'YOLO_MODEL': '/nonexistent/phone-test-model.pt'}):
            runtime = self.server.PhoneRuntime()
        app = self.server.create_app(runtime, decoder=lambda value: Image())
        client = app.test_client()
        self.assertEqual(client.get('/health').status_code, 503)
        response = client.post('/detect', json={'image': 'fixture'})
        self.assertEqual(response.status_code, 503)
        self.assertEqual(response.json['state'], 'no_model')

    def test_bad_image_is_not_an_empty_detection(self):
        runtime = self.server.PhoneRuntime(predictor=lambda image: [])
        def bad_image(value):
            raise ValueError('Image decode failed')
        client = self.server.create_app(runtime, decoder=bad_image).test_client()
        self.assertEqual(client.post('/detect', json={'image': 'bad'}).status_code, 400)

    def test_empty_data_url_is_invalid(self):
        with self.assertRaises(ValueError):
            self.server.decode_image('data:image/jpeg;base64,')


if __name__ == '__main__':
    unittest.main()
