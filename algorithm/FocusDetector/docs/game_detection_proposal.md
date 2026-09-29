# 摄像头画面提交大模型判断"是否在玩游戏"方案

## 1. 方案概述

### 1.1 目标

通过 PC 摄像头采集画面，提交给视觉大模型（VLM），判断图中的人是否在玩游戏，返回结构化结果。

### 1.2 整体架构

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  摄像头采集  │ ──→ │  帧采样/压缩  │ ──→ │  VLM 推理    │ ──→ │  结果解析    │
│  OpenCV     │     │  Base64编码   │     │  云端/本地    │     │  JSON输出    │
└─────────────┘     └──────────────┘     └──────────────┘     └──────────────┘
```

### 1.3 实施策略：云端验证 → 本地化迁移

| 阶段 | 目标 | 时间 | 成本 |
|------|------|------|------|
| **Phase 1** | 云端 API 快速验证可行性 | 1-2 天 | ~$10 测试费 |
| **Phase 2** | 优化 Prompt 和采样策略 | 3-5 天 | ~$50 |
| **Phase 3** | 本地 VLM 部署，替代云端 | 3-5 天 | 硬件成本 |
| **Phase 4** | 混合方案（本地初筛 + 云端复核） | 2-3 天 | 降低 80%+ API 费用 |

---

## 2. Phase 1：云端 API 快速验证

### 2.1 推荐模型

| 优先级 | 模型 | 单帧成本 | 延迟 | 推荐理由 |
|--------|------|---------|------|---------|
| **首选** | GPT-4o mini | ~$0.0006 | ~0.8s | 性价比最高，速度快，准确度够用 |
| 备选 | Gemini 2.5 Flash | ~$0.0009 | ~1s | 更便宜，Google 生态 |
| 高精度 | GPT-4o | ~$0.003 | ~0.8s | 准确度最高，成本高 |

### 2.2 核心代码

```python
import cv2
import base64
import json
import time
from openai import OpenAI

client = OpenAI(api_key="YOUR_OPENAI_API_KEY")

SYSTEM_PROMPT = """你是一个行为分析助手。根据摄像头画面判断图中的人是否在玩游戏。

关注以下线索：
1. 是否面对屏幕（电脑/手机/平板）
2. 手部是否在键盘、鼠标、手柄或手机上操作
3. 屏幕内容是否为游戏画面
4. 身体姿态是否符合游戏状态（前倾、专注等）

只返回JSON格式：
{
  "playing_game": true/false,
  "confidence": 0.0-1.0,
  "activity": "简短描述当前行为",
  "screen_visible": true/false,
  "hands_on_device": true/false
}"""

def capture_frame(camera_index=0):
    """采集一帧画面"""
    cap = cv2.VideoCapture(camera_index)
    ret, frame = cap.read()
    cap.release()
    if not ret:
        raise RuntimeError("摄像头采集失败")
    return frame

def frame_to_base64(frame, quality=70, max_size=1024):
    """压缩并编码为Base64"""
    h, w = frame.shape[:2]
    if max(h, w) > max_size:
        scale = max_size / max(h, w)
        frame = cv2.resize(frame, (int(w * scale), int(h * scale)))
    _, buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return base64.b64encode(buffer).decode('utf-8')

def analyze_frame(frame, model="gpt-4o-mini"):
    """提交给VLM分析"""
    b64_image = frame_to_base64(frame)

    response = client.chat.completions.create(
        model=model,
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": [
                {"type": "text", "text": "请分析这张摄像头画面"},
                {"type": "image_url", "image_url": {
                    "url": f"data:image/jpeg;base64,{b64_image}",
                    "detail": "low"  # 低精度模式，85 tokens，节省成本
                }}
            ]}
        ],
        max_tokens=150,
        temperature=0.1  # 低温度，输出更稳定
    )

    try:
        result = json.loads(response.choices[0].message.content)
    except json.JSONDecodeError:
        result = {"raw_response": response.choices[0].message.content}

    result["model"] = model
    result["tokens_used"] = response.usage.total_tokens
    return result

# 使用示例
if __name__ == "__main__":
    frame = capture_frame()
    result = analyze_frame(frame)
    print(json.dumps(result, ensure_ascii=False, indent=2))
