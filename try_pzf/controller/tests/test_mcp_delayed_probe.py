"""Execute the production C worker with host RTOS hooks, including real loopback WS.

The WS receiver is a host fixture, NOT the ESP SDK receive task or real cloud.
Firmware transport glue is compiled by IDF and must still be checked on board.
"""
import asyncio
import ctypes as C
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / 'components/mcp_delayed_probe'
HOST = ROOT / 'tests/mcp_probe_host'
ALLOC=C.CFUNCTYPE(C.c_void_p,C.c_size_t)
FREE=C.CFUNCTYPE(None,C.c_void_p)
CREATE=C.CFUNCTYPE(C.c_void_p)
TAKE=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32)
GIVE=C.CFUNCTYPE(C.c_int,C.c_void_p)
LOCK=C.CFUNCTYPE(None,C.c_void_p)
SPAWN=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p)
SLEEP=C.CFUNCTYPE(None,C.c_uint32)
NOW=C.CFUNCTYPE(C.c_int64)
SEND=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_char_p,C.c_void_p,C.c_void_p)
GUARD=C.CFUNCTYPE(C.c_bool,C.c_void_p)
ENTRY=C.CFUNCTYPE(None,C.c_void_p)
WORK=C.CFUNCTYPE(C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_void_p,C.c_void_p)

class Ops(C.Structure):
    _fields_=[('alloc',ALLOC),('release',FREE),('create_sem',CREATE),('delete_sem',FREE),
              ('take',TAKE),('give',GIVE),('enter',LOCK),('leave',LOCK),
              ('spawn',SPAWN),('sleep_ms',SLEEP),('now_us',NOW)]

class NativeWorker:
    def __init__(self, lib):
        self.lib=lib;self.memory={};self.sems={};self.locks={};self.workers=[];self.responses=[]
        self.spawn_fail=False;self.alloc_fail=False;self.send_blocked=False;self.wire_callback=None
        self.send_entered=threading.Event();self.send_release=threading.Event();self.sent=threading.Event()
        def alloc(n):
            if self.alloc_fail:self.alloc_fail=False;return None
            buf=C.create_string_buffer(n);address=C.addressof(buf);self.memory[address]=buf;return address
        def create():
            sem=threading.BoundedSemaphore(1);sem.acquire();key=id(sem);self.sems[key]=sem;return key
        def spawn(fn,arg):
            if self.spawn_fail:return 0
            entry=ENTRY(fn)
            thread=threading.Thread(target=lambda:entry(arg),daemon=True)
            self.workers.append(thread);thread.start();return 1
        self.ops=Ops(ALLOC(alloc),FREE(lambda p:self.memory.pop(p)),CREATE(create),FREE(lambda p:self.sems.pop(p)),
            TAKE(lambda p,t:int(self.sems[p].acquire(timeout=None if t==0xffffffff else t/1000))),
            GIVE(lambda p:(self.sems[p].release() or 1)),
            LOCK(lambda p:self.locks.setdefault(p,threading.RLock()).acquire()),
            LOCK(lambda p:self.locks[p].release()),SPAWN(spawn),SLEEP(lambda n:time.sleep(n/1000)),
            NOW(lambda:int(time.monotonic()*1000000)))
        lib.probe_host_set_ops(C.byref(self.ops))
        self.sender=SEND(self._send)
        self.handle=lib.mcp_delayed_probe_create(self.sender,None)
        if not self.handle:raise RuntimeError('create failed')
        lib.mcp_delayed_probe_resume(self.handle)
        lib.mcp_delayed_probe_connection(self.handle,True)
    def _send(self,context,wire,guard,guard_context):
        self.send_entered.set()
        if self.send_blocked and not self.send_release.wait(3):return -1
        # Models the production WebSocket guard after the send/state lock.
        if not GUARD(guard)(guard_context):return 0x103
        if self.wire_callback:self.wire_callback(wire.decode())
        self.responses.append(json.loads(wire));self.sent.set();return 0
    def submit(self,id='request-A',delay=3000,session='same-session',text='校验码7301'):
        wire=json.dumps({'session_id':session,'type':'mcp','payload':{'jsonrpc':'2.0','id':id,
            'result':{'content':[{'type':'text','text':text}],'isError':False}}},ensure_ascii=False).encode()
        return self.lib.mcp_delayed_probe_submit(self.handle,wire,json.dumps(id).encode(),session.encode(),delay)
    def join(self):
        for thread in self.workers:
            thread.join(5)
            if thread.is_alive():raise AssertionError('worker did not exit')
    def close(self):
        if self.handle:self.lib.mcp_delayed_probe_destroy(self.handle);self.handle=None
        self.join()
        if self.memory or self.sems:raise AssertionError('host allocation/semaphore remains')

