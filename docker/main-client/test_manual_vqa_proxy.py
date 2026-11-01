import ast
import asyncio
import json
from pathlib import Path
import types
import unittest
from unittest import mock

class ProxyTests(unittest.IsolatedAsyncioTestCase):
    def load(self):
        tree=ast.parse(Path(__file__).with_name('main.py').read_text(encoding='utf-8'))
        fn=next(n for n in tree.body if isinstance(n,ast.AsyncFunctionDef) and n.name=='handle_manual_vqa')
        ns={'asyncio':asyncio,'aiohttp':types.SimpleNamespace(ClientTimeout=lambda **k:k,ClientError=RuntimeError),
            'web':types.SimpleNamespace(json_response=lambda data,status=200:(status,data))}
        exec(compile(ast.Module(body=[fn],type_ignores=[]),'main.py','exec'),ns)
        return ns['handle_manual_vqa']
    async def test_forward_exact_snapshot_and_lookup_id_preserving_busy_and_notfound(self):
        calls=[]
        class Response:
            status=503
            async def __aenter__(self):return self
            async def __aexit__(self,*args):pass
            async def json(self):return {'success':False,'error':'busy'}
        response=Response()
        class Session:
            def post(self,url,**kwargs):calls.append(('POST',url,kwargs));return response
            def get(self,url,**kwargs):calls.append(('GET',url,kwargs));return response
        client=types.SimpleNamespace(algorithm_urls={'vlm_visual_qa':'http://qa:9013'},session=Session())
        payload={'question':'问题','image':'exact-image','frame_seq':42,'stream_id':3,'url':'must not forward'}
        request=types.SimpleNamespace(app={'client':client},method='POST',json=mock.AsyncMock(return_value=payload))
        handler=self.load();status,data=await handler(request);self.assertEqual(status,503)
        self.assertEqual(calls[0][1],'http://qa:9013/trigger_async');self.assertNotIn('url',calls[0][2]['json'])
        self.assertEqual(calls[0][2]['json']['image'],'exact-image')
        response.status=404;request.method='GET';request.query={'request_id':'abcd-1234'}
        self.assertEqual((await handler(request))[0],404);self.assertEqual(calls[1][2]['params'],{'request_id':'abcd-1234'})
    async def test_pointing_and_enabled_algorithm_cannot_automatically_upload(self):
        tree=ast.parse(Path(__file__).with_name('main.py').read_text(encoding='utf-8'))
        cls=next(n for n in tree.body if isinstance(n,ast.ClassDef) and n.name=='MainClient')
        fn=next(n for n in cls.body if isinstance(n,ast.FunctionDef) and n.name=='_check_vlm_triggers')
        self.assertIsInstance(fn.body[1],ast.Return)
        source=ast.get_source_segment(Path(__file__).with_name('main.py').read_text(encoding='utf-8'),next(n for n in cls.body if isinstance(n,ast.AsyncFunctionDef) and n.name=='_schedule_algorithms'))
        self.assertIn("'phone_use', 'vlm_visual_qa'",source)

if __name__=='__main__':unittest.main()
