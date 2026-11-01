"""Exercise the real main-client integration with HTTP/model imports stubbed."""
import asyncio
from pathlib import Path
import sys
import time
import unittest
sys.path.insert(0,'D:/Docker_demo2/docker/main-client')
from test_algorithm_dispatch import main
from algorithm_dispatch import AlgorithmFrame

class Integration(unittest.IsolatedAsyncioTestCase):
    async def test_focus_transport_reads_nested_engagement_and_fails_closed(self):
        client=main.MainClient()
        try:
            response={'success':True,'result':{'sampling':{'state':'ready','source_epoch':'provider-A',
                'inference_seq':300,'classification_age_ms':0},
                'engage_detect_result':{'engage_level_name':'Engaged'}}}
            async def call(*args,**kwargs): return response
            client.call_algorithm=call
            frame=AlgorithmFrame('fake-image',1,1,time.monotonic())
            await client._process_continuous_algorithm('focus',frame)
            value=client._build_algo_arguments()['focus']
            self.assertTrue(value['valid']);self.assertEqual(value['engage_level_name'],'Engaged')
            seq=value['sample_seq']
            value2=client._build_algo_arguments()['focus'];self.assertEqual(value2['sample_seq'],seq)
            async def fail(*args,**kwargs): return None
            client.call_algorithm=fail
            frame=AlgorithmFrame('fake-image',2,2,time.monotonic())
            await client._process_continuous_algorithm('focus',frame)
            self.assertFalse(client._build_algo_arguments()['focus']['valid'])
        finally:
            await client.algorithm_dispatcher.close()
            client._preprocess_executor.shutdown(wait=True)

if __name__=='__main__':unittest.main()
