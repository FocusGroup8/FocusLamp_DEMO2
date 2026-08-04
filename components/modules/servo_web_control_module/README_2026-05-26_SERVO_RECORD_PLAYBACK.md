# 2026-05-26 机械臂录制、存储、播放链路修复说明

> 给后续接手的 AI / 工程师看的快速上下文：本轮需求是把机械臂“录制 -> 存储 -> 播放”的固件链路理顺，Web 前端不改；预设动作允许硬编码；重点处理 `-32768` 读数失败、EM3 数据可信度、以及预设动作可稳定复现。

## 1. 本轮结论

这次不是单纯加几个动作，而是把机械臂动作系统分成两条明确链路：

1. 录制动作链路：用户拖动/摆动机械臂，固件采样舵机当前位置，清洗后保存到 LittleFS。
2. 预设动作链路：终端调用硬编码动作库，多个舵机按固定关键帧协同运动，不依赖 Web 前端。

核心目标已经落地：

- `-32768` 被统一视为“舵机读数失败”，不会作为有效位置进入录制文件。
- EM3 和 LX 读位置时增加协议长度和 checksum 校验，减少串口脏数据被误判为真实位置。
- 录制首帧必须完整有效，否则不开始录制，也不会覆盖原 slot 数据。
- 录制中偶发读失败会用上一帧有效值兜底；没有兜底时跳过该采样。
- 保存前、加载后、播放前都会做动作帧清洗。
- 播放改成 FreeRTOS 后台任务，`servo_stop` 可以请求中断播放。
- Web 前端文件不改；只调整固件和 WebSocket 后端状态。
- `idf.py build` 已通过，生成 `build/radar_test.bin`。

## 2. 舵机编号、范围和基准位置

本项目当前机械臂按“从下往上”编号：

| 逻辑 ID | 实际舵机 | 范围 | 本轮使用的基准 |
|---|---|---:|---:|
| ID1 | LX 底部/底座方向 | 400-1000，代码安全校验按 0-1000 | 初始 700，中位 700 |
| ID2 | LX 第二关节 | 0-1000 | 初始 200，中位 500 |
| ID3 | LX 第三关节 | 0-1000 | 初始 0，中位 500 |
| ID4 | EM3 头部/上部主关节 | 0-3000 | 初始 900，中位 900 |
| ID5 | LX 顶部/头部小关节 | 0-1000 | 初始 350，中位 500 |

用户给定：

```text
初始位置: 700, 200, 0, 900, 350
中间位置: 700, 500, 500, 900, 500
```

注意：`servo_control_module` 里 LX 的通道数组仍是 `{1, 2, 3, 5}`，EM3 单独用 ID4；帧结构中 `lx_pos[0..3]` 对应 ID1、ID2、ID3、ID5。

## 3. 修改文件清单

### `components/servo_control_module/include/servo_control_module.h`

做了两件关键事：

- 增加 `<stdint.h>`，让 `uint32_t` 等类型在头文件里自洽。
- `servo_control_frame_t.time_ms` 从 `uint16_t` 改为 `uint32_t`。

原因：旧版 `uint16_t` 时间戳最长约 65 秒，稍长一点的录制就会溢出，播放时相邻帧时间差会错乱。

### `components/servo_control_module/src/servo_control_module.c`

这是本轮最核心的改动文件。

新增/强化的能力：

- 定义 `SERVO_CONTROL_INVALID_POS = -32768`，统一处理舵机读数失败。
- 新增 `em3_pos_valid()`、`lx_pos_valid()`、`frame_positions_valid()`，所有读数都先过范围校验。
- 新增 `read_valid_em3_pos()`、`read_valid_lx_pos()`，读位置时会短延时重试。
- 新增 `read_current_frame()`：
  - 首帧没有 fallback，必须所有舵机有效。
  - 录制中有上一帧 fallback，单个舵机偶发失败时可用上一有效值补齐。
  - 只有最终整帧有效，才会更新 `s_last_good_frame`。
- 新增 `sanitize_frame()` 和 `sanitize_slot_frames()`：
  - 首帧无效会丢弃。
  - 后续帧某个舵机无效，会尝试用上一帧补齐。
  - 时间戳非递增时，按采样间隔修正。
  - 第一帧时间固定为 0。
