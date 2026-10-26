# 第一阶段：五轴只读反馈缓存与采集验收

本阶段增加 controller 的空闲采样缓存和 `GET /api/arm/pose`，供查看真实舵机反馈及导出五轴只读表。此文是操作和验收说明，不是已上板通过记录。未通过本文代替现场参考位姿、机械安全范围或运动中 3D 同步验证。head、网页及现有 MCP 动作保留；不新增运动、归位、写限位接口。

## 范围和调度

- controller 启动后保留一个低优先级采样任务，仅在动作空闲时按轴轮转读取。每次调用一个轴现有的读取函数，函数内部最多尝试 3 次；函数调用结束后至少冷却 250 ms。忙时暂停，不追赶补采。
- 采样先非阻塞尝试取得动作准入锁，再检查 `!busy`；持有准入锁完成该次单轴读取，随后释放。状态锁不跨舵机读取持有。若动作与正在进行的单轴读取相遇，动作可能等待该读取结束，不能宣称采样对动作零影响或零等待。未把此描述当作实测最坏响应期限。
- GET 只复制缓存，不触发舵机读取、不启动采样任务、不发动作。HTTP 请求间隔与总线采样间隔不同；连续 GET 可能拿到同一份缓存。
- 五轴分别读取，时间不一致。这不是五轴同时测量，也不包含运动过程的实时 3D 同步；该工作属于后续阶段。
- 原有启动位置日志 `motion_log_positions` 的读取也写入缓存，所以首次看到的 `attempts` 通常已经为 1。计数按现有读函数调用累计，一次函数内最多 3 次尝试仍记 1 次 `attempts`，不是线上 UART 请求数量。`uint32` 计数达到最大值后饱和。

## GET 契约

采样已启动时返回 HTTP 200，根对象包括：

| 字段 | 含义 |
| --- | --- |
| `ok: true`、`read_only: true` | 成功返回只读缓存，不表示五轴均读到有效数据 |
| `source: "servo-feedback-cache"` | 真实反馈的最近缓存；不是默认 home 或命令目标值 |
| `sampling_mode: "idle_only"` | 仅动作空闲时采样 |
| `sampling_started` | 采样任务已成功启动，不代表五轴均成功读取 |
| `motion_busy` | 复制快照时的动作忙状态；不是整段采集期间一直空闲的保证 |
| `captured_at_ms` | controller 单调时钟上的快照时间，单位 ms |
| `stale_after_ms: 10000` | 有历史成功值时，成功值年龄大于 10000 ms 才过期 |
| `axes` | 始终包含轴 0 至 4，共五项 |

采样未启动或快照不可用时可返回 HTTP 503。某一轴未读到值、最近读取失败或已过期，仍保留五轴槽位，并以该轴字段表达，不把它伪装成零位置。

| axis | bus_id | protocol |
| --- | --- | --- |
| 0 | 4 | `em3` |
| 1 | 1 | `lx` |
| 2 | 2 | `lx` |
| 3 | 3 | `lx` |
| 4 | 5 | `lx` |

每轴字段：

| 字段 | 含义 |
| --- | --- |
| `raw` | 最近一次成功读取的协议原始数值；没有历史成功时为 `null`，合法数值 0 必须保留。协议单位不同，不自动转成角度 |
| `valid` | 最新一次读取成功，且历史成功值没有过期。最新读取失败时立即为 false，即使仍保留旧 raw |
| `stale` | 没有历史成功，或距最后成功完成时间大于 10000 ms。读取失败但旧值仍未过期时，可同时 `valid=false, stale=false` |
| `age_ms` | 快照时距最后成功完成时间的年龄，由 controller 单调时钟计算；从未成功时为 `null` |
| `last_attempt_at_ms` | 最近一次读函数调用完成时间，无论成功失败；尚未尝试时为 `null` |
| `last_ok_at_ms` | 最近一次成功读函数调用完成时间；尚未成功时为 `null` |
| `attempts`、`successes`、`failures` | 累计已完成读函数调用次数、成功次数、失败次数；饱和计数 |