```

### 2.3 采样策略

```python
import threading
from collections import deque

class GameMonitor:
    """周期性采样并分析"""

    def __init__(self, interval=5.0, model="gpt-4o-mini"):
        self.interval = interval  # 采样间隔（秒）
        self.model = model
        self.results = deque(maxlen=100)  # 保留最近100条结果
        self.running = False

    def start(self):
        self.running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self.running = False

    def _loop(self):
        while self.running:
            try:
                frame = capture_frame()
                result = analyze_frame(frame, self.model)
                result["timestamp"] = time.time()
                self.results.append(result)
                print(f"[{time.strftime('%H:%M:%S')}] "
                      f"玩游戏: {result.get('playing_game')} "
                      f"置信度: {result.get('confidence')} "
                      f"行为: {result.get('activity')}")
            except Exception as e:
                print(f"分析失败: {e}")
            time.sleep(self.interval)

    def get_current_status(self):
        """综合最近N帧判断"""
        if not self.results:
            return None
        recent = list(self.results)[-5:]  # 最近5帧
        playing_count = sum(1 for r in recent if r.get("playing_game"))
        avg_confidence = sum(r.get("confidence", 0) for r in recent) / len(recent)
        return {
            "playing_game": playing_count > len(recent) / 2,
            "confidence": avg_confidence,
            "sample_count": len(recent)
        }
```

### 2.4 成本估算

| 采样间隔 | 每天帧数（8h） | GPT-4o mini 日费 | GPT-4o 日费 | Gemini Flash 日费 |
|----------|---------------|-----------------|------------|------------------|
| 3 秒 | 9,600 | $5.76 | $28.8 | $8.64 |
| 5 秒 | 5,760 | $3.46 | $17.3 | $5.18 |
| 10 秒 | 2,880 | $1.73 | $8.64 | $2.59 |
| 30 秒 | 960 | $0.58 | $2.88 | $0.86 |

**建议**：Phase 1 用 5 秒间隔 + GPT-4o mini，日成本约 $3.5。

---

## 3. Phase 2：Prompt 优化与采样策略

### 3.1 Prompt 优化方向

#### 基础版（Phase 1 使用）

```
判断图中的人是否在玩游戏。只返回JSON。
```

#### 增强版（Phase 2 使用）

```
你是一个专注力监控系统。根据摄像头画面判断用户当前行为。

## 判断标准
- **玩游戏**：面对游戏画面，手在键盘/鼠标/手柄上，身体前倾专注
- **工作/学习**：面对文档/代码/网页，手在键盘上，姿态端正
- **休息**：离开座位、看手机、闭眼等
- **无法判断**：画面模糊、无人、遮挡

## 关键区分
- 玩游戏 vs 工作：看屏幕内容（游戏UI vs 代码/文档）和手部动作频率
- 玩手机游戏 vs 刷视频：看手指是否频繁触控操作

返回JSON：
{
  "playing_game": boolean,
  "confidence": float,
  "activity": string,
  "screen_visible": boolean,
  "hands_on_device": boolean,
  "screen_content_hint": "game_ui/code/document/video/social_media/other/unknown"
}
```

### 3.2 采样策略优化

```python
class AdaptiveSampler:
    """自适应采样：状态变化时加密采样，稳定时降低频率"""

    def __init__(self):
        self.base_interval = 10.0     # 基础间隔10秒
        self.density_interval = 3.0   # 密集采样间隔3秒
        self.last_status = None
        self.stable_count = 0

    def get_interval(self, current_status):
        if self.last_status is None:
            self.last_status = current_status
            return self.base_interval

        # 状态变化 → 密集采样
        if current_status != self.last_status:
            self.stable_count = 0
            self.last_status = current_status
            return self.density_interval

        # 状态稳定 → 逐渐降低频率
        self.stable_count += 1
        if self.stable_count > 3:
            return self.base_interval
        return self.density_interval
```

### 3.3 多帧投票机制

```python
class MultiFrameVoter:
    """多帧投票，减少误判"""

    def __init__(self, window_size=5, threshold=0.6):
        self.window_size = window_size
        self.threshold = threshold
        self.history = deque(maxlen=window_size)

    def vote(self, result):
        self.history.append(result.get("playing_game", False))
        if len(self.history) < 3:
            return result  # 样本不足，直接返回

        playing_ratio = sum(self.history) / len(self.history)
        result["playing_game_voted"] = playing_ratio >= self.threshold
        result["vote_confidence"] = playing_ratio
        return result