- LittleFS 存储改为 v2 文件格式：
  - 文件头包含 magic、version、frame_size、frame_count。
  - 保存前先清洗数据。
  - 读取时校验文件头和帧数量。
  - 兼容旧版“裸 frame_count + frame array”格式。
- LittleFS 挂载逻辑修复：
  - 如果其他模块已经挂载 `storage` 到 `/littlefs`，`esp_vfs_littlefs_register()` 会返回 `ESP_ERR_INVALID_STATE`。
  - 旧逻辑这里会加载失败。
  - 新逻辑把它视为“已挂载，可复用”，继续加载 slot 文件。
- 录制开始逻辑修复：
  - 不再一开始就清空 slot。
  - 先卸载 LX、关闭 EM3 扭矩、等待稳定，再读完整首帧。
  - 首帧有效才清空当前 slot 并进入 RECORDING。
  - 首帧失败则恢复状态，保留原 slot 数据。
- 录制停止逻辑修复：
  - 重新打开 EM3 torque。
  - 清洗帧。
  - 有有效帧才保存到 Flash。
- 播放逻辑重构：
  - 旧阻塞播放逻辑已禁用。
  - 新增 `servo_control_playback_task()`，后台播放。
  - `servo_control_start_playback()` 只创建任务并返回。
  - `servo_control_stop_playback()` 只设置停止标志，由播放任务在 20ms 粒度内响应。
  - 播放前移动到录制首帧，保证动作从正确起点复现。
  - EM3 速度根据位移和段时长计算，并 clamp 到配置范围。
  - EM3 大跨度位移仍保留分段策略，降低反向/跳变风险。
- slot 切换保护：
  - 录制或播放中禁止切 slot。
- 参数保护：
  - `max_frames`、`max_slots` 按配置上限检查，避免越界。

### `components/servo_driver/src/servo_driver.c`

本轮重点是让 EM3 / LX 读数更可信。

EM3：

- `servo_em3_read_pos()` 现在在解析响应前检查：
  - 数据包长度字段是否合理。
  - checksum 是否匹配。
  - position 是否在 `0-3000`。
- 不满足条件则继续找下一段响应或最终返回 `-32768`。

LX：

- `servo_lx_read_pos()` 现在在解析响应前检查：
  - 包长度是否至少覆盖位置字段和 checksum。
  - 包没有越过本次 UART buffer。
  - `ld_checksum()` 是否匹配。
  - position 是否在 `0-1000`。
- 不满足条件返回失败哨兵值，不进入录制。

这能解决“串口残包/错包刚好被解析成一个位置”的问题。尤其 EM3 数据是否正确，本轮判断标准是：协议头、ID、长度、checksum、范围全部通过，才认为正确。

### `components/ws_command_handler/src/ws_command_executor.c`

用户明确说 Web 端不用改，所以本文件只做后端兼容增强。

改动点：

- 增加 `<stdio.h>`、`<stdlib.h>` 相关依赖，保证 `snprintf/free` 等使用明确。
- WebSocket `servo_status` 的 `data` 下新增 `control` 对象：

```json
{
  "type": "servo_status",
  "data": {
    "em3_4": 900,
    "lx_1": 700,
    "lx_2": 500,
    "lx_3": 500,
    "lx_5": 500,
    "control": {
      "state": 2,
      "slot": 0,
      "frame_count": 120,
      "slot0_frames": 120,
      "slot1_frames": 0,
      "slot2_frames": 0
    }
  }
}
```

- 状态刷新只在 `IDLE` 或 `HAS_DATA` 时主动读取真实舵机位置；录制/播放中优先使用缓存，避免频繁读总线干扰动作。
- 默认缓存改为本轮中位姿态：

```text
em3_4 = 900
lx_1  = 700
lx_2  = 500
lx_3  = 500
lx_5  = 500
```

- `SERVO_PLAY` 不再在 WebSocket 后端强制“先回初始位”，而是交给 `servo_control_start_playback()` 自己移动到录制首帧。
- `SERVO_RECORD / SERVO_PLAY / SERVO_STOP / SERVO_SLOT` 执行后都会回传最新状态。

### `main/app_console_commands.c`

这是终端调用入口，主要做两类事：录制播放命令修正、硬编码预设动作库。

录制播放命令：

- `servo_record`
  - 如果当前在录制，就停止录制。
  - 如果不在录制，就尝试开始录制。
  - 开始失败时会提示检查舵机读数和当前状态。
