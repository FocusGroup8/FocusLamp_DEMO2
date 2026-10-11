"""
WebSocket stress test script for ESP32.
Tests throughput, latency, and stability with rapid message sending.
"""
import asyncio
import websockets
import time

ESP32_IP = "192.168.120.4"
ESP32_PORT = 80
WS_PATH = "/ws"

# Test parameters
NUM_MESSAGES = 100       # Total messages per client
MSG_INTERVAL_S = 0.05    # 50ms between messages (20 msg/sec)
MSG_SIZE_SMALL = 32      # Small message size (bytes)
MSG_SIZE_LARGE = 512     # Large message size (bytes)

class TestResults:
    def __init__(self):
        self.sent = 0
        self.received = 0
        self.errors = 0
        self.latencies = []
        self.start_time = None
        self.end_time = None

async def stress_test_small():
    """Test with small messages at high rate"""
    results = TestResults()
    uri = f"ws://{ESP32_IP}:{ESP32_PORT}{WS_PATH}"

    print(f"=== Stress Test: Small Messages ({MSG_SIZE_SMALL}B) ===")
    print(f"Target: {uri}, Messages: {NUM_MESSAGES}, Interval: {MSG_INTERVAL_S*1000:.0f}ms")

    try:
        async with websockets.connect(uri) as ws:
            results.start_time = time.time()

            for i in range(NUM_MESSAGES):
                msg = f"Stress-{i:04d}-{'x' * (MSG_SIZE_SMALL - 16)}"
                send_time = time.time()
                try:
                    await ws.send(msg)
                    results.sent += 1
                except Exception as e:
                    results.errors += 1
                    print(f"  Send error at msg {i}: {e}")
                    continue

                # Try to receive echo
                try:
                    async with asyncio.timeout(2):
                        response = await ws.recv()
                        results.received += 1
                        latency = (time.time() - send_time) * 1000
                        results.latencies.append(latency)
                except asyncio.TimeoutError:
                    results.errors += 1
                    print(f"  Timeout receiving echo for msg {i}")

                await asyncio.sleep(MSG_INTERVAL_S)

            # Wait for remaining echoes
            try:
                async with asyncio.timeout(5):
                    while results.received < results.sent:
                        response = await ws.recv()
                        results.received += 1
            except asyncio.TimeoutError:
                pass

            results.end_time = time.time()

    except Exception as e:
        print(f"Connection error: {e}")
        results.errors += 1

    print_results("Small Messages", results)
    return results

async def stress_test_large():
    """Test with large messages"""
    results = TestResults()
    uri = f"ws://{ESP32_IP}:{ESP32_PORT}{WS_PATH}"

    num_msgs = 20  # Fewer large messages
    print(f"\n=== Stress Test: Large Messages ({MSG_SIZE_LARGE}B) ===")
    print(f"Target: {uri}, Messages: {num_msgs}")

    try:
        async with websockets.connect(uri) as ws:
            results.start_time = time.time()

            for i in range(num_msgs):
                msg = f"Large-{i:04d}-" + "A" * (MSG_SIZE_LARGE - 12)
                send_time = time.time()
                try:
                    await ws.send(msg)
                    results.sent += 1
                except Exception as e:
                    results.errors += 1
                    print(f"  Send error at msg {i}: {e}")
                    continue

                try:
                    async with asyncio.timeout(3):
                        response = await ws.recv()
                        results.received += 1
                        latency = (time.time() - send_time) * 1000
                        results.latencies.append(latency)
                except asyncio.TimeoutError:
                    results.errors += 1

                await asyncio.sleep(0.1)

            results.end_time = time.time()

    except Exception as e:
        print(f"Connection error: {e}")
        results.errors += 1

    print_results("Large Messages", results)
    return results

def print_results(test_name, results):
    duration = results.end_time - results.start_time if results.end_time else 0
    avg_latency = sum(results.latencies) / len(results.latencies) if results.latencies else 0
    max_latency = max(results.latencies) if results.latencies else 0
    min_latency = min(results.latencies) if results.latencies else 0
    throughput = results.sent / duration if duration > 0 else 0

    print(f"\n--- {test_name} Results ---")
    print(f"Duration:         {duration:.2f}s")
    print(f"Sent:             {results.sent}")
    print(f"Received:         {results.received}")
    print(f"Errors:           {results.errors}")
    print(f"Throughput:       {throughput:.1f} msg/s")
    print(f"Latency (avg):    {avg_latency:.1f} ms")
    print(f"Latency (min):    {min_latency:.1f} ms")
    print(f"Latency (max):    {max_latency:.1f} ms")

    if results.sent > 0 and results.received > results.sent * 0.9:
        print("PASS: >90% messages echoed successfully")
    else:
        print("FAIL: Too many message losses or errors")

async def main():
    print("=== ESP32 WebSocket Stress Test ===\n")
    await stress_test_small()
    await stress_test_large()
    print("\n=== All stress tests complete ===")

if __name__ == "__main__":
    asyncio.run(main())
