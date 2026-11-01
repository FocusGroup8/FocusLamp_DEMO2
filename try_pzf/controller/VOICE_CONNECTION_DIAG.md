# 语音连接只读诊断

2026-10-28：仅增加 `[XZ_LINK]` 日志，不修改身份、连接地址、请求载荷、超时、重连或配置写入行为。

在 ESP-IDF 终端、controller 目录运行 `idf.py -p COM6 flash monitor`。head 不需要更新；不执行 erase-flash。

保留从启动至连接成功的 `[XZ_LINK]` 行：

- `OTA request host=... path=...`：配置的 OTA 请求地址。
- `OTA Device-Id=... Client-Id=...`：本次用于 OTA 请求的 MAC/UUID。
- `OTA websocket_section=present store_result=...`：本次响应的 WebSocket 配置保存函数结果；缺失时为 `absent store_attempted=0`。
- `OTA effective host=... path=...`：HTTP 客户端最终使用的地址（包含重定向后的变化）。
- `OTA HTTP status=... transport_result=... response_result=... has_ws=...`：本次 HTTP、传输与响应处理结果，三者应分别判断。
- `WS Device-Id=... Client-Id=...`：构造 WebSocket 请求头时使用的身份。
- `WS start config_source=keystore host=... path=...`：这次启动实际传给 WebSocket 客户端的地址。启动记录本身不代表握手成功，需配合原有 Connected 日志。

URL 日志去掉 userinfo、query 和 fragment；无凭据或凭据尾号。不记录完整响应配置。MAC/UUID 是识别设备所需信息，分享给其他人时可按需遮挡。

日志不包含云端实际 agent 或音色，不能仅凭域名或 UUID 推断会话归属。这批用于获取证据，不宣称修复音色或历史记录。

源文件改动位于托管组件，已保留 `patches/c7_voice_connection_diag_20261028.patch`。重拉依赖后需重新应用。改动前两个源文件在上级 `version_snapshots/2026-10-28_before_voice_connection_diag/` 留存。

验证：两个源文件编译、controller 完整链接、生成 BIN 和分区尺寸检查通过；未烧录、未做实机验证。
