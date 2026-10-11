"""
TTS Inject Test Script - Verify if Xiaozhi server supports proactive TTS playback

This script tests two approaches for the "久坐检测 → TTS播报" scenario:
  Approach A: Send wake word + listen message with text → server ASR → LLM → TTS
  Approach B: Send MCP notification → server processes → TTS playback

The script first performs OTA to get WebSocket URL and token, then connects
to the Xiaozhi server and attempts to trigger TTS playback.

Protocol reference: https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md
"""

import asyncio
import json
import sys
import ssl
import struct
import time

try:
    import websockets
except ImportError:
    print("ERROR: 'websockets' package not installed. Run: pip install websockets")
    sys.exit(1)

try:
    import httpx
except ImportError:
    print("WARNING: 'httpx' not installed, OTA step will be skipped. Run: pip install httpx")
    httpx = None

# ===== Configuration =====
# Device credentials from serial log (feedback.txt)
DEVICE_MAC = "e8:f6:0a:e0:3c:f1"  # Device-Id from P4 MAC
DEVICE_UUID = ""                   # Client-Id (UUID) - empty in NVS currently
DEVICE_TOKEN = "test-token"        # Token from NVS (placeholder)

# MCP endpoint token (from previous session)
MCP_TOKEN = "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjkzMzYzNiwiYWdlbnRJZCI6MjEyNjk3MCwiZW5kcG9pbnRJZCI6ImFnZW50XzIxMjY5NzAiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzg0ODc1ODU1LCJleHAiOjE4MTY0MzM0NTV9.PezUodmu9VZBYsBVx_jhO5IVEc-OeNJSvnz2nH8SiBPMrD4Ny0nSTWGcOTjCC2Nu5QN0oqOgZInoBCEOrbCH6g"

# OTA URL for getting WebSocket URL and token
OTA_URL = "https://api.tenclass.net/xiaozhi/ota/"

# Default WebSocket URL (fallback if OTA fails)
DEFAULT_WS_URL = "wss://api.tenclass.net/xiaozhi/v1/"

# Preset text for TTS playback test
TTS_TEST_TEXT = "您已经久坐了，请站起来活动活动"

# ===== OTA: Get WebSocket URL and Token =====
async def get_ws_url_and_token():
    """Perform OTA request to get WebSocket URL and authentication token"""
    if httpx is None:
        print("    httpx not available, using default URL")
        return DEFAULT_WS_URL, MCP_TOKEN

    print(f"    Requesting OTA from: {OTA_URL}")

    # Device board info for OTA request
    board_info = {
        "board": "esp32p4",
        "version": "1.0.0",
        "features": {"mcp": True}
    }

    try:
        async with httpx.AsyncClient(verify=False, timeout=15) as client:
            ota_headers = {
                "Content-Type": "application/json",
                "Device-Id": DEVICE_MAC,     # Use real device MAC from serial log
                "Client-Id": DEVICE_UUID,     # Use device UUID (may be empty)
                "User-Agent": "esp32p4",
            }
            # Remove empty Client-Id to avoid "Invalid client ID" error
            if not ota_headers["Client-Id"]:
                del ota_headers["Client-Id"]
            print(f"    OTA headers: Device-Id={ota_headers.get('Device-Id')}, Client-Id={ota_headers.get('Client-Id', '<empty>')}")

            resp = await client.post(
                OTA_URL,
                json=board_info,
                headers=ota_headers,
            )

            if resp.status_code != 200:
                print(f"    OTA failed: HTTP {resp.status_code}")
                print(f"    Response: {resp.text[:500]}")
                return DEFAULT_WS_URL, MCP_TOKEN

            data = resp.json()
            print(f"    OTA response keys: {list(data.keys())}")

            ws_url = data.get("websocket_url", data.get("url", DEFAULT_WS_URL))
            token = data.get("token", MCP_TOKEN)

            # Check for activation requirement
            if "activation" in data:
                print(f"    Device requires activation! Code: {data['activation'].get('code', 'N/A')}")

            print(f"    WebSocket URL: {ws_url}")
            print(f"    Token: {token[:30]}..." if len(token) > 30 else f"    Token: {token}")

            return ws_url, token

    except Exception as e:
        print(f"    OTA request failed: {e}")
        return DEFAULT_WS_URL, MCP_TOKEN


