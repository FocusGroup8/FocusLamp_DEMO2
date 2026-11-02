import asyncio
import socket
import struct
import websockets
import json
import base64
import aiohttp
from aiohttp import web
import time
import os
import logging
import cv2
import numpy as np
from typing import Dict, Any, Optional, Set
import threading
import hashlib
import math
from concurrent.futures import ThreadPoolExecutor
from algorithm_dispatch import AlgorithmFrame, LatestAlgorithmDispatcher
from perception_state import PerceptionState
from focus_metadata import FocusMetadata, clock_residence_ms
from phone_use_policy import PHONE_MAX_AGE_S
from frontend_stream import LatestFrontend

logger = logging.getLogger('main_client')

# 算法超时配置（秒）
ALGORITHM_TIMEOUTS = {
    'focus': 8.0,
    'vlm_visual_qa': 30.0,
    'vlm_game_detector': 8.0,
}
ALGORITHM_TIMEOUT_DEFAULT = 3.0


class AlgoName:
    FOCUS = 'focus'
    EMOTION = 'emotion'
    FATIGUE = 'fatigue'
    FACE_DETECTOR = 'face_detector'
    GESTURE_MANTIS = 'gesture_mantis'
    POSE = 'pose'
    FACE_LANDMARKER = 'face_landmarker'
    FACE_POSITION = 'face_position'
    VLM_VISUAL_QA = 'vlm_visual_qa'
    VLM_GAME_DETECTOR = 'vlm_game_detector'


FPS_STATS_WINDOW = 30  # 每 N 帧重置一次平均耗时统计


def camera_retry_delay(retry_count: int, received_frames: int) -> float:
    # Only an established image stream gets a fast reconnect. Empty handshakes
    # retain exponential backoff so a rejected singleton slot is not hammered.
    if received_frames >= 10:
        return 0.5
    return min(2 ** min(retry_count, 5), 30)


def decode_linux_tcp_info(data):
    # Linux UAPI tcp_info fixed prefix, kernel v6.18 include/uapi/linux/tcp.h.
    if len(data) < 104:
        return None
    u32 = lambda offset: struct.unpack_from("=I", data, offset)[0]
    return {"state": data[0], "pc_rto_ms": u32(8)/1000,
            "pc_rtt_ms": u32(68)/1000, "last_tcp_data_recv_ms": u32(52),
            "pc_unacked": u32(24), "pc_total_retrans": u32(100)}


