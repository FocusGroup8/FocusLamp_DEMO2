"""Bound continuous inference to one worker and one replaceable pending frame.

All entry points run on the same asyncio event loop. submit() and the worker's
exit bookkeeping contain no await, so dispatch/exit need no additional lock.
HTTP concurrency remains the caller's responsibility (the existing semaphore).
"""
import asyncio
import copy
import logging
from dataclasses import dataclass
from typing import Awaitable, Callable, Dict, Optional

logger = logging.getLogger('main_client')


@dataclass(frozen=True)
class AlgorithmFrame:
    image_base64: str
    timestamp: int
    frame_seq: int
    received_at: float  # PC monotonic clock, never the device timestamp
    extra_params: Optional[dict] = None


@dataclass
class _Slot:
    worker: Optional[asyncio.Task] = None
    pending: Optional[AlgorithmFrame] = None
    inflight: bool = False  # Includes waiting for the existing global semaphore
    dropped: int = 0
    errors: int = 0
    result_age_ms: Optional[float] = None
    result_frame_seq: Optional[int] = None


class LatestAlgorithmDispatcher:
    def __init__(self, process: Callable[[str, AlgorithmFrame], Awaitable[Optional[float]]]):
        # process returns the monotonic time of result-cache storage, or None.
        self._process = process
        self._slots: Dict[str, _Slot] = {}
        self._closed = False

    def submit(self, name: str, frame: AlgorithmFrame) -> None:
        if self._closed:
            return
        slot = self._slots.setdefault(name, _Slot())
        # Snapshot dependent parameters together with the frame. Never retain
        # an old pose/emotion payload when replacing the image.
        frame = AlgorithmFrame(frame.image_base64, frame.timestamp, frame.frame_seq,
                               frame.received_at, copy.deepcopy(frame.extra_params))
        if slot.pending is not None:
            slot.dropped += 1
        slot.pending = frame
        if slot.worker is None or slot.worker.done():
            task = asyncio.create_task(self._run(name, slot), name=f'algorithm:{name}')
            slot.worker = task
            task.add_done_callback(lambda done: self._worker_done(slot, done))

    @staticmethod
    def _worker_done(slot: _Slot, task: asyncio.Task) -> None:
        # Cancellation before the coroutine's first step never enters its
        # finally block. Identity protects a replacement worker from this hook.
        if slot.worker is task:
            slot.worker = None
            slot.inflight = False
            if slot.pending is not None:
                slot.dropped += 1
                slot.pending = None

    async def _run(self, name: str, slot: _Slot) -> None:
        try:
            while slot.pending is not None:
                frame = slot.pending
                slot.pending = None
                slot.inflight = True
                try:
                    stored_at = await self._process(name, frame)
                    if stored_at is not None:
                        slot.result_age_ms = max(0.0, (stored_at - frame.received_at) * 1000)
                        slot.result_frame_seq = frame.frame_seq
                except asyncio.CancelledError:
                    raise
                except Exception:
                    slot.errors += 1
                    logger.exception('Algorithm worker failed: %s', name)
                finally:
                    slot.inflight = False
                # Release this payload before the next request can wait.
                del frame
        finally:
            if slot.worker is asyncio.current_task():
                slot.worker = None
                slot.inflight = False
                if slot.pending is not None:
                    slot.dropped += 1
                    slot.pending = None

    def snapshot(self) -> dict:
        return {
            name: {
                'worker': int(slot.worker is not None and not slot.worker.done()),
                'inflight': int(slot.inflight),
                'queued': int(slot.pending is not None),
                'dropped': slot.dropped,
                'errors': slot.errors,
                'result_age_ms': slot.result_age_ms,
                'result_frame_seq': slot.result_frame_seq,
            }
            for name, slot in self._slots.items()
        }

    async def close(self) -> None:
        self._closed = True
        tasks = [slot.worker for slot in self._slots.values() if slot.worker is not None]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for slot in self._slots.values():
            if slot.pending is not None:
                slot.dropped += 1
            slot.pending = None
            slot.worker = None
            slot.inflight = False
