import asyncio
import websockets

async def test():
    try:
        async with websockets.connect(
            'ws://host.docker.internal:9007',
            ping_interval=5,
            ping_timeout=10,
            close_timeout=5,
            max_size=10*1024*1024,
            open_timeout=10
        ) as ws:
            print('WS connected!')
            data = await asyncio.wait_for(ws.recv(), timeout=5)
            print(f'Received: {type(data)} len={len(data)}')
    except Exception as e:
        print(f'Error: {e}')

asyncio.run(test())