class MainClient:
    def __init__(self):
        self.esp32p4_ip = os.environ.get('ESP32P4_IP', '192.168.126.143')
        self.esp32p4_port = os.environ.get('ESP32P4_PORT', '80')
        self.camera_capture_host = os.environ.get('CAMERA_CAPTURE_HOST', 'camera-capture')
        self.camera_capture_port = os.environ.get('CAMERA_CAPTURE_PORT', '9007')
        
        self.input_sources = {
            'esp32p4': f"ws://{self.esp32p4_ip}:{self.esp32p4_port}/camera",
            'camera': f"ws://{self.camera_capture_host}:{self.camera_capture_port}"
        }

        self.current_input_source = os.environ.get('INPUT_SOURCE', 'esp32p4')
        self.ws_url = self.input_sources.get(self.current_input_source, self.input_sources['esp32p4'])

        # /algo WebSocket：向 ESP32 回传算法结果（JSON-RPC tools.call: algorithm.result）
        self.algo_ws_url = f"ws://{self.esp32p4_ip}:{self.esp32p4_port}/algo"
        self.algo_ws = None
        self.algo_rpc_id = 0
        self.presence_accum_start_ts = None  # presence.duration_s 累计起始时间戳
        
        self.input_source_switch_requested = False
        self.pending_input_source = None
        
        self.algorithm_urls = {
            'focus': os.environ.get('FOCUS_URL', 'http://127.0.0.1:9003'),
            'emotion': os.environ.get('EMOTION_URL', 'http://127.0.0.1:9004'),
            'fatigue': os.environ.get('FATIGUE_URL', 'http://127.0.0.1:9005'),
            'face_detector': os.environ.get('FACE_DETECTOR_URL', 'http://127.0.0.1:9006'),
            'gesture_mantis': os.environ.get('GESTURE_MANTIS_URL', 'http://127.0.0.1:9009'),
            'pose': os.environ.get('POSE_URL', 'http://127.0.0.1:9010'),
            'face_landmarker': os.environ.get('FACE_LANDMARKER_URL', 'http://127.0.0.1:9011'),
            'face_position': os.environ.get('FACE_POSITION_URL', 'http://127.0.0.1:9012'),
            'vlm_visual_qa': os.environ.get('VLM_VISUAL_QA_URL', 'http://127.0.0.1:9013'),
            'vlm_game_detector': os.environ.get('VLM_GAME_DETECTOR_URL', 'http://127.0.0.1:9014'),
            'phone_use': os.environ.get('PHONE_USE_URL', 'http://127.0.0.1:9015')
        }

        self.enabled_algorithms = {
            'focus': True,
            'emotion': True,
            'fatigue': True,
            'face_detector': False,
            'gesture_mantis': False,
            'pose': False,
            'face_landmarker': False,
            'face_position': False,
            'vlm_visual_qa': False,
            'vlm_game_detector': False,
            'phone_use': False
        }

        self.forced_algorithms = {
            'focus', 'emotion', 'fatigue', 'gesture_mantis', 'pose',
            'face_landmarker', 'face_position', 'vlm_game_detector'
        }

        for algo in self.forced_algorithms:
            self.enabled_algorithms[algo] = True

        # A focused deployment must not probe or forcibly enable containers
        # which were not started (in particular paid VLM services).
        selected = os.environ.get('ENABLED_ALGORITHMS', '').strip()
        self.deployed_algorithms = set(self.algorithm_urls)
        if selected:
            self.deployed_algorithms = {name.strip() for name in selected.split(',') if name.strip()}
            unknown = self.deployed_algorithms - set(self.algorithm_urls)
            if unknown:
                raise ValueError(f'Unknown ENABLED_ALGORITHMS: {sorted(unknown)}')
            self.forced_algorithms &= self.deployed_algorithms
            self.enabled_algorithms = {name: name in self.deployed_algorithms for name in self.algorithm_urls}
        if self.enabled_algorithms.get('phone_use'):
            if not self.enabled_algorithms.get('gesture_mantis'):
                raise ValueError('phone_use requires the existing gesture_mantis hand landmarks')
            self.forced_algorithms.add('phone_use')
        self.camera_connected = asyncio.Event()
        self.inference_success_counts = {}

        gesture_sample_every = os.environ.get('GESTURE_SAMPLE_EVERY', '3').strip()
        if (not gesture_sample_every.isascii() or not gesture_sample_every.isdecimal() or
                int(gesture_sample_every) < 1):
            raise ValueError('GESTURE_SAMPLE_EVERY must be a positive integer')
        self.skip_frames = {
            'focus': 2,
            'emotion': 6,
            'fatigue': 3,
            'gesture_mantis': int(gesture_sample_every),
            'pose': 3,
            'face_landmarker': 3,
            'face_position': 3,
            'vlm_game_detector': 3
        }
        
        self.frame_counter = 0
        self.esp32_frame_seq = 0
        self.last_esp32_seq = -1
        self.frame_drop_count = 0
        self.frame_out_of_order_count = 0
        self.latest_frame_seq = 0
        
        self.pending_frame_header = None
        self.pending_delivery_frame = None
        self.camera_tcp_socket = None
        self.delivery_frame_ready = asyncio.Event()
        self.rt_diag = {"raw": 0, "overwritten": 0, "raw_gap_ms": 0.0, "source_gap_ms": None, "preprocess_ms": 0.0, "delivery_age_ms": 0.0}
        self.rt_last_raw = self.rt_last_source = None
        self.rt_last_log = time.monotonic()
        
        self.session = None
        self.latest_frame = None
        self.latest_frame_bytes = None
        self.latest_frame_received_at = None
        self.latest_frame_sha256 = None
        self.received_image_count = 0
        self.changed_image_count = 0
        self.latest_results = {}
        self.focus_metadata = FocusMetadata()
        self.focus_sent_source = None
        self.focus_head_clock = None
        self.focus_head_clock_received_at = None
        self.perception = PerceptionState()
        self._phone_generation = 0
        self._phone_hand_revision = 0
        self._phone_last_sample_at = None
        self._phone_input_epoch_at = 0.0
        self.forward_emotion_to_head = os.environ.get('FORWARD_EMOTION_TO_HEAD', 'false').lower() == 'true'
        self.frame_lock = threading.Lock()
        
        self.frontend_clients: Set[web.WebSocketResponse] = set()
        self.frontend_senders = {}
        self._camera_generation = 0
        self._camera_input_epoch_at = 0.0
        
        self.last_other_algo_time = 0
        self.detect_fps = 0.0
        self.detect_frame_count = 0
        self.detect_fps_start = time.time()
        self.total_detect_time = 0.0
        self.detect_count = 0
        
        # 情绪+手势联动定义
        self.EMOTION_GESTURE_COMBOS = {
            ("Happiness", "Open_Palm"): "Welcome",
            ("Happiness", "Thumb_Up"): "Good",
        }

        # VLM 触发状态
        self.vlm_pointing_start_time = None
        self.vlm_pointing_triggered = False
        self.vlm_pointing_hold_threshold = float(os.environ.get('VLM_POINTING_HOLD_THRESHOLD', '1.5'))
        self.vlm_last_game_detect_time = 0.0
        self.vlm_game_sample_interval = int(os.environ.get('VLM_GAME_SAMPLE_INTERVAL', '10'))
        self.vlm_visual_qa_in_progress = False
        self.vlm_game_detect_in_progress = False

        # 算法调用并发上限与健康状态（防止容器被突发流量打爆 / 无效请求堆积）
        self.algorithm_semaphore = asyncio.Semaphore(int(os.environ.get('ALGORITHM_MAX_CONCURRENCY', '4')))
        self.algorithm_dispatcher = LatestAlgorithmDispatcher(self._process_continuous_algorithm)
        self._last_algorithm_stats_log_at = time.monotonic()
        self.algorithm_healthy = {name: True for name in self.algorithm_urls}
        self.algo_frame_counters = {}
        self.health_check_interval = int(os.environ.get('HEALTH_CHECK_INTERVAL', '30'))
        # 帧预处理专用线程（cv2 为同步阻塞操作，不能占住事件循环）
        self._preprocess_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix='preprocess')
    
    def _preprocess_frame_sync(self, frame_bytes: bytes) -> bytes:
        """将上一版显示画面再上下翻转一次；两次上下翻转抵消，供网页和算法共用。"""
        try:
            nparr = np.frombuffer(frame_bytes, np.uint8)
            frame = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            if frame is None:
                return frame_bytes
            flipped = cv2.flip(frame, 1)
            rotated = cv2.rotate(flipped, cv2.ROTATE_90_CLOCKWISE)
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, 80]
            ok, buffer = cv2.imencode('.jpg', rotated, encode_params)
            if not ok:
                raise ValueError('Failed to encode rotated camera frame')
            return buffer.tobytes()
        except Exception as e:
            logger.warning("Frame preprocessing failed: %s", e)
            return frame_bytes

    async def _preprocess_frame(self, frame_bytes: bytes) -> bytes:
        """帧预处理：esp32p4 源在独立线程执行，其他源直接透传"""
        if self.current_input_source != 'esp32p4':
            return frame_bytes
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(self._preprocess_executor, self._preprocess_frame_sync, frame_bytes)
    
    async def check_algorithm_health(self):
        logger.info("Checking algorithm containers health...")
        for name, url in self.algorithm_urls.items():
            if name not in self.deployed_algorithms:
                self.algorithm_healthy[name] = False
                continue
            try:
                async with self.session.get(
                    f"{url}/health", 
                    timeout=aiohttp.ClientTimeout(total=2)
                ) as response:
                    healthy = response.status == 200
                    if healthy:
                        logger.info("%s is healthy", name)
                    else:
                        logger.warning("%s returned status %s", name, response.status)
            except Exception as e:
                healthy = False
                logger.error("%s health check failed: %s", name, e)
            self.algorithm_healthy[name] = healthy

    async def health_check_loop(self):
        """周期健康检查：算法容器中途挂掉后自动停止向其分发请求，恢复后自动续传"""
        while True:
            await asyncio.sleep(self.health_check_interval)
            try:
                await self.check_algorithm_health()
            except Exception as e:
                logger.error("Health check loop error: %s", e)
    
    async def call_algorithm(self, algorithm_name: str, image_base64: str, timestamp: int, extra_params: dict = None, force: bool = False) -> Optional[Dict]:
        if not force and not self.enabled_algorithms.get(algorithm_name, False):
            return None
        # 服务不可用时直接跳过，避免无效请求堆积
        if not self.algorithm_healthy.get(algorithm_name, True):
            return None

        url = self.algorithm_urls[algorithm_name]
        timeout_seconds = ALGORITHM_TIMEOUTS.get(algorithm_name, ALGORITHM_TIMEOUT_DEFAULT)
        
        payload = {
            "image": image_base64,
            "timestamp": timestamp
        }
        if extra_params:
            payload.update(extra_params)
        
        # Global HTTP concurrency; continuous pending work is bounded by the dispatcher.
        async with self.algorithm_semaphore:
            try:
                async with self.session.post(
                    f"{url}/{'detect' if algorithm_name in ('gesture_mantis', 'emotion', 'focus', 'phone_use') else 'detect_visualize'}",
                    json=payload,
                    timeout=aiohttp.ClientTimeout(total=timeout_seconds)
                ) as response:
                    if response.status == 200:
                        data = await response.json()
                        if data.get('success'):
                            pt = data.get('processing_time', 0)
                            logger.debug("call_algorithm %s: processing_time=%.3fs, has_vis=%s", algorithm_name, pt, 'visualized_image' in data)
                            return data
                    elif response.status == 503:
                        return None
                    else:
                        logger.error("%s returned status %s", algorithm_name, response.status)
                        return None
            except asyncio.TimeoutError:
                logger.warning("Algorithm %s timeout", algorithm_name)
                return None
            except Exception as e:
                logger.error("Algorithm %s error: %s", algorithm_name, e)
                return None
        
        return None
    
    async def call_algorithm_and_store(self, algorithm_name: str, image_base64: str, timestamp: int, extra_params: dict = None):
        result = await self.call_algorithm(algorithm_name, image_base64, timestamp, extra_params=extra_params)
        if result is not None:
            count = self.inference_success_counts.get(algorithm_name, 0) + 1
            self.inference_success_counts[algorithm_name] = count
            if count == 1 or count % 30 == 0:
                logger.info("Inference OK: algorithm=%s count=%d has_image=%s",
                            algorithm_name, count, bool(result.get('visualized_image')))
            has_vis = 'visualized_image' in result if isinstance(result, dict) else False
            logger.debug("Algorithm %s result: success=%s, has_visualized_image=%s", algorithm_name, result.get('success') if isinstance(result, dict) else 'N/A', has_vis)
            with self.frame_lock:
                self.latest_results[algorithm_name] = result
                stored_at = time.monotonic()
            return stored_at
        else:
            logger.debug("Algorithm %s returned None", algorithm_name)
        return None

    async def _process_continuous_algorithm(self, name: str, frame: AlgorithmFrame):
        if name == 'phone_use':
            return await self._process_phone_use(frame)
        generation = self._camera_generation
        if frame.received_at < self._camera_input_epoch_at:
            return None
        stored_at = await self.call_algorithm_and_store(
            name, frame.image_base64, frame.timestamp, extra_params=frame.extra_params)
        if generation != self._camera_generation:
            return None
        if name == 'focus':
            with self.frame_lock:
                self.focus_metadata.commit(self.latest_results.get(name) if stored_at is not None else None,
                    frame.frame_seq, frame.received_at, generation)
        if stored_at is not None:
            with self.frame_lock:
                response = self.latest_results.get(name)
            if response is not None:
                self.perception.commit(name, frame, response, stored_at)
                if name == 'gesture_mantis':
                    self.broadcast_hand_overlay()
                    self._schedule_phone_use(frame, response, stored_at)
        else:
            self.perception.failed(name)
            if name == 'gesture_mantis':
                self.broadcast_hand_overlay()
            if name == 'gesture_mantis' and self.enabled_algorithms.get('phone_use'):
                self._invalidate_phone_use(failed=True, reset_sampling=False)
        return stored_at

    def _invalidate_phone_use(self, failed=False, reset_sampling=True):
        self._phone_generation += 1
        if reset_sampling:
            self._phone_last_sample_at = None
        self.perception.reset_phone()
        if failed:
            self.perception.failed('phone_use')
        with self.frame_lock:
            self.latest_results.pop('phone_use', None)

    def _schedule_phone_use(self, frame: AlgorithmFrame, response: dict, now: float):
        # The exact gesture input (not the latest camera image) is handed to YOLO.
        # No second hand model and no independent per-camera-frame phone loop.
        if not self.enabled_algorithms.get('phone_use'):
            return
        if frame.received_at < self._phone_input_epoch_at:
            return  # A gesture response may also arrive after reconnect.
        if (not self.camera_connected.is_set() or
                now - frame.received_at > PHONE_MAX_AGE_S):
            self._invalidate_phone_use(failed=True, reset_sampling=False)
            return
        result = response.get('result')
        if not isinstance(result, dict) or result.get('error') or not isinstance(result.get('hand_detected'), bool):
            self._invalidate_phone_use(failed=True, reset_sampling=False)
            return
        points = PerceptionState._points(result)
        hand_detected = result['hand_detected']
        if not hand_detected or points is None:
            # Even a gesture frame skipped by the 1 Hz YOLO gate is evidence
            # that overlap was interrupted. Keep the sampling cadence intact.
            self._phone_hand_revision += 1
            interrupted = self.perception.interrupt_phone_hand()
            if interrupted is not None:
                with self.frame_lock:
                    if 'phone_use' in self.latest_results:
                        self.latest_results['phone_use']['result'] = interrupted
        if (self._phone_last_sample_at is not None and
                frame.received_at - self._phone_last_sample_at < 1.0):
            return
        self._phone_last_sample_at = frame.received_at
        params = {'frame_seq': frame.frame_seq, 'hand_frame_seq': frame.frame_seq,
                  'hand_detected': hand_detected,
                  'hand_landmarks': points if hand_detected else [],
                  '_phone_generation': self._phone_generation,
                  '_phone_hand_revision': self._phone_hand_revision}
        self.algorithm_dispatcher.submit('phone_use',
            AlgorithmFrame(frame.image_base64, frame.timestamp, frame.frame_seq,
                           frame.received_at, params))

    async def _process_phone_use(self, frame: AlgorithmFrame):
        params = dict(frame.extra_params or {})
        generation = params.pop('_phone_generation', None)
        hand_revision = params.pop('_phone_hand_revision', None)
        def current():
            return (generation == self._phone_generation and
                    self.enabled_algorithms.get('phone_use') and
                    self.enabled_algorithms.get('gesture_mantis') and
                    self.camera_connected.is_set())
        if not current():
            return None
        if time.monotonic() - frame.received_at > PHONE_MAX_AGE_S:
            self.perception.failed('phone_use')
            return None
        response = await self.call_algorithm('phone_use', frame.image_base64,
                                             frame.timestamp, extra_params=params)
        if not current():
            return None  # An old connection's late response cannot restore a result.
        stored_at = time.monotonic()
        if not response:
            self.perception.failed('phone_use')
            return None
        if hand_revision != self._phone_hand_revision and isinstance(response.get('result'), dict):
            # Keep the phone observation, but a newer missing/invalid hand has
            # already disproved the continuity of this in-flight overlap.
            response = {**response, 'result': {**response['result'], 'hand_overlap': None}}
        result = self.perception.commit('phone_use', frame, response, stored_at)
        if result is None:
            return None
        with self.frame_lock:
            self.latest_results['phone_use'] = {**response, 'result': result}
        self.inference_success_counts['phone_use'] = self.inference_success_counts.get('phone_use', 0) + 1
        return stored_at

    @staticmethod
    def _get_result_data(latest_results: dict, algo_name: str) -> dict:
        """安全获取算法结果数据，返回 result 子字典或空字典"""
        resp = latest_results.get(algo_name)
        return resp.get('result', {}) if resp else {}

    def _build_enabled_algorithms(self, algorithm_list: list) -> dict:
        """从算法名称列表构建启用字典，forced 算法始终启用"""
        enabled = {name: (name in algorithm_list and name in self.deployed_algorithms)
                   for name in self.algorithm_urls}
        for algo in self.forced_algorithms:
            enabled[algo] = True
        return enabled

    async def _trigger_vlm_algorithm(self, algorithm_name: str, image_base64: str,
                                      timestamp: int, extra_params: dict = None):
        """通用 VLM 算法触发（防重入 + 调用 + 存储）"""
        progress_attr = {
            'vlm_visual_qa': 'vlm_visual_qa_in_progress',
            'vlm_game_detector': 'vlm_game_detect_in_progress',
        }.get(algorithm_name)
        if not progress_attr:
            logger.error("Unknown VLM algorithm: %s", algorithm_name)
            return
        if getattr(self, progress_attr):
            logger.info("VLM-%s: 跳过：正在处理中", algorithm_name)
            return
        setattr(self, progress_attr, True)
        try:
            result = await self.call_algorithm(algorithm_name, image_base64, timestamp,
                                                extra_params=extra_params, force=True)
            if result is not None:
                with self.frame_lock:
                    self.latest_results[algorithm_name] = result
                logger.info("VLM-%s: 触发完成: success=%s", algorithm_name, result.get('success'))
            else:
                logger.info("VLM-%s: 触发返回 None", algorithm_name)
        except Exception as e:
            logger.error("VLM-%s: 触发异常: %s", algorithm_name, e)
        finally:
            setattr(self, progress_attr, False)

    async def trigger_vlm_visual_qa(self, image_base64: str, timestamp: int, fingertip_x=None, fingertip_y=None):
        """触发 VLM 视觉问答（异步调用豆包 API）"""
        extra = {}
        if fingertip_x is not None and fingertip_y is not None:
            extra = {'fingertip_x': fingertip_x, 'fingertip_y': fingertip_y}
        await self._trigger_vlm_algorithm('vlm_visual_qa', image_base64, timestamp, extra_params=extra)

    async def trigger_vlm_game_detector(self, image_base64: str, timestamp: int):
        """触发 VLM 游戏检测（异步调用豆包 API）"""
        await self._trigger_vlm_algorithm('vlm_game_detector', image_base64, timestamp)

    def _check_vlm_triggers(self, image_base64: str, timestamp: int):
        """检查并触发 VLM 算法（在 detect_async 中每帧调用）"""
        # Visual QA uploads are manual only. A pointing gesture must not send
        # images to a paid cloud service without the page's explicit action.
        return
        current_time = time.time()

        # ===== 1. Pointing_Up 手势触发 vlm_visual_qa =====
        with self.frame_lock:
            gesture_resp = self.latest_results.get('gesture_mantis')
            gesture_data = gesture_resp.get('result', {}) if gesture_resp else {}
            current_gesture = gesture_data.get('gesture') if gesture_data else None
            hand_landmarks = gesture_data.get('hand_landmarks') if gesture_data else None

        is_pointing = (current_gesture == "Pointing_Up")
        fingertip_norm = None
        if is_pointing and hand_landmarks and len(hand_landmarks) > 8:
            # 食指尖 = landmark 8
            tip = hand_landmarks[8]
            fingertip_norm = (float(tip.get('x', 0.5)), float(tip.get('y', 0.5)))

        if is_pointing and fingertip_norm is not None:
            if self.vlm_pointing_start_time is None:
                self.vlm_pointing_start_time = current_time
                self.vlm_pointing_triggered = False
            elif not self.vlm_pointing_triggered and \
                    (current_time - self.vlm_pointing_start_time >= self.vlm_pointing_hold_threshold):
                # 触发 VLM 视觉问答
                self.vlm_pointing_triggered = True
                asyncio.create_task(
                    self.trigger_vlm_visual_qa(image_base64, timestamp,
                                               fingertip_x=fingertip_norm[0],
                                               fingertip_y=fingertip_norm[1])
                )
                logger.info("VLM-QA: Pointing_Up 持续 %ss，触发视觉问答", self.vlm_pointing_hold_threshold)
        else:
            self.vlm_pointing_start_time = None
            self.vlm_pointing_triggered = False

        # ===== 2. vlm_game_detector 已通过 forced_algorithms 每帧调用 =====
        # vlm_game_detector 内部根据 YOLO+手部接触检测结果决定是否调用豆包 API
        # 不再需要定时触发
    
    def _track_frame_stats(self, frame_seq: int):
        """帧计数、丢帧检测、乱序检测"""
        current_seq = frame_seq
        current_time = time.time()

        if hasattr(self, 'last_frame_time') and self.last_frame_time > 0:
            frame_interval = (current_time - self.last_frame_time) * 1000
            if frame_interval > 150:
                logger.debug("Long frame interval: %.0fms, seq: %s", frame_interval, current_seq)
        self.last_frame_time = current_time

        if self.last_esp32_seq >= 0:
            expected_seq = self.last_esp32_seq + 1
            if current_seq > expected_seq:
                dropped = current_seq - expected_seq
                self.frame_drop_count += dropped
                logger.debug("Frame drop detected: expected %s, got %s, dropped %s frames, total drops: %s", expected_seq, current_seq, dropped, self.frame_drop_count)
            elif current_seq < expected_seq:
                self.frame_out_of_order_count += 1
                logger.debug("Frame out of order: expected %s, got %s, total out of order: %s", expected_seq, current_seq, self.frame_out_of_order_count)

        self.last_esp32_seq = current_seq

    async def _schedule_algorithms(self, image_base64: str, timestamp: int, frame_seq: int,
                                   received_at: float = None):
        """算法调度逻辑：每帧算法、VLM 触发检查、节流调度"""
        enabled_list = [k for k, v in self.enabled_algorithms.items() if v]
        if received_at is None:
            received_at = time.monotonic()
        if received_at < self._camera_input_epoch_at:
            return

        # face_detector 和 focus 每帧独立调用，不受节流限制
        if 'face_detector' in self.enabled_algorithms and self.enabled_algorithms['face_detector']:
            self.algorithm_dispatcher.submit('face_detector',
                AlgorithmFrame(image_base64, timestamp, frame_seq, received_at))
        if 'focus' in self.enabled_algorithms and self.enabled_algorithms['focus']:
            self.algorithm_dispatcher.submit('focus',
                AlgorithmFrame(image_base64, timestamp, frame_seq, received_at))

        # VLM 触发检查：Pointing_Up 手势触发视觉问答 + 定时触发游戏检测
        # 仅当 gesture_mantis 已启用时才检查手势触发
        if 'gesture_mantis' in self.enabled_algorithms and self.enabled_algorithms['gesture_mantis']:
            self._check_vlm_triggers(image_base64, timestamp)

        other_algo_interval = 0.1
        now = time.time()
        if now - self.last_other_algo_time >= other_algo_interval:
            self.last_other_algo_time = now
            # 获取当前情绪用于传给gesture_mantis
            with self.frame_lock:
                current_emotion = None
                emotion_resp = self.latest_results.get('emotion')
                if emotion_resp and emotion_resp.get('result'):
                    current_emotion = emotion_resp['result'].get('type')

                # 获取最新pose结果用于传给face_position
                pose_landmarks_for_face_pos = None
                pose_resp = self.latest_results.get('pose')
                if pose_resp and pose_resp.get('result'):
                    pose_landmarks_for_face_pos = pose_resp['result'].get('pose_landmarks')

            for algorithm_name in enabled_list:
                if algorithm_name in ('face_detector', 'focus', 'phone_use', 'vlm_visual_qa'):
                    continue
                skip_interval = self.skip_frames.get(algorithm_name, 1)
                # 每算法独立计数器，避免各算法在同一帧集中触发造成突发流量
                counter = self.algo_frame_counters.get(algorithm_name, 0) + 1
                self.algo_frame_counters[algorithm_name] = counter
                if counter % skip_interval == 0:
                    extra = None
                    if algorithm_name == 'gesture_mantis' and current_emotion:
                        extra = {'current_emotion': current_emotion}
                    elif algorithm_name == 'face_position' and pose_landmarks_for_face_pos:
                        extra = {'pose_landmarks': pose_landmarks_for_face_pos}
                    if algorithm_name in ('vlm_visual_qa', 'vlm_game_detector'):
                        # Keep trigger/paid-service semantics outside this change.
                        asyncio.create_task(
                            self.call_algorithm_and_store(algorithm_name, image_base64, timestamp, extra_params=extra)
                        )
                    else:
                        self.algorithm_dispatcher.submit(algorithm_name,
                            AlgorithmFrame(image_base64, timestamp, frame_seq, received_at, extra))

    def _build_algo_arguments(self) -> dict:
        """构建 algorithm.result 工具的 arguments：从 latest_results 提取 ESP32 关心的字段。

        字段集（与 ESP32 algo_result_state_t 对齐）：
        - focus: {engage_level_name, focus_level_name, focus_score}
        - emotion: 字符串
        - fatigue: 整数
        - gesture: 字符串（取自 gesture_mantis.gesture）
        - vlm_game_detector: {judgment, trigger_source, reason}
        """
        arguments = {}

        with self.frame_lock:
            latest = self.latest_results

        with self.frame_lock:
            focus_payload = self.focus_metadata.payload()
        if self.enabled_algorithms.get('focus') and focus_payload:
            arguments['focus'] = focus_payload

        emotion_data = self._get_result_data(latest, 'emotion')
        if self.forward_emotion_to_head and emotion_data and emotion_data.get('type'):
            arguments["emotion"] = emotion_data.get('type')

        fatigue_data = self._get_result_data(latest, 'fatigue')
        if fatigue_data and fatigue_data.get('rating') is not None:
            arguments["fatigue"] = fatigue_data.get('rating')

        gesture_data = self._get_result_data(latest, 'gesture_mantis')
        gesture_name = gesture_data.get('gesture') if gesture_data else None
        if gesture_name and gesture_name not in (None, 'None'):
            arguments["gesture"] = gesture_name

        # VLM 游戏检测：trigger_source 嵌套在 detection 子字段下
        vlm_game_data = self._get_result_data(latest, 'vlm_game_detector')
        if vlm_game_data:
            detection = vlm_game_data.get('detection', {}) or {}
            arguments["vlm_game_detector"] = {
                'judgment': vlm_game_data.get('judgment', '') or '',
                'trigger_source': detection.get('trigger_source'),
                'reason': vlm_game_data.get('reason', '') or '',
            }

        # 用户在位检测：从 face_position.valid 派生 present，累计 duration_s
        face_pos_data = self._get_result_data(latest, 'face_position')
        present = bool(face_pos_data and face_pos_data.get('valid', False))
        if present:
            if self.presence_accum_start_ts is None:
                self.presence_accum_start_ts = time.time()
            duration_s = time.time() - self.presence_accum_start_ts
        else:
            self.presence_accum_start_ts = None
            duration_s = 0.0
        arguments["presence"] = {"present": present, "duration_s": round(duration_s, 1)}

        return arguments

    def _build_algo_rpc_payload(self) -> Optional[dict]:
        """构建 JSON-RPC 2.0 tools.call payload（tool=algorithm.result）。无字段时返回 None。"""
        arguments = self._build_algo_arguments()
        if not arguments:
            return None
        self.algo_rpc_id += 1
        return {
            "jsonrpc": "2.0",
            "id": self.algo_rpc_id,
            "method": "tools.call",
            "params": {
                "name": "algorithm.result",
                "arguments": arguments,
            }
        }

    async def detect_async(self, frame_bytes: bytes, frame_seq: int = None, frame_timestamp: int = None,
                           received_at: float = None):
        if received_at is None:
            received_at = time.monotonic()
        if received_at < self._camera_input_epoch_at:
            return
        self.frame_counter += 1

        if frame_seq is None:
            frame_seq = self.frame_counter

        if frame_timestamp is None:
            frame_timestamp = int(time.time() * 1000)

        self._track_frame_stats(frame_seq)

        image_base64 = base64.b64encode(frame_bytes).decode('utf-8')
        self.perception.update_frame(image_base64, frame_seq, received_at)
        with self.frame_lock:
            self.latest_frame = image_base64
            self.latest_frame_bytes = frame_bytes
            self.latest_frame_seq = frame_seq
            self.latest_frame_received_at = received_at
            digest = hashlib.sha256(frame_bytes).hexdigest()
            if digest != self.latest_frame_sha256:
                self.changed_image_count += 1
            self.latest_frame_sha256 = digest
            self.received_image_count += 1

        t0 = time.time()
        await self._schedule_algorithms(image_base64, frame_timestamp, frame_seq, received_at)
        now_mono = time.monotonic()
        if now_mono - self._last_algorithm_stats_log_at >= 10:
            logger.info('Algorithm backpressure: %s', json.dumps(self.algorithm_dispatcher.snapshot()))
            self._last_algorithm_stats_log_at = now_mono
        dt = time.time() - t0

        # 帧率统计
        self.total_detect_time += dt
        self.detect_count += 1
        if self.detect_count >= FPS_STATS_WINDOW:
            self.total_detect_time = 0
            self.detect_count = 0
        self.detect_frame_count += 1
        detect_elapsed = time.time() - self.detect_fps_start
        if detect_elapsed >= 1.0:
            self.detect_fps = self.detect_frame_count / detect_elapsed
            self.detect_frame_count = 0
            self.detect_fps_start = time.time()
            logger.info("Detect FPS: %.1f, avg detect time: %.0fms, frame_seq: %s, drops: %s, out_of_order: %s", self.detect_fps, dt*1000, frame_seq, self.frame_drop_count, self.frame_out_of_order_count)

    def _adapt_results_for_frontend(self, latest_results: dict) -> dict:
        adapted = {}

        focus_data = self._get_result_data(latest_results, 'focus')
        if focus_data:
            engage = focus_data.get('engage_detect_result', {})
            adapted['focus'] = {
                'success': True,
                'result': {
                    'engage_level': engage.get('engage_level'),
                    'engage_level_name': engage.get('engage_level_name'),
                    'focus_level': focus_data.get('focus_level'),
                    'focus_level_name': focus_data.get('focus_level_name'),
                    'focus_score': focus_data.get('focus_score'),
                    'engage_detect_result': engage,
                    'eye_detect_result': focus_data.get('eye_detect_result'),
                    'face_detect_result': focus_data.get('face_detect_result'),
                    'gesture_detect_result': focus_data.get('gesture_detect_result')
                }
            }
            focus_resp = latest_results.get('focus')
            if focus_resp and focus_resp.get('visualized_image'):
                adapted['focus']['visualized_image'] = focus_resp['visualized_image']

        fatigue_data = self._get_result_data(latest_results, 'fatigue')
        fatigue_resp = latest_results.get('fatigue')
        if fatigue_data:
            adapted['fatigue'] = {
                'success': True,
                'result': {
                    'rating': fatigue_data.get('rating'),
                    'ear': fatigue_data.get('ear'),
                    'mar': fatigue_data.get('mar'),
                    'face_detected': fatigue_data.get('face_detected', False),
                    'blink_count': fatigue_data.get('blink_count', 0),
                    'yawn_count': fatigue_data.get('yawn_count', 0),
                    'nod_count': fatigue_data.get('nod_count', 0)
                }
            }
            if fatigue_resp and fatigue_resp.get('visualized_image'):
                adapted['fatigue']['visualized_image'] = fatigue_resp['visualized_image']

        emotion_data = self._get_result_data(latest_results, 'emotion')
        emotion_resp = latest_results.get('emotion')
        if emotion_data:
            adapted['emotion'] = {
                'success': True,
                'result': {
                    'type': emotion_data.get('type'),
                    'confidence': emotion_data.get('confidence'),
                    'rating': emotion_data.get('rating'),
                    'scores': emotion_data.get('scores')
                }
            }
            if emotion_resp and emotion_resp.get('visualized_image'):
                adapted['emotion']['visualized_image'] = emotion_resp['visualized_image']

        face_data = self._get_result_data(latest_results, 'face_detector')
        face_resp = latest_results.get('face_detector')
        if face_data:
            adapted['face_detector'] = {
                'success': True,
                'result': {
                    'face_count': face_data.get('face_count', 0),
                    'faces': face_data.get('faces')
                }
            }
            if face_resp and face_resp.get('visualized_image'):
                adapted['face_detector']['visualized_image'] = face_resp['visualized_image']

        gesture_mantis_data = self._get_result_data(latest_results, 'gesture_mantis')
        gesture_mantis_resp = latest_results.get('gesture_mantis')
        if gesture_mantis_data:
            adapted['gesture_mantis'] = {
                'success': True,
                'result': {
                    'gesture': gesture_mantis_data.get('gesture'),
                    'confidence': gesture_mantis_data.get('confidence'),
                    'hand_detected': gesture_mantis_data.get('hand_detected', False),
                    'combo_gesture': gesture_mantis_data.get('combo_gesture'),
                }
            }
            if gesture_mantis_resp and gesture_mantis_resp.get('visualized_image'):
                adapted['gesture_mantis']['visualized_image'] = gesture_mantis_resp['visualized_image']

        pose_data = self._get_result_data(latest_results, 'pose')
        pose_resp = latest_results.get('pose')
        if pose_data:
            neck_action = pose_data.get('neck_action', {})
            adapted['pose'] = {
                'success': True,
                'result': {
                    'pose_detected': pose_data.get('pose_detected', False),
                    'neck_action': neck_action.get('name', 'None'),
                    'angle': neck_action.get('angle', 0),
                    'amplitude_score': neck_action.get('amplitude_score', 0),
                    'hold_score': neck_action.get('hold_score', 0),
                    'total_score': neck_action.get('total_score', 0),
                    'hold_duration': neck_action.get('hold_duration', 0),
                }
            }
            if pose_resp and pose_resp.get('visualized_image'):
                adapted['pose']['visualized_image'] = pose_resp['visualized_image']

        face_lm_data = self._get_result_data(latest_results, 'face_landmarker')
        face_lm_resp = latest_results.get('face_landmarker')
        if face_lm_data:
            adapted['face_landmarker'] = {
                'success': True,
                'result': {
                    'face_detected': face_lm_data.get('face_detected', False),
                    'landmarks_count': face_lm_data.get('landmarks_count', 0),
                }
            }
            if face_lm_resp and face_lm_resp.get('visualized_image'):
                adapted['face_landmarker']['visualized_image'] = face_lm_resp['visualized_image']

        face_pos_data = self._get_result_data(latest_results, 'face_position')
        face_pos_resp = latest_results.get('face_position')
        if face_pos_data:
            adapted['face_position'] = {
                'success': True,
                'result': {
                    'valid': face_pos_data.get('valid', False),
                    'face_detected': face_pos_data.get('face_detected', False),
                    'x_cm': face_pos_data.get('x_cm', 0),
                    'y_cm': face_pos_data.get('y_cm', 0),
                    'z_cm': face_pos_data.get('z_cm', 0),
                    'distance_cm': face_pos_data.get('distance_cm', 0),
                    'confidence': face_pos_data.get('confidence', 0),
                }
            }
            if face_pos_resp and face_pos_resp.get('visualized_image'):
                adapted['face_position']['visualized_image'] = face_pos_resp['visualized_image']

        # VLM 视觉问答结果
        vlm_qa_data = self._get_result_data(latest_results, 'vlm_visual_qa')
        vlm_qa_resp = latest_results.get('vlm_visual_qa')
        if vlm_qa_data:
            adapted['vlm_visual_qa'] = {
                'success': True,
                'result': {
                    'answer': vlm_qa_data.get('answer', ''),
                    'timestamp': vlm_qa_data.get('timestamp', 0),
                    'fingertip_provided': vlm_qa_data.get('fingertip_provided', False),
                }
            }
            if vlm_qa_resp and vlm_qa_resp.get('visualized_image'):
                adapted['vlm_visual_qa']['visualized_image'] = vlm_qa_resp['visualized_image']

        # VLM 游戏检测结果
        vlm_game_data = self._get_result_data(latest_results, 'vlm_game_detector')
        vlm_game_resp = latest_results.get('vlm_game_detector')
        if vlm_game_data:
            detection = vlm_game_data.get('detection', {}) or {}
            adapted['vlm_game_detector'] = {
                'success': True,
                'result': {
                    'judgment': vlm_game_data.get('judgment', ''),
                    'reason': vlm_game_data.get('reason', ''),
                    'timestamp': vlm_game_data.get('timestamp', 0),
                    'detection': detection,
                }
            }
            if vlm_game_resp and vlm_game_resp.get('visualized_image'):
                adapted['vlm_game_detector']['visualized_image'] = vlm_game_resp['visualized_image']

        return adapted

    def camera_transport_info(self):
        sock = self.camera_tcp_socket
        if sock is None or not hasattr(socket, "TCP_INFO"):
            return None
        try:
            return decode_linux_tcp_info(sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104))
        except (OSError, ValueError):
            return None

    async def monitor_event_loop(self):
        """Monotonic loop lag; report even when no camera frame arrives."""
        expected = time.monotonic() + 0.1
        window_start = time.monotonic()
        max_lag_ms = 0.0
        ticks = 0
        last_stall_log = 0.0
        while True:
            await asyncio.sleep(max(0.0, expected-time.monotonic()))
            now = time.monotonic()
            max_lag_ms = max(max_lag_ms, max(0.0, now-expected)*1000)
            ticks += 1
            expected = now + 0.1
            if (self.camera_connected.is_set() and self.rt_last_raw is not None
                    and now-self.rt_last_raw >= 0.5 and now-last_stall_log >= 1.0):
                logger.info("CAMERA_STALL raw_age_ms=%.1f loop_max_lag_ms=%.1f tcp=%s",
                            (now-self.rt_last_raw)*1000, max_lag_ms,
                            json.dumps(self.camera_transport_info()))
                last_stall_log = now
            if now-window_start >= 5.0:
                raw_age_ms = None if self.rt_last_raw is None else round((now-self.rt_last_raw)*1000, 1)
                logger.info("EVENT_LOOP_DIAG window_s=%.2f ticks=%d max_lag_ms=%.1f raw_age_ms=%s", now-window_start, ticks, max_lag_ms, raw_age_ms)
                window_start = now
                ticks = 0
                max_lag_ms = 0.0

    async def deliver_latest_frames(self):
        """Bounded latest-frame slot: browser/preprocessing never waits in recv()."""
        while True:
            await self.delivery_frame_ready.wait()
            item = self.pending_delivery_frame
            self.pending_delivery_frame = None
            self.delivery_frame_ready.clear()
            if item is None:
                continue
            data, frame_seq, frame_timestamp, received_at = item
            try:
                prep_start = time.monotonic()
                processed_data = await self._preprocess_frame(data)
                self.rt_diag["preprocess_ms"] = max(self.rt_diag["preprocess_ms"], (time.monotonic()-prep_start)*1000)
                self.rt_diag["delivery_age_ms"] = max(self.rt_diag["delivery_age_ms"], (time.monotonic()-received_at)*1000)
                if received_at < self._camera_input_epoch_at:
                    continue
                await self.detect_async(processed_data, frame_seq, frame_timestamp, received_at)
                await self.broadcast_to_frontends(processed_data, frame_seq, frame_timestamp, received_at)
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("Latest-frame delivery failed")

    def register_frontend(self, ws):
        self.frontend_clients.add(ws)
        def forget():
            self.frontend_clients.discard(ws)
            self.frontend_senders.pop(ws, None)
        sender = LatestFrontend(ws, forget)
        self.frontend_senders[ws] = sender
        sender.offer_reply({'type': 'camera_status', 'stream_id': self._camera_generation,
                            'connected': self.camera_connected.is_set(),
                            'stale': not self.camera_connected.is_set()})
        sender.offer_overlay(self.perception.hand_overlay(
            self.enabled_algorithms, self.algorithm_healthy, self.camera_connected.is_set()))
        return sender

    async def unregister_frontend(self, ws):
        self.frontend_clients.discard(ws)
        sender = self.frontend_senders.pop(ws, None)
        if sender is not None:
            await sender.close()

    def broadcast_hand_overlay(self):
        overlay = self.perception.hand_overlay(
            self.enabled_algorithms, self.algorithm_healthy, self.camera_connected.is_set())
        for ws in tuple(self.frontend_clients):
            sender = self.frontend_senders.get(ws)
            if sender is not None:
                sender.offer_overlay(overlay)

    async def broadcast_to_frontends(self, frame_bytes: bytes, frame_seq: int = None, frame_timestamp: int = None, received_at=None):
        if not self.frontend_clients:
            return
        
        if frame_seq is None:
            frame_seq = self.frame_counter
        
        server_timestamp = int(time.time() * 1000)
        
        header = json.dumps({
            'type': 'frame_header',
            'seq': frame_seq,
            'stream_id': self._camera_generation,
            'timestamp': server_timestamp
        })
        
        results_json = None
        results_snapshot = None
        with self.frame_lock:
            if self.latest_results:
                results_snapshot = dict(self.latest_results)  # 浅拷贝，减少锁持有时间

        if results_snapshot:
            adapted = self._adapt_results_for_frontend(results_snapshot)
            results_json = json.dumps({
                'type': 'algorithm_results',
                'results': adapted
            })
        
        if results_json:
            logger.debug("Sending algorithm results to %s clients, keys: %s", len(self.frontend_clients), list(self.latest_results.keys()))
        
        received_at = time.monotonic() if received_at is None else received_at
        for ws in tuple(self.frontend_clients):
            sender = self.frontend_senders.get(ws)
            if sender is not None:
                sender.offer_frame(json.loads(header), frame_bytes, received_at, results_json)
    
    async def manage_algo_ws_connection(self):
        """维护到 ESP32 /algo 端点的 WebSocket 连接（算法结果回传通道）。

        独立于主帧接收连接：连 /algo，仅接收 ESP32 对 algorithm.result 的 JSON-RPC 响应（文本帧）。
        ESP32 的 /algo 端点不会向本连接发送二进制帧（摄像头广播帧仅走 /camera）。
        断线指数退避重连。
        """
        ping_interval = int(os.environ.get('WEBSOCKET_PING_INTERVAL', 10))
        ping_timeout = int(os.environ.get('WEBSOCKET_PING_TIMEOUT', 20))
        close_timeout = int(os.environ.get('WEBSOCKET_CLOSE_TIMEOUT', 10))
        retry = 0
        while True:
            # Establish the image connection first. A broken /algo retry loop
            # must not consume all head sessions before /camera can connect.
            await self.camera_connected.wait()
            try:
                async with websockets.connect(
                    self.algo_ws_url,
                    proxy=None,  # Local device traffic must not use an HTTP proxy.
                    open_timeout=5,
                    ping_interval=ping_interval,
                    ping_timeout=ping_timeout,
                    close_timeout=close_timeout,
                    max_size=10 * 1024 * 1024
                ) as ws:
                    self.algo_ws = ws
                    self.focus_sent_source = None
                    self.focus_head_clock = None
                    self.focus_head_clock_received_at = None
                    logger.info("Connected to /algo (algo results channel): %s", self.algo_ws_url)
                    async for msg in ws:
                        retry = 0  # Receipt, not handshake alone, proves a working link.
                        # ESP32 仅返回 JSON-RPC 响应（文本帧）；/algo 端点不会发送二进制帧
                        if isinstance(msg, str):
                            logger.debug("algo ws response: %s", msg[:200])
                            try:
                                clock=json.loads(msg).get('focus_clock_ms')
                                if isinstance(clock,int) and 0<=clock<=0xffffffff:
                                    self.focus_head_clock=clock
                                    self.focus_head_clock_received_at=time.monotonic()
                            except (ValueError,AttributeError):
                                pass
            except Exception as e:
                logger.warning("algo ws disconnected: %r", e)
            finally:
                self.algo_ws = None
            retry += 1
            wait = min(2 ** min(retry, 5), 30)
            logger.info("Algo retry %s in %ss", retry, wait)
            await asyncio.sleep(wait)

    async def send_algo_results_loop(self):
        """周期向 ESP32 /algo 发送 algorithm.result JSON-RPC。"""
        interval = float(os.environ.get('ALGO_SEND_INTERVAL', '1.0'))
        logger.info("algo results send loop started, interval=%ss", interval)
        while True:
            try:
                if self.algo_ws is not None and self.camera_connected.is_set():
                    payload = self._build_algo_rpc_payload()
                    if payload is not None:
                        focus = payload.get('params', {}).get('arguments', {}).get('focus')
                        if focus and focus['source_epoch'] != self.focus_sent_source:
                            self.focus_head_clock = None
                            self.focus_head_clock_received_at = None
                            self.focus_sent_source = focus['source_epoch']
                            await self.algo_ws.send(json.dumps({'jsonrpc':'2.0', 'method':'focus.source',
                                'params':{'source_epoch':focus['source_epoch']}}))
                        if focus:
                            if self.focus_head_clock is None or self.focus_head_clock_received_at is None:
                                payload['params']['arguments'].pop('focus',None)
                            else:
                                focus['head_clock_ms']=self.focus_head_clock
                                focus['head_clock_hold_ms']=clock_residence_ms(
                                    self.focus_head_clock_received_at,time.monotonic())
                        await self.algo_ws.send(json.dumps(payload))
                        args = payload.get("params", {}).get("arguments", {})
                        logger.info("algo result sent: id=%s keys=%s", payload.get("id"), sorted(args.keys()))
            except Exception as e:
                logger.warning("algo ws send failed: %s", e)
                self.algo_ws = None
            await asyncio.sleep(interval)

    async def run(self):
        logger.info("Connecting to %s", self.ws_url)
        
        self.session = aiohttp.ClientSession()
        
        await self.check_algorithm_health()
        
        ping_interval = int(os.environ.get('WEBSOCKET_PING_INTERVAL', 10))
        ping_timeout = int(os.environ.get('WEBSOCKET_PING_TIMEOUT', 20))
        close_timeout = int(os.environ.get('WEBSOCKET_CLOSE_TIMEOUT', 10))
        
        retry_count = 0
        while True:
            if self.input_source_switch_requested and self.pending_input_source:
                logger.info("Switching input source from %s to %s", self.current_input_source, self.pending_input_source)
                self.current_input_source = self.pending_input_source
                self.ws_url = self.input_sources[self.current_input_source]
                self.pending_input_source = None
                self.input_source_switch_requested = False
                retry_count = 0
                logger.info("Now connecting to %s", self.ws_url)
            
            connection_frame_count = 0
            try:
                async with websockets.connect(
                    self.ws_url, 
                    proxy=None,
                    open_timeout=5,
                    ping_interval=ping_interval,
                    ping_timeout=ping_timeout,
                    close_timeout=close_timeout,
                    max_size=10 * 1024 * 1024,
                    max_queue=2,
                    compression=None
                ) as ws:
                    self.camera_tcp_socket = ws.transport.get_extra_info("socket")
                    logger.info("CAMERA_TCP local=%s remote=%s", ws.local_address, ws.remote_address)
                    source_name = "ESP32P4" if self.current_input_source == 'esp32p4' else "Camera"
                    logger.info("Connected to %s (ping_interval=%ss)", source_name, ping_interval)
                    
                    while True:
                        if self.input_source_switch_requested and self.pending_input_source:
                            logger.info("Input source switch requested, disconnecting...")
                            break
                        
                        try:
                            data = await ws.recv()
                            
                            if isinstance(data, bytes):
                                received_at = time.monotonic()
                                connection_frame_count += 1
                                retry_count = 0
                                frame_seq = self.esp32_frame_seq
                                frame_timestamp = int(time.time() * 1000)
                                
                                if self.pending_frame_header:
                                    try:
                                        header = json.loads(self.pending_frame_header)
                                        if header.get('type') == 'frame_header':
                                            frame_seq = header.get('seq', self.esp32_frame_seq)
                                            frame_timestamp = header.get('timestamp', frame_timestamp)
                                            frame_timestamp = int(frame_timestamp / 1000)
                                    except Exception as e:
                                        logger.warning("Failed to parse frame header: %s", e)
                                    finally:
                                        self.pending_frame_header = None
                                
                                if not self.camera_connected.is_set():
                                    logger.info("Camera first image received: %s bytes; enabling /algo", len(data))
                                    self.camera_connected.set()
                                    for sender in tuple(self.frontend_senders.values()):
                                        sender.offer_reply({'type': 'camera_status', 'stream_id': self._camera_generation,
                                                            'connected': True, 'stale': False})

                                self.rt_diag["raw"] += 1
                                if self.rt_last_raw is not None:
                                    self.rt_diag["raw_gap_ms"] = max(self.rt_diag["raw_gap_ms"], (received_at-self.rt_last_raw)*1000)
                                self.rt_last_raw, self.rt_last_source = received_at, frame_timestamp
                                if self.pending_delivery_frame is not None:
                                    self.rt_diag["overwritten"] += 1
                                if received_at-self.rt_last_log >= 5:
                                    logger.info("REALTIME_DIAG window_s=%.2f data=%s", received_at-self.rt_last_log, json.dumps(self.rt_diag))
                                    self.rt_last_log = received_at
                                    self.rt_diag = dict.fromkeys(self.rt_diag, 0)
                                    self.rt_diag["source_gap_ms"] = None
                                self.pending_delivery_frame = (data, frame_seq, frame_timestamp, received_at)
                                self.delivery_frame_ready.set()
                            else:
                                message_str = data if isinstance(data, str) else data.decode('utf-8')
                                try:
                                    message = json.loads(message_str)
                                    if message.get('type') == 'frame_header':
                                        self.pending_frame_header = message_str
                                    elif message.get('type') == 'control':
                                        algorithms = message.get('algorithms', [])
                                        self.enabled_algorithms = self._build_enabled_algorithms(algorithms)
                                        logger.info("Algorithm state updated: %s", self.enabled_algorithms)
                                except json.JSONDecodeError:
                                    pass
                        
                        except websockets.exceptions.ConnectionClosed as e:
                            logger.warning("WebSocket connection closed: %s", e)
                            break
                        except Exception as e:
                            logger.error("Error in main loop: %s", e)
                            continue
            
            except Exception as e:
                logger.error("Camera connection error: %r", e)
            finally:
                self.camera_tcp_socket = None
                self.camera_connected.clear()
                self._camera_generation += 1
                self._camera_input_epoch_at = time.monotonic()
                self.pending_frame_header = self.pending_delivery_frame = None
                self.delivery_frame_ready.clear()
                self.perception.reset_video(self._camera_generation)
                for sender in tuple(self.frontend_senders.values()):
                    sender.camera_status({'type': 'camera_status', 'stream_id': self._camera_generation,
                                          'connected': False, 'stale': True})
                self.broadcast_hand_overlay()
                self._phone_input_epoch_at = time.monotonic()
                self._invalidate_phone_use()

            # Applies to both failed handshakes AND immediate post-upgrade
            # closes. The original successful-handshake path had no backoff.
            if not self.input_source_switch_requested:
                retry_count += 1
                wait_time = camera_retry_delay(retry_count, connection_frame_count)
                logger.info("Camera retry %s in %ss", retry_count, wait_time)
                await asyncio.sleep(wait_time)
        