# ===== Approach A: Wake Word + Listen Text Injection =====
async def test_approach_a(ws_url, token):
    """
    Approach A: Inject text via listen message

    Flow:
    1. Connect to WebSocket with auth headers
    2. Send hello message
    3. Wait for server hello
    4. Send listen message with state="detect" and text=preset_text
       (This simulates wake word detection with preset text)
    5. Send listen message with state="start" mode="auto"
    6. Wait for TTS audio from server
    """
    print()
    print("=" * 60)
    print("APPROACH A: Wake Word + Listen Text Injection")
    print("=" * 60)

    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    # Step 1: Connect
    print("\n[1] Connecting to Xiaozhi server...")
    try:
        # Build headers using real device credentials
        headers = {
            "Device-Id": DEVICE_MAC,
        }
        if DEVICE_UUID:
            headers["Client-Id"] = DEVICE_UUID
        # Token may be Bearer format or plain JWT
        if token and len(token) > 0 and token != "test-token":
            headers["Authorization"] = f"Bearer {token}"
        headers["Protocol-Version"] = "1"
        print(f"    Headers: Device-Id={DEVICE_MAC}, Client-Id={DEVICE_UUID or '<empty>'}, Auth={'Bearer' if token and token != 'test-token' else 'none'}")

        ws = await websockets.connect(
            ws_url,
            ssl=ssl_context,
            additional_headers=headers,
            ping_interval=20,
            ping_timeout=10,
            close_timeout=5,
        )
        print(f"    Connected! Remote: {ws.remote_address}")
    except Exception as e:
        print(f"    FAILED to connect: {e}")
        print(f"    NOTE: If authentication fails, you need to get a valid token from OTA")
        print(f"    OTA URL: {OTA_URL}")
        return False

    session_id = ""

    # Step 2: Send hello
    print("\n[2] Sending hello message...")
    hello_msg = {
        "type": "hello",
        "version": 1,
        "features": {"mcp": True},
        "transport": "websocket",
        "audio_params": {
            "format": "opus",
            "sample_rate": 16000,
            "channels": 1,
            "frame_duration": 60
        }
    }
    await ws.send(json.dumps(hello_msg))
    print(f"    Sent: {json.dumps(hello_msg, indent=2)[:300]}")

    # Step 3: Wait for server hello
    print("\n[3] Waiting for server hello...")
    try:
        msg = await asyncio.wait_for(ws.recv(), timeout=15)
        data = json.loads(msg)
        print(f"    Received: type={data.get('type', 'unknown')}")

        if data.get("type") == "hello":
            session_id = data.get("session_id", "")
            print(f"    Session ID: {session_id}")
            server_audio = data.get("audio_params", {})
            print(f"    Server audio: {server_audio}")
        else:
            print(f"    Unexpected message: {json.dumps(data, indent=2)[:500]}")
    except asyncio.TimeoutError:
        print("    TIMEOUT - no server hello received")
        await ws.close()
        return False

    # Step 4: Send wake word detection with preset text
    print(f"\n[4] Sending wake word detect with text: \"{TTS_TEST_TEXT}\"")
    listen_detect_msg = {
        "session_id": session_id,
        "type": "listen",
        "state": "detect",
        "text": TTS_TEST_TEXT
    }
    await ws.send(json.dumps(listen_detect_msg))
    print(f"    Sent: {json.dumps(listen_detect_msg, ensure_ascii=False)}")

    # Step 5: Start listening
    print("\n[5] Sending start listening...")
    listen_start_msg = {
        "session_id": session_id,
        "type": "listen",
        "state": "start",
        "mode": "auto"
    }
    await ws.send(json.dumps(listen_start_msg))
    print(f"    Sent: {json.dumps(listen_start_msg)}")

    # Step 6: Wait for server responses (TTS, STT, etc.)
    print("\n[6] Waiting for server responses (30s window)...")
    got_tts = False
    got_stt = False
    got_mcp = False
    binary_count = 0

    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=30)

            if isinstance(msg, bytes):
                # Binary data = OPUS audio (TTS playback!)
                binary_count += 1
                if not got_tts:
                    got_tts = True
                    print(f"    >>> TTS AUDIO RECEIVED! First binary frame size: {len(msg)} bytes")
                if binary_count % 10 == 0:
                    print(f"    ... Received {binary_count} audio frames so far")
                continue

            # Text data = JSON message
            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "tts":
                state = data.get("state", "")
                if state == "start":
                    print(f"    >>> TTS START - server is sending audio!")
                    got_tts = True
                elif state == "stop":
                    print(f"    >>> TTS STOP - audio segment finished")
                elif state == "sentence_start":
                    text = data.get("text", "")
                    print(f"    >>> TTS sentence: \"{text}\"")
                else:
                    print(f"    >>> TTS state: {state}")

            elif msg_type == "stt":
                text = data.get("text", "")
                print(f"    >>> STT result: \"{text}\"")
                got_stt = True

            elif msg_type == "llm":
                emotion = data.get("emotion", "")
                text = data.get("text", "")
                print(f"    >>> LLM: emotion={emotion} text={text}")

            elif msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                print(f"    >>> MCP: method={method}")
                got_mcp = True

            elif msg_type == "goodbye":
                print(f"    >>> Server goodbye received")
                break

            elif msg_type == "alert":
                print(f"    >>> Alert: {data.get('message', '')}")

            else:
                print(f"    >>> Message: type={msg_type}")
                if len(json.dumps(data)) < 500:
                    print(f"        {json.dumps(data, ensure_ascii=False)}")

    except asyncio.TimeoutError:
        print(f"    No more messages (30s timeout)")
    except websockets.exceptions.ConnectionClosed as e:
        print(f"    Connection closed: code={e.code}, reason={e.reason}")

    # Summary
    await ws.close()
    print()
    print("--- Approach A Summary ---")
    print(f"  Got STT:    {got_stt}")
    print(f"  Got TTS:    {got_tts}")
    print(f"  Got MCP:    {got_mcp}")
    print(f"  Audio frames: {binary_count}")
    print(f"  Result: {'SUCCESS - TTS triggered!' if got_tts else 'FAILED - No TTS received'}")

    return got_tts


