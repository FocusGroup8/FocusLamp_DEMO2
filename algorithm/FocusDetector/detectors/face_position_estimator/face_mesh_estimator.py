"""
Face Mesh + Pose 融合的面部空间坐标估计器

输入：FaceLandmarker 和 PoseLandmarker 的检测结果
输出：FacePosition（X/Y/Z cm + distance_cm + 置信度）

算法：
  - X/Y：用 Face Mesh 的鼻尖归一化坐标减原点，除以瞳距归一化值，乘瞳距物理先验 (6.3cm)
    公式：X_cm = (nose_x_norm - cx_norm) / pupil_dist_norm * pupil_distance_cm
          Y_cm = -(nose_y_norm - cy_norm) / pupil_dist_norm * pupil_distance_cm  ← Y 取负号使 + 向上
    特性：用归一化坐标比值，与图像尺寸无关
  - Z：用 Pose 的鼻尖 z 减双肩中点 z，乘头长物理先验 (23cm)
    语义：Z 表示鼻尖相对双肩中点的深度差（+ 低头 / - 后仰）
    限制：单目 Pose 无法给绝对距离，Z 是相对深度，不是到摄像头距离
  - distance_cm：用针孔模型 + FOV 假设估算鼻尖到摄像头绝对距离
    公式：distance_cm = pupil_distance_cm / (2 × tan(FOV/2) × pupil_dist_norm)
    限制：依赖 FOV 假设（默认 60°），未标定精度 ±10~20%
  - 滤波：一阶低通 α + 异常值剔除 + 连续无效重置

坐标系（修订后）：
  - 原点：摄像头光心（Phase 1 用画面几何中心近似）
  - X 轴：水平向右（+ 右 / - 左）
  - Y 轴：垂直向上（+ 上 / - 下）
  - Z 轴：沿光轴朝向用户（+ 朝向用户，正值）
  - 单位：厘米 cm
"""

import math

from detectors.face_position_estimator.base import FacePosition
from utils.logger import setup_logging

TAG = __name__


# MediaPipe Face Mesh 关键点索引
NOSE_TIP = 1                  # 鼻尖
LEFT_EYE_OUTER = 33           # 左眼外角
RIGHT_EYE_OUTER = 263         # 右眼外角

# MediaPipe Pose 关键点索引
POSE_NOSE = 0
POSE_LEFT_SHOULDER = 11
POSE_RIGHT_SHOULDER = 12


