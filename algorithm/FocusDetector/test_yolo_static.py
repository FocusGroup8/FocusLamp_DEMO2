"""
测试 YOLOv8 在静态图片上对鼠标键盘的检测能力。
从网络下载几张包含鼠标键盘的图片，用不同模型(n/s)检测对比。
"""
import os
import urllib.request
from ultralytics import YOLO
import cv2

# 测试图片（公开免费图片）
# 1. Unsplash 免费图片（直接CDN）
# 2. Pixabay 免费图片
TEST_IMAGES = [
    # Unsplash: 桌面+键盘+鼠标
    ("test_kb1.jpg", "https://images.unsplash.com/photo-1518770660439-4636190af475?w=640&q=80"),
    # Unsplash: 办公桌键盘特写
    ("test_kb2.jpg", "https://images.unsplash.com/photo-1587825140708-dfaf72ae4b05?w=640&q=80"),
    # Pixabay: 桌面键盘鼠标
    ("test_kb3.jpg", "https://cdn.pixabay.com/photo/2015/09/09/19/56/office-932926_640.jpg"),
    # Pixabay: 笔记本键盘
    ("test_kb4.jpg", "https://cdn.pixabay.com/photo/2014/09/24/14/29/macbook-459196_640.jpg"),
]

# COCO 类别 ID
PHONE_CLASS_ID = 67
MOUSE_CLASS_ID = 64  # COCO 0-indexed: mouse（修正:73 是 book）
KEYBOARD_CLASS_ID = 66  # COCO 0-indexed: keyboard（修正:76 是 scissors）
TARGET_CLASSES = [PHONE_CLASS_ID, MOUSE_CLASS_ID, KEYBOARD_CLASS_ID]
CLASS_NAMES = {PHONE_CLASS_ID: "phone", MOUSE_CLASS_ID: "mouse", KEYBOARD_CLASS_ID: "keyboard"}


def download_images():
    """下载测试图片"""
    for name, url in TEST_IMAGES:
        if not os.path.exists(name):
            print(f"下载: {name} <- {url}")
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
                with urllib.request.urlopen(req, timeout=15) as resp, open(name, "wb") as f:
                    f.write(resp.read())
                print(f"  下载成功，大小: {os.path.getsize(name)} bytes")
            except Exception as e:
                print(f"  下载失败: {e}")
        else:
            print(f"已存在: {name}")


def test_model(model_path, image_files):
    """用指定模型检测图片列表"""
    print(f"\n{'=' * 60}")
    print(f"模型: {model_path}")
    print(f"{'=' * 60}")
    model = YOLO(model_path)

    for img_path in image_files:
        if not os.path.exists(img_path):
            continue
        print(f"\n图片: {img_path}")
        results = model(img_path, verbose=False, classes=TARGET_CLASSES, conf=0.1)

        detections = []
        for result in results:
            for box in result.boxes:
                cls_id = int(box.cls[0])
                conf = float(box.conf[0])
                x1, y1, x2, y2 = box.xyxy[0].tolist()
                detections.append({
                    "class": CLASS_NAMES.get(cls_id, f"id={cls_id}"),
                    "conf": conf,
                    "bbox": [int(v) for v in [x1, y1, x2, y2]],
                })

        if detections:
            print(f"  检测到 {len(detections)} 个目标:")
            for d in detections:
                print(f"    - {d['class']}: conf={d['conf']:.3f}, bbox={d['bbox']}")
        else:
            print("  未检测到任何 phone/mouse/keyboard")

        # 可视化保存
        annotated = results[0].plot()
        out_path = f"result_{os.path.basename(img_path)}"
        cv2.imwrite(out_path, annotated)
        print(f"  可视化结果: {out_path}")


def test_user_snapshot():
    """如果存在用户的截图也测试"""
    snapshots = [f for f in os.listdir(".") if f.endswith(".jpg") and f.startswith("snap")]
    return snapshots


def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    print("=" * 60)
    print("YOLOv8 鼠标键盘检测能力测试")
    print("=" * 60)

    # 1. 下载测试图片
    download_images()

    # 2. 检查用户截图
    user_snaps = test_user_snapshot()
    if user_snaps:
        print(f"\n发现用户截图: {user_snaps}")

    image_files = [name for name, _ in TEST_IMAGES] + user_snaps

    # 3. 用 yolo26s.pt 测试（2026最新,优化小物体检测）
    test_model("yolo26s.pt", image_files)

    print("\n" + "=" * 60)
    print("测试完成")
    print("查看 result_*.jpg 文件可看到可视化结果")
    print("=" * 60)


if __name__ == "__main__":
    main()