```

---

## 4. Phase 3：本地 VLM 部署

### 4.1 工作站硬件要求

| 配置等级 | GPU | 显存 | 推荐模型 | 推理速度 |
|---------|-----|------|---------|---------|
| **最低** | RTX 3060 | 12GB | Gemma3:4b | 2~3s/帧 |
| **推荐** | RTX 4070/4090 | 12~24GB | Qwen2.5-VL:7b | 1~2s/帧 |
| **高性能** | A100/4090×2 | 40GB+ | Qwen2.5-VL:32b | 1~2s/帧 |

### 4.2 Ollama 部署步骤

```bash
# 1. 安装 Ollama（Linux）
curl -fsSL https://ollama.com/install.sh | sh

# 2. 拉取视觉模型
ollama pull qwen2.5-vl:7b    # 推荐，中文理解好
# 或
ollama pull gemma3:4b         # 更轻量

# 3. 验证服务
curl http://localhost:11434/v1/models

# 4. 测试推理
ollama run qwen2.5-vl:7b "描述这张图片" /path/to/image.jpg
```

### 4.3 Python 调用（OpenAI 兼容 API）

```python
from openai import OpenAI

# 指向本地 Ollama 服务
client = OpenAI(
    base_url="http://工作站IP:11434/v1",
    api_key="ollama"  # Ollama 不需要真实 key
)

def analyze_frame_local(frame, model="qwen2.5-vl:7b"):
    b64_image = frame_to_base64(frame)

    response = client.chat.completions.create(
        model=model,
        messages=[{
            "role": "user",
            "content": [
                {"type": "text", "text": SYSTEM_PROMPT + "\n请分析这张摄像头画面"},
                {"type": "image_url", "image_url": {
                    "url": f"data:image/jpeg;base64,{b64_image}"
                }}
            ]
        }],
        max_tokens=150,
        temperature=0.1
    )
    return response.choices[0].message.content
```

### 4.4 模型选择建议

| 模型 | 中文能力 | 场景理解 | 推理速度 | 显存占用 | 推荐场景 |
|------|---------|---------|---------|---------|---------|
| **Qwen2.5-VL:7b** | 优秀 | 强 | 2~3s | ~8GB | **首选**，中文场景最佳 |
| Gemma3:4b | 一般 | 中 | 1~2s | ~4GB | 显存不足时的备选 |
| Qwen3-VL:4b | 优秀 | 中 | 1~2s | ~5GB | 速度优先 |
| Llama3.2-Vision:11b | 一般 | 强 | 3~5s | ~12GB | 高精度需求 |

---

## 5. Phase 4：混合方案（本地初筛 + 云端复核）

### 5.1 架构

```
摄像头帧
  │
  ▼
本地VLM初筛（每5秒）
  │
  ├─ confidence >= 0.8 → 直接采用结果
  │
  └─ confidence < 0.8 → 提交云端API复核
                          │
                          ▼
                       最终结果
```

### 5.2 实现

```python
class HybridAnalyzer:
    """混合分析器：本地初筛 + 云端复核"""

    def __init__(self, local_model="qwen2.5-vl:7b",
                 cloud_model="gpt-4o-mini",
                 confidence_threshold=0.8):
        self.local_client = OpenAI(
            base_url="http://工作站IP:11434/v1",
            api_key="ollama"
        )
        self.cloud_client = OpenAI(api_key="YOUR_KEY")
        self.local_model = local_model
        self.cloud_model = cloud_model
        self.confidence_threshold = confidence_threshold

        self.stats = {"local_only": 0, "cloud_review": 0}

    def analyze(self, frame):
        # Step 1: 本地初筛
        local_result = self._call_vlm(self.local_client, self.local_model, frame)

        if local_result.get("confidence", 0) >= self.confidence_threshold:
            self.stats["local_only"] += 1
            local_result["source"] = "local"
            return local_result

        # Step 2: 云端复核
        cloud_result = self._call_vlm(self.cloud_client, self.cloud_model, frame)
        cloud_result["source"] = "cloud"
        cloud_result["local_result"] = local_result
        self.stats["cloud_review"] += 1
        return cloud_result

    def _call_vlm(self, client, model, frame):
        b64 = frame_to_base64(frame)
        response = client.chat.completions.create(
            model=model,
            messages=[{
                "role": "user",
                "content": [
                    {"type": "text", "text": SYSTEM_PROMPT + "\n请分析这张摄像头画面"},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/jpeg;base64,{b64}"
                    }}
                ]
            }],
            max_tokens=150,
            temperature=0.1
        )
        try:
            return json.loads(response.choices[0].message.content)
        except json.JSONDecodeError:
            return {"raw_response": response.choices[0].message.content, "confidence": 0}
