import ast
import __future__
import threading
import types
import unittest
from pathlib import Path
from unittest import mock


def load_provider():
    path = Path(__file__).with_name('mantis.py')
    if not path.exists():
        path = Path(__file__).parent / 'core/providers/engage_detector/mantis.py'
    source = ast.parse(path.read_text(encoding='utf-8'))
    original = next(node for node in source.body if isinstance(node, ast.ClassDef))
    methods = [node for node in original.body if isinstance(node, ast.FunctionDef) and node.name in ('get_detect_result', 'sampling_metadata', 'reset')]
    cls = ast.ClassDef(name='Provider', bases=[], keywords=[], body=methods, decorator_list=[])
    class Tensor:
        def reshape(self, *args): return self
        def transpose(self, *args): return self
        def numpy(self): return self
    class Scalar(int):
        def item(self): return int(self)
    ns = {'time': types.SimpleNamespace(monotonic=lambda:100),
          'np':types.SimpleNamespace(array=lambda *a,**k:Tensor(),float32=0,argmax=lambda value:Scalar(1)),
          'torch':types.SimpleNamespace(tensor=lambda value,**k:value,float32=0),
          'F':types.SimpleNamespace(interpolate=lambda value,**k:value)}
    tree=ast.fix_missing_locations(ast.Module(body=[cls],type_ignores=[]))
    exec(compile(tree,'mantis.py','exec',flags=__future__.annotations.compiler_flag),ns)
    provider=ns['Provider']();provider.features_lock=threading.Lock();provider.reset()
    provider.label_map={1:'Engaged'}
    def infer(*args):
        assert provider.features_lock.acquire(blocking=False), 'Inference blocks callback lock'
        provider.features_lock.release()
        return [[1]]
    provider.engage_detector=types.SimpleNamespace(run=mock.Mock(side_effect=infer))
    return provider,ns


class FocusTests(unittest.TestCase):
    def test_preview_stride_does_not_change_score_cadence_or_recount_cached_results(self):
        p,ns=load_provider()
        p.features_queue=[[0]*52]*299;p.valid_samples=299
        self.assertIsNone(p.get_detect_result())
        p.features_queue.append([0]*52);p.valid_samples=300
        self.assertTrue(p.get_detect_result()['_score_sample_due'])
        self.assertIsNone(p.get_detect_result())
        p.valid_samples=329;self.assertIsNone(p.get_detect_result())
        p.valid_samples=330;self.assertFalse(p.get_detect_result()['_score_sample_due'])
        p.valid_samples=450;self.assertTrue(p.get_detect_result()['_score_sample_due'])
        self.assertEqual(p.next_score_sample,600)
        self.assertEqual(p.engage_detector.run.call_count,3)

    def test_face_absence_and_real_inference_age_are_independent_from_http_refresh(self):
        p,ns=load_provider();self.assertEqual(p.sampling_metadata()['state'],'no_face')
        p.last_face_at=99;p.valid_samples=10;p.features_queue=[[0]*52]*10
        self.assertEqual(p.sampling_metadata()['state'],'warming_up')
        self.assertEqual(p.sampling_metadata()['next_inference_samples'],290)
        p.last_inference_at=80;p.last_inference_result={'engage_level':1}
        self.assertEqual(p.sampling_metadata()['classification_age_ms'],20000)
        ns['time'].monotonic=lambda:103
        self.assertEqual(p.sampling_metadata()['state'],'no_face')
        p.reset();self.assertEqual(p.valid_samples,0);self.assertIsNone(p.last_inference_at)

    def test_server_only_feeds_due_predictions_to_original_score_window(self):
        tree=ast.parse(Path(__file__).with_name('server.py').read_text(encoding='utf-8'))
        function=next(node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name=='process_video_frame')
        detector=mock.Mock();detector.last_inference_result=None;detector.sampling_metadata.return_value={}
        detector.get_detect_result.side_effect=[{'engage_level':1,'_score_sample_due':True},{'engage_level':2,'_score_sample_due':False},None]
        manager=mock.Mock();manager.last_focus_result=None;manager.window_metadata.return_value={}
        ns={'cv2':types.SimpleNamespace(flip=lambda f,n:f,cvtColor=lambda f,c:f,COLOR_BGR2RGB=1),
            'engage_detector':detector,'pomodoro_manager':manager,'eye_detector':None,'face_detector':None,'gesture_detector':None}
        exec(compile(ast.Module(body=[function],type_ignores=[]),'server.py','exec'),ns)
        for _ in range(3):ns['process_video_frame']('image')
        manager.update.assert_called_once_with(engage_level=1)

if __name__=='__main__':unittest.main()
