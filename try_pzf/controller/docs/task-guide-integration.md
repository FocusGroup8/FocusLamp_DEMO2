# 三步任务引导：实现接线与验证

2026-10-31。controller 只管理用户请求、MCP 内容校验和 head 状态记录；head 是唯一计时源。
没有修改云端角色、联网调用、烧录或擦除设备数据。

## 云端角色说明需要补充的规则

把下段合并到设备实际绑定智能体的角色说明中，保留原来的设备规则。语音识别后的模型回复仍需遵守工具规则。网页文字入口已在固件提交前附加明确工具指令，无需把网页文本当普通闲聊。

> 普通 idle 或 companion 模式中，当用户明确说“帮我拆解/拆分/分解某个任务”或“清理桌面分3步，提醒我怎么做”时，先调用 `self.task_guide.pending` 获取当前请求令牌和原始任务。
> 只为工具返回的那个任务生成三条简单、具体、容易马上行动的步骤；每条 1–18 个 Unicode 字符，不加编号、换行、Markdown，三条不能重复。
> 调用 `self.task_guide.set`，原样传回 `request_token`，`steps` 传长度为 3 的字符串数组。
> 如果返回内容校验错误，可在原请求有效期内改写三条再提交；如果令牌过期、请求已退出或不在普通模式，停止此请求，不取得新令牌来重放旧任务。
> 工具确认接收后，只简短告知“三步已准备好，点击屏幕开始”。不要在语音中罗列三步，也不要宣称屏幕已经显示或计时已经开始。
> 用户没有明确发起拆解任务时，不调用这些工具。运行中的引导需要用户点击屏幕退出，普通聊天不停止计时。

## 协议

`GET /api/mode` 原字段保留，新增 `task_guide` 对象：

```json
{
  "controller_boot_id": 123,
  "request_id": 1,
  "state": "ready",
  "wait_timeout_ms": 30000,
  "steps": ["丢掉桌面垃圾", "把物品分类放好", "擦净桌面"]
}
```

- `state`：`idle / waiting / ready / timeout / dismissed / done`。
- 只有 `ready` 带三条 `steps`；`waiting` 不含半份结果。
- 请求身份是 `(controller_boot_id, request_id)`，不能只比较一个自增数字。
- `CONFIG_TASK_GUIDE_WAIT_TIMEOUT_MS` 默认 30000，menuconfig 可设置 5000–120000；超时文案为“暂时没收到结果，再说一次试试”。超时不推断 AI/网络故障原因。
- 普通模式实际是 idle；同时允许 companion，禁止 focus。USER STT 支持拆解/拆分/分解及“分3步/分三步/分成三步”等明确请求，否定或询问词义不打开。
- `/api/tts/speak` 的任务原文仍作为短队列项；真正发送前生成携带当前令牌与 MCP 规则的长提示。精确回声不会替换令牌，提交失败取消对应请求。HTTP queued 只表示入队，不表示屏幕已显示。
- 新的普通用户话语会作废尚在等待的请求；已经下发给 head 的引导不被普通聊天或另一次工具调用替换。
- 模式退出或模式 revision 改变会作废该请求。controller 重启采用新的 boot ID。

`POST /api/mode/task_guide` 只记录 head 的本地状态：

```json
{
  "controller_boot_id": 123,
  "request_id": 1,
  "report_seq": 4,
  "state": "running",
  "step": 0,
  "remaining_ms": 179000
}
```

- 状态：`preview / running / done / dismissed`；step 为 0–2。
- head 为每次变化/心跳生成递增的 `report_seq`，重试可以使用同一序号；旧报告被确认但不覆盖新状态。
- `done/dismissed` 是终态，即使更大的旧 running 报告随后到达也不能重新派发。
- head 网络中断时本地显示、计时、点击退出照常；恢复后上报最新状态，controller 不发送计时修正。
- `remaining_ms` 是记录值，接口不会自行递减；只有 head 拥有计时权威。

## 验证边界

- `controller/tests/task_guide_gate_test.c` 直接运行生产 gate/intent 代码，覆盖超时、旧令牌、重启身份、普通聊天、模式切换、终态报告和重放。
- 输入文本的 UTF-8/长度/三条校验由 `shared/task_guide_protocol.h` 与 head 共用。
- 编译通过不代表云端工具调用、GT911 实际触摸、中文屏幕可读性或 9 分钟真机流程已验收。
- 等待超时首版显示中性提示，不额外调用云端 TTS；不实现口头编辑单步。
- 首版不新增独立 ASSISTANT 整段拼接：MCP 不依赖该通道，现有 `TTS_SENTENCE` 仍可用于诊断。