- `servo_play`
  - 只有 `HAS_DATA` 且当前 slot 有帧时才播放。
  - 不再播放前硬编码移动到某个旧初始位。
  - 播放是否真正进入 PLAYING 会二次检查。
- `servo_stop`
  - RECORDING 状态下停止录制。
  - PLAYING 状态下请求停止播放。
  - 其他状态提示 Nothing to stop。
- `servo_status`
  - 按当前 `max_slots` 打印所有 slot 帧数。
- `servo_pos`
  - 读取并打印 EM3/LX 实时位置。

硬编码预设动作：

- 新增统一初始位：

```c
#define ARM_HOME_ID1 700
#define ARM_HOME_ID2 200
#define ARM_HOME_ID3 0
#define ARM_HOME_ID4 900
#define ARM_HOME_ID5 350
```

- 新增 `move_arm_all()`，让 ID1、ID2、ID3、ID4、ID5 同步运动。
- EM3 的第三参数不是时间，而是速度；因此新增 `arm_em3_speed_for_move()`，根据“目标位置差 / 动作时间”计算 EM3 速度并限幅。
- 新增动作关键帧结构：

```c
typedef struct {
    uint16_t p1;
    uint16_t p2;
    uint16_t p3;
    uint16_t p4;
    uint16_t p5;
    uint16_t time_ms;
} arm_pose_step_t;
```

当前终端动作：

```text
arm_action init
arm_action center
arm_action happy
arm_action surprise
arm_action shake_head
arm_action nod
arm_action curious
arm_action excited
arm_action angry
```

兼容别名：

- `arm_action shake` 等同 `shake_head`
- `arm_action scared` 等同 `surprise`

动作库原则：

- 每个动作都包含多个舵机协同，不做单舵机孤立动作。
- 除 `init` 外，动作开始前先回到初始位 `700,200,0,900,350`，再执行动作关键帧。
- 多数动作最后回到中位 `700,500,500,900,500`，方便连续动作衔接。

## 4. 录制、存储、播放完整链路

### 4.1 开始录制

入口：

```text
servo_record
```

内部流程：

1. 检查当前状态，不允许在 PLAYING 或已有播放任务时开始。
2. 暂时卸载 LX，关闭 EM3 torque，便于人工摆动机械臂。
3. 等待 100ms 总线稳定。
4. 读取完整首帧：
   - EM3 必须有效。
   - ID1/2/3/5 必须有效。
   - 任意一个返回 `-32768` 或越界，录制不开始。
5. 首帧有效后才清空当前 slot。
6. 首帧写入 `s_recorded_frames`，状态进入 `RECORDING`。

### 4.2 录制中采样

入口任务：

```c
servo_control_task()
```

内部流程：

1. 每 `sampling_ms` 采一次。
2. 当前读数有效则写入新帧。
3. 当前读数无效但上一帧有效，则使用上一帧对应舵机位置补齐。
4. 无法补齐时跳过本次采样。
5. 达到 `max_frames` 自动停止并保存。

### 4.3 停止录制并保存

入口：

```text
servo_record
servo_stop
```

内部流程：

1. 打开 EM3 torque。
2. 清洗当前 slot 的所有帧。
3. 有有效帧才写入 LittleFS。
4. 保存路径：

```text
/littlefs/slot0.bin
/littlefs/slot1.bin
/littlefs/slot2.bin
```

5. 新文件格式为：

```text
servo_control_file_header_t
servo_control_frame_t[]
```

### 4.4 开机/初始化加载

入口：

```c
servo_control_init()
```

内部流程：

1. 尝试挂载 LittleFS。
2. 如果 LittleFS 已被其他模块挂载，复用 `/littlefs`。
3. 逐个读取 slot 文件。
4. 新格式按 header 校验后读取。
5. 旧格式按兼容逻辑读取。
6. 加载后清洗帧。
7. 当前 slot 有帧则状态设为 `HAS_DATA`。

### 4.5 播放

入口：

```text
servo_play
```

内部流程：

1. 只有 `HAS_DATA` 且当前 slot 帧数大于 0 才允许播放。
2. 播放前清洗帧。
3. 创建后台任务。
4. 先移动到录制首帧，确保“动作从录制起点复现”。
5. 按帧时间差播放后续帧。
6. LX 用 group move。
7. EM3 用速度控制，速度按段位移和时间计算。
8. 播放结束后状态回到 `HAS_DATA`。

### 4.6 中断播放

