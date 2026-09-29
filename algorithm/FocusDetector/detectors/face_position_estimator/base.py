"""
面部空间坐标估计 - 基础数据结构

定义 FacePosition 数据类，用于表示用户面部相对摄像头的三维空间坐标。

坐标系约定（修订后，更符合人类直觉）：
  - 原点：摄像头光心（Phase 1 用画面几何中心近似）
  - X 轴：水平向右（+ 右 / - 左）
  - Y 轴：垂直向上（+ 上 / - 下）  ← 修订：原 OpenCV 约定是 Y 下，现改为 Y 上
  - Z 轴：沿光轴朝向用户（+ 朝向用户，正值）
  - 单位：厘米 cm

字段说明：
  - x_cm, y_cm：鼻尖相对画面中心的水平/垂直偏移
  - z_cm：鼻尖相对双肩中点的深度差（+ 低头 / - 后仰），不是绝对距离
  - distance_cm：鼻尖到摄像头的绝对距离估算（基于针孔模型 + FOV 假设）
"""

from dataclasses import dataclass


@dataclass
class FacePosition:
    """面部空间坐标检测结果"""

    x_cm: float              # 水平偏移：+ 向右 / - 向左
    y_cm: float              # 垂直偏移：+ 向上 / - 向下
    z_cm: float              # 鼻尖相对双肩深度差：+ 低头 / - 后仰（不是绝对距离）
    distance_cm: float       # 到摄像头绝对距离估算（cm，基于针孔模型 + FOV 假设）
    confidence: float        # 置信度 0~1（基于关键点可见度均值）
    valid: bool              # 是否有效输出（False 时其他字段可能为上一帧保持值）

    @classmethod
    def invalid(cls) -> "FacePosition":
        """构造一个无效的占位 FacePosition"""
        return cls(x_cm=0.0, y_cm=0.0, z_cm=0.0, distance_cm=0.0, confidence=0.0, valid=False)

    def __str__(self) -> str:
        if not self.valid:
            return "FacePosition(invalid)"
        return (f"FacePosition(x={self.x_cm:+.1f}, y={self.y_cm:+.1f}, "
                f"z={self.z_cm:+.1f}, dist={self.distance_cm:.1f}) cm, conf={self.confidence:.2f}")
