"""Local XiaoZhi-compatible MCP probe server: 0 ms, then 3000 ms + concurrent ping.

No cloud calls, no TTS/audio synthesis, no device configuration changes.
An actual controller must be explicitly configured for this fixture separately.
"""
import argparse
import asyncio
import json
import time
import uuid

TOOL = 'self.test.delayed_result'


async def exchange(ws):
    session = 'probe-' + uuid.uuid4().hex[:16]

    async def receive(timeout):
        deadline = time.monotonic() + timeout
        while True:
            raw = await asyncio.wait_for(ws.recv(), max(.001, deadline-time.monotonic()))
            if isinstance(raw, bytes):
                continue  # Ignore uplink audio; this fixture never stores or forwards it.
            message = json.loads(raw)
            if message.get('type') == 'mcp':
                if message.get('session_id') != session:
                    raise AssertionError('MCP response has wrong session_id')
                return message['payload']

    async def send(method, params, request_id=None):
        payload = {'jsonrpc': '2.0', 'method': method, 'params': params}
        if request_id is not None:
            payload['id'] = request_id
        await ws.send(json.dumps({'type':'mcp', 'session_id':session, 'payload':payload}, ensure_ascii=False))

    async def expect(request_id, timeout=3):
        payload = await receive(timeout)
        if type(payload.get('id')) is not type(request_id) or payload.get('id') != request_id:
            raise AssertionError(f'Expected response ID {request_id!r}, got {payload.get("id")!r}')
        if 'error' in payload or payload.get('result', {}).get('isError'):
            raise AssertionError(f'Tool failed: {payload}')
        return payload['result']

    hello = json.loads(await asyncio.wait_for(ws.recv(), 10))
    if hello.get('type') != 'hello':
        raise AssertionError('Expected XiaoZhi hello')
    await ws.send(json.dumps({'type':'hello', 'version':1, 'transport':'websocket',
        'session_id':session, 'audio_params':{'format':'opus', 'sample_rate':16000,
                                             'channels':1, 'frame_duration':60}}))
    await send('initialize', {'protocolVersion':'2024-11-05', 'capabilities':{},
        'clientInfo':{'name':'local-mcp-probe', 'version':'1'}}, 'initialize')
    await expect('initialize')
    await send('notifications/initialized', {})
    names = set()
    cursor = None
    for page in range(20):
        page_id = f'list-{page}'
        await send('tools/list', {'cursor':cursor} if cursor else {}, page_id)
        result = await expect(page_id)
        names.update(tool['name'] for tool in result.get('tools', []))
        cursor = result.get('nextCursor')
        if not cursor:
            break
    if not {TOOL, 'self.test.ping'}.issubset(names):
        raise AssertionError('Probe/ping tool not advertised')

    start = time.monotonic()
    await send('tools/call', {'name':TOOL, 'arguments':{'delay_ms':0, 'text':'基线校验7301'}}, 7300)
    zero = await expect(7300)
    if zero.get('content', [{}])[0].get('text') != '基线校验7301':
        raise AssertionError('Zero-delay calibration text mismatch')
    baseline_ms = round((time.monotonic()-start)*1000)

    start = time.monotonic()
    await send('tools/call', {'name':TOOL, 'arguments':{'delay_ms':3000, 'text':'延迟校验7301'}}, 'slow-7301')
    await send('tools/call', {'name':'self.test.ping', 'arguments':{}}, 'ping-7302')
    await expect('ping-7302', timeout=2)
    ping_ms = round((time.monotonic()-start)*1000)
    delayed = await expect('slow-7301', timeout=5)
    delayed_ms = round((time.monotonic()-start)*1000)
    if delayed_ms < 2950 or delayed.get('content', [{}])[0].get('text') != '延迟校验7301':
        raise AssertionError('Delayed response timing/text mismatch')
    return {'session_id':session, 'baseline_ms':baseline_ms, 'ping_ms':ping_ms,
            'delayed_ms':delayed_ms, 'local_transport_pass':True,
            'real_cloud_acceptance':'not tested', 'tts_and_audibility':'not tested'}


async def serve(host, port):
    import websockets
    completed = asyncio.get_running_loop().create_future()

    async def connected(ws, path=None):
        if completed.done():
            await ws.close()
            return
        try:
            report = await exchange(ws)
            print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
            completed.set_result(None)
        except Exception as error:
            if not completed.done():
                completed.set_exception(error)
    async with websockets.serve(connected, host, port, ping_interval=None):
        print(f'Local fixture ws://{host}:{port}/ ; waiting for a separately configured controller', flush=True)
        await completed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', default=9011, type=int)
    args = parser.parse_args()
    asyncio.run(serve(args.host, args.port))
