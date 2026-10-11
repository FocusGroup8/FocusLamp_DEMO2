# 陪伴联动与久坐提醒验收

本批只接陪伴联动、雷达在位提醒和 LCD 心率显示，不改摄像头模型、音频/唤醒/舵机实现，不改 sdkconfig/分区，不做 L6。

## 实现范围

- controller 的独立后台任务（6144 字节栈、优先级 3）执行联动，不在计时器、唤醒回调或模式锁内等待 HTTP。
- 开陪伴：本地灯带以 50% 原始亮度进入 breathing；随后向自动发现且已校验的 head 依次提交 mode=companion、灯头灯关闭、大屏开启、happy 表情、摄像头开启。
- head 只改摄像头 REST start 的幂等性：已开启也返回成功，不重复创建任务。
- 成功判据是 HTTP 200 且 JSON code=0。全部确认才置 head_sync_ready；这个字段不是物理动作、图像识别或网络推流成功的证明。
- head 未上线或请求失败时保留待同步状态，每 5 秒重试，无静态 IP 回退。不同模式代次的旧响应不计入新模式完成状态。
- 语音开启陪伴：MCP 返回固定 announcement，要求云端播报“主人，跟Focus聊聊天吧～”，避免主动注入抢占正在进行的对话。云端是否逐字播报仍取决于它的工具结果处理。
- 无对话时 HTTP 开启陪伴：通过现有 xiaozhi_manager_speak 入队同一句欢迎语。队列受理不等于已经出声；云端/网络仍须可用。
- 离开陪伴：关掉本批开启的呼吸灯，head 模式恢复 normal 或 focus；不自动移动机械臂、不默认恢复灯头原亮度，不自动关闭摄像头。
- LCD 加模式/心率显示，过期/无人/无有效心率显示 HR:--。LCD 尚未点亮，软件绘制完成不代表屏幕验收通过。

## 摄像头算法边界

已核对 head/main/algo/algo_result_manager.c：陪伴模式消费 gesture 字段的 Thumb_Up / Thumb_Down，经 base_bridge 动态取址转发到 controller 的 /api/arm/gesture。controller 使用既有串行执行器，LX ID5，向上 600、向下 400，运动时间 2000ms。

无需先重写模型。摄像头开启只启动视频链路，算法服务必须仍在运行、接收图像并向 head 提交 /algo 结果；没有算法结果就没有手势动作。若实际输出字段/枚举不同，再针对实际结果调整映射。

久坐提醒不用摄像头，复用雷达 presence。它是 Demo 的“持续在位”代理，不是坐姿识别：普通/专注模式连续新鲜在位 60 秒触发，同一段在位只入队一次。陪伴不触发；离开、presence 数据超过 3 秒未更新、采样间隔异常则重新计时。对话中只置 pending，结束对话后若仍在位才入队。摄像头已有 presence 逻辑不播报，避免重复提醒。

## 编译与烧录

这批需分别编译烧录 controller 和 head；不要烧 base/voice，不要 fullclean。PC、controller、head 必须在同一热点，地址使用本次 Got IP，不能沿用历史 IP。

当前验证：controller 7 个变更源文件及 head network_manager.c 通过 ESP-IDF GCC 的 -fsyntax-only；head 有已有 LVGL 配置弃用警告。尚未完整链接、烧录或实机验证。CMake 已接入新源文件。

## 1. 陪伴

先启动算法服务，再让两板联网。语音说“开启陪伴模式”，应看到：

1. DEMO: Demo worker started ... sedentary=60s
2. DEMO: Local mode actions applied: companion，实际灯带呼吸
3. 五条 Head action acknowledged，最后 Head mode sync acknowledged: companion
4. 灯头灯关闭、大屏开心、摄像头推流；欢迎语实际可听

PC 查询（先替换地址）：

```powershell
$ctl = "http://本次controller的IP"
curl.exe --max-time 8 -i "$ctl/api/mode"
```

应为 mode=companion，demo_worker=true，最终 companion_local_ready=true、head_sync_ready=true。开模式后短时间为 false 是待执行，不是假成功。

用拇指向上/向下交替测试：head 应有 gesture: Thumb_Up/Thumb_Down -> arm ... forwarded；controller 应有 accepted/running 及最终 completed/unconfirmed/failed。仅动作受理日志不能算到位。

退出再开启陪伴，验证摄像头已运行时不会因重复 start 卡在最后一步。head 起初断电、稍后联网，验证自动发现后补同步；不存在 head_sync_ready 时不能宣称灯头已联动。

## 2. 久坐提醒

普通或专注模式，雷达对着用户，持续坐在探测范围内超过 60 秒。查询 /api/mode 可见 radar_fresh=true、radar_present=true、sedentary_sec 增加。

- 无对话：60 秒后出现 Announcement queued ... 久坐文案，并实际听到提示。
- 继续留在原位：不应反复入队同一句。
- 先离开（presence=false），再回来：重新计时，60 秒后可再提醒。
- 60 秒时正在对话：sedentary_pending=true，不抢播。结束对话后仍在位，才应提醒。
- 待提醒期间离开/雷达断流：pending 取消，不能回来就立即播旧提醒。
- 陪伴模式：不计时、不播久坐提示。

必要时主动结束对话，必须 POST，然后查询 voice_active 确认异步副作用：

```powershell
'{}' | curl.exe --max-time 8 -i -H "Content-Type: application/json" --data-binary '@-' "$ctl/api/chat/end"
curl.exe --max-time 8 -i "$ctl/api/mode"
```

## 3. 回归

LCD 不亮时，陪伴灯光、手势、提醒仍应可工作。LCD 恢复后才验模式/HR/倒计时实物显示。

连续对话并穿插机械臂动作至少 10 分钟，查看新增 Worker memory before/after；确认 OPUS 启动预创建仍成功、无 -2 丢帧、无 panic。新任务确实消耗 RAM，语法检查不能替代这一项。

默认阈值 60 秒用于 Demo，不是健康监测结论；心率只显示雷达估算，不作医疗判断。
