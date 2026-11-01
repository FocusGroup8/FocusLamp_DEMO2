import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('focus_metadata', Path(__file__).parent/'docker/main-client/focus_metadata.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class TestMetadata(unittest.TestCase):
    def setUp(self):
        self.now = 10.
        self.m = module.FocusMetadata(lambda: self.now)
        self.result = {'result': {'sampling': {'state':'ready','source_epoch':'A',
            'inference_seq':300,'classification_age_ms':200},
            'engage_detect_result': {'engage_level_name':'Engaged'}}}

    def test_repeat_ages_not_seq(self):
        self.m.commit(self.result,1,9.9,1)
        first=self.m.payload();self.now+=2;second=self.m.payload()
        self.assertEqual(first['sample_seq'],second['sample_seq'])
        self.assertEqual(second['age_ms']-first['age_ms'],2000)

    def test_frame_and_classification_distinct(self):
        self.m.commit(self.result,1,9.9,1);a=self.m.payload()
        self.m.commit(self.result,2,9.9,1);b=self.m.payload()
        self.assertGreater(b['sample_seq'],a['sample_seq'])
        self.assertEqual(a['inference_seq'],b['inference_seq'])

    def test_invalid_not_red(self):
        self.result['result']['sampling']['state']='no_face'
        self.m.commit(self.result,1,9.9,1)
        self.assertFalse(self.m.payload()['valid'])
        self.assertEqual(self.m.payload()['engage_level_name'],'Unknown')

    def test_failure_and_epoch(self):
        self.m.commit(self.result,1,9.9,1);a=self.m.payload()
        self.m.commit(None,2,9.9,1);failed=self.m.payload()
        self.assertFalse(failed['valid'])
        self.assertEqual(a['source_epoch'],failed['source_epoch'])
        self.assertEqual(a['inference_seq'],failed['inference_seq'])
        self.assertEqual(failed['engage_level_name'],'Unknown')
        self.m.commit(self.result,3,9.9,1)
        self.assertEqual(a['source_epoch'],self.m.payload()['source_epoch'])
        self.assertTrue(self.m.payload()['valid'])
        self.m.commit(self.result,4,9.9,2)
        self.assertNotEqual(a['source_epoch'],self.m.payload()['source_epoch'])

    def test_real_provider_restart_rotates_epoch(self):
        self.m.commit(self.result,1,9.9,1);a=self.m.payload()
        self.result['result']['sampling']['source_epoch']='B'
        self.result['result']['sampling']['inference_seq']=1
        self.m.commit(self.result,2,9.9,1);b=self.m.payload()
        self.assertNotEqual(a['source_epoch'],b['source_epoch'])
        self.assertEqual(b['inference_seq'],1)
        self.m.commit(None,3,9.9,1)
        self.assertEqual(b['source_epoch'],self.m.payload()['source_epoch'])
        self.assertFalse(self.m.payload()['valid'])

    def test_failure_after_camera_generation_change_does_not_reuse_old_source(self):
        self.m.commit(self.result,1,9.9,1);a=self.m.payload()
        self.m.commit(None,2,9.9,2);failed=self.m.payload()
        self.assertNotEqual(a['source_epoch'],failed['source_epoch'])
        self.assertEqual(failed['inference_seq'],0)
        self.assertFalse(failed['valid'])
        self.m.commit(self.result,3,9.9,2)
        self.assertNotEqual(a['source_epoch'],self.m.payload()['source_epoch'])
        self.assertNotEqual(failed['source_epoch'],self.m.payload()['source_epoch'])

    def test_missing_epoch_never_becomes_valid_by_reusing_known_source(self):
        self.m.commit(self.result,1,9.9,1);a=self.m.payload()
        del self.result['result']['sampling']['source_epoch']
        self.m.commit(self.result,2,9.9,1);b=self.m.payload()
        self.assertEqual(a['source_epoch'],b['source_epoch'])
        self.assertEqual(a['inference_seq'],b['inference_seq'])
        self.assertFalse(b['valid'])

    def test_unsupported_old_payload(self):
        self.m.commit({'result':{'engage_level_name':'Engaged'}},1,9.9,1)
        self.assertFalse(self.m.payload()['valid'])

    def test_duplicate_input_frame_cannot_renew(self):
        self.m.commit(self.result, 12, 9.9, 1)
        before=self.m.payload()
        self.now += 2
        self.m.commit(self.result, 12, self.now, 1)
        after=self.m.payload()
        self.assertEqual(before['sample_seq'],after['sample_seq'])
        self.assertEqual(after['sample_age_ms']-before['sample_age_ms'],2000)

    def test_actual_provider_label_spellings(self):
        for raw, expected in [('Highly-Engaged','HighlyEngaged'), ('Engaged','Engaged'),
                              ('Barely-engaged','BarelyEngaged'), ('Not-Engaged','NotEngaged')]:
            with self.subTest(raw=raw):
                self.result['result']['engage_detect_result']['engage_level_name']=raw
                self.m.commit(self.result,self.m.seq+1,9.9,1)
                value=self.m.payload()
                self.assertTrue(value['valid'])
                self.assertEqual(value['engage_level_name'],expected)

    def test_clock_echo_does_not_double_count_sender_wait(self):
        # Receiver clock at send=1000, round trip=200ms, sender holds 1s.
        hold=module.clock_residence_ms(10.,11.)
        self.assertEqual(hold,1000)
        self.assertEqual((2200-1000)-hold,200)
        self.assertEqual(module.clock_residence_ms(11.,10.),0)
        self.assertEqual(module.clock_residence_ms(0.,1e10),0xffffffff)

if __name__ == '__main__': unittest.main()