入口：

```text
servo_stop
```

内部流程：

1. 设置 `s_playback_stop_requested = true`。
2. 播放任务每 20ms 检查停止标志。
3. 停止后状态恢复为 `HAS_DATA` 或 `IDLE`。

## 5. `-32768` 处理规则

`-32768` 在本项目中不是一个真实舵机位置，而是读位置失败返回值。

本轮之后的规则：

| 场景 | 处理 |
|---|---|
| 录制首帧读到 `-32768` | 拒绝开始录制，保留旧 slot 数据 |
| 录制中某个舵机读到 `-32768` | 有上一有效帧就补齐，没有就跳过 |
| 保存前发现 `-32768` | 清洗；无法清洗的帧丢弃 |
| 加载旧文件发现 `-32768` | 清洗；无法清洗的帧丢弃 |
| 播放前发现 `-32768` | 清洗；没有有效帧则拒绝播放 |
| Web 状态读到失败 | 忙碌状态使用缓存，不直接把失败值当真实姿态展示 |

如果后续 AI 要改这块，不要把 `-32768` 当作“机械臂极限位置”或“归零位置”。它只能表示读失败。

## 6. EM3 数据正确性的判断标准

EM3 的位置范围是 `0-3000`，中位约 `900`。本轮后，EM3 读数必须同时满足：

1. UART 响应里能找到正确包头。
2. ID 匹配。
3. 长度字段合理。
4. checksum 正确。
5. 位置值在 `0-3000`。

只有全部满足，才返回真实位置；否则返回 `-32768`。

EM3 播放时要注意：

- `servo_em3_move(id, pos, speed)` 第三个参数是速度，不是毫秒。
- 因此硬编码预设动作和录制播放都不能直接把 `time_ms` 当 EM3 speed。
- 本轮已用 `diff / time` 算速度，并限制在 `SERVO_CONTROL_EM3_MIN_SPEED` 到 `SERVO_CONTROL_EM3_MAX_SPEED`。

## 7. 终端验证建议

烧录后建议按这个顺序验证：

```text
servo_status
servo_pos
arm_action init
arm_action center
arm_action happy
arm_action surprise
arm_action shake_head
arm_action nod
arm_action curious
arm_action excited
arm_action angry
```

录制播放验证：

```text
servo_slot 0
servo_record
# 手动摆动机械臂
servo_stop
servo_status
servo_play
servo_stop
```

重启持久化验证：

```text
servo_status
servo_play
```

如果重启后 `slot0_frames` 仍大于 0，并且 `servo_play` 可以播放，说明 LittleFS 存储链路正常。

## 8. 构建验证

本轮已执行：

```text
idf.py build
```

构建成功，输出：

```text
Generated E:/focus/lamp/Laptop/build/radar_test.bin
Project build complete.
```

由于当前 PowerShell 环境没有直接配置好 ESP-IDF PATH，实际构建时使用了本机 ESP-IDF 工具链路径：

```text
C:\Espressif\tools\python\v5.5.3\venv\Scripts\python.exe
D:\esp\v5.5.3\esp-idf\tools\idf.py
C:\Espressif\tools\cmake\3.30.2\bin
C:\Espressif\tools\ninja\1.12.1
C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin
```

## 9. 后续 AI 注意事项

1. 不要为了修播放复现而改 Web 前端；用户明确要求 Web 端不用改。
2. 不要在 `servo_play` 前强制回某个硬编码姿态；录制播放应该回到“录制首帧”，否则无法完美复现。
3. 不要把 EM3 的 speed 参数当 duration。
4. 不要保存 `-32768`。
5. 不要在录制开始时先清空 slot；必须首帧有效后再清空。
6. 如果要调整预设动作，优先改 `main/app_console_commands.c` 的 `arm_pose_step_t` 数组。
7. 如果要调整录制采样策略，优先看 `servo_control_module.c` 的 `read_current_frame()` 和 `sanitize_slot_frames()`。
8. 如果出现“重启后没有动作数据”，优先检查 LittleFS 是否被别的模块先挂载，以及 `ESP_ERR_INVALID_STATE` 是否被正确复用处理。
9. 如果出现“动作突然抽动”，优先查 EM3/LX 读数是否被 checksum 拦住，以及是否有旧文件中的坏帧没有被清洗。

## 10. 测试与验证方法、指标和现象