失败不更新 `last_ok_at_ms`，不抹掉最后有效 `raw`，也不把失败完成时间当成旧值的新鲜时间。所有 `*_at_ms` 和 `age_ms` 都属于 ESP 单调时钟域，重启后会重新计时；PC 的日期时间仅用于采集审计，不能与 ESP uptime 相减。仅有 `raw` 非空或 `stale=false` 不能推断最近一次读取成功。

## PC 只读采集

在 PowerShell 5.1 或更新版本中，从本工作区运行；把 `$controllerOrigin` 设置为当前 controller 的实际 origin。不要把文档示例当成设备固定 IP。

```powershell
$controllerOrigin = Read-Host 'Controller origin (http://host[:port] or https://host[:port])'
.\controller\tools\read_arm_pose.ps1 -ControllerUrl $controllerOrigin
```

可选参数：`-Samples 20`（有限次 GET，默认 20）、`-IntervalSeconds 2`（每次请求完成后等待，默认 2 秒）、`-OutputDirectory <目录>`。未指定输出目录时，在当前目录下创建 `arm_readonly_capture/<本次唯一目录>/`。每个请求超时上限 8 秒；网络/HTTP/契约失败写入记录，不无限重试、不补发、不追赶。工具只请求所指定 origin 的 `/api/arm/pose`；只允许 http/https origin，不接受用户名密码、额外路径、query 或 fragment，禁止自动重定向。

输出：

- `arm_pose_samples.csv`：每次 GET 都保存五行，包含完整根字段、各轴字段、PC 审计时间及请求错误。失败请求也保留五轴行，但未知反馈字段留空；JSON `null` 保持空格子，不能替换成 0。
- `arm_pose_summary.csv`：五轴只读表，包含最后符合契约的 HTTP 响应的 raw/valid/stale/age、累计计数、重复缓存次数及新增观察数量；同时打印简表。
- `arm_pose_report.txt`：采集范围和判读边界。未知参考值和机械安全上下限留空，稳定性和机械安全性均为 `not_assessed`。

首个有效响应是基线，不计为本次采集期间新完成的独立读取。后续仅在该轴 `attempts` 增加或 `last_attempt_at_ms` 改变时计为新观察；重复缓存不累计独立样本。`new_completed_attempts` 为计数差值，可能大于 PC 实际捕获的 `new_attempt_observations`；差值之间未捕获的原始数值未知。计数饱和后仅时间戳改变时按至少 1 次计，并标为下界。观测到时间/计数回退时重新设基线，不跨重启相减；没有观测到回退不代表能排除发生过重启。

`fresh_valid_observations` 只计新观察中设备报告 `valid=true` 的记录；raw 最小/最大值也只取这些观测，不含基线或重复缓存。若两次 GET 间发生多次读取，只能观察到最新一份。只成功读取 1 次不能称稳定，有限次成功也不自动证明机械安全。摘要 `latest_*` 是最后符合契约的 HTTP 响应时刻的状态，不是报告落盘时重新获取的新鲜状态。请保留失败记录，不只截取成功的一行。

## 编译与现场验收边界

可以离线用 PowerShell parser 检查脚本，以下命令不执行采集脚本、不联网：

```powershell
$tokens = $null
$parseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path .\controller\tools\read_arm_pose.ps1), [ref]$tokens, [ref]$parseErrors) | Out-Null
if ($parseErrors.Count -gt 0) { $parseErrors; throw 'PowerShell parse failed' }
```

源码检查、主机缓存测试和 controller 编译只能证明各自覆盖的部分；不等于板端总线/调度、动作延迟、读取稳定性或机械安全通过。实际构建结果以本批交付记录为准，不沿用目录里旧 bin 的时间或哈希作为新版本构建证明。

本批已在电脑端完成 controller 编译，未烧录。后续如需重新编译，在正常 ESP-IDF 工程终端进入 `controller` 执行 `idf.py build`。现场烧录由使用者核对目标、分区和产物后执行，目标为基座 **COM6**；head 不改动、不烧录。此文和只读脚本均不自动连接串口、重启或烧录，也不要求反复重启来验证稳定。

