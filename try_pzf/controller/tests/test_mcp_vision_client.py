"""Execute production mcp_vision_client.c + real cJSON with scripted HTTP events.

No Python mirror of the client policy. The HTTP transport and clock are host
fixtures; these tests do not connect to a device, Docker or a cloud API.
"""
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from urllib.parse import urlsplit

ROOT=Path(__file__).resolve().parents[1]
HOST=ROOT/'tests/voice_vision_host'
CORE=ROOT/'components/mcp_delayed_probe'
RID='11111111-2222-3333-4444-555555555555'
GUARD=C.CFUNCTYPE(C.c_bool,C.c_void_p)
HTTP=C.CFUNCTYPE(C.c_int,C.c_char_p,C.c_char_p,C.c_int,C.POINTER(C.c_int),C.c_void_p,C.c_uint)


class VisionClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='vision-client-host-')
        directory=Path(cls.temp.name)
        cls.output=directory/'vision_client.dll'
        zig=ROOT/'build/host_toolchain/ziglang/zig.exe'
        cjson=Path('C:/esp/v5.5.4/esp-idf/components/json/cJSON')
        environment=os.environ.copy()
        environment['ZIG_GLOBAL_CACHE_DIR']=str(ROOT/'build/host_toolchain/cache')
        command=[str(zig),'cc','--target=x86_64-windows-gnu','-shared','-O1','-std=c11',
            '-Wall','-Wextra','-Werror','-Wl,--export-all-symbols',
            '-I'+str(HOST/'include'),'-I'+str(CORE/'include'),'-I'+str(cjson),
            str(CORE/'mcp_vision_client.c'),str(HOST/'platform.c'),str(cjson/'cJSON.c'),'-o',str(cls.output)]
        subprocess.run(command,check=True,env=environment,timeout=120)
        cls.lib=C.CDLL(str(cls.output));lib=cls.lib
        lib.mcp_vision_set_url=lib.host_vision_set_url
        lib.mcp_vision_get_url=lib.host_vision_get_url
        lib.mcp_vision_answer=lib.host_vision_answer
        lib.vision_host_init.argtypes=[HTTP]
        lib.vision_host_free.argtypes=[C.c_void_p]
        lib.vision_host_live_clients.restype=C.c_uint
        lib.vision_host_advance_ms.argtypes=[C.c_uint]
        lib.mcp_vision_set_url.argtypes=[C.c_char_p];lib.mcp_vision_set_url.restype=C.c_int
        lib.mcp_vision_get_url.argtypes=[C.c_void_p,C.c_size_t]
        lib.mcp_vision_answer.argtypes=[C.c_char_p,GUARD,C.c_void_p,C.POINTER(C.c_bool)]
        lib.mcp_vision_answer.restype=C.c_void_p

    @classmethod
    def tearDownClass(cls):
        import _ctypes
        _ctypes.FreeLibrary(cls.lib._handle);cls.temp.cleanup()

    def setUp(self):
        self.calls=[];self.current=True;self.responses=[];self.on_request=None
        self.guard=GUARD(lambda _:self.current)
        self.http=HTTP(self.transport)
        self.lib.vision_host_init(self.http)

    def tearDown(self):
        self.assertEqual(self.lib.vision_host_live_clients(),0,'HTTP handle leaked')

    def transport(self,url,body,timeout,status,out,capacity):
        url=url.decode();body=None if body is None else json.loads(body.decode())
        self.calls.append({'url':url,'body':body,'timeout_ms':timeout})
        if self.on_request:self.on_request(self.calls[-1])
        if not self.responses:
            status[0]=500
            return -1
        response=self.responses.pop(0)
        status[0]=response.get('status',200)
        if response.get('advance_ms'):self.lib.vision_host_advance_ms(response['advance_ms'])
        if response.get('cancel'):self.current=False
        if response.get('timeout'):return -1
        raw=response.get('raw')
        encoded=raw.encode() if raw is not None else json.dumps(response.get('json',{}),ensure_ascii=False).encode()
        if len(encoded)>capacity:return -1
        C.memmove(out,encoded,len(encoded));return len(encoded)

    def answer(self,question='这是什么？',register=True):
        if register:self.assertEqual(self.lib.mcp_vision_set_url(b'http://10.23.38.2:9000'),0)
        failed=C.c_bool()
        pointer=self.lib.mcp_vision_answer(question.encode(),self.guard,None,C.byref(failed))
        self.assertTrue(pointer)
        try:return C.string_at(pointer).decode(),failed.value
        finally:self.lib.vision_host_free(pointer)

    def probes(self,processing=False):
        return [{'json':{'success':True,'current_source':'esp32p4'}},
                {'json':{'success':True,'configured':True,'processing':processing}}]

    def record(self,state='running',**extra):
        return {'request_id':RID,'state':state,'answer':None,'error':None,
                'frame_seq':222,'stream_id':7,'sampled_at':1793088600000,**extra}

    def reply(self,record=None,status=200):
        return {'status':status,'json':{'success':True,'request_id':RID,'result':record or self.record()}}

    def test_00_unregistered_does_not_make_network_request(self):
        text,failed=self.answer(register=False)
        self.assertTrue(failed);self.assertIn('尚未注册',text);self.assertEqual(self.calls,[])

    def test_url_validation_and_normalization(self):
        accepted={'http://10.23.38.2/':'http://10.23.38.2',
                  'http://192.168.1.20:9000':'http://192.168.1.20:9000',
                  'http://172.28.85.9:9011/':'http://172.28.85.9:9011'}
        for url,expected in accepted.items():
            self.assertEqual(self.lib.mcp_vision_set_url(url.encode()),0)
            out=C.create_string_buffer(128);self.lib.mcp_vision_get_url(out,128)
            self.assertEqual(out.value.decode(),expected)
        for url in ['https://10.0.0.2','http://127.0.0.1:9000','http://8.8.8.8',
                    'http://10.0.0.2/vqa','http://10.0.0.2:0','http://10.0.0.2:65536',
                    'http://10.0.0.0','http://172.15.0.2','http://u:p@10.0.0.2','http://10.999.0.1']:
            with self.subTest(url=url):self.assertEqual(self.lib.mcp_vision_set_url(url.encode()),0x102)

    def test_running_done_posts_question_only_and_preserves_metadata(self):
        self.responses=self.probes()+[self.reply(status=202),self.reply(self.record('running')),
            self.reply(self.record('done',answer='这是盒装牛奶。'))]
        text,failed=self.answer();self.assertFalse(failed)
        self.assertEqual(json.loads(text),{'answer':'这是盒装牛奶。','frame_seq':222,'stream_id':7,'sampled_at':1793088600000})
        posts=[c for c in self.calls if c['body'] is not None]
        self.assertEqual(len(posts),1);self.assertEqual(posts[0]['body'],{'question':'这是什么？'})
        self.assertEqual([urlsplit(c['url']).path for c in self.calls],
            ['/input_source','/vqa/health','/vqa/recognize','/vqa/state','/vqa/state'])
        self.assertTrue(all('request_id='+RID in c['url'] for c in self.calls[3:]))

    def test_registered_url_is_snapshotted_for_entire_job(self):
        def change(_):self.lib.mcp_vision_set_url(b'http://192.168.5.20:9999')
        self.on_request=change
        self.responses=self.probes()+[self.reply(status=202),self.reply(self.record('done',answer='原地址结果'))]
        text,failed=self.answer();self.assertFalse(failed)
        self.assertTrue(all(c['url'].startswith('http://10.23.38.2:9000/') for c in self.calls))

    def test_bad_probe_json_and_http_failure_never_upload(self):
        for response in [{'status':200,'json':{'success':True}}, {'status':503,'json':{}}, {'timeout':True}]:
            self.calls=[];self.responses=[response]
            text,failed=self.answer();self.assertTrue(failed);self.assertEqual(len(self.calls),1)

    def test_health_unconfigured_and_busy_do_not_upload(self):
        for health in [{'success':True,'configured':False}, {'success':True,'configured':True,'processing':True}]:
            self.calls=[];self.responses=[self.probes()[0],{'json':health}]
            text,failed=self.answer();self.assertTrue(failed);self.assertEqual(len(self.calls),2)

    def test_missing_camera_and_busy_errors_propagate_without_retry(self):
        for code,error in [('CAMERA_NOT_FRESH','摄像头没有新鲜画面'),('VQA_BUSY','正在回答，请稍后再问')]:
            self.calls=[];self.responses=self.probes()+[{'status':503,'json':{'success':False,'code':code,'error':error}}]
            text,failed=self.answer();self.assertTrue(failed);self.assertEqual(text,error)
            self.assertEqual(sum(c['body'] is not None for c in self.calls),1)

    def test_post_timeout_reports_unknown_and_never_reposts(self):
        self.responses=self.probes()+[{'timeout':True}]
        text,failed=self.answer();self.assertTrue(failed);self.assertIn('未知',text)
        self.assertEqual(sum(c['body'] is not None for c in self.calls),1)

    def test_failed_and_missing_request_do_not_reuse_old_answer(self):
        for response in [self.reply(self.record('failed',error='识图服务失败')),
                         {'status':404,'json':{'success':False,'code':'REQUEST_NOT_FOUND','error':'请求记录不存在'}}]:
            self.calls=[];self.responses=self.probes()+[self.reply(status=202),response]
            text,failed=self.answer();self.assertTrue(failed)
            self.assertIn(text,['识图服务失败','请求记录不存在'])

    def test_wrong_record_id_is_rejected(self):
        wrong=self.record('done',request_id='aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee',answer='不可混入的其他答案')
        self.responses=self.probes()+[self.reply(status=202),self.reply(wrong)]
        text,failed=self.answer();self.assertTrue(failed);self.assertIn('不匹配',text)
        self.assertNotIn('其他答案',text)

    def test_cancellation_during_post_does_not_poll_or_accept_answer(self):
        response=self.reply(self.record('done',answer='过期答案'),202);response['cancel']=True
        self.responses=self.probes()+[response]
        text,failed=self.answer();self.assertTrue(failed)
        self.assertNotIn('过期答案',text);self.assertEqual(len(self.calls),3)

    def test_deadline_running_reports_unknown_and_post_is_once(self):
        running=self.reply(self.record('running'));running['advance_ms']=10000
        self.responses=self.probes()+[self.reply(status=202),running,running,running,running]
        text,failed=self.answer();self.assertTrue(failed);self.assertIn('等待超时',text)
        self.assertEqual(sum(c['body'] is not None for c in self.calls),1)
        self.assertLessEqual(len(self.calls),7)

    def test_malformed_and_oversized_http_payloads_fail_cleanly(self):
        for response in [{'raw':'{not json'}, {'raw':'x'*16000}]:
            self.calls=[];self.responses=self.probes()+[response]
            text,failed=self.answer();self.assertTrue(failed);self.assertIn('未知',text)


if __name__=='__main__':unittest.main(verbosity=2)
