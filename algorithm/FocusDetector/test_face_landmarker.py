"""
独立测试 FaceLandmarkerProvider

验证目标：
1. 模型加载成功
2. 能从摄像头获取 478 个关键点（468 面部 + 10 虹膜）
3. 显示鼻尖(1)、左眼外角(33)、右眼外角(263) 的归一化坐标
4. 计算并显示瞳距像素值
5. 在画面上可视化关键点

操作：
  - 按 'q' 退出
  - 按 'd' 切换全关键点可视化
"""

import os
import sys
import time
import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from detectors.face_landmarker.mediapipe import FaceLandmarkerProvider

# 关键点索引（MediaPipe Face Mesh）
NOSE_TIP = 1
LEFT_EYE_OUTER = 33
RIGHT_EYE_OUTER = 263

# 摄像头索引
CAMERA_INDEX = 0
# 模型路径
MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "checkpoint", "face_landmarker.task")

# 颜色（BGR）
COLOR_NOSE = (0, 255, 255)        # 黄色
COLOR_LEFT_EYE = (255, 0, 0)     # 蓝色
COLOR_RIGHT_EYE = (0, 0, 255)    # 红色
COLOR_ALL_POINTS = (200, 200, 200)  # 浅灰
COLOR_PUPIL_LINE = (0, 255, 0)   # 绿色


def main():
    print("=" * 60)
    print("FaceLandmarkerProvider 独立测试")
    print("=" * 60)
    print(f"模型路径: {MODEL_PATH}")
    print(f"摄像头索引: {CAMERA_INDEX}")
    print(f"关注关键点: 鼻尖(1)、左眼外角(33)、右眼外角(263)")
    print("按 'q' 退出 | 'd' 切换全关键点显示")
    print("=" * 60)

    # 检查模型文件
    if not os.path.exists(MODEL_PATH):
        print(f"[错误] 模型文件不存在: {MODEL_PATH}")
        return

    # 初始化检测器
    print("[初始化] 加载 FaceLandmarkerProvider...")
    t0 = time.time()
    try:
        detector = FaceLandmarkerProvider(
            model_path=MODEL_PATH,
            num_faces=1,
            min_face_detection_confidence=0.5,
            min_face_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
    except Exception as e:
        print(f"[错误] 初始化失败: {e}")
        import traceback
        traceback.print_exc()
        return
    print(f"[初始化完成] 耗时 {time.time()-t0:.2f}s")

    # 打开摄像头
    cap = cv2.VideoCapture(CAMERA_INDEX)
    if not cap.isOpened():
        print(f"[错误] 无法打开摄像头 (index={CAMERA_INDEX})")
        return

    print("[摄像头] 已打开")
    print("-" * 60)

    show_all_points = False
    frame_count = 0
    last_print_time = time.time()

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
            detector.run(rgb_frame)
            result = detector.get_detect_result()

            frame_count += 1
            display = frame.copy()

            if result and result.get('face_landmarks'):
                landmarks = result['face_landmarks']
                num_points = len(landmarks)

                # 提取关键点像素坐标
                def to_px(lm):
                    return int(lm.x * w), int(lm.y * h)

                nose_px = to_px(landmarks[NOSE_TIP])
                l_eye_px = to_px(landmarks[LEFT_EYE_OUTER])
                r_eye_px = to_px(landmarks[RIGHT_EYE_OUTER])

                # 计算瞳距（像素）
                pupil_dist_px = np.sqrt(
                    (landmarks[RIGHT_EYE_OUTER].x - landmarks[LEFT_EYE_OUTER].x) ** 2 +
                    (landmarks[RIGHT_EYE_OUTER].y - landmarks[LEFT_EYE_OUTER].y) ** 2
                ) * w

                # 绘制瞳距连线
                cv2.line(display, l_eye_px, r_eye_px, COLOR_PUPIL_LINE, 2)

                # 绘制关键点（带标签）
                cv2.circle(display, nose_px, 6, COLOR_NOSE, -1)
                cv2.circle(display, nose_px, 6, (0, 0, 0), 1)
                cv2.putText(display, "nose", (nose_px[0]+8, nose_px[1]),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_NOSE, 1)

                cv2.circle(display, l_eye_px, 5, COLOR_LEFT_EYE, -1)
                cv2.putText(display, "L_eye", (l_eye_px[0]+8, l_eye_px[1]),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_LEFT_EYE, 1)

                cv2.circle(display, r_eye_px, 5, COLOR_RIGHT_EYE, -1)
                cv2.putText(display, "R_eye", (r_eye_px[0]+8, r_eye_px[1]),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_RIGHT_EYE, 1)

                # 全关键点可视化
                if show_all_points:
                    for lm in landmarks:
                        px = int(lm.x * w)
                        py = int(lm.y * h)
                        cv2.circle(display, (px, py), 1, COLOR_ALL_POINTS, -1)

                # 顶部状态栏
                overlay = display.copy()
                cv2.rectangle(overlay, (0, 0), (w, 100), (0, 0, 0), -1)
                display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)

                cv2.putText(display, f"Points: {num_points}", (10, 25),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
                cv2.putText(display, f"Nose: ({landmarks[NOSE_TIP].x:.3f}, {landmarks[NOSE_TIP].y:.3f}, z={landmarks[NOSE_TIP].z:.3f})",
                            (10, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_NOSE, 1)
                cv2.putText(display, f"L_eye: ({landmarks[LEFT_EYE_OUTER].x:.3f}, {landmarks[LEFT_EYE_OUTER].y:.3f})",
                            (10, 70), cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_LEFT_EYE, 1)
                cv2.putText(display, f"R_eye: ({landmarks[RIGHT_EYE_OUTER].x:.3f}, {landmarks[RIGHT_EYE_OUTER].y:.3f})",
                            (10, 90), cv2.FONT_HERSHEY_SIMPLEX, 0.5, COLOR_RIGHT_EYE, 1)

                # 右侧瞳距信息
                cv2.putText(display, f"PupilDist: {pupil_dist_px:.1f}px",
                            (w - 250, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, COLOR_PUPIL_LINE, 2)

                # 每秒打印一次详细日志
                if time.time() - last_print_time > 1.0:
                    print(f"[frame {frame_count}] points={num_points}, "
                          f"nose=({landmarks[NOSE_TIP].x:.3f},{landmarks[NOSE_TIP].y:.3f},z={landmarks[NOSE_TIP].z:.3f}), "
                          f"pupil_dist={pupil_dist_px:.1f}px")
                    last_print_time = time.time()
            else:
                # 未检测到脸
                overlay = display.copy()
                cv2.rectangle(overlay, (0, 0), (w, 50), (0, 0, 0), -1)
                display = cv2.addWeighted(overlay, 0.6, display, 0.4, 0)
                cv2.putText(display, "No face detected", (10, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (100, 100, 100), 2)

            cv2.imshow("FaceLandmarker Test", display)

            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('d'):
                show_all_points = not show_all_points
                print(f"[切换] 全关键点显示: {'ON' if show_all_points else 'OFF'}")

    except KeyboardInterrupt:
        print("\n[用户中断]")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        detector.close()
        print("[清理] 资源已释放")


if __name__ == "__main__":
    main()
