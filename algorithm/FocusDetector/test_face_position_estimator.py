"""
独立测试 FacePositionEstimator

验证目标：
1. 同时运行 FaceLandmarker + PoseLandmarker，融合输出 FacePosition
2. X 坐标：用户左右移动时数值变化（+ 右 / - 左）
3. Y 坐标：用户上下移动时数值变化（+ 下 / - 上）
4. Z 坐标：用户低头/后仰时数值变化（+ 低头 / - 后仰）
5. 滤波平滑性：无突变
6. 异常值剔除：手动快速晃动时无突变

操作：
  - 按 'q' 退出
  - 按 'd' 切换调试信息显示

使用方法：
  1. 启动后正对摄像头，保持头部可见
  2. 缓慢左右移动头部观察 X 变化
  3. 缓慢上下移动头部观察 Y 变化
  4. 缓慢低头/后仰观察 Z 变化
"""

import os
import sys
import time
import math
import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from detectors.face_landmarker.mediapipe import FaceLandmarkerProvider
from detectors.pose_detector.mediapipe import PoseDetectorProvider
from detectors.face_position_estimator.face_mesh_estimator import FacePositionEstimator
from detectors.face_position_estimator.base import FacePosition

# 配置常量
CAMERA_INDEX = 0
FACE_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "checkpoint", "face_landmarker.task")
POSE_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "checkpoint", "pose_landmarker_full.task")

# 默认配置（与 config/face_position.yaml 一致）
DEFAULT_CONFIG = {
    'face_position': {
        'pupil_distance_cm': 6.3,
        'head_length_cm': 23.0,
        'camera_fov_degrees': 60.0,
        'image_center_mode': 'geometric',
        'calibrated_cx': None,
        'calibrated_cy': None,
        'min_visibility': 0.5,
        'smoothing_alpha': 0.3,
        'outlier_jump_threshold_cm': 30.0,
        'invalid_reset_frames': 10,
    }
}

# 颜色（BGR）
COLOR_VALID = (0, 255, 0)        # 绿色：有效
COLOR_INVALID = (100, 100, 100)  # 灰色：无效
COLOR_AXIS_X = (0, 0, 255)       # 红色：X 轴
COLOR_AXIS_Y = (255, 0, 0)       # 蓝色：Y 轴
COLOR_AXIS_Z = (0, 255, 255)     # 黄色：Z 轴


