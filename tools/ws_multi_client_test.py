"""
WebSocket multi-client concurrent test script.
Tests ESP32 WebSocket server with multiple simultaneous connections.
"""
import asyncio
import websockets
import time

ESP32_IP = "192.168.4.4"
ESP32_PORT = 80
WS_PATH = "/ws"
NUM_CLIENTS = 4
TEST_MESSAGES = 3

results = {
    "connected": 0,
    "messages_sent": 0,
    "messages_received": 0,
    "errors": 0,
    "broadcast_received": 0,
}

async def ws_client(client_id):
    uri = f"ws://{ESP32_IP}:{ESP32_PORT}{WS_PATH}"
    try:
        async with websockets.connect(uri) as ws:
            results["connected"] += 1
            print(f"[Client {client_id}] Connected")

            # Send test messages
            for i in range(TEST_MESSAGES):
                msg = f"Client {client_id} msg {i+1}"
                try:
                    await ws.send(msg)
                    results["messages_sent"] += 1
                    print(f"[Client {client_id}] Sent: {msg}")
                except Exception as e:
                    results["errors"] += 1
                    print(f"[Client {client_id}] Send error: {e}")
                    return

            # Wait for responses and broadcast messages
            try:
                async with asyncio.timeout(15):
                    while True:
                        response = await ws.recv()
                        if response.startswith("Server push"):
                            results["broadcast_received"] += 1
                        else:
                            results["messages_received"] += 1
                            print(f"[Client {client_id}] Received: {response}")
            except asyncio.TimeoutError:
                pass

            print(f"[Client {client_id}] Test complete")
    except Exception as e:
        results["errors"] += 1
        print(f"[Client {client_id}] Connection error: {e}")

async def main():
    print(f"=== Multi-Client Concurrent Test ===")
    print(f"Target: ws://{ESP32_IP}:{ESP32_PORT}{WS_PATH}")
    print(f"Clients: {NUM_CLIENTS}, Messages per client: {TEST_MESSAGES}")
    print()

    # Connect all clients concurrently
    tasks = [ws_client(i+1) for i in range(NUM_CLIENTS)]
    await asyncio.gather(*tasks)

    print()
    print("=== Test Results ===")
    print(f"Connected clients:    {results['connected']}/{NUM_CLIENTS}")
    print(f"Messages sent:        {results['messages_sent']}")
    print(f"Messages received:    {results['messages_received']}")
    print(f"Broadcast received:   {results['broadcast_received']}")
    print(f"Errors:               {results['errors']}")
    print()
    if results["connected"] == NUM_CLIENTS and results["errors"] == 0:
        print("PASS: All clients connected and communicated successfully")
    else:
        print("FAIL: Some clients failed to connect or communicate")

if __name__ == "__main__":
    asyncio.run(main())