现场只读验收建议：

1. 保留 controller 当前状态，确认机械臂周围适合观察；不为采样调用 home、移动到未知参考位置或修改限位。默认 home 不是实测参考位姿。
2. 对当前 controller 运行采集工具，核对 HTTP 200 的来源、`idle_only` 和五轴映射。记录未读到、失败、保留旧 raw、过期及忙时暂停等实际出现的状态；未出现的状态标记未验证，不通过触发动作或重启强行造场景。
3. 查看每轴计数/完成时间是否产生新观察，并结合 valid、stale、age 和 failures 判读。只有重复 GET 成功不说明总线读取正常。
4. 留存 CSV 和报告。现场参考 raw、允许偏差、已知机械安全范围尚未定义，保持未知；由后续明确的机械校准与安全流程提供。本阶段不将任意 raw 自动判为安全。

## 修改前留存与安全回退

修改前留存为 `version_snapshots/2026-10-22_controller_before_arm_readonly/controller-related-before.zip`。压缩包包含 16 个相关原路径文件及 `SNAPSHOT_MANIFEST.json`，含旧 `controller.bin`、`controller.elf`、当时 flash 参数、配置及相关源码。它是相关文件快照，**不是整棵工程树的备份，不是从板上读取的固件，也没有备份 NVS**；旧 bin 不证明当前板上正运行该版本。

源码回退时先保留当前工作，再撤回本批新增文件，按 manifest 中原路径恢复本批改过的已有文件；不要整体覆盖工作区或其他批次的修改。快照之外的文件不能由此恢复，NVS/网络凭据等状态也不能由此恢复。恢复后重新编译并核对实际构建产物。

若使用旧 app bin 回退，必须先确认它与板上当前 bootloader、分区表、app 大小/偏移兼容，才可由使用者按工程现有流程只刷 app 分区；不能根据旧 flash 参数盲目整片写入。此回退说明不包含擦除 flash、清 NVS、整片写或 head 操作，也不把旧 bin 直接刷入当作默认步骤。

本批工具离线检查记录：Windows PowerShell 5.1.26100.9711 parser 通过；纯内存替身运行覆盖 10 次请求/50 行五轴记录、null 与数值 0、重复缓存、读失败保留历史 raw、GET 失败、计数饱和及观测到复位后重建基线；6 种无效 origin 均拒绝。替身禁止实际网络和文件输出，因此不证明 HTTP/板端实测通过。

## 本批电脑端验证记录

- controller 完整增量构建通过，生成 `build/controller.bin`，大小 4,017,216 字节；应用分区为 0x600000 字节，构建大小检查通过。
- controller 离线 Python 回归共 100 项通过，其中包含新增 HTTP/采样源码接线检查及 PowerShell 采集器连接临时 localhost 假服务的真实 HTTP/CSV 测试。假服务测试覆盖 null 与合法 0、重复缓存、HTTP 503 和不跟随 302 重定向；不是实机读取。
- `tools/test_motion_position_cache.c` 直接包含生产缓存头；RISC-V GCC/Clang 严格语法检查通过。电脑现有工具链没有 x86 主机后端，因此 **C fixture 的断言尚未实际执行**，不把语法检查计作 C 运行通过。具备主机编译器的环境可运行 `tools/test_motion_position_cache.py --cc <host-clang>`。
- 改动前留存中的舵机驱动、运动范围配置、sdkconfig、依赖锁均未修改；本批仅添加只读观察路径，不开放五轴网页运动、不提供未验证的安全范围。
- 未连接串口、未向板子发命令、未烧录，也未取得实机五轴数值。参考位姿、稳定性和机械安全范围仍待现场验证。

本批产物 SHA256（仅标识电脑上的这一份构建，不代表已上板）：

```text
controller.bin  31134d359f8535391c8d34c2dd789ad93caa9f6895398a8450102a0543a5ec1e
controller.elf  90962dd30a36564a4c17c0c072eb46016ca9f7bcbe8b9abe7e67f0911388a749
```
