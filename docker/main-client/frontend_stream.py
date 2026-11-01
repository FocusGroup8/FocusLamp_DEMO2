"""One writer per browser socket; input delivery never waits for that writer."""
import asyncio
import json
import time
from collections import deque


class LatestFrontend:
    def __init__(self, ws, on_dead, send_timeout=0.4, close_timeout=0.2):
        self.ws = ws
        self.on_dead = on_dead
        self.send_timeout = send_timeout
        self.close_timeout = close_timeout
        self.frame = None
        self.overlay = None
        self.replies = deque(maxlen=8)
        self.closed = False
        self.ready = asyncio.Event()
        self.task = asyncio.create_task(self._run(), name='frontend-latest-writer')

    def offer_frame(self, header, image, received_at, results=None):
        if not self.closed:
            self.frame = (dict(header), image, received_at, results)
            self.ready.set()

    def offer_overlay(self, overlay):
        if not self.closed:
            self.overlay = (dict(overlay), time.monotonic())
            self.ready.set()

    def offer_reply(self, reply):
        if not self.closed:
            self.replies.append(reply)
            self.ready.set()

    def camera_status(self, status):
        self.frame = self.overlay = None
        self.offer_reply(status)

    async def _send_frame(self, frame):
        header, image, received_at, results = frame
        # timestamp is the PC send time, not a camera capture clock.
        header['timestamp'] = int(time.time() * 1000)
        header['age_ms'] = max(0, int((time.monotonic() - received_at) * 1000))
        await self.ws.send_str(json.dumps(header))
        await self.ws.send_bytes(image)
        if results is not None:
            await self.ws.send_str(results)

    async def _send_overlay(self, value):
        overlay, offered_at = value
        age = overlay.get('age_ms')
        if age is not None:
            overlay['age_ms'] = age + max(0, int((time.monotonic() - offered_at) * 1000))
        await self.ws.send_str(json.dumps(overlay))

    async def _run(self):
        try:
            while not self.closed:
                await self.ready.wait()
                self.ready.clear()
                # Snapshot and clear both bounded slots. Newly offered items
                # replace the pending item while this bundle is being written.
                while self.replies:
                    reply = self.replies.popleft()
                    await asyncio.wait_for(self.ws.send_str(json.dumps(reply)), self.send_timeout)
                frame, overlay = self.frame, self.overlay
                self.frame = self.overlay = None
                if frame is not None:
                    await asyncio.wait_for(self._send_frame(frame), self.send_timeout)
                if overlay is not None:
                    await asyncio.wait_for(self._send_overlay(overlay), self.send_timeout)
        except asyncio.CancelledError:
            raise
        except Exception:
            # A partially written bundle must not be reused by another frame.
            self.closed = True
            self.frame = self.overlay = None
            self.replies.clear()
            self.on_dead()
            try:
                await asyncio.wait_for(self.ws.close(), self.close_timeout)
            except Exception:
                pass

    async def close(self):
        self.closed = True
        self.frame = self.overlay = None
        self.replies.clear()
        self.task.cancel()
        try:
            await self.task
        except asyncio.CancelledError:
            pass
