"""Single-flight request lifecycle, independent of cloud SDK and image models."""
import copy
from collections import OrderedDict
import threading
import time
import uuid


class Busy(Exception):
    pass


class Requests:
    def __init__(self, limit=32):
        self.lock = threading.Lock()
        self.records = OrderedDict()
        self.active = None
        self.limit = limit

    def begin(self, metadata):
        with self.lock:
            if self.active is not None:
                raise Busy('正在回答，请等待当前请求结束')
            request_id = str(uuid.uuid4())
            self.active = request_id
            self.records[request_id] = dict(metadata, request_id=request_id, state='running',
                                            started_at_ms=int(time.time()*1000),
                                            answer=None, error=None, elapsed_ms=0)
            while len(self.records) > self.limit:
                self.records.popitem(last=False)
            return request_id

    def finish(self, request_id, answer=None, error=None):
        with self.lock:
            if request_id != self.active:
                return
            record = self.records[request_id]
            record.update(state='failed' if error else 'done', answer=None if error else answer,
                          error=error, elapsed_ms=max(0,int(time.time()*1000)-record['started_at_ms']))
            self.active = None

    def get(self, request_id):
        with self.lock:
            record = self.records.get(request_id)
            return copy.deepcopy(record) if record else None

    def running(self):
        with self.lock:
            return self.active is not None

    def launch(self, metadata, work, asynchronous=True, thread_factory=threading.Thread):
        request_id = self.begin(metadata)
        def run():
            try:
                answer = work()
                if not isinstance(answer, str) or not answer.strip():
                    raise ValueError('Empty answer')
                self.finish(request_id, answer=answer.strip())
            except Exception:
                # Do not expose cloud credentials, transport bodies or tracebacks.
                self.finish(request_id, error='云端问答失败或超时，请检查服务配置及网络')
        if asynchronous:
            try:
                thread_factory(target=run, daemon=True).start()
            except Exception:
                self.finish(request_id, error='无法启动问答任务')
        else:
            run()
        return request_id