# ===== Approach A2: Direct listen with text (no wake word detect) =====
async def test_approach_a2(ws_url, token):
    """
    Approach A2: Send preset text directly via listen state="detect"

    Some servers may treat the text field in the "detect" listen message
    as the user's actual speech input, bypassing ASR entirely.
    """
    print()
    print("=" * 60)
    print("APPROACH A2: Direct Text via Listen Detect")
    print("=" * 60)

    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    print("\n[1] Connecting...")
    try:
        a2_headers = {
            "Protocol-Version": "1",
            "Device-Id": DEVICE_MAC,
        }
        if DEVICE_UUID:
            a2_headers["Client-Id"] = DEVICE_UUID
        if token and len(token) > 0 and token != "test-token":
            a2_headers["Authorization"] = f"Bearer {token}"
        print(f"    Headers: Device-Id={DEVICE_MAC}, Client-Id={DEVICE_UUID or '<empty>'}")

        ws = await websockets.connect(
            ws_url,
            ssl=ssl_context,
            additional_headers=a2_headers,
            ping_interval=20,
            ping_timeout=10,
        )
        print(f"    Connected!")
    except Exception as e:
        print(f"    FAILED: {e}")
        return False

    session_id = ""

    # Hello handshake
    hello_msg = {
        "type": "hello",
        "version": 1,
        "features": {"mcp": True},
        "transport": "websocket",
        "audio_params": {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60}
    }
    await ws.send(json.dumps(hello_msg))

    try:
        msg = await asyncio.wait_for(ws.recv(), timeout=15)
        data = json.loads(msg)
        if data.get("type") == "hello":
            session_id = data.get("session_id", "")
            print(f"    Session ID: {session_id}")
        else:
            print(f"    Unexpected: {data.get('type')}")
            await ws.close()
            return False
    except asyncio.TimeoutError:
        print("    TIMEOUT waiting for hello")
        await ws.close()
        return False

    # Wait for MCP initialize (server may send it after hello)
    print("\n[2] Handling MCP initialization (if any)...")
    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=5)
            if isinstance(msg, bytes):
                continue
            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                req_id = payload.get("id")

                if method == "initialize":
                    print(f"    Got MCP initialize request (id={req_id})")
                    # Respond with device capabilities
                    mcp_response = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "protocolVersion": "2024-11-05",
                                "capabilities": {"tools": {}},
                                "serverInfo": {"name": "focuslamp-tts-test", "version": "1.0.0"}
                            }
                        }
                    }
                    await ws.send(json.dumps(mcp_response))
                    print(f"    Sent MCP initialize response")

                    # Send initialized notification
                    init_notif = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "method": "notifications/initialized"
                        }
                    }
                    await ws.send(json.dumps(init_notif))

                elif method == "tools/list":
                    print(f"    Got MCP tools/list request (id={req_id})")
                    # Respond with empty tools
                    tools_response = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {"tools": []}
                        }
                    }
                    await ws.send(json.dumps(tools_response))

                elif method == "ping":
                    pong = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {"jsonrpc": "2.0", "id": req_id, "result": {}}
                    }
                    await ws.send(json.dumps(pong))

                else:
                    print(f"    Got MCP method: {method}")
            else:
                print(f"    Got message: type={msg_type}")
    except asyncio.TimeoutError:
        print("    No more MCP init messages (5s timeout)")

    # Send listen detect with preset text
    print(f"\n[3] Sending listen detect with text: \"{TTS_TEST_TEXT}\"")
    listen_msg = {
        "session_id": session_id,
        "type": "listen",
        "state": "detect",
        "text": TTS_TEST_TEXT
    }
    await ws.send(json.dumps(listen_msg))
    print(f"    Sent detect message")

    # Start listening
    listen_start = {
        "session_id": session_id,
        "type": "listen",
        "state": "start",
        "mode": "auto"
    }
    await ws.send(json.dumps(listen_start))
    print(f"    Sent start listening")

    # Wait for TTS
    print("\n[4] Waiting for TTS responses (30s window)...")
    got_tts = False
    binary_count = 0

    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=30)

            if isinstance(msg, bytes):
                binary_count += 1
                if not got_tts:
                    got_tts = True
                    print(f"    >>> TTS AUDIO! First frame: {len(msg)} bytes")
                continue

            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "tts":
                state = data.get("state", "")
                if state == "start":
                    got_tts = True
                    print(f"    >>> TTS START!")
                elif state == "stop":
                    print(f"    >>> TTS STOP")
                elif state == "sentence_start":
                    print(f"    >>> TTS sentence: \"{data.get('text', '')}\"")
            elif msg_type == "stt":
                print(f"    >>> STT: \"{data.get('text', '')}\"")
            elif msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                req_id = payload.get("id")
                # Handle MCP requests
                if method == "tools/call":
                    tool_name = payload.get("params", {}).get("name", "")
                    print(f"    >>> MCP tools/call: {tool_name}")
                    # Respond to tool calls
                    tool_resp = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "content": [{"type": "text", "text": "true"}],
                                "isError": False
                            }
                        }
                    }
                    await ws.send(json.dumps(tool_resp))
                else:
                    print(f"    >>> MCP: {method}")
            elif msg_type == "goodbye":
                print(f"    >>> Goodbye")
                break
            else:
                raw = json.dumps(data, ensure_ascii=False)[:200]
                print(f"    >>> {msg_type}: {raw.encode('ascii', 'replace').decode('ascii')}")

    except asyncio.TimeoutError:
        print(f"    Timeout (30s)")
    except websockets.exceptions.ConnectionClosed as e:
        print(f"    Connection closed: code={e.code}")

    await ws.close()
    print()
    print("--- Approach A2 Summary ---")
    print(f"  Got TTS:    {got_tts}")
    print(f"  Audio frames: {binary_count}")
    print(f"  Result: {'SUCCESS!' if got_tts else 'FAILED'}")

    return got_tts


