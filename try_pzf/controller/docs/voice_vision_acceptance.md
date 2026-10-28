# 语音识图最小实现（2026-10-27）

## 使用

1. 只烧 **controller / COM6**：在本工程的 ESP-IDF 终端执行 `idf.py -p COM6 flash monitor`。head 不需要重新烧录。
2. 刷新 `http://127.0.0.1:47834/`，选择真实设备并连接；网页服务重载后回到模拟模式，这是原启动行为。电脑和台灯需在同一局域网。
3. 网页后端自动给 controller 注册电脑可达的视觉服务 URL；本机当前自动选出 `http://10.23.38.14:9000`。IP 变化时重新计算，每 30 秒续登记，controller 仅保存在 RAM。
4. 确认相机画面持续更新，拿好物体，正常唤醒后说：“请用摄像头看看我手里拿的是什么。”必要时明确说“调用识图工具”。
5. 本轮问题触发一次截图上传到豆包。小智取得工具答案后在当前对话回答，使用原云端语音配置。回答可有转述，不是逐字 TTS。

本批实现的是**语音提问识图**。网页“问问小智”截图功能保持原样；网页“播报回答”仍走原文字注入，没有在本批改成另一条朗读接口。

## 链路

`self.vision.recognize(question)` → 已验证的后台 MCP worker → controller 轻量探活 → Docker 固定一张新鲜共用帧 → 现有 VQA 单飞 → 按 request_id 查询 → 原 JSON-RPC ID / session / 连接代际回包 → 原小智对话回答。

- 保留 `self.test.delayed_result` 供回归；两个工具共用最多一项在途，不排队。
- controller 不下载/上传 JPEG，只发送问题；HTTP 工作不占用 MCP 接收回调。
- 固定 `image + frame_seq + stream_id + sampled_at`，后续查询不重新取图。`sampled_at` 是 PC 估计的帧接收 Unix 毫秒时间。
- Docker 检查相机已连接、帧年龄不超过 3 秒、当前相机代际；否则 `CAMERA_NOT_FRESH`，不发旧图。
- 复用现有 VQA 的 running/done/failed、25 秒云端客户端超时、零自动重试。controller 等待窗口约 30 秒；网络调用的阶段超时不构成严格的整个工具截止时间。
- POST 只执行一次；提交超时返回“结果未知”，不偷偷重发。状态按同一 request_id 查询，错误 ID / 查不到 / 服务失败分开处理。
- 断线、重连、关闭原音频会话、切换 session 后，worker 等在途调用实际结束并丢弃旧结果；不强制删任务。
- 动态答案不经过旧的 255 字节 speak 队列。响应有明确内存上限，超大/无效响应报错，不截断 UTF-8。

## 新接口

Controller：

- `POST /api/vision/service {"url":"http://<PC私网IPv4>:9000"}` → `{ok:true,url}`，RAM-only。只接受私网 IPv4 HTTP origin，非回环地址、无路径/认证信息。
- HTTP URI 容量从 20 调到 24，以容纳新增登记接口；没有调整 socket 数量、原语音超时或心跳。

Docker main-client：

- `GET /input_source`：检查 HTTP 可达及预期 JSON。
- `GET /vqa/health`：检查 VQA 服务可用/配置/忙状态；与摄像头帧新鲜度分开。
- `POST /vqa/recognize {"question":"这是什么？"}`：只接受问题，自己固定共用帧，返回 202 / 请求记录；服务忙 503 不排队。
- `GET /vqa/state?request_id=...`：复用现有状态查询。

网页地址登记是独立后台请求，失败不会把设备标为离线，也不阻塞视频或控制。新固件未烧录时旧固件返回404属于预期。

## 验证与当前现场状态

- 生产 C worker 主机运行测试 **13/13**：原0/3秒、复制所有权、动态答案大于255字节、断线后等待实际工作返回并丢弃等。
- 生产 C 识图客户端 + 真实 cJSON 主机运行测试 **13/13**：单次 question-only POST、地址快照、按ID轮询、忙/失败/过期、取消、超时不重发等。ESP HTTP/RTOS与时钟由 fixture 提供。
- Docker HTTP处理函数 + mock VQA **12项新增 + 2项原代理测试通过**；不执行真实模型或 Ark。
- 网页完整回归 **199/199**；原播报生命周期参考/源码测试 **13/13**。
- controller 最终编译通过，BIN为4,042,656字节，app分区余36%。未烧录、未进行本批真实云端截图请求。
- Docker两服务已构建并重建运行，其他四个算法容器未重建；两服务healthy、VQA配置可用，部署源码与容器文件SHA一致。网页监听已重载。
- **当前现场仍无新鲜相机输入**：main-client连接灯头后立即被对端断开，`/frame` 返回 `No frame available`；head `/api/status` 却报告camera_streaming=true。可能是摄像头单客户端槽位或发送组关闭，现有日志不足以归因。本批没有改摄像头固件、参数、Clash或停止/重启相机任务。
- 因而真实局域网 controller→PC 请求、实际截图问答与原音色播报，仍需在烧录并恢复连续相机画面后验收。此前0/3秒固定文字测试通过，不能替代本批真实识图验收。

BIN SHA256：`59e0c8e815dfc710f35191e1a4853150c21b38e62a93a5cdf542975efe08c7f5`

ELF SHA256：`9552f302294bc4c22f38ab43cae4dd997ee3e094f6035d846d1b9ac529e4c12e`

## 留存与部署

- 本批前controller相关源文件和BIN：`C:\Users\Administrator\Desktop\try_PZF\version_snapshots\2026-10-27_before_voice_vision\before.zip`。
- Docker旧文件：同目录 `docker/`；实际更新只涉及 `D:\Docker_demo2\docker\main-client\main.py`、`algorithms/vlm-visual-qa/server.py`。
- Docker暂存源码/测试：`tools/voice_vision_stage/`，其中 `_testdeps` 仅供主机测试，未复制到实际Docker源码或镜像。
- 网页旧文件：`C:\Users\Administrator\Desktop\focus_code\focus-studio-pzf\version_snapshots\2026-10-27_before_voice_vision_registration/`。
- 托管组件增量：`patches/c6_voice_vision_20261027.patch`，以C5现状为基线，临时目录回放逐字节通过；必须同时保留更新后的应用组件与HTTP接口，不能只应用补丁。

测试工具不冒充实机：本批没有验证真实小智在全部10/20/30秒延迟下的成功率，也没有承诺任意物体都能正确识别。
