import ast,math,unittest,types
from pathlib import Path
p=Path(__file__).parent/'docker/algorithms/focus/core/providers/engage_detector/mantis.py'
tree=ast.parse(p.read_text(encoding='utf8'));f=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='orientation_from_landmarks')
ns={'math':math};exec(compile(ast.Module(body=[f],type_ignores=[]),'orientation','exec'),ns);rule=ns['orientation_from_landmarks']
def points(offset=0,roll=0):
 p=[types.SimpleNamespace(x=.5,y=.5) for _ in range(264)]
 for i,x,y in [(33,-.1,0),(263,.1,0),(1,offset*.2,.08)]:
  p[i]=types.SimpleNamespace(x=.5+x*math.cos(roll)-y*math.sin(roll),y=.5+x*math.sin(roll)+y*math.cos(roll))
 return p
class Test(unittest.TestCase):
 def test_facing(self):
  r=rule(points());self.assertEqual(r['state'],'facing_camera');self.assertFalse(r['controls_timer']);self.assertTrue(r['shadow'])
 def test_mirrored_turns(self):
  self.assertEqual(rule(points(.45))['state'],'turned');self.assertEqual(rule(points(-.45))['state'],'turned')
 def test_roll(self):
  self.assertEqual(rule(points(.45,.7))['state'],'turned')
 def test_uncertain(self):self.assertEqual(rule(points(.27))['state'],'uncertain')
 def test_invalid(self):
  self.assertEqual(rule(None)['state'],'unknown');p=points();p[1].x=float('nan');self.assertEqual(rule(p)['state'],'unknown')
 def test_small(self):
  p=points();p[263]=p[33];self.assertEqual(rule(p)['reason'],'face_too_small')
 def test_shadow_budget_does_not_throttle_original_or_control_timer(self):
  from unittest.mock import Mock
  clock=types.SimpleNamespace(value=0.)
  fake_time=types.SimpleNamespace(monotonic=lambda:clock.value)
  original=next(n for n in tree.body if isinstance(n,ast.ClassDef))
  method=next(n for n in original.body if isinstance(n,ast.FunctionDef) and n.name=='run')
  scope={'np':types.SimpleNamespace(ndarray=object),'time':fake_time,'orientation_from_landmarks':rule,
   'mp':types.SimpleNamespace(Image=lambda **k:object(),ImageFormat=types.SimpleNamespace(SRGB=1))}
  exec(compile(ast.Module(body=[method],type_ignores=[]),'actual_run','exec'),scope)
  import threading
  p=types.SimpleNamespace(face_landmarker=Mock(),orientation_landmarker=Mock(),logger=Mock(),
   orientation_seq=0,orientation_last_attempt=None,features_lock=threading.Lock(),_next_timestamp=lambda:1)
  p.orientation_landmarker.detect.return_value=types.SimpleNamespace(face_landmarks=[points()])
  for value in (0.,.1,.6):clock.value=value;scope['run'](p,object())
  self.assertEqual(p.face_landmarker.detect_async.call_count,3)
  self.assertEqual(p.orientation_landmarker.detect.call_count,2)
  self.assertFalse(p.orientation_shadow['controls_timer'])
  self.assertEqual(p.orientation_shadow['state'],'facing_camera')
if __name__=='__main__':unittest.main()