# ===== Approach B: MCP Notification Trigger =====
async def test_approach_b(ws_url, token):
    """
    Approach B: Send MCP notification to trigger AI response

    After MCP initialization, send a log/notification message via MCP
    to trigger AI processing. The AI should then respond with TTS.

    Two sub-approaches:
    B1: Send MCP notifications/message with the preset text
    B2: Send MCP tool call result containing the preset text
    """
    print()
    print("=" * 60)
    print("APPROACH B: MCP Notification Trigger")
    print("=" * 60)

    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    print("\n[1] Connecting...")
    try:
        b_headers = {
            "Protocol-Version": "1",
            "Device-Id": DEVICE_MAC,
        }
        if DEVICE_UUID:
            b_headers["Client-Id"] = DEVICE_UUID
        if token and len(token) > 0 and token != "test-token":
            b_headers["Authorization"] = f"Bearer {token}"

        ws = await websockets.connect(
            ws_url,
            ssl=ssl_context,
            additional_headers=b_headers,
            ping_interval=20,
            ping_timeout=10,
        )
        print(f"    Connected!")
    except Exception as e:
        print(f"    FAILED: {e}")
        return False

    session_id = ""

    # Hello handshake
    hello_msg = {
        "type": "hello",
        "version": 1,
        "features": {"mcp": True},
        "transport": "websocket",
        "audio_params": {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60}
    }
    await ws.send(json.dumps(hello_msg))

    try:
        msg = await asyncio.wait_for(ws.recv(), timeout=15)
        data = json.loads(msg)
        if data.get("type") == "hello":
            session_id = data.get("session_id", "")
            print(f"    Session ID: {session_id}")
        else:
            print(f"    Unexpected: {data.get('type')}")
            await ws.close()
            return False
    except asyncio.TimeoutError:
        print("    TIMEOUT waiting for hello")
        await ws.close()
        return False

    # Handle MCP initialization
    print("\n[2] Handling MCP initialization...")
    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=10)
            if isinstance(msg, bytes):
                continue
            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                req_id = payload.get("id")

                if method == "initialize":
                    print(f"    Got MCP initialize (id={req_id})")
                    mcp_response = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "protocolVersion": "2024-11-05",
                                "capabilities": {"tools": {"listChanged": False}},
                                "serverInfo": {"name": "focuslamp-tts-test", "version": "1.0.0"},
                                "instructions": "This is a FocusLamp device that can speak notifications to the user."
                            }
                        }
                    }
                    await ws.send(json.dumps(mcp_response))
                    # Send initialized notification
                    init_notif = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "method": "notifications/initialized"
                        }
                    }
                    await ws.send(json.dumps(init_notif))
                    print(f"    Sent MCP initialize response + initialized notification")

                elif method == "tools/list":
                    print(f"    Got MCP tools/list (id={req_id})")
                    # Register notification.speak tool to enable server-initiated TTS
                    tools_response = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "tools": [
                                    {
                                        "name": "self.notification.speak",
                                        "description": "主动向用户播报信息，支持不同优先级",
                                        "inputSchema": {
                                            "type": "object",
                                            "properties": {
                                                "message": {"type": "string", "description": "要播报的消息内容"},
                                                "priority": {"type": "integer", "description": "优先级 (0-3)", "minimum": 0, "maximum": 3}
                                            },
                                            "required": ["message"]
                                        }
                                    }
                                ]
                            }
                        }
                    }
                    await ws.send(json.dumps(tools_response))
                    print(f"    Sent tools/list response with notification.speak")
                    break  # MCP init done

                elif method == "ping":
                    pong = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {"jsonrpc": "2.0", "id": req_id, "result": {}}
                    }
                    await ws.send(json.dumps(pong))

                else:
                    print(f"    Got MCP method: {method}")
            else:
                print(f"    Got message: type={msg_type}")
    except asyncio.TimeoutError:
        print("    Timeout waiting for MCP init (10s)")

    # B1: Send MCP log notification with preset text
    print(f"\n[3] B1: Sending MCP log notification with text: \"{TTS_TEST_TEXT}\"")
    log_notif = {
        "session_id": session_id,
        "type": "mcp",
        "payload": {
            "jsonrpc": "2.0",
            "method": "notifications/message",
            "params": {
                "level": "info",
                "data": f"[HealthMonitor] {TTS_TEST_TEXT}"
            }
        }
    }
    await ws.send(json.dumps(log_notif))
    print(f"    Sent MCP log notification")

    # Also send a listen start to allow server to respond
    listen_start = {
        "session_id": session_id,
        "type": "listen",
        "state": "start",
        "mode": "auto"
    }
    await ws.send(json.dumps(listen_start))
    print(f"    Sent start listening")

    # Wait for responses
    print("\n[4] Waiting for AI/TTS responses (30s window)...")
    got_tts = False
    got_stt = False
    got_llm = False
    binary_count = 0

    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=30)

            if isinstance(msg, bytes):
                binary_count += 1
                if not got_tts:
                    got_tts = True
                    print(f"    >>> TTS AUDIO! First frame: {len(msg)} bytes")
                continue

            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "tts":
                state = data.get("state", "")
                if state == "start":
                    got_tts = True
                    print(f"    >>> TTS START!")
                elif state == "stop":
                    print(f"    >>> TTS STOP")
                elif state == "sentence_start":
                    print(f"    >>> TTS sentence: \"{data.get('text', '')}\"")
            elif msg_type == "stt":
                print(f"    >>> STT: \"{data.get('text', '')}\"")
                got_stt = True
            elif msg_type == "llm":
                emotion = data.get('emotion', '')
                text = data.get('text', '')
                # Safe print: strip emojis to avoid GBK encoding errors on Windows
                safe_emotion = emotion.encode('ascii', 'replace').decode('ascii')
                safe_text = text.encode('ascii', 'replace').decode('ascii')
                print(f"    >>> LLM: emotion={safe_emotion} text={safe_text}")
                got_llm = True
            elif msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                req_id = payload.get("id")
                if method == "tools/call":
                    tool_name = payload.get("params", {}).get("name", "")
                    tool_args = payload.get("params", {}).get("arguments", {})
                    print(f"    >>> MCP tools/call: {tool_name} args={json.dumps(tool_args, ensure_ascii=False)}")
                    # Respond to tool call
                    tool_resp = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "content": [{"type": "text", "text": "true"}],
                                "isError": False
                            }
                        }
                    }
                    await ws.send(json.dumps(tool_resp))
                elif method == "ping":
                    pong = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {"jsonrpc": "2.0", "id": req_id, "result": {}}
                    }
                    await ws.send(json.dumps(pong))
                else:
                    print(f"    >>> MCP: {method}")
            elif msg_type == "alert":
                print(f"    >>> Alert: {data.get('message', '')}")
            elif msg_type == "goodbye":
                print(f"    >>> Goodbye")
                break
            else:
                raw = json.dumps(data, ensure_ascii=False)[:300]
                print(f"    >>> {msg_type}: {raw.encode('ascii', 'replace').decode('ascii')}")

    except asyncio.TimeoutError:
        print(f"    Timeout (30s)")
    except websockets.exceptions.ConnectionClosed as e:
        print(f"    Connection closed: code={e.code}")

    await ws.close()
    print()
    print("--- Approach B Summary ---")
    print(f"  Got STT:    {got_stt}")
    print(f"  Got LLM:    {got_llm}")
    print(f"  Got TTS:    {got_tts}")
    print(f"  Audio frames: {binary_count}")
    print(f"  Result: {'SUCCESS!' if got_tts else 'FAILED'}")

    return got_tts