本节用于指导后续 AI / 工程师做完整验收。测试时建议串口日志打开，机械臂周围留出安全空间，先低频、短动作验证，再做长录制。

### 10.1 测试前置条件

| 项目 | 要求 |
|---|---|
| 固件 | 已包含本轮修改并成功 `idf.py build` |
| 烧录 | 使用最新生成的 `build/radar_test.bin` |
| 供电 | 舵机电源稳定，避免录制/播放中掉压 |
| 机械状态 | 机械臂没有卡死、碰撞、线束拉扯 |
| 初始姿态 | 建议先执行 `arm_action init` |
| 串口 | 能看到 console 输出和 ESP_LOG 日志 |

### 10.2 构建验证

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| 编译 | 执行 `idf.py build` | 退出码为 0 | 输出 `Project build complete`，生成 `build/radar_test.bin` | C 编译错误、链接错误、缺少头文件 |
| 产物大小 | 查看 build 输出 | app 分区剩余空间大于 0 | 日志显示 binary size 小于分区大小 | `partition size overflow` |
| 修改文件语法 | 编译覆盖 `servo_control_module.c`、`servo_driver.c`、`ws_command_executor.c`、`app_console_commands.c` | 无 warning-as-error | 四个文件均可编译 | `format-truncation`、未声明函数、重复符号 |

### 10.3 舵机读数验证

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| EM3 位置读取 | 执行 `servo_pos` | EM3 在 `0-3000` | `EM3[4]` 输出一个合理正数 | 输出 `-32768` 或明显跳变 |
| LX 位置读取 | 执行 `servo_pos` | LX 在 `0-1000` | `LX[1/2/3/5]` 均为合理正数 | 某路输出 `-32768` |
| checksum 过滤 | 在总线稳定时连续执行 `servo_pos` | 不应出现随机大跳变 | 数值随姿态缓慢变化 | 静止时数值突然跳到边界或负数 |
| 中位姿态 | 执行 `arm_action center` 后 `servo_pos` | 接近 `700,500,500,900,500` | 各舵机接近目标值 | 某舵机方向反、差值很大、机械碰撞 |

判定说明：

- `-32768` 只表示读失败，不是有效舵机角度。
- 稳定供电和接线下，`servo_pos` 不应长期出现 `-32768`。
- 如果偶发出现，可以继续验证录制兜底；如果持续出现，应先修硬件通信。

### 10.4 预设动作验证

| 动作 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| 初始位 | `arm_action init` | 到达 `700,200,0,900,350` 附近 | 机械臂回到图片 1 类似折叠姿态 | EM3 跑到旧值 1000/2000 或姿态不对 |
| 中位 | `arm_action center` | 到达 `700,500,500,900,500` 附近 | 机械臂接近图片 2 中位姿态 | 某关节过冲或方向相反 |
| 开心 | `arm_action happy` | 多舵机协同，最后回中位 | 有左右摆动和轻快抬头效果 | 只有单舵机动，动作割裂 |
| 惊喜 | `arm_action surprise` | 多舵机协同，最后回中位 | 上部抬起、头部展开感明显 | 头部无动作或 EM3 突跳 |
| 摇头 | `arm_action shake_head` | ID1 与 ID5 协同左右变化 | 左右摇头，幅度可控 | 只底座转或只头部动 |
| 点头 | `arm_action nod` | ID2/ID3/ID4/ID5 协同 | 明显点头两次 | 下压过深、撞限位 |
| 好奇 | `arm_action curious` | 偏头、探身组合 | 左右观察感 | 姿态僵硬、无协同 |
| 兴奋 | `arm_action excited` | 快速多关键帧 | 快速、有节奏但不乱跳 | 抖动大、供电掉压 |
| 生气 | `arm_action angry` | 压低、左右短摆 | 压迫感姿态，最后回中位 | EM3 大跨度异常反转 |

合格指标：

- 每个动作至少两个舵机同时参与。
- 不出现撞机械限位、线束拉扯、明显反向暴冲。
- 动作结束后可继续执行下一条命令。
- EM3 不应因为把 duration 当 speed 而慢得不动或快得抽动。

### 10.5 录制开始验证

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| 正常开始录制 | `servo_slot 0` 后执行 `servo_record` | 状态进入 RECORDING | 终端打印 `Recording started` | 打印 not started |
| 首帧保护 | 让某一路读数持续失败后执行 `servo_record` | 不进入 RECORDING，旧数据不丢 | 打印 `Recording not started`，原 slot 帧数不变 | slot 被清空，后续无法播放旧动作 |
| torque 状态 | 开始录制后手动摆动机械臂 | LX 可被摆动，EM3 不硬顶 | 人工能录动作 | 舵机仍锁死，无法摆动 |

