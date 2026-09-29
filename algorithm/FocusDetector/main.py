import sys
import os
import time
import cv2
from contextlib import ExitStack

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.config import read_config, read_config_or_default
from utils.logger import setup_logging
from detectors.emotion_detector.emotiefflib import EmotionDetectorProvider
from detectors.gesture_detector.mediapipe import GestureDetectorProvider
from detectors.combo_gesture import ComboGestureDetector, EMOTION_GESTURE_COMBOS
from detectors.pose_detector.mediapipe import PoseDetectorProvider
from detectors.pose_detector.neck_exercise import NeckExerciseDetector, NECK_ACTION_COLORS
from detectors.face_landmarker.mediapipe import FaceLandmarkerProvider
from detectors.face_position_estimator.face_mesh_estimator import FacePositionEstimator

from ui.colors import (
    get_emotion_color, get_gesture_color,
    COMBO_GESTURE_COLORS, EMOTION_COMBO_COLORS,
)
from ui.draw import draw_hand_landmarks, draw_pose_landmarks
from ui.overlay import draw_overlay

TAG = "main"


def main():
    config = read_config("config/main.yaml")
    logger = setup_logging(log_level=config.get("log_level", "DEBUG"))

    camera_index = config.get("camera_index", 0)

    # 使用 ExitStack 统一管理 detector 资源（自动调用 close）
    with ExitStack() as stack:
        # 初始化情绪检测器（同步 API）
        logger.info("Initializing emotion detector...")
        emotion_config = read_config("config/emotion_detector.yaml")
        emotion_detector = stack.enter_context(EmotionDetectorProvider(device=emotion_config.get("device", "cpu")))
        logger.info("Emotion detector initialized.")

        # 初始化手势检测器（MediaPipe 异步）
        logger.info("Initializing gesture detector...")
        gesture_config = read_config("config/gesture_detector.yaml")
        gesture_detector = stack.enter_context(GestureDetectorProvider(
            model_path=gesture_config.get("model_path", "checkpoint/gesture_recognizer.task"),
            num_hands=gesture_config.get("num_hands", 1),
            min_hand_detection_confidence=gesture_config.get("min_hand_detection_confidence", 0.5),
            min_hand_presence_confidence=gesture_config.get("min_hand_presence_confidence", 0.5),
            min_tracking_confidence=gesture_config.get("min_tracking_confidence", 0.5),
        ))
        logger.info("Gesture detector initialized.")

        # 初始化肢体检测器（MediaPipe 异步）
        logger.info("Initializing pose detector...")
        pose_config = read_config("config/pose_detector.yaml")
        pose_detector = stack.enter_context(PoseDetectorProvider(
            model_path=pose_config.get("model_path", "checkpoint/pose_landmarker_full.task"),
            min_pose_detection_confidence=pose_config.get("min_pose_detection_confidence", 0.5),
            min_pose_presence_confidence=pose_config.get("min_pose_presence_confidence", 0.5),
            min_tracking_confidence=pose_config.get("min_tracking_confidence", 0.5),
        ))
        logger.info("Pose detector initialized.")

        # 颈部保健操检测器（无外部资源，无需进 ExitStack）
        neck_detector = NeckExerciseDetector()
        logger.info("Neck exercise detector initialized.")

        # 组合手势检测器
        combo_detector = ComboGestureDetector()
        last_combo = None

        # 面部空间坐标估计器
        face_pos_config = read_config("config/face_position.yaml")
        face_pos_enabled = face_pos_config.get("face_position", {}).get("enabled", True)
        face_landmarker = None
        face_pos_estimator = None
        if face_pos_enabled:
            logger.info("Initializing face landmarker...")
            # face_detector.yaml 不存在时使用默认空配置（不报错，仅 warning）
            face_config = read_config_or_default("config/face_detector.yaml", default={})
            face_landmarker = stack.enter_context(FaceLandmarkerProvider(
                model_path=face_config.get("model_path", "checkpoint/face_landmarker.task"),
                num_faces=face_config.get("num_faces", 1),
                min_face_detection_confidence=face_config.get("min_face_detection_confidence", 0.5),
                min_face_presence_confidence=face_config.get("min_face_presence_confidence", 0.5),
                min_tracking_confidence=face_config.get("min_tracking_confidence", 0.5),
            ))
            face_pos_estimator = FacePositionEstimator(face_pos_config)
            logger.info("Face position estimator initialized.")

        cap = cv2.VideoCapture(camera_index)
        if not cap.isOpened():
            logger.error(f"Cannot open camera at index {camera_index}")
            return

        logger.info("Camera opened. Press 'q' to quit.")

        try:
            # FPS 计算（一阶低通滤波，避免抖动）
            prev_frame_time = time.time()
            fps = 0.0

            while True:
                ret, frame = cap.read()
                if not ret:
                    logger.error("Failed to read frame from camera.")
                    break

                frame = cv2.flip(frame, 1)
                rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

                # 情绪检测
                emotion_result = emotion_detector.detect(image=rgb_frame)
                emotion_text = emotion_result['emotion'] if emotion_result else "No face"
                emotion_color = get_emotion_color(emotion_text)

                # 手势检测
                gesture_detector.run(image=rgb_frame)
                gesture_result = gesture_detector.get_detect_result()
                gesture_text = gesture_result['gesture'] if gesture_result else "No gesture"
                gesture_color = get_gesture_color(gesture_text)

                # 肢体检测
                pose_detector.run(image=rgb_frame)
                pose_result = pose_detector.get_detect_result()

                # 颈部保健操检测
                pose_landmarks = pose_result['pose_landmarks'] if pose_result else None
                neck_action = neck_detector.update(pose_landmarks)
                neck_color = NECK_ACTION_COLORS.get(neck_action.name, (200, 200, 200))

                # 面部空间坐标估计
                face_pos = None
                if face_landmarker is not None and face_pos_estimator is not None:
                    face_landmarker.run(image=rgb_frame)
                    face_result = face_landmarker.get_detect_result()
                    h, w = frame.shape[:2]
                    face_pos = face_pos_estimator.update(face_result, pose_result, w, h)

                # 组合手势检测
                current_single_gesture = gesture_result['gesture'] if gesture_result else None
                combo_result = combo_detector.update(current_single_gesture)
                if combo_result:
                    last_combo = combo_result
                combo_color = COMBO_GESTURE_COLORS.get(last_combo, (200, 200, 200)) if last_combo else (200, 200, 200)
                combo_text = last_combo if last_combo else "None"

                # 情绪+手势联动（实时状态：只有当前帧同时满足才显示）
                current_emotion = emotion_result['emotion'] if emotion_result else None
                emotion_combo_key = (current_emotion, current_single_gesture)
                emotion_combo_name = EMOTION_GESTURE_COMBOS.get(emotion_combo_key) if current_emotion and current_single_gesture else None
                emotion_combo_color = EMOTION_COMBO_COLORS.get(emotion_combo_name, (200, 200, 200)) if emotion_combo_name else (200, 200, 200)
                emotion_combo_text = emotion_combo_name if emotion_combo_name else "None"

                # 计算 FPS（一阶低通滤波）
                new_frame_time = time.time()
                dt = new_frame_time - prev_frame_time
                if dt > 0:
                    current_fps = 1.0 / dt
                    # 一阶低通：10% 当前 + 90% 历史，避免数值抖动
                    fps = 0.1 * current_fps + 0.9 * fps
                prev_frame_time = new_frame_time

                # 在画面上显示结果
                display_frame = frame.copy()
                draw_overlay(
                    display_frame,
                    emotion_text=emotion_text, emotion_color=emotion_color,
                    gesture_text=gesture_text, gesture_color=gesture_color,
                    combo_text=combo_text, combo_color=combo_color,
                    pose_detected=bool(pose_result and pose_result.get('pose_landmarks')),
                    neck_action=neck_action, neck_color=neck_color,
                    face_pos=face_pos,
                    emotion_combo_text=emotion_combo_text, emotion_combo_color=emotion_combo_color,
                    fps=fps,
                )

                # 绘制手部关键点
                if gesture_result and gesture_result.get('hand_landmarks'):
                    draw_hand_landmarks(display_frame, gesture_result['hand_landmarks'])

                # 绘制肢体关键点
                if pose_result and pose_result.get('pose_landmarks'):
                    draw_pose_landmarks(display_frame, pose_result['pose_landmarks'])

                cv2.imshow("FocusDetector", display_frame)

                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
        except KeyboardInterrupt:
            logger.info("Interrupted by user.")
        finally:
            cap.release()
            cv2.destroyAllWindows()
            # detector 的 close() 由 ExitStack 自动调用
            logger.info("Resources released. Bye.")


if __name__ == "__main__":
    main()