async def handle_index(request):
    static_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'static', 'index.html')
    with open(static_path, 'r', encoding='utf-8') as f:
        html_content = f.read()
    return web.Response(text=html_content, content_type='text/html')

async def handle_frame(request):
    client = request.app['client']
    with client.frame_lock:
        if client.latest_frame:
            return web.json_response({
                "success": True,
                "image": client.latest_frame,
                "frame_seq": client.latest_frame_seq,
                "received_images": client.received_image_count,
                "changed_images": client.changed_image_count,
                "sha256": client.latest_frame_sha256,
                "age_ms": int((time.monotonic() - client.latest_frame_received_at) * 1000),
                "camera_connected": client.camera_connected.is_set(),
                "stale": (not client.camera_connected.is_set() or
                          time.monotonic() - client.latest_frame_received_at > 3)
            })
    return web.json_response({"success": False, "error": "No frame available"})

async def handle_results(request):
    client = request.app['client']
    with client.frame_lock:
        return web.json_response({
            "success": True,
            "results": client.latest_results,
            "algorithm_diagnostics": client.algorithm_dispatcher.snapshot(),
            "focus_transport": client.focus_metadata.payload()
        })

async def handle_perception(request):
    client = request.app['client']
    include_image = getattr(request, 'query', {}).get('include_image', 'true').lower() != 'false'
    snapshot = client.perception.snapshot(
        client.enabled_algorithms, client.algorithm_healthy, client.camera_connected.is_set(),
        include_image=include_image)
    from fusion_shadow import FusionShadow
    if not hasattr(client, 'fusion_shadow'):
        client.fusion_shadow = FusionShadow()
    snapshot['fusion'] = client.fusion_shadow.evaluate(snapshot)
    return web.json_response(snapshot)