# ===== Approach C: Wake Word + Short Command =====
async def test_approach_c(ws_url, token):
    """
    Approach C: Send a proper wake word first, then a short command text

    The server rejected long text in listen detect, but may accept
    a short wake word + a short command. We send:
    1. listen detect with wake word "你好小智"
    2. listen start
    3. Then inject text as STT result via listen detect with short text
    """
    print()
    print("=" * 60)
    print("APPROACH C: Wake Word + Short Command Trigger")
    print("=" * 60)

    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    print("\n[1] Connecting...")
    try:
        c_headers = {
            "Protocol-Version": "1",
            "Device-Id": DEVICE_MAC,
        }
        if DEVICE_UUID:
            c_headers["Client-Id"] = DEVICE_UUID
        if token and len(token) > 0 and token != "test-token":
            c_headers["Authorization"] = f"Bearer {token}"

        ws = await websockets.connect(
            ws_url,
            ssl=ssl_context,
            additional_headers=c_headers,
            ping_interval=20,
            ping_timeout=10,
        )
        print(f"    Connected!")
    except Exception as e:
        print(f"    FAILED: {e}")
        return False

    session_id = ""

    # Hello handshake
    hello_msg = {
        "type": "hello",
        "version": 1,
        "features": {"mcp": True},
        "transport": "websocket",
        "audio_params": {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60}
    }
    await ws.send(json.dumps(hello_msg))

    try:
        msg = await asyncio.wait_for(ws.recv(), timeout=15)
        data = json.loads(msg)
        if data.get("type") == "hello":
            session_id = data.get("session_id", "")
            print(f"    Session ID: {session_id}")
        else:
            print(f"    Unexpected: {data.get('type')}")
            await ws.close()
            return False
    except asyncio.TimeoutError:
        print("    TIMEOUT waiting for hello")
        await ws.close()
        return False

    # Handle MCP initialization
    print("\n[2] Handling MCP initialization...")
    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=10)
            if isinstance(msg, bytes):
                continue
            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                req_id = payload.get("id")

                if method == "initialize":
                    print(f"    Got MCP initialize (id={req_id})")
                    mcp_response = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "protocolVersion": "2024-11-05",
                                "capabilities": {"tools": {"listChanged": False}},
                                "serverInfo": {"name": "focuslamp-tts-test", "version": "1.0.0"},
                                "instructions": "This is a FocusLamp device that can speak notifications."
                            }
                        }
                    }
                    await ws.send(json.dumps(mcp_response))
                    init_notif = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "method": "notifications/initialized"
                        }
                    }
                    await ws.send(json.dumps(init_notif))

                elif method == "tools/list":
                    print(f"    Got MCP tools/list (id={req_id})")
                    tools_response = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "tools": [
                                    {
                                        "name": "self.notification.speak",
                                        "description": "主动向用户播报信息",
                                        "inputSchema": {
                                            "type": "object",
                                            "properties": {
                                                "message": {"type": "string"},
                                                "priority": {"type": "integer", "minimum": 0, "maximum": 3}
                                            },
                                            "required": ["message"]
                                        }
                                    }
                                ]
                            }
                        }
                    }
                    await ws.send(json.dumps(tools_response))
                    break

                elif method == "ping":
                    pong = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {"jsonrpc": "2.0", "id": req_id, "result": {}}
                    }
                    await ws.send(json.dumps(pong))
            else:
                print(f"    Got message: type={msg_type}")
    except asyncio.TimeoutError:
        print("    Timeout waiting for MCP init")

    # Send wake word detect with short wake word text
    print(f"\n[3] Sending wake word detect: '你好小智'")
    wake_detect = {
        "session_id": session_id,
        "type": "listen",
        "state": "detect",
        "text": "你好小智"
    }
    await ws.send(json.dumps(wake_detect))

    # Start listening
    listen_start = {
        "session_id": session_id,
        "type": "listen",
        "state": "start",
        "mode": "auto"
    }
    await ws.send(json.dumps(listen_start))
    print(f"    Sent start listening")

    # Wait a moment, then send a short command via STT-like text
    # Note: The server may process the wake word and wait for audio input.
    # We can try sending the command as if it was recognized by ASR.
    await asyncio.sleep(2)

    print(f"\n[4] Sending command via listen detect: '提醒我休息'")
    command_detect = {
        "session_id": session_id,
        "type": "listen",
        "state": "detect",
        "text": "提醒我休息"
    }
    await ws.send(json.dumps(command_detect))

    # Wait for responses
    print("\n[5] Waiting for AI/TTS responses (30s window)...")
    got_tts = False
    got_stt = False
    got_llm = False
    binary_count = 0

    try:
        while True:
            msg = await asyncio.wait_for(ws.recv(), timeout=30)

            if isinstance(msg, bytes):
                binary_count += 1
                if not got_tts:
                    got_tts = True
                    print(f"    >>> TTS AUDIO! First frame: {len(msg)} bytes")
                continue

            data = json.loads(msg)
            msg_type = data.get("type", "unknown")

            if msg_type == "tts":
                state = data.get("state", "")
                if state == "start":
                    got_tts = True
                    print(f"    >>> TTS START!")
                elif state == "stop":
                    print(f"    >>> TTS STOP")
                elif state == "sentence_start":
                    print(f"    >>> TTS sentence: \"{data.get('text', '')}\"")
            elif msg_type == "stt":
                print(f"    >>> STT: \"{data.get('text', '')}\"")
                got_stt = True
            elif msg_type == "llm":
                emotion = data.get('emotion', '')
                text = data.get('text', '')
                # Safe print: strip emojis to avoid GBK encoding errors on Windows
                safe_emotion = emotion.encode('ascii', 'replace').decode('ascii')
                safe_text = text.encode('ascii', 'replace').decode('ascii')
                print(f"    >>> LLM: emotion={safe_emotion} text={safe_text}")
                got_llm = True
            elif msg_type == "mcp":
                payload = data.get("payload", {})
                method = payload.get("method", "")
                req_id = payload.get("id")
                if method == "tools/call":
                    tool_name = payload.get("params", {}).get("name", "")
                    tool_args = payload.get("params", {}).get("arguments", {})
                    print(f"    >>> MCP tools/call: {tool_name} args={json.dumps(tool_args, ensure_ascii=False)}")
                    tool_resp = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {
                            "jsonrpc": "2.0",
                            "id": req_id,
                            "result": {
                                "content": [{"type": "text", "text": "true"}],
                                "isError": False
                            }
                        }
                    }
                    await ws.send(json.dumps(tool_resp))
                elif method == "ping":
                    pong = {
                        "session_id": session_id,
                        "type": "mcp",
                        "payload": {"jsonrpc": "2.0", "id": req_id, "result": {}}
                    }
                    await ws.send(json.dumps(pong))
                else:
                    print(f"    >>> MCP: {method}")
            elif msg_type == "alert":
                print(f"    >>> Alert: {data.get('message', '')}")
            elif msg_type == "goodbye":
                print(f"    >>> Goodbye")
                break
            else:
                raw = json.dumps(data, ensure_ascii=False)[:300]
                print(f"    >>> {msg_type}: {raw.encode('ascii', 'replace').decode('ascii')}")

    except asyncio.TimeoutError:
        print(f"    Timeout (30s)")
    except websockets.exceptions.ConnectionClosed as e:
        print(f"    Connection closed: code={e.code}")

    await ws.close()
    print()
    print("--- Approach C Summary ---")
    print(f"  Got STT:    {got_stt}")
    print(f"  Got LLM:    {got_llm}")
    print(f"  Got TTS:    {got_tts}")
    print(f"  Audio frames: {binary_count}")
    print(f"  Result: {'SUCCESS!' if got_tts else 'FAILED'}")

    return got_tts


