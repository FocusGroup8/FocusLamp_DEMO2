"""
豆包视觉大模型 - 纯API调用测试（无摄像头/GUI依赖）

测试步骤：
1. 生成一张测试图片（纯色+文字）
2. 编码为 Base64
3. 调用豆包 Responses API
4. 打印返回结果
"""

import os
import time
import base64
import numpy as np
import cv2
from openai import OpenAI

# ============ 配置 ============
# 密钥必须通过环境变量 ARK_API_KEY 注入，禁止硬编码
API_KEY = os.getenv("ARK_API_KEY", "")
MODEL_ENDPOINT = "doubao-seed-2-0-mini-260428"
BASE_URL = "https://ark.cn-beijing.volces.com/api/v3"
# ==============================

PROMPT = """你是一个行为分析助手。请观察这张图片中的人，判断他/她是否在玩电脑游戏。

判断依据：
- 正在玩游戏：屏幕显示游戏画面、手持游戏手柄、键盘快速操作、明显的游戏界面
- 不是玩游戏：正常办公、浏览网页、看视频、写代码等日常电脑使用

请严格按以下格式回答，不要输出其他内容：
判断：是/否/不确定
理由：一句话说明"""


def test_api_with_generated_image():
    """用生成的测试图片测试API"""
    # 生成一张简单的测试图片（模拟办公场景）
    img = np.ones((480, 640, 3), dtype=np.uint8) * 240  # 浅灰背景
    cv2.putText(img, "Test Image - Simulated Office Scene", (50, 240),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 0), 2)
    cv2.putText(img, "Person working at desk", (50, 280),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (100, 100, 100), 1)

    # 编码为 Base64
    _, buffer = cv2.imencode('.jpg', img, [cv2.IMWRITE_JPEG_QUALITY, 50])
    b64_image = base64.b64encode(buffer).decode('utf-8')
    data_uri = f"data:image/jpeg;base64,{b64_image}"

    print(f"图片大小: {len(b64_image)} bytes (Base64)")
    print(f"模型端点: {MODEL_ENDPOINT}")
    print("正在调用豆包 API...")

    client = OpenAI(
        base_url=BASE_URL,
        api_key=API_KEY,
    )

    try:
        t_start = time.time()
        response = client.responses.create(
            model=MODEL_ENDPOINT,
            input=[
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "input_image",
                            "image_url": data_uri,
                        },
                        {
                            "type": "input_text",
                            "text": PROMPT,
                        },
                    ],
                }
            ],
            extra_body={
                "thinking": {"type": "disabled"},  # 关闭深度思考模式，降低响应时间
            },
        )
        t_api = time.time() - t_start
        print(f"\n[耗时] API 调用: {t_api:.2f}s")

        print("\n" + "=" * 50)
        print("API 调用成功！")
        print("=" * 50)

        # 解析 Responses API 返回结构
        print(f"\n原始响应类型: {type(response)}")
        print(f"响应属性: {dir(response)}")

        # 尝试多种方式提取文本
        result_text = ""

        # 方式1: output 列表
        if hasattr(response, 'output') and response.output:
            print(f"\noutput 类型: {type(response.output)}")
            for i, item in enumerate(response.output):
                print(f"  output[{i}] type={type(item)}, attrs={dir(item)}")
                if hasattr(item, 'content') and item.content:
                    for j, content_item in enumerate(item.content):
                        print(f"    content[{j}] type={type(content_item)}, attrs={dir(content_item)}")
                        if hasattr(content_item, 'text'):
                            result_text += content_item.text

        # 方式2: 直接转字符串
        if not result_text:
            result_text = str(response)

        print(f"\n模型回复:\n{result_text}")

        # 提取判断和理由
        judgment = "未解析"
        reason = ""
        for line in result_text.split('\n'):
            line = line.strip()
            if '判断' in line and ('：' in line or ':' in line):
                judgment = line.split('：', 1)[-1].split(':', 1)[-1].strip()
            elif '理由' in line and ('：' in line or ':' in line):
                reason = line.split('：', 1)[-1].split(':', 1)[-1].strip()

        print(f"\n解析结果:")
        print(f"  判断: {judgment}")
        print(f"  理由: {reason}")

    except Exception as e:
        print(f"\nAPI 调用失败: {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc()


def test_api_with_url_image():
    """用公网URL图片测试API"""
    print("\n" + "=" * 50)
    print("测试2: 使用公网URL图片")
    print("=" * 50)

    client = OpenAI(
        base_url=BASE_URL,
        api_key=API_KEY,
    )

    try:
        response = client.responses.create(
            model=MODEL_ENDPOINT,
            input=[
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "input_image",
                            "image_url": "https://ark-project.tos-cn-beijing.volces.com/doc_image/ark_demo_img_1.png",
                        },
                        {
                            "type": "input_text",
                            "text": "你看见了什么？请简要描述。",
                        },
                    ],
                }
            ],
            extra_body={
                "thinking": {"type": "disabled"},  # 关闭深度思考模式，降低响应时间
            },
        )

        print("API 调用成功！")

        result_text = ""
        if hasattr(response, 'output') and response.output:
            for item in response.output:
                if hasattr(item, 'content') and item.content:
                    for content_item in item.content:
                        if hasattr(content_item, 'text'):
                            result_text += content_item.text

        if not result_text:
            result_text = str(response)

        print(f"模型回复:\n{result_text}")

    except Exception as e:
        print(f"API 调用失败: {type(e).__name__}: {e}")


if __name__ == "__main__":
    print("豆包视觉大模型 API 测试")
    print("=" * 50)

    # 测试1: Base64图片
    test_api_with_generated_image()

    # 测试2: URL图片
    test_api_with_url_image()