class FacePositionEstimator:
    """Face Mesh + Pose 融合的面部空间坐标估计器"""

    def __init__(self, config: dict):
        """
        Args:
            config: 配置字典。可以是 {'face_position': {...}} 或直接 {...}。
                    必需字段：pupil_distance_cm, head_length_cm, min_visibility,
                              smoothing_alpha, outlier_jump_threshold_cm, invalid_reset_frames
                    可选字段：image_center_mode, calibrated_cx, calibrated_cy, camera_fov_degrees
        """
        self.logger = setup_logging()
        # 兼容两种 config 形式
        if 'face_position' in config:
            self.cfg = config['face_position']
        else:
            self.cfg = config

        # 预计算针孔模型系数（依赖 FOV，不随帧变化）
        # distance_cm = pupil_distance_cm / (2 × tan(FOV/2) × pupil_dist_norm)
        # 提取常数部分：k = 1 / (2 × tan(FOV/2))
        fov_deg = self.cfg.get('camera_fov_degrees', 60.0)
        fov_rad = math.radians(fov_deg)
        self._pinhole_k = 1.0 / (2.0 * math.tan(fov_rad / 2.0))

        # 滤波状态
        self._last_valid_pos: FacePosition | None = None
        self._invalid_count = 0

    @classmethod
    def from_config_file(cls, config_path: str) -> "FacePositionEstimator":
        """从 YAML 配置文件构造"""
        from utils.config import read_config
        return cls(read_config(config_path))

    def update(
        self,
        face_result: dict | None,
        pose_result: dict | None,
        image_w: int,
        image_h: int,
    ) -> FacePosition:
        """更新坐标估计

        Args:
            face_result: FaceLandmarkerProvider.get_detect_result() 返回值，含 face_landmarks
            pose_result: PoseDetectorProvider.get_detect_result() 返回值，含 pose_landmarks
            image_w: 图像宽度（像素），保留接口为未来扩展
            image_h: 图像高度（像素），保留接口为未来扩展

        Returns:
            FacePosition
        """
        # 1. 检查 Face Mesh 有效性
        if not face_result or not face_result.get('face_landmarks'):
            return self._handle_invalid()
        face_lms = face_result['face_landmarks']

        # 2. 检查 Pose 有效性
        if not pose_result or not pose_result.get('pose_landmarks'):
            return self._handle_invalid()
        pose_lms = pose_result['pose_landmarks']  # List[(x, y, z, visibility)]

        # 3. 关键点索引检查
        needed_face_indices = [NOSE_TIP, LEFT_EYE_OUTER, RIGHT_EYE_OUTER]
        if any(idx >= len(face_lms) for idx in needed_face_indices):
            return self._handle_invalid()

        if POSE_NOSE >= len(pose_lms) or POSE_LEFT_SHOULDER >= len(pose_lms) or POSE_RIGHT_SHOULDER >= len(pose_lms):
            return self._handle_invalid()

        # 4. Pose 可见度检查
        nose_x, nose_y, _nose_z, nose_vis = pose_lms[POSE_NOSE]
        _l_sh_x, _l_sh_y, _l_sh_z, l_sh_vis = pose_lms[POSE_LEFT_SHOULDER]
        _r_sh_x, _r_sh_y, _r_sh_z, r_sh_vis = pose_lms[POSE_RIGHT_SHOULDER]

        min_vis = self.cfg.get('min_visibility', 0.5)
        if min(nose_vis, l_sh_vis, r_sh_vis) < min_vis:
            return self._handle_invalid()

        # 5. 计算瞳距（归一化）
        left_eye = face_lms[LEFT_EYE_OUTER]
        right_eye = face_lms[RIGHT_EYE_OUTER]
        nose_face = face_lms[NOSE_TIP]

        pupil_dist_norm = math.sqrt(
            (right_eye.x - left_eye.x) ** 2 +
            (right_eye.y - left_eye.y) ** 2
        )
        # 归一化瞳距过小（< 0.5% 画面宽）→ 视为无效
        if pupil_dist_norm < 0.005:
            return self._handle_invalid()

        # 6. 计算 X/Y (cm)
        # 用归一化坐标的比值，与图像尺寸无关
        # X + 向右：nose.x 增大 = 向右，直接用
        # Y + 向上：nose.y 增大 = 向下（图像坐标），取负号使其变为 + 向上
        pupil_distance_cm = self.cfg.get('pupil_distance_cm', 6.3)
        cx_norm, cy_norm = self._get_center_norm()

        x_cm = (nose_face.x - cx_norm) / pupil_dist_norm * pupil_distance_cm
        y_cm = -(nose_face.y - cy_norm) / pupil_dist_norm * pupil_distance_cm  # Y 取负号

        # 7. 计算 Z (cm) - Pose z 差值 × 头长先验
        # MediaPipe Pose z：相对臀部中点归一化，单位约等于"头长"
        # z 越小（负值越大）= 关键点越靠近摄像头
        # 鼻尖 z - 双肩中点 z：负值 = 鼻尖在双肩前面（低头），正值 = 鼻尖在双肩后面（后仰）
        nose_z = pose_lms[POSE_NOSE][2]
        mid_shoulder_z = (pose_lms[POSE_LEFT_SHOULDER][2] + pose_lms[POSE_RIGHT_SHOULDER][2]) / 2
        z_diff = nose_z - mid_shoulder_z

        head_length_cm = self.cfg.get('head_length_cm', 23.0)
        # Z 轴约定：+ 朝向用户。MediaPipe z 越小越靠近摄像头，所以取负号让"低头时 Z 为正"
        z_cm = -z_diff * head_length_cm

        # 8. 计算 distance_cm - 鼻尖到摄像头绝对距离（针孔模型 + FOV 假设）
        # 公式：distance_cm = pupil_distance_cm / (2 × tan(FOV/2) × pupil_dist_norm)
        #    = self._pinhole_k × pupil_distance_cm / pupil_dist_norm
        distance_cm = self._pinhole_k * pupil_distance_cm / pupil_dist_norm

        # 9. 置信度（基于关键点可见度均值）
        confidence = (nose_vis + l_sh_vis + r_sh_vis) / 3

        # 10. 异常值剔除
        new_pos = FacePosition(
            x_cm=x_cm,
            y_cm=y_cm,
            z_cm=z_cm,
            distance_cm=distance_cm,
            confidence=confidence,
            valid=True,
        )
        if self._is_outlier(new_pos):
            self.logger.bind(tag=TAG).debug(
                f"Outlier detected: jump > {self.cfg.get('outlier_jump_threshold_cm', 30.0)}cm, "
                f"last=({self._last_valid_pos.x_cm:+.1f},{self._last_valid_pos.y_cm:+.1f},{self._last_valid_pos.z_cm:+.1f}), "
                f"new=({x_cm:+.1f},{y_cm:+.1f},{z_cm:+.1f})"
            )
            # 丢弃本帧，保持上一帧输出
            return self._last_valid_pos if self._last_valid_pos else FacePosition.invalid()

        # 11. 一阶低通滤波
        if self._last_valid_pos is not None and self._last_valid_pos.valid:
            alpha = self.cfg.get('smoothing_alpha', 0.3)
            new_pos.x_cm = alpha * new_pos.x_cm + (1 - alpha) * self._last_valid_pos.x_cm
            new_pos.y_cm = alpha * new_pos.y_cm + (1 - alpha) * self._last_valid_pos.y_cm
            new_pos.z_cm = alpha * new_pos.z_cm + (1 - alpha) * self._last_valid_pos.z_cm
            new_pos.distance_cm = alpha * new_pos.distance_cm + (1 - alpha) * self._last_valid_pos.distance_cm

        self._last_valid_pos = new_pos
        self._invalid_count = 0
        return new_pos

    def _get_center_norm(self) -> tuple:
        """获取原点的归一化坐标 (cx_norm, cy_norm)"""
        mode = self.cfg.get('image_center_mode', 'geometric')
        if mode == 'calibrated':
            cx = self.cfg.get('calibrated_cx')
            cy = self.cfg.get('calibrated_cy')
            if cx is not None and cy is not None:
                return cx, cy
        # 默认用画面几何中心 (0.5, 0.5)
        return 0.5, 0.5

    def _is_outlier(self, new_pos: FacePosition) -> bool:
        """检测单帧跳变是否过大"""
        if self._last_valid_pos is None or not self._last_valid_pos.valid:
            return False
        threshold = self.cfg.get('outlier_jump_threshold_cm', 30.0)
        dx = abs(new_pos.x_cm - self._last_valid_pos.x_cm)
        dy = abs(new_pos.y_cm - self._last_valid_pos.y_cm)
        dz = abs(new_pos.z_cm - self._last_valid_pos.z_cm)
        # distance_cm 允许更大跳变（前后移动 50cm 也合理），用 2 倍阈值
        d_dist = abs(new_pos.distance_cm - self._last_valid_pos.distance_cm)
        return (dx > threshold or dy > threshold or dz > threshold
                or d_dist > threshold * 2)

    def _handle_invalid(self) -> FacePosition:
        """处理无效帧：保持上一帧输出或返回 invalid"""
        self._invalid_count += 1
        reset_threshold = self.cfg.get('invalid_reset_frames', 10)
        if self._invalid_count >= reset_threshold:
            self._last_valid_pos = None
            self._invalid_count = 0
            return FacePosition.invalid()

        # 保持上一帧输出，但标记为低置信度
        if self._last_valid_pos is not None:
            return FacePosition(
                x_cm=self._last_valid_pos.x_cm,
                y_cm=self._last_valid_pos.y_cm,
                z_cm=self._last_valid_pos.z_cm,
                distance_cm=self._last_valid_pos.distance_cm,
                confidence=0.0,
                valid=False,
            )
        return FacePosition.invalid()