首帧保护是关键验收点：如果首帧有 `-32768`，录制必须失败，而不是保存坏数据。

### 10.6 录制中采样验证

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| 帧数增长 | 录制 5 秒后停止 | 帧数约为 `5s / sampling_ms + 1` | `servo_status` 显示当前 slot 有帧 | 帧数为 0 |
| 偶发读失败兜底 | 录制时观察日志 | 偶发失败不会中断录制 | 日志可出现 skipped 或 fallback，但最终有有效帧 | 大量坏帧导致无法保存 |
| 时间戳递增 | 播放时观察动作节奏 | 动作按录制节奏复现 | 速度接近录制过程 | 速度忽快忽慢、停顿异常 |
| 自动满帧停止 | 长录制直到上限 | 到上限后自动停止并保存 | 日志提示 limit reached / auto-saved | 越界、崩溃、重启 |

### 10.7 存储和重启验证

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| 保存 | 录制后执行 `servo_stop` | LittleFS 写入成功 | 日志显示 slot saved，`servo_status` 帧数大于 0 | 提示 cannot open file 或 no valid frames |
| 多 slot | 分别录 `slot 0/1/2` | 各 slot 帧数独立 | `servo_status` 显示各自帧数 | 切 slot 后数据串槽 |
| 重启加载 | 重启设备后 `servo_status` | 原 slot 帧数仍存在 | 状态为 HAS_DATA 或 slot 有帧 | 重启后帧数归 0 |
| LittleFS 复用 | 同时启用音频存储模块 | servo_control 可复用 `/littlefs` | 不因 `ESP_ERR_INVALID_STATE` 放弃加载 | 日志出现 partition already used 后数据不加载 |

### 10.8 播放复现验证

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| 播放起点 | 录制一个明显非初始首帧动作后 `servo_play` | 播放先回录制首帧 | 机械臂先到录制开始姿态 | 播放前强制回硬编码初始位 |
| 完整播放 | 播放刚录制动作 | 轨迹与录制一致 | 关键姿态顺序一致 | 姿态错乱、跳帧 |
| 节奏复现 | 录制快慢变化动作 | 播放时间比例接近配置速度 | 快慢节奏可辨认 | 全程同速或突然加速 |
| 停止播放 | 播放中执行 `servo_stop` | 20ms 粒度内响应停止请求 | 终端显示 stop requested，播放中断 | 命令卡死或必须等播放结束 |
| 播放后状态 | 播放完成后 `servo_status` | 状态回 HAS_DATA | 可再次 `servo_play` | 状态卡在 PLAYING |

### 10.9 WebSocket 后端状态验证

Web 前端本轮不改，但后端状态包需要验证。

| 验证项 | 方法 | 指标 | 正常现象 | 异常现象 |
|---|---|---|---|---|
| servo_status 包 | WebSocket 请求状态 | 返回 JSON 带 `data.control` | 有 `state/slot/frame_count/slotN_frames` | 只有舵机位置，没有控制状态 |
| 忙碌状态缓存 | 录制/播放中请求状态 | 不频繁读舵机总线 | 状态可返回，动作不被读数干扰 | 播放时频繁读导致卡顿 |
| 默认中位缓存 | 刚启动还未成功读位置 | 默认值为中位 | `em3_4=900,lx_1=700,lx_2=500,lx_3=500,lx_5=500` | 出现旧默认 `em3_4=2000` |

### 10.10 验收通过标准

整体通过需要同时满足：

1. 固件完整编译通过。
2. `servo_pos` 在硬件正常时不持续出现 `-32768`。
3. 所有硬编码动作可执行，且没有机械撞限位。
4. 录制首帧失败时不会清空旧数据。
5. 正常录制后当前 slot 帧数大于 0。
6. 重启后 slot 数据仍可加载。
7. `servo_play` 从录制首帧开始复现，而不是从硬编码初始位开始。
8. 播放中 `servo_stop` 可中断，播放后状态能恢复。
9. WebSocket 状态包包含 `control` 信息。
10. 旧文件或坏帧不会把 `-32768` 播放成真实位置。
