"""
WebSocket Echo Server for testing ESP32 WebSocket client.
Receives messages and echoes them back.
"""
import asyncio
import websockets

async def echo(websocket):
    addr = websocket.remote_address
    print(f"[CONNECT] Client connected: {addr}")
    try:
        async for message in websocket:
            print(f"[RECV] {addr}: {message}")
            await websocket.send(f"Echo: {message}")
            print(f"[SEND] {addr}: Echo: {message}")
    except websockets.ConnectionClosed:
        print(f"[DISCONNECT] Client disconnected: {addr}")

async def main():
    print("WebSocket Echo Server starting on 0.0.0.0:8765")
    async with websockets.serve(echo, "0.0.0.0", 8765):
        print("Server ready. Waiting for connections...")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
