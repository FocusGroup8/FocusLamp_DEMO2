import threading
import unittest
from unittest import mock
from qa_requests import Requests, Busy


class LifecycleTests(unittest.TestCase):
    def test_single_flight_retains_identity_and_no_id_for_rejection(self):
        r=Requests(limit=2);started=threading.Event();release=threading.Event()
        def work():started.set();release.wait(2);return '答案'
        first=r.launch({'frame_seq':7,'stream_id':1},work);self.assertTrue(started.wait(1))
        with self.assertRaises(Busy):r.launch({},lambda:'not called')
        self.assertEqual(len(r.records),1);self.assertEqual(r.get(first)['state'],'running')
        r.finish('wrong-id',answer='wrong');self.assertTrue(r.running())
        release.set()
        for thread in threading.enumerate():
            if thread is not threading.current_thread() and thread.daemon:thread.join(.1)
        self.assertEqual(r.get(first)['answer'],'答案');self.assertEqual(r.get(first)['frame_seq'],7)
        r.launch({},lambda:'2',False);r.launch({},lambda:'3',False)
        self.assertIsNone(r.get(first))

    def test_failures_and_thread_start_failure_release_busy(self):
        r=Requests()
        def fail():raise RuntimeError('secret must not escape')
        request_id=r.launch({},fail,False)
        self.assertEqual(r.get(request_id)['state'],'failed');self.assertNotIn('secret',r.get(request_id)['error'])
        factory=mock.Mock();factory.return_value.start.side_effect=RuntimeError()
        request_id=r.launch({},lambda:'x',thread_factory=factory)
        self.assertEqual(r.get(request_id)['state'],'failed');self.assertFalse(r.running())


class HttpTests(unittest.TestCase):
    def setUp(self):
        import server
        self.s=server;self.registry=Requests();self.patch=mock.patch.object(server,'requests',self.registry);self.patch.start()
        self.engine=mock.patch.object(server,'engine');self.fake=self.engine.start();self.fake.client=object()
        self.fake.query_sync.return_value=('这是杯子',None)
        self.decoder=mock.patch.object(server,'decode_image_b64',return_value='decoded');self.decoder.start()
        self.http=server.app.test_client()
        self.payload={'image':'jpeg','question':'怎么使用？','frame_seq':7,'stream_id':2}
    def tearDown(self):self.patch.stop();self.engine.stop();self.decoder.stop()
    def test_all_three_endpoints_share_busy_and_unknown_id_never_returns_last_answer(self):
        self.registry.begin({})
        for endpoint in ('/detect','/detect_visualize','/trigger_async'):
            response=self.http.post(endpoint,json=self.payload)
            self.assertEqual(response.status_code,503);self.assertNotIn('request_id',response.json)
        self.fake.query_sync.assert_not_called()
        self.assertEqual(self.http.get('/state?request_id=missing').status_code,404)
    def test_custom_question_frame_identity_and_typed_state(self):
        response=self.http.post('/detect',json=self.payload)
        self.assertEqual(response.status_code,200)
        record=response.json['result'];self.assertEqual(record['state'],'done');self.assertEqual(record['answer'],'这是杯子')
        self.assertEqual(record['frame_seq'],7);self.assertEqual(record['stream_id'],2)
        self.fake.query_sync.assert_called_once_with('decoded',None,'怎么使用？')
        state=self.http.get('/state?request_id='+record['request_id']).json
        self.assertEqual(state['result'],record)
    def test_unconfigured_does_not_start_or_decode_and_bad_request_is_rejected(self):
        self.fake.client=None
        self.assertEqual(self.http.post('/trigger_async',json=self.payload).status_code,503)
        self.fake.query_sync.assert_not_called()
        self.fake.client=object()
        self.assertEqual(self.http.post('/trigger_async',json={**self.payload,'question':''}).status_code,400)

if __name__=='__main__':unittest.main()