```

### 5.3 成本对比

| 方案 | 日成本（8h，5s间隔） | 说明 |
|------|---------------------|------|
| 纯云端 GPT-4o | $17.3 | 全量云端 |
| 纯云端 GPT-4o mini | $3.46 | 全量云端 |
| 纯本地 Qwen2.5-VL | $0 | 仅电费 |
| **混合方案** | **$0.5~1.0** | 80%+ 本地处理，仅低置信度帧上云 |

---

## 6. 集成到现有 FocusDetector 项目

### 6.1 模块设计

```
FocusDetector/
├── detectors/
│   ├── pose_detector/          # 现有：肢体检测
│   └── game_detector/          # 新增：游戏行为检测
│       ├── __init__.py
│       ├── base.py             # 基类
│       ├── cloud_detector.py   # 云端API检测器
│       ├── local_detector.py   # 本地VLM检测器
│       └── hybrid_detector.py  # 混合检测器
├── config/
│   └── game_detector.yaml      # 配置文件
└── main.py                     # 主程序集成
```

### 6.2 配置文件示例

```yaml
# config/game_detector.yaml
game_detector:
  enabled: true
  mode: "hybrid"  # cloud / local / hybrid

  sampling:
    interval: 5.0          # 采样间隔（秒）
    adaptive: true          # 自适应采样
    min_interval: 3.0       # 最小间隔
    max_interval: 15.0      # 最大间隔

  cloud:
    provider: "openai"      # openai / gemini
    model: "gpt-4o-mini"
    api_key_env: "OPENAI_API_KEY"
    detail: "low"           # low / high / auto
    max_tokens: 150

  local:
    provider: "ollama"
    model: "qwen2.5-vl:7b"
    base_url: "http://工作站IP:11434/v1"

  hybrid:
    confidence_threshold: 0.8   # 低于此值触发云端复核

  voting:
    enabled: true
    window_size: 5
    threshold: 0.6

  image:
    quality: 70               # JPEG 压缩质量
    max_size: 1024            # 最大边长
```

### 6.3 主程序集成

```python
# main.py 中添加
from detectors.game_detector import HybridGameDetector

# 初始化
game_detector = HybridGameDetector.from_config("config/game_detector.yaml")

# 在主循环中（与现有 pose 检测并行）
game_result = game_detector.update(frame)  # 内部自行控制采样频率

# 显示结果
if game_result.playing_game:
    color = (0, 0, 255)  # 红色
    text = f"GAMING ({game_result.confidence:.0%})"
else:
    color = (0, 255, 0)  # 绿色
    text = f"Working ({game_result.confidence:.0%})"