async def handle_proxy_algorithm(request):
    client = request.app['client']
    algorithm_name = request.match_info.get('algorithm')
    
    if algorithm_name not in client.algorithm_urls:
        return web.json_response({"success": False, "error": "Unknown algorithm"}, status=404)
    
    try:
        data = await request.json()
        url = client.algorithm_urls[algorithm_name]
        
        async with client.session.post(
            f"{url}/detect_visualize",
            json=data,
            timeout=aiohttp.ClientTimeout(total=5.0)
        ) as response:
            result = await response.json()
            return web.json_response(result)
    except Exception as e:
        return web.json_response({"success": False, "error": str(e)}, status=500)


async def handle_manual_vqa(request):
    """Only this explicit POST may upload a user-selected screenshot."""
    client = request.app['client']
    service_url = client.algorithm_urls['vlm_visual_qa'].rstrip('/')
    try:
        if request.method == 'POST':
            data = await request.json()
            if (not isinstance(data, dict) or not isinstance(data.get('question'), str) or
                    not data['question'].strip() or len(data['question']) > 1000 or
                    not isinstance(data.get('image'), str) or len(data['image']) > 5*1024*1024 or
                    any(isinstance(data.get(k), bool) or not isinstance(data.get(k), int) or data[k] < 0
                        for k in ('frame_seq', 'stream_id'))):
                return web.json_response({'success':False,'error':'问题、截图或帧号无效'},status=400)
            payload = {key:data[key] for key in ('question','image','frame_seq','stream_id')}
            call = client.session.post(service_url+'/trigger_async',json=payload,timeout=aiohttp.ClientTimeout(total=8))
        else:
            request_id = request.query.get('request_id','')
            if len(request_id)>64 or not request_id or any(c not in '0123456789abcdef-' for c in request_id):
                return web.json_response({'success':False,'error':'请求编号无效'},status=400)
            call = client.session.get(service_url+'/state',params={'request_id':request_id},timeout=aiohttp.ClientTimeout(total=5))
        async with call as response:
            result = await response.json()
            return web.json_response(result,status=response.status)
    except (asyncio.TimeoutError, aiohttp.ClientError):
        return web.json_response({'success':False,'code':'VQA_UNAVAILABLE','error':'视觉问答服务暂不可达；请求结果未知，请勿重复提交'},status=503)
    except Exception:
        return web.json_response({'success':False,'error':'视觉问答请求无效或服务返回异常'},status=400)