class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='mcp-probe-c-')
        spec=importlib.util.spec_from_file_location('compiler_helper',ROOT/'tools/test_speak_cycle.py')
        helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
        zig=ROOT/'build/host_toolchain/ziglang/zig.exe'
        compiler=None
        for candidate in helper.compiler_candidates(None):
            targets=subprocess.run([candidate,'--print-targets'],capture_output=True,text=True).stdout
            if 'x86-64' in targets or 'x86_64' in targets:
                compiler=candidate;break
        if not compiler and not zig.is_file():
            raise RuntimeError('Host compiler missing: install ziglang==0.13.0 into build/host_toolchain or provide host clang')
        command=[compiler] if compiler else [str(zig),'cc']
        target='--target=x86_64-pc-windows-msvc' if compiler else '--target=x86_64-windows-gnu'
        environment=os.environ.copy()
        environment['ZIG_GLOBAL_CACHE_DIR']=str(ROOT/'build/host_toolchain/cache')
        directory=Path(cls.temp.name);objects=[]
        for source in (CORE/'mcp_delayed_probe.c',HOST/'platform.c'):
            obj=directory/(source.stem+'.obj');objects.append(obj)
            subprocess.run([*command,target,'-std=c11','-O1','-Wall','-Wextra','-Werror',
                '-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe',
                '-I'+str(HOST/'include'),'-I'+str(CORE/'include'),'-c',str(source),'-o',str(obj)],check=True,env=environment)
        output=directory/'mcp_probe.dll'
        exports=['probe_host_set_ops','mcp_delayed_probe_create','mcp_delayed_probe_resume',
            'mcp_delayed_probe_connection','mcp_delayed_probe_session_changed','mcp_delayed_probe_drain',
            'mcp_delayed_probe_destroy','mcp_delayed_probe_submit','mcp_delayed_probe_submit_work']
        linker=Path(compiler).with_name('ld.lld.exe') if compiler else Path(
            'C:/Espressif/tools/esp-clang/esp-19.1.2_20250312/esp-clang/bin/ld.lld.exe')
        subprocess.run([str(linker),'-flavor','link','/dll','/noentry','/nodefaultlib',
            *['/export:'+s for s in exports],'/out:'+str(output),*map(str,objects)],check=True)
        cls.lib=C.CDLL(str(output));lib=cls.lib
        lib.probe_host_set_ops.argtypes=[C.POINTER(Ops)]
        lib.mcp_delayed_probe_create.argtypes=[SEND,C.c_void_p];lib.mcp_delayed_probe_create.restype=C.c_void_p
        for name in ('resume','session_changed','drain','destroy'):
            fn=getattr(lib,'mcp_delayed_probe_'+name);fn.argtypes=[C.c_void_p];fn.restype=None
        lib.mcp_delayed_probe_connection.argtypes=[C.c_void_p,C.c_bool];lib.mcp_delayed_probe_connection.restype=None
        lib.mcp_delayed_probe_submit.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_uint32]
        lib.mcp_delayed_probe_submit.restype=C.c_int
        lib.mcp_delayed_probe_submit_work.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p,WORK]
        lib.mcp_delayed_probe_submit_work.restype=C.c_int
    @classmethod
    def tearDownClass(cls):
        import _ctypes
        _ctypes.FreeLibrary(cls.lib._handle);cls.temp.cleanup()
    def setUp(self):self.worker=NativeWorker(self.lib)
    def tearDown(self):self.worker.close()
    def test_zero_delay_retains_numeric_id_and_original_session(self):
        self.assertEqual(self.worker.submit(42,0,session='session-original'),0)
        self.assertTrue(self.worker.sent.wait(1));self.worker.join()
        self.assertEqual(self.worker.responses[0]['payload']['id'],42)
        self.assertEqual(self.worker.responses[0]['session_id'],'session-original')
    def test_three_seconds_returns_immediately_but_worker_waits(self):
        start=time.monotonic();self.assertEqual(self.worker.submit(),0)
        self.assertLess(time.monotonic()-start,.5);self.assertFalse(self.worker.sent.wait(.1))
        self.assertTrue(self.worker.sent.wait(4));self.assertGreaterEqual(time.monotonic()-start,2.95)
    def test_reconnect_with_same_session_invalidates_old_job(self):
        self.assertEqual(self.worker.submit(),0)
        self.lib.mcp_delayed_probe_connection(self.worker.handle,False)
        self.lib.mcp_delayed_probe_connection(self.worker.handle,True)
        self.worker.join();self.assertEqual(self.worker.responses,[])
    def test_new_session_invalidates_old_job(self):
        self.assertEqual(self.worker.submit(),0)
        self.lib.mcp_delayed_probe_session_changed(self.worker.handle)
        self.worker.join();self.assertEqual(self.worker.responses,[])
    def test_guard_rechecks_after_waiting_for_send_lock(self):
        self.worker.send_blocked=True;self.assertEqual(self.worker.submit(delay=0),0)
        self.assertTrue(self.worker.send_entered.wait(1))
        self.lib.mcp_delayed_probe_connection(self.worker.handle,False)
        self.lib.mcp_delayed_probe_connection(self.worker.handle,True)
        self.worker.send_release.set();self.worker.join();self.assertEqual(self.worker.responses,[])
    def test_drain_waits_for_inflight_send_without_freeing_context(self):
        self.worker.send_blocked=True;self.assertEqual(self.worker.submit(delay=0),0)
        self.assertTrue(self.worker.send_entered.wait(1));done=threading.Event()
        thread=threading.Thread(target=lambda:(self.lib.mcp_delayed_probe_drain(self.worker.handle),done.set()))
        thread.start();self.assertFalse(done.wait(.1));self.assertTrue(self.worker.memory)
        self.worker.send_release.set();self.assertTrue(done.wait(2));thread.join();self.worker.join()
        self.assertEqual(self.worker.responses,[])
    def test_busy_does_not_queue_and_duplicate_does_not_create_second_worker(self):
        self.assertEqual(self.worker.submit(),0)
        self.assertEqual(self.worker.submit(id='other'),0x103)
        self.assertEqual(self.worker.submit(),0x7301)
        self.assertEqual(len(self.worker.workers),1)
    def test_invalid_delay_allocation_and_task_start_failures_are_recoverable(self):
        self.assertEqual(self.worker.submit(delay=10000),0x102)
        self.worker.alloc_fail=True;self.assertEqual(self.worker.submit(delay=0),0x101)
        self.worker.spawn_fail=True;self.assertEqual(self.worker.submit(delay=0),0x101)
        self.worker.spawn_fail=False;self.assertEqual(self.worker.submit(delay=0),0)
        self.assertTrue(self.worker.sent.wait(1))
    def test_submit_copies_caller_buffers_before_return(self):
        raw=C.create_string_buffer(b'{"session_id":"original","payload":{"id":"owned"}}')
        id=C.create_string_buffer(b'"owned"');session=C.create_string_buffer(b'original')
        self.assertEqual(self.lib.mcp_delayed_probe_submit(self.worker.handle,raw,id,session,3000),0)
        raw.value=b'{}';id.value=b'"new"';session.value=b'changed'
        self.assertTrue(self.worker.sent.wait(4))
        self.assertEqual(self.worker.responses[0]['payload']['id'],'owned')
        self.assertEqual(self.worker.responses[0]['session_id'],'original')
    def test_repeated_zero_delay_releases_job_resources(self):
        baseline=len(self.worker.memory)
        for i in range(12):
            self.worker.sent.clear();self.assertEqual(self.worker.submit(i,0),0)
            self.assertTrue(self.worker.sent.wait(1));self.worker.join()
            self.assertEqual(len(self.worker.memory),baseline)
    def test_background_answer_is_owned_and_not_limited_by_old_speak_queue(self):
        entered=threading.Event();release=threading.Event();observed=[];w=self.worker
        answer='这是摄像头里拿着的一盒牛奶。'*25
        @WORK
        def work(id,session,question,guard,context):
            entered.set();release.wait(2);observed.append(question.decode())
            wire=json.dumps({'session_id':session.decode(),'type':'mcp','payload':{'jsonrpc':'2.0',
                'id':json.loads(id),'result':{'content':[{'type':'text','text':answer}],'isError':False}}},ensure_ascii=False).encode()
            memory=w.ops.alloc(len(wire)+1);C.memmove(memory,wire+b'\0',len(wire)+1);return memory
        question=C.create_string_buffer('这是什么物体？'.encode())
        self.assertEqual(self.lib.mcp_delayed_probe_submit_work(w.handle,b'"vision-1"',b'voice-session',question,work),0)
        self.assertTrue(entered.wait(1));question.value=b'changed'
        self.assertEqual(w.submit(id='other',delay=0),0x103)
        release.set();self.assertTrue(w.sent.wait(2));w.join()
        self.assertEqual(observed,['这是什么物体？'])
        self.assertEqual(w.responses[0]['payload']['result']['content'][0]['text'],answer)
        self.assertEqual(w.responses[0]['session_id'],'voice-session')
    def test_disconnect_during_work_waits_actual_return_and_discards_answer(self):
        entered=threading.Event();release=threading.Event();drained=threading.Event();w=self.worker
        @WORK
        def work(id,session,question,guard,context):
            entered.set();release.wait(2)
            self.assertFalse(GUARD(guard)(context))
            data=b'{"late":true}\0';p=w.ops.alloc(len(data));C.memmove(p,data,len(data));return p
        self.assertEqual(self.lib.mcp_delayed_probe_submit_work(w.handle,b'8',b'old',b'look',work),0)
        self.assertTrue(entered.wait(1))
        self.lib.mcp_delayed_probe_connection(w.handle,False)
        thread=threading.Thread(target=lambda:(self.lib.mcp_delayed_probe_drain(w.handle),drained.set()))
        thread.start();self.assertFalse(drained.wait(.1))
        release.set();self.assertTrue(drained.wait(2));thread.join();w.join()
        self.assertEqual(w.responses,[])
    def test_loopback_websocket_zero_and_three_second_normal_responses(self):
        import websockets
        spec=importlib.util.spec_from_file_location('probe_fixture',ROOT/'tools/mcp_probe_fixture.py')
        fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
        async def scenario():
            loop=asyncio.get_running_loop();finished=loop.create_future();w=self.worker
            async def cloud(ws,path=None):
                try:
                    report=await fixture.exchange(ws)
                    self.assertTrue(report['local_transport_pass'])
                    self.assertLess(report['ping_ms'],2000)
                    self.assertGreaterEqual(report['delayed_ms'],2950)
                    print('loopback fixture:',json.dumps(report),flush=True)
                    finished.set_result(True)
                except Exception as error:
                    if not finished.done():finished.set_exception(error)
            async with websockets.serve(cloud,'127.0.0.1',0,ping_interval=None) as server:
                port=server.sockets[0].getsockname()[1]
                async with websockets.connect(f'ws://127.0.0.1:{port}',proxy=None,ping_interval=None) as device:
                    w.wire_callback=lambda wire:asyncio.run_coroutine_threadsafe(device.send(wire),loop).result(2)
                    await device.send(json.dumps({'type':'hello','transport':'websocket','version':1}))
                    await device.recv()
                    async def receive():
                        async for raw in device:
                            request=json.loads(raw);payload=request['payload'];params=payload['params']
                            method=payload['method']
                            if method=='notifications/initialized':continue
                            result=None
                            if method=='initialize':result={'protocolVersion':'2024-11-05','capabilities':{}}
                            elif method=='tools/list':result={'tools':[{'name':fixture.TOOL},{'name':'self.test.ping'}]}
                            elif params['name']=='self.test.ping':result={'content':[{'type':'text','text':'pong'}]}
                            if result is not None:
                                await device.send(json.dumps({'type':'mcp','session_id':request['session_id'],
                                    'payload':{'jsonrpc':'2.0','id':payload['id'],'result':result}}))
                            else:
                                self.assertEqual(w.submit(payload['id'],params['arguments']['delay_ms'],request['session_id'],params['arguments']['text']),0)
                    receiver=asyncio.create_task(receive())
                    try:await asyncio.wait_for(finished,9)
                    finally:receiver.cancel();await asyncio.gather(receiver,return_exceptions=True);w.wire_callback=None
        asyncio.run(scenario())

if __name__=='__main__':unittest.main(verbosity=2)