# ===== Main =====
async def main():
    print("=" * 60)
    print("Xiaozhi TTS Inject Test")
    print("Testing: 久坐检测 → 预设文本 → TTS播报")
    print("=" * 60)

    # Get WebSocket URL and token via OTA
    print("\n[OTA] Getting WebSocket URL and token...")
    ws_url, token = await get_ws_url_and_token()

    # Test Approach B (MCP notification trigger)
    result_b = await test_approach_b(ws_url, token)

    # Test Approach C (wake word + short command)
    result_c = await test_approach_c(ws_url, token)

    # Final summary
    print()
    print("=" * 60)
    print("FINAL RESULTS")
    print("=" * 60)
    print(f"  Approach B (MCP notification trigger):    {'PASS' if result_b else 'FAIL'}")
    print(f"  Approach C (wake word + short command):   {'PASS' if result_c else 'FAIL'}")
    print()

    if result_b or result_c:
        print("CONCLUSION: At least one approach works for triggering TTS!")
        if result_b:
            print("  -> Approach B (MCP notification) is viable for device implementation")
        if result_c:
            print("  -> Approach C (wake word + command) is viable for device implementation")
    else:
        print("CONCLUSION: None of the tested approaches triggered TTS.")
        print("Next steps to consider:")
        print("  1. Use the MCP standalone endpoint (wss://api.xiaozhi.me/mcp/) for TTS")
        print("  2. Check if server needs specific prompt/instructions in MCP init")
        print("  3. Deploy custom server with tts_inject support")
        print("  4. Use local TTS (Edge TTS / espeak) as fallback")


if __name__ == "__main__":
    asyncio.run(main())