def freeze_voice_vqa_frame(client):
    """Copy the shared frame identity as one snapshot; never open /camera again.

    sampled_at is the PC's estimated frame-receipt time in Unix milliseconds,
    not the board's monotonic timestamp. The copied base64 string is immutable.
    """
    with client.frame_lock:
        now = time.monotonic()
        received_at = client.latest_frame_received_at
        image = client.latest_frame
        seq = client.latest_frame_seq
        stream_id = client._camera_generation
        if (not client.camera_connected.is_set() or
                not isinstance(image, str) or not image or len(image) > 5*1024*1024 or
                isinstance(received_at, bool) or not isinstance(received_at, (int, float)) or
                not math.isfinite(received_at) or received_at < client._camera_input_epoch_at or
                not 0 <= now-received_at <= 3.0 or
                any(isinstance(value, bool) or not isinstance(value, int) or value < 0
                    for value in (seq, stream_id))):
            return None
        age_ms = max(0, int((now-received_at)*1000))
        return {'image':image, 'frame_seq':seq, 'stream_id':stream_id,
                'sampled_at':int(time.time()*1000)-age_ms,
                'age_ms_at_capture':age_ms}


async def handle_voice_vqa(request):
    """Explicit MCP-triggered question: fix one fresh frame, then submit once."""
    client = request.app['client']
    try:
        data = await request.json()
    except Exception:
        return web.json_response({'success':False,'code':'INVALID_QUESTION','error':'问题格式无效'},status=400)
    if (not isinstance(data, dict) or set(data) != {'question'} or
            not isinstance(data['question'], str) or not data['question'].strip() or
            len(data['question']) > 1000):
        return web.json_response({'success':False,'code':'INVALID_QUESTION','error':'仅接受 1–1000 字的问题'},status=400)
    payload = freeze_voice_vqa_frame(client)
    if payload is None:
        return web.json_response({'success':False,'code':'CAMERA_NOT_FRESH',
                                  'error':'摄像头没有新鲜画面，请先启动相机'},status=503)
    payload['question'] = data['question'].strip()
    service_url = client.algorithm_urls['vlm_visual_qa'].rstrip('/')
    try:
        async with client.session.post(service_url+'/trigger_async', json=payload,
                                       timeout=aiohttp.ClientTimeout(total=8)) as response:
            result = await response.json()
            return web.json_response(result,status=response.status)
    except (asyncio.TimeoutError, aiohttp.ClientError):
        # No retry: the service may already have accepted this exact snapshot.
        return web.json_response({'success':False,'code':'VQA_UNAVAILABLE',
                                  'error':'视觉问答请求结果未知，请勿重复提交'},status=503)
    except Exception:
        return web.json_response({'success':False,'code':'VQA_UNAVAILABLE',
                                  'error':'视觉问答服务返回异常，结果未知'},status=503)


