# L5 软件收尾（不含 LCD 硬件验收，不实施 L6）

本轮取消旧基座启动 ping；focuslamp_bridge 改为纯本地服务适配器。
移除 HTTP/CRC helper、静态目标读取和相关依赖/私有宏。
sdkconfig/Kconfig 的旧目标字段保持原样兼容，但本组件不再读取或使用。

## 保留和停用

- 保留灯带 5、屏幕开关/数字背光 3、舵机 2、专注/陪伴 4、机械臂动作 6，共 20 个工具。
- 未迁移的 `self.focuslamp.lcd.next_page/set_expression/enable_blink/disable_blink`
  不再注册；对应公开 C 接口保留并立即返回 ESP_ERR_NOT_SUPPORTED。
- 这四个是底座 LCD 功能，不是 head 工具；后续恢复应接本地实现。
- `self.mipi_dsi.*` 保持不变，仍经发现组件取 head 地址。
- LCD 尚未点亮，不能据此宣称 L5 全部实机验收通过。
- L6 暂不做，继续使用既定热点名称/密码及 DHCP。

## 编译烧录与验证

只在 VSCode 编译 controller，成功后烧录当前底座板；无需 fullclean。
本轮不要求重刷 head。连接 head 后另做两板联调。

启动应出现：

```text
FocusLamp local bridge initialized (no remote base HTTP)
FocusLamp local MCP tools registered (20 tools; LCD page/expression/blink not advertised)
```

不应再有旧 `FOCUSLAMP_BRIDGE` target 地址日志或启动 `Ping failed`。
仅取消这一次旧请求，不代表 Wi-Fi/TLS/云端握手没有自身等待。

回归已有功能：灯带开关/亮度/效果、机械臂单轴/动作、专注/陪伴状态与倒计时。
机械臂的“已受理”和“已完成”语义保持原样。
模型可能说出不存在的功能，不以语音措辞证明工具存在；需要时查实际 tools/list。

预期：这四个底座 LCD 工具不再被公布，或旧缓存调用被工具引擎拒绝，
不能再引出 `/api/lcd/*` 的静态地址 HTTP 超时。
屏幕开关工具保留，但 LCD 实际是否发亮仍单独记为未验收。

## 本轮验证范围

已做 GCC -fsyntax-only（bridge/main）、源代码调用/工具计数核对、受保护文件哈希比对。
尚未代替完整构建、烧录或板上回归。
