import asyncio
import websockets
import json
import cv2
import time
import os
from typing import Set

class CameraCapture:
    def __init__(self):
        self.camera_index = int(os.environ.get('CAMERA_INDEX', 0))
        self.fps = int(os.environ.get('FPS', 60))
        self.jpeg_quality = int(os.environ.get('JPEG_QUALITY', 80))
        self.port = int(os.environ.get('PORT', 9007))
        
        self.clients: Set = set()
        self.frame_seq = 0
        self.cap = None
        self.running = False
        
    def init_camera(self):
        print(f"[INFO] Initializing camera {self.camera_index}...")
        self.cap = cv2.VideoCapture(self.camera_index)
        
        if not self.cap.isOpened():
            print(f"[ERROR] Failed to open camera {self.camera_index}")
            return False
        
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        self.cap.set(cv2.CAP_PROP_FPS, self.fps)
        
        width = int(self.cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(self.cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        actual_fps = self.cap.get(cv2.CAP_PROP_FPS)
        
        print(f"[INFO] Camera initialized: {width}x{height} @ {actual_fps} FPS")
        return True
    
    def capture_frame(self):
        if self.cap is None or not self.cap.isOpened():
            return None
        
        ret, frame = self.cap.read()
        if not ret:
            return None
        
        encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_quality]
        _, jpeg_buffer = cv2.imencode('.jpg', frame, encode_params)
        
        return jpeg_buffer.tobytes()
    
    async def broadcast_frame(self, frame_bytes: bytes):
        if not self.clients:
            return
        
        self.frame_seq += 1
        timestamp = int(time.time() * 1000)
        
        header = json.dumps({
            'type': 'frame_header',
            'seq': self.frame_seq,
            'timestamp': timestamp
        })
        
        dead_clients = set()
        for client in self.clients:
            try:
                await client.send(header)
                await client.send(frame_bytes)
            except Exception as e:
                print(f"[WARN] Client send error: {e}")
                dead_clients.add(client)
        
        self.clients -= dead_clients
    
    async def handle_client(self, websocket):
        self.clients.add(websocket)
        client_addr = websocket.remote_address
        print(f"[INFO] Client connected: {client_addr}, total: {len(self.clients)}")
        
        try:
            async for message in websocket:
                try:
                    data = json.loads(message)
                    if data.get('type') == 'ping':
                        await websocket.send(json.dumps({'type': 'pong'}))
                except Exception:
                    pass
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            self.clients.discard(websocket)
            print(f"[INFO] Client disconnected: {client_addr}, remaining: {len(self.clients)}")
    
    async def capture_loop(self):
        frame_interval = 1.0 / self.fps
        last_fps_time = time.time()
        frame_count = 0
        
        while self.running:
            start_time = time.time()
            
            frame_bytes = self.capture_frame()
            if frame_bytes:
                await self.broadcast_frame(frame_bytes)
                frame_count += 1
            
            current_time = time.time()
            if current_time - last_fps_time >= 1.0:
                actual_fps = frame_count / (current_time - last_fps_time)
                print(f"[INFO] Capture FPS: {actual_fps:.1f}, clients: {len(self.clients)}, seq: {self.frame_seq}")
                frame_count = 0
                last_fps_time = current_time
            
            elapsed = time.time() - start_time
            sleep_time = max(0, frame_interval - elapsed)
            if sleep_time > 0:
                await asyncio.sleep(sleep_time)
    
    async def start(self):
        if not self.init_camera():
            print("[ERROR] Camera initialization failed, retrying in 5 seconds...")
            await asyncio.sleep(5)
            return
        
        self.running = True
        
        server = await websockets.serve(
            self.handle_client,
            '0.0.0.0',
            self.port,
            ping_interval=10,
            ping_timeout=20
        )
        
        print(f"[INFO] WebSocket server started on port {self.port}")
        
        await self.capture_loop()
        
        server.close()
        await server.wait_closed()
    
    def stop(self):
        self.running = False
        if self.cap:
            self.cap.release()
            self.cap = None

async def main():
    capture = CameraCapture()
    
    try:
        await capture.start()
    except KeyboardInterrupt:
        print("\n[INFO] Shutting down...")
        capture.stop()
    except Exception as e:
        print(f"[ERROR] {e}")
        capture.stop()

if __name__ == '__main__':
    asyncio.run(main())
