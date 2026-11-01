from pathlib import Path
p=Path('tools/focus-ring/docker/algorithms/focus/core/providers/engage_detector/mantis.py');s=p.read_text(encoding='utf8').replace('import time\n','import time\nimport uuid\n');s=s.replace('            self.last_inference_at = None','            self.last_inference_at = None\n            self.source_epoch = uuid.uuid4().hex');s=s.replace("return {'state': state, 'valid_samples': self.valid_samples,", "return {'source_epoch': self.source_epoch, 'inference_seq': self.last_inference_samples,\n                    'state': state, 'valid_samples': self.valid_samples,");s=s.replace('            self.last_face_at = self.last_inference_at = None','            self.last_face_at = self.last_inference_at = None\n            self.source_epoch = uuid.uuid4().hex');p.write_text(s,encoding='utf8')
p=Path('tools/focus-ring/docker/main-client/main.py');s=p.read_text(encoding='utf8').replace('from perception_state import PerceptionState','from perception_state import PerceptionState\nfrom focus_metadata import FocusMetadata');s=s.replace('        self.latest_results = {}','        self.latest_results = {}\n        self.focus_metadata = FocusMetadata()\n        self.focus_sent_source = None');s=s.replace('        if stored_at is not None:\n            with self.frame_lock:', '''        if name == 'focus':
            with self.frame_lock:
                self.focus_metadata.commit(self.latest_results.get(name) if stored_at is not None else None,
                    frame.frame_seq, frame.received_at, generation)
        if stored_at is not None:
            with self.frame_lock:''',1)
a=s.index("        focus_data = self._get_result_data(latest, 'focus')",s.index('    def _build_algo_arguments'));b=s.index('        emotion_data',a)
s=s[:a]+'''        with self.frame_lock:
            focus_payload = self.focus_metadata.payload()
        if self.enabled_algorithms.get('focus') and focus_payload:
            arguments['focus'] = focus_payload

'''+s[b:]
s=s.replace('                    self.algo_ws = ws','                    self.algo_ws = ws\n                    self.focus_sent_source = None')
s=s.replace('                        await self.algo_ws.send(json.dumps(payload))','''                        focus = payload.get('params', {}).get('arguments', {}).get('focus')
                        if focus and focus['source_epoch'] != self.focus_sent_source:
                            await self.algo_ws.send(json.dumps({'jsonrpc':'2.0', 'method':'focus.source',
                                'params':{'source_epoch':focus['source_epoch']}}))
                            self.focus_sent_source = focus['source_epoch']
                        await self.algo_ws.send(json.dumps(payload))''')
p.write_text(s,encoding='utf8')
p=Path('head/main/network/network_manager.c');s=p.read_text(encoding='utf8').replace('!strcmp(focus_method->valuestring,"tools/call")','(!strcmp(focus_method->valuestring,"tools/call") || !strcmp(focus_method->valuestring,"tools.call"))');p.write_text(s,encoding='utf8')