async def handle_vqa_health(request):
    """Probe VQA readiness independently of HTTP reachability and camera age."""
    client = request.app['client']
    service_url = client.algorithm_urls['vlm_visual_qa'].rstrip('/')
    try:
        async with client.session.get(service_url+'/health',timeout=aiohttp.ClientTimeout(total=3)) as response:
            result = await response.json()
            if (not isinstance(result, dict) or result.get('success') is not True or
                    result.get('service') != 'vlm-visual-qa' or
                    not isinstance(result.get('configured'), bool) or
                    not isinstance(result.get('processing'), bool)):
                raise ValueError('Unexpected health response')
            return web.json_response(result,status=response.status)
    except Exception:
        return web.json_response({'success':False,'code':'VQA_UNAVAILABLE',
                                  'error':'视觉问答服务暂不可用'},status=503)

async def handle_video_feed(request):
    client = request.app['client']
    
    response = web.StreamResponse()
    response.content_type = 'multipart/x-mixed-replace; boundary=frame'
    await response.prepare(request)
    
    last_frame_bytes = None
    last_frame_seq = 0
    frame_count = 0
    start_time = time.time()
    
    try:
        while True:
            with client.frame_lock:
                current_frame = client.latest_frame_bytes
                current_seq = client.latest_frame_seq
            
            if current_frame and current_frame != last_frame_bytes:
                frame_count += 1
                elapsed = time.time() - start_time
                if elapsed >= 1.0:
                    fps = frame_count / elapsed
                    logger.debug("MJPEG Stream FPS: %.1f, frame_seq: %s, last_seq: %s", fps, current_seq, last_frame_seq)
                    frame_count = 0
                    start_time = time.time()
                
                if current_seq < last_frame_seq:
                    logger.warning("MJPEG frame out of order: current=%s, last=%s", current_seq, last_frame_seq)
                
                last_frame_seq = current_seq
                
                await response.write(
                    b'--frame\r\n'
                    b'Content-Type: image/jpeg\r\n'
                    b'X-Frame-Seq: ' + str(current_seq).encode() + b'\r\n\r\n' + current_frame + b'\r\n'
                )
                last_frame_bytes = current_frame
            
            await asyncio.sleep(0.001)
    except (ConnectionResetError, BrokenPipeError):
        pass
    except Exception as e:
        logger.debug("MJPEG stream error: %s", e)
    
    return response