def main():
    print("=" * 60)
    print("FacePositionEstimator 独立测试")
    print("=" * 60)
    print(f"Face 模型: {FACE_MODEL_PATH}")
    print(f"Pose 模型: {POSE_MODEL_PATH}")
    print(f"配置: {DEFAULT_CONFIG['face_position']}")
    print("-" * 60)
    print("测试步骤：")
    print("  1. 正对摄像头坐好")
    print("  2. 左右移动头部 → 观察 X 变化（+ 右 / - 左）")
    print("  3. 上下移动头部 → 观察 Y 变化（+ 下 / - 上）")
    print("  4. 低头/后仰 → 观察 Z 变化（+ 低头 / - 后仰）")
    print("  5. 快速晃动 → 验证无突变")
    print("按 'q' 退出 | 'd' 切换调试")
    print("=" * 60)

    # 检查模型文件
    if not os.path.exists(FACE_MODEL_PATH):
        print(f"[错误] Face 模型不存在: {FACE_MODEL_PATH}")
        return
    if not os.path.exists(POSE_MODEL_PATH):
        print(f"[错误] Pose 模型不存在: {POSE_MODEL_PATH}")
        return

    # 初始化检测器
    print("[初始化] 加载 FaceLandmarker...")
    t0 = time.time()
    try:
        face_landmarker = FaceLandmarkerProvider(
            model_path=FACE_MODEL_PATH,
            num_faces=1,
            min_face_detection_confidence=0.5,
            min_face_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
    except Exception as e:
        print(f"[错误] FaceLandmarker 初始化失败: {e}")
        import traceback
        traceback.print_exc()
        return
    print(f"[初始化完成] FaceLandmarker 耗时 {time.time()-t0:.2f}s")

    print("[初始化] 加载 PoseLandmarker...")
    t0 = time.time()
    try:
        pose_landmarker = PoseDetectorProvider(
            model_path=POSE_MODEL_PATH,
            min_pose_detection_confidence=0.5,
            min_pose_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
    except Exception as e:
        print(f"[错误] PoseLandmarker 初始化失败: {e}")
        import traceback
        traceback.print_exc()
        return
    print(f"[初始化完成] PoseLandmarker 耗时 {time.time()-t0:.2f}s")

    # 初始化估计器
    estimator = FacePositionEstimator(DEFAULT_CONFIG)

    # 打开摄像头
    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"[错误] 无法打开摄像头 (index={CAMERA_INDEX})")
        return

    print("[摄像头] 已打开，开始检测...")
    print("-" * 60)

    show_debug = True
    frame_count = 0
    last_print_time = time.time()
    valid_count = 0
    invalid_count = 0

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("[错误] 读取摄像头帧失败")
                break

            frame = cv2.flip(frame, 1)
            rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            h, w = frame.shape[:2]

            # 异步检测
            face_landmarker.run(rgb_frame)
            pose_landmarker.run(rgb_frame)

            face_result = face_landmarker.get_detect_result()
            pose_result = pose_landmarker.get_detect_result()

            # 估计面部位置
            face_pos = estimator.update(face_result, pose_result, w, h)

            frame_count += 1
            if face_pos.valid:
                valid_count += 1
            else:
                invalid_count += 1

            # 显示
            display = frame.copy()

            # 绘制坐标可视化（画面中心十字 + 鼻尖位置标记）
            cx_px, cy_px = w // 2, h // 2
            cv2.line(display, (cx_px - 20, cy_px), (cx_px + 20, cy_px), (200, 200, 200), 1)
            cv2.line(display, (cx_px, cy_px - 20), (cx_px, cy_px + 20), (200, 200, 200), 1)

            # 在鼻尖像素位置画标记
            if face_result and face_result.get('face_landmarks'):
                nose_face = face_result['face_landmarks'][1]  # NOSE_TIP = 1
                nose_px = (int(nose_face.x * w), int(nose_face.y * h))
                color = COLOR_VALID if face_pos.valid else COLOR_INVALID
                cv2.circle(display, nose_px, 8, color, -1)
                cv2.circle(display, nose_px, 8, (0, 0, 0), 1)
                # 从原点到鼻尖的连线
                cv2.line(display, (cx_px, cy_px), nose_px, color, 1)

            # 顶部状态栏
            overlay = display.copy()
            cv2.rectangle(overlay, (0, 0), (w, 160), (0, 0, 0), -1)
            display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

            # 主坐标显示（大字号）
            if face_pos.valid:
                color = COLOR_VALID
                main_text = f"XYZ: ({face_pos.x_cm:+.1f}, {face_pos.y_cm:+.1f}, {face_pos.z_cm:+.1f}) Dist: {face_pos.distance_cm:.1f}cm"
            else:
                color = COLOR_INVALID
                if face_pos.x_cm != 0 or face_pos.y_cm != 0 or face_pos.z_cm != 0:
                    main_text = f"XYZ: ({face_pos.x_cm:+.1f}, {face_pos.y_cm:+.1f}, {face_pos.z_cm:+.1f}) Dist: {face_pos.distance_cm:.1f}cm [INVALID]"
                else:
                    main_text = "Face XYZ: -- (no detection)"

            cv2.putText(display, main_text, (10, 30),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)

            # 分轴显示
            cv2.putText(display, f"X: {face_pos.x_cm:+6.1f} cm  (+right/-left)",
                        (10, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_AXIS_X, 1)
            cv2.putText(display, f"Y: {face_pos.y_cm:+6.1f} cm  (+up/-down)",
                        (10, 80), cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_AXIS_Y, 1)
            cv2.putText(display, f"Z: {face_pos.z_cm:+6.1f} cm  (+nod/-lean back)",
                        (10, 100), cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_AXIS_Z, 1)
            cv2.putText(display, f"Dist: {face_pos.distance_cm:6.1f} cm  (distance to camera)",
                        (10, 120), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 200, 200), 1)

            # 置信度和有效性
            cv2.putText(display, f"Confidence: {face_pos.confidence:.2f}  Valid: {face_pos.valid}",
                        (10, 145), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)

            # 调试信息
            if show_debug and face_result and face_result.get('face_landmarks') and pose_result and pose_result.get('pose_landmarks'):
                face_lms = face_result['face_landmarks']
                pose_lms = pose_result['pose_landmarks']

                # 瞳距像素
                left_eye = face_lms[33]
                right_eye = face_lms[263]
                pupil_dist_px = math.sqrt(
                    (right_eye.x - left_eye.x) ** 2 +
                    (right_eye.y - left_eye.y) ** 2
                ) * w
                pupil_dist_norm = math.sqrt(
                    (right_eye.x - left_eye.x) ** 2 +
                    (right_eye.y - left_eye.y) ** 2
                )

                # Pose 关键点可见度
                nose_vis = pose_lms[0][3]
                l_sh_vis = pose_lms[11][3]
                r_sh_vis = pose_lms[12][3]

                # Pose z 差值
                nose_z = pose_lms[0][2]
                mid_sh_z = (pose_lms[11][2] + pose_lms[12][2]) / 2
                z_diff = nose_z - mid_sh_z

                cv2.putText(display, f"PupilDist: {pupil_dist_px:.1f}px (norm={pupil_dist_norm:.3f})",
                            (10, 145), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (180, 180, 180), 1)

                # 右侧调试面板
                dbg_lines = [
                    f"Nose vis: {nose_vis:.2f}",
                    f"L sh vis: {l_sh_vis:.2f}",
                    f"R sh vis: {r_sh_vis:.2f}",
                    f"Nose z: {nose_z:+.3f}",
                    f"MidSh z: {mid_sh_z:+.3f}",
                    f"z_diff: {z_diff:+.3f}",
                ]
                for i, line in enumerate(dbg_lines):
                    cv2.putText(display, line, (w - 200, 25 + i * 18),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.4, (180, 180, 180), 1)

            # 底部状态
            total = valid_count + invalid_count
            valid_rate = valid_count / total * 100 if total > 0 else 0
            cv2.putText(display, f"Frame: {frame_count} | Valid: {valid_count} ({valid_rate:.0f}%) | Invalid: {invalid_count}",
                        (10, h - 15), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (200, 200, 200), 1)

            cv2.imshow("FacePosition Test", display)

            # 每秒打印一次详细日志
            if time.time() - last_print_time > 1.0:
                print(f"[frame {frame_count}] valid={face_pos.valid} "
                      f"X={face_pos.x_cm:+.2f} Y={face_pos.y_cm:+.2f} Z={face_pos.z_cm:+.2f} "
                      f"Dist={face_pos.distance_cm:.2f}cm "
                      f"conf={face_pos.confidence:.2f}")
                last_print_time = time.time()

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('d'):
                show_debug = not show_debug
                print(f"[切换] 调试信息: {'ON' if show_debug else 'OFF'}")

    except KeyboardInterrupt:
        print("\n[用户中断]")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        face_landmarker.close()
        pose_landmarker.close()
        print(f"[统计] 总帧数: {frame_count}, 有效: {valid_count} ({valid_count/max(frame_count,1)*100:.0f}%), 无效: {invalid_count}")
        print("[清理] 资源已释放")


if __name__ == "__main__":
    main()
