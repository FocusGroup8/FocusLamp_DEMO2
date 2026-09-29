import asyncio
import websockets

async def test():
    try:
        async with websockets.connect('ws://127.0.0.1:9007', open_timeout=5) as ws:
            print('Connected!')
            data = await asyncio.wait_for(ws.recv(), timeout=3)
            print(f'Got {type(data)} len={len(data)}')
    except Exception as e:
        print(f'Error: {e}')

asyncio.run(test())