async def handle_websocket(request):
    client = request.app['client']
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    
    sender = client.register_frontend(ws)
    logger.info("Frontend WebSocket connected, total: %s", len(client.frontend_clients))
    
    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                    if data.get('type') == 'ping':
                        sender.offer_reply({'type': 'pong'})
                    elif data.get('type') == 'switch_input':
                        source = data.get('source', 'esp32p4')
                        if source in client.input_sources:
                            client.pending_input_source = source
                            client.input_source_switch_requested = True
                            sender.offer_reply({'type': 'input_source_switching', 'source': source})
                            logger.info("Input source switch requested: %s", source)
                        else:
                            sender.offer_reply({'type': 'error', 'message': f'Unknown input source: {source}'})
                    elif data.get('type') == 'control':
                        algorithms = data.get('algorithms', [])
                        client.enabled_algorithms = client._build_enabled_algorithms(algorithms)
                        logger.info("Algorithm control from Focus Lamp: %s", client.enabled_algorithms)
                        sender.offer_reply({'type': 'control_ack', 'algorithms': client.enabled_algorithms})
                        client.broadcast_hand_overlay()
                    elif data.get('type') == 'report_config':
                        share_live = data.get('shareLive', False)
                        logger.info("Report config from Focus Lamp: shareLive=%s", share_live)
                        sender.offer_reply({'type': 'report_config_ack', 'shareLive': share_live})
                except Exception:
                    pass
            elif msg.type == web.WSMsgType.ERROR:
                logger.warning("Frontend WebSocket error: %s", ws.exception())
    finally:
        await client.unregister_frontend(ws)
        logger.info("Frontend WebSocket disconnected, remaining: %s", len(client.frontend_clients))
    
    return ws

