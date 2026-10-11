# DEMO2 WebSocket 连接修复与验收

## 本批范围与判因

修改 head/components/websocket_manager/src/websocket_manager.c，以及 PC 端 D:/Docker_demo2/docker/main-client/main.py、docker-compose.yml。不改 controller、音频、舵机、发现组件、sdkconfig 或分区。不新增服务端心跳任务。

4 是应用跟踪数组容量，原实现并未强制 WebSocket 准入；6 是 HTTPD 所有会话的总上限，不能理解为协议分开的连接池。recv_wait_timeout=300 是单次接收超时，不是静默连接固定 5 分钟的寿命。

head 的 network_manager 已将 JSON-RPC 响应交给 message_queue，/algo 并非只收不发。Python 客户端会接收响应，但未设业务 ACK 等待超时。因此不能把“无业务响应”当作已确认的立即断开原因。

已有历史知识库记录单独 /camera 也立即断开。当前变更修复可确认的准入与重连行为，并提供隔离诊断，尚未证明首次断连根因消失。

## 具体行为

- WebSocket 超出 4 个跟踪槽会拒绝，不再接受未跟踪的客户端。
- /algo 只保留一个结果生产者；新生产者接入时，请求 HTTPD 关闭旧会话，并释放旧跟踪记录。实际 socket 关闭仍由 HTTPD 完成。
- /camera 健康客户端的单槽约束保留。升级响应已经发出后不再往 WebSocket 发 HTTP 403。
- 开启 LRU；成功发送摄像头二进制帧或 header/JPEG 对时更新会话活跃记录，避免把活跃的只收流连接当成闲置会话。
- 断开通知移到跟踪/发送互斥锁外；帧接收失败记录 fd、长度、类型、错误码与 errno。
- 保留 300 秒接收超时、TCP keepalive 和客户端 Ping；由 IDF 自动处理 Pong/Close。
- Python 先收到首张图像再建立 /algo；有握手但没有图像时仍是单路 /camera 隔离状态。
- 握手失败、握手后立即断开、正常 Close 都有退避，最大间隔 30 秒；收到图像/回执才重置失败计数。
- ENABLED_ALGORITHMS 指定本次部署的算法，健康检查及前端启用都限制在该集合。Compose 默认 gesture_mantis；需要其他算法时显式扩展变量并启动对应容器。未启动付费 VLM。

## 当前验证

head WebSocket 源文件通过 ESP-IDF GCC -fsyntax-only，无新告警。Python 模块导入、算法选择、未知算法拒绝测试通过。Docker 镜像已重建，main-client 和 gesture-mantis-detector 健康；实际配置为 gesture_mantis。

当前未收到 head 图像，/frame 返回 No frame available；新客户端只有 /camera 握手重试，没有抢先 /algo 重连。完整 head 固件尚未构建、烧录及实机验证。

## 操作步骤

1. 在 VSCode 只编译、烧录 head（此前 COM4，以实际连接为准），不要 fullclean、erase-flash，不烧 controller/base/voice。
2. PC、head、controller 同连 DAI；确认 Got IP 和身份接口 role=head。当前地址为 10.87.104.143，变更时修改算法 .env 的 ESP32P4_IP 并重建/重建容器配置。
3. 启动应出现：
   `WS limits: tracked=4 httpd_sessions=6 lru=1 algo_producers=1 recv_timeout=300s`。
4. Docker 主客户端在自动重试，无需重装 Docker。首条接入必须是 new /camera client，tracked 应为 1/4。
5. 先检查 Camera first image received 和 cam_stream sent 连续增加，再检查 new /algo client、WS recv 和回执。成功接入两个主链路时通常 tracked=2/4，其他控制客户端会增加计数。
6. 若仍立即断开，收集首次 /camera 接入前后约 10 秒的完整串口日志，以及主客户端日志。因 /algo 尚未启动，此时可以排除两路连接竞争，但不能排除弱 Wi-Fi 或固件 I/O 问题。
7. 用 Docker Restart 重启主客户端一次，确认 /algo 替换/slot released/WS tracking removed 出现，计数不持续递增。
8. 连接过程中 curl /api/device/info 和 /api/status 应仍能响应。只看容器 healthy 不算图像链路通过。

电脑查询：

```powershell
curl.exe --max-time 5 "http://10.87.104.143/api/device/info"
curl.exe --max-time 5 "http://10.87.104.143/api/status"
```

最后再语音开启陪伴，用真实拇指向上/向下验证算法结果到机械臂。动作 accepted 不等于到位，仍看既有 completed/unconfirmed/failed 判据。
# Camera transport follow-up (2026-10-11)

Camera data had reached the gesture algorithm but connections repeatedly closed
with keepalive timeouts. This patch does not claim that all hotspot/driver
transport issues are resolved.

- `/camera` opts into manual control frames. Pong and Close now use the same
  mutex and full-write path as JPEG data, so IDF automatic replies cannot be
  interleaved into an unfinished JPEG frame.
- Failed raw-frame writes call shutdown. The camera pair sender also closes
  its session immediately, including congestion failures; it never appends a
  new frame to a possibly unfinished frame. HTTPD remains the owner of close().
- The Docker client explicitly uses `proxy=None` for both device WebSockets.
  This bypasses application HTTP/SOCKS proxy selection, not Windows TUN routing.
  No global proxy settings were changed.
- Local gesture inference on a synthetic no-hand JPEG returned success and a
  visualized image. Both WebSocket calls were checked to specify direct mode.
  This proves the local model endpoint works, not real-hand recognition.

Build/flash head only. The client has already been rebuilt. The new startup
marker is `Camera WS: serialized JPEG/Pong/Close, partial-send failure closes
session`. Check continuous image delivery, actual `Inference OK:
algorithm=gesture_mantis` logs and real-hand responses. Do not interpret
healthy-container status or webpage WebSocket status as image delivery proof.