cv2.putText(display_frame, text, (10, 320),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
```

---

## 7. 风险与注意事项

### 7.1 隐私问题

| 风险 | 应对措施 |
|------|---------|
| 云端方案上传摄像头画面 | 使用本地方案或混合方案；加密传输 |
| 画面中可能包含敏感信息 | 压缩画质、裁剪敏感区域 |
| 数据留存 | 选择不训练的API（OpenAI默认不训练API数据） |

### 7.2 准确度问题

| 场景 | 难度 | 应对 |
|------|------|------|
| 玩电脑游戏 vs 编程 | 中 | 关注屏幕内容（游戏UI vs 代码） |
| 玩手机游戏 vs 刷视频 | 高 | 多帧判断操作频率 |
| 看游戏直播 vs 玩游戏 | 高 | 关注手部是否在操作设备 |
| 画面中无人 | 低 | VLM可直接判断 |

### 7.3 延迟问题

- 云端 API：0.8~2s/帧，5秒采样间隔下可接受
- 本地 VLM：1~3s/帧，取决于 GPU 性能
- **不适合**需要毫秒级响应的场景，但"是否在玩游戏"判断不需要实时性

---

## 8. 实施计划

| 阶段 | 任务 | 交付物 | 预计时间 |
|------|------|--------|---------|
| Phase 1 | 云端 API 验证 | 可运行的 cloud_detector.py | 1-2 天 |
| Phase 2 | Prompt 优化 + 采样策略 | 优化后的检测器 + 测试报告 | 3-5 天 |
| Phase 3 | 工作站部署 Ollama + VLM | local_detector.py | 2-3 天 |
| Phase 4 | 混合方案 + 集成到 FocusDetector | hybrid_detector.py + main.py 集成 | 2-3 天 |
| Phase 5 | 测试调优 | 准确度报告 + 参数调优 | 2-3 天 |

---

## 9. 免费视觉大模型 API 汇总

### 9.1 国际平台

| 平台/模型 | 免费额度 | Vision 支持 | 限制 | 推荐度 |
|-----------|---------|------------|------|--------|
| **Google AI Studio (Gemini)** | ~500 次/天 | Gemini 2.5 Flash/Pro 原生支持 | 需翻墙；60 RPM 限速 | ★★★★★ |
| **OpenAI** | 新用户 $5 一次性额度 | GPT-4o / GPT-4o mini | 用完即止；需翻墙 | ★★★★ |
| **Google Cloud Vision API** | 1,000 次/月（永久免费） | 标签检测、物体识别 | 不是 VLM，是传统 CV API | ★★★ |
| **Google Cloud 新用户** | $300 / 90天 | 可用于 Gemini API on Vertex AI | 需信用卡；90天到期 | ★★★★ |
| **Clarifai** | 1,000 次/月 | 通用图像识别 | 非 VLM，不支持复杂推理 | ★★ |

### 9.2 国内平台（无需翻墙）

| 平台/模型 | 免费额度 | Vision 支持 | 限制 | 推荐度 |
|-----------|---------|------------|------|--------|
| **腾讯云 TokenHub** | 100万 Tokens/90天（多模型） | GLM-5、DeepSeek-V4 等 | 90天有效 | ★★★★★ |
| **中国移动 MoMA** | 9000万 Tokens 新用户包 | 多模态模型 | 2026年5月新上线 | ★★★★ |
| **百度千帆** | 200次/日 | 多模态模型 | 单次≤4096 tokens | ★★★ |
| **阿里云百炼** | 免费体验额度 | Qwen-VL 系列 | 额度有限 | ★★★★ |
| **DeepSeek 官方** | 免费额度 | DeepSeek-V4 | 价格极低（1元/百万Token） | ★★★★ |

### 9.3 推荐的免费验证路径

**首选：Google AI Studio + Gemini 2.5 Flash**
- 每天 500 次免费调用
- 原生支持图像输入
- 10秒采样间隔下，每天8小时 ≈ 2,880帧，在免费额度内

```python
import google.generativeai as genai

genai.configure(api_key="YOUR_GEMINI_API_KEY")
model = genai.GenerativeModel("gemini-2.5-flash")

response = model.generate_content([
    SYSTEM_PROMPT,
    {"inline_data": {"mime_type": "image/jpeg", "data": b64_image}}
])
```

**备选：腾讯云 TokenHub + GLM-5**
- 100万 Tokens 免费额度，国内无需翻墙
- 支持视觉理解

---

## 10. 参考资源

- [OpenAI Vision API 文档](https://platform.openai.com/docs/guides/vision)
- [Ollama 官网](https://ollama.com/)
- [Qwen2.5-VL 模型](https://ollama.com/library/qwen2.5-vl)
- [Live VLM WebUI - NVIDIA 实时摄像头+VLM方案](https://github.com/NVIDIA-AI-IOT/live-vlm-webui)
- [AI Vision API Pricing 2026 对比](https://aicostcheck.com/blog/ai-vision-multimodal-api-pricing-2026)