async def handle_input_source(request):
    client = request.app['client']
    return web.json_response({
        "success": True,
        "current_source": client.current_input_source,
        "available_sources": list(client.input_sources.keys())
    })

async def handle_switch_input_source(request):
    client = request.app['client']
    try:
        data = await request.json()
        source = data.get('source', 'esp32p4')
        
        if source not in client.input_sources:
            return web.json_response({"success": False, "error": f"Unknown input source: {source}"}, status=400)
        
        client.pending_input_source = source
        client.input_source_switch_requested = True
        
        return web.json_response({
            "success": True,
            "message": f"Switching to {source}",
            "current_source": client.current_input_source
        })
    except Exception as e:
        return web.json_response({"success": False, "error": str(e)}, status=500)

async def start_web_server(client, port=9000):
    app = web.Application(client_max_size=6*1024*1024)
    app['client'] = client
    
    app.router.add_get('/', handle_index)
    app.router.add_get('/frame', handle_frame)
    app.router.add_post('/vqa', handle_manual_vqa)
    app.router.add_post('/vqa/recognize', handle_voice_vqa)
    app.router.add_get('/vqa/health', handle_vqa_health)
    app.router.add_get('/vqa/state', handle_manual_vqa)
    app.router.add_get('/results', handle_results)
    app.router.add_get('/perception', handle_perception)
    app.router.add_get('/ws', handle_websocket)
    app.router.add_get('/video_feed', handle_video_feed)
    app.router.add_post('/algorithm/{algorithm}', handle_proxy_algorithm)
    app.router.add_get('/input_source', handle_input_source)
    app.router.add_post('/input_source', handle_switch_input_source)
    
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, '0.0.0.0', port)
    await site.start()
    logger.info("Web server started on port %s", port)
    return runner

async def main():
    logging.basicConfig(
        level=logging.INFO,
        format='[%(asctime)s] [%(name)s] [%(levelname)s] %(message)s',
        datefmt='%H:%M:%S'
    )
    client = MainClient()
    
    web_port = int(os.environ.get('PORT', 9000))
    await start_web_server(client, web_port)

    # 启动 ESP32 /algo 算法结果回传通道（独立于主帧接收循环）
    asyncio.create_task(client.manage_algo_ws_connection())
    asyncio.create_task(client.send_algo_results_loop())
    # 周期健康检查：算法容器挂掉后自动停止分发请求，恢复后自动续传
    asyncio.create_task(client.health_check_loop())
    asyncio.create_task(client.deliver_latest_frames())
    loop_monitor = asyncio.create_task(client.monitor_event_loop())

    try:
        await client.run()
    finally:
        loop_monitor.cancel()
        await asyncio.gather(loop_monitor, return_exceptions=True)
        await client.algorithm_dispatcher.close()
        await asyncio.gather(*(client.unregister_frontend(ws)
                               for ws in tuple(client.frontend_senders)))

if __name__ == '__main__':
    asyncio.run(main())

