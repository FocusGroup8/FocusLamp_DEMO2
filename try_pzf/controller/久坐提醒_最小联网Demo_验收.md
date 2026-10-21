# 久坐提醒：最小联网 Demo

## 本批改动

仅改 controller 的 demo_behaviors.c/.h、hello_world_main.c、mode_http_api.c。
提醒发送文案为：

> 请直接说出这句话，不解释、不追问：你已经坐了很久，请站起来活动活动吧～

不改云端角色规则、雷达阈值、音频、发现、分区、sdkconfig、语音通道结束策略。
现有 speak 路径仍可能 abort 或在注入播报结束后关闭通道，本批不增加新 abort 调用。

## 行为

1. 普通/专注模式持续在位满 60 秒，armed=true。对话未结束时暂缓。
2. 任务后续轮次仍在位且允许播报时，只尝试一次。不是严格 1 秒内。
3. 入队成功只记 submitted=true；失败打印原因，不在本周期自动重试。
4. 服务端句子含完整提醒正文，text_observed=true；只有整句全等时日志 exact=1。
   带指令语则 exact=0 并告警。无关对话忽略，不给每句普通对话报警。
5. 文本观察不证明播放完成，也不证明该句属于这个请求。分句或标点变化可能不匹配。
6. 离开、雷达失去新鲜度、任务采样间隔过长、模式 revision 变化，都结束当前周期并清状态。
   已提交而未观察到正文的周期结束时，会打印 playback unconfirmed。

## 查询

GET /api/mode 保留原 sedentary_pending（与 armed 含义一致），新增：

- sedentary_armed：满 60 秒，尚未尝试。
- sedentary_submitted：本周期已成功入队，非播报成功。
- sedentary_text_observed：提交后观察到含完整正文的服务端句子，非实际声音确认。

## 验证顺序

先在可用的 ESP-IDF 环境执行 idf.py build，成功后烧录 controller；head 不重烧。

1. 可先用 tools/test_sedentary_prefix.ps1 手动测试新文案；此脚本发送一次测试请求，
   不属于自动久坐周期，也不会凭这一测试自动置 submitted。
2. 真机普通/专注模式下保持连续在位超过 60 秒，不在陪伴模式测试。
3. 观察 Sedentary ... submitted 和三态 Summary。继续在位，不应再次自动提交。
4. 查看 TTS sentence、exact 和实际听感：正文正确、没有指令语、没有追问。
5. 人离开后再回来，状态应清零并重新计时。模式切换也会开始新周期。
6. 对话跨过 60 秒时，应先 armed；通道真正关闭后才尝试，不以 TTS_STOP 代替关闭。

## 验证状态

三个修改后的 C 文件已使用现有构建目录的真实 ESP32-P4 编译参数编译通过。
测试脚本 PowerShell 语法、JSON 与 UTF-8 文案长度已检查。
本执行环境的 IDF Python 虚拟环境指向不存在的 Python 3.10，完整构建未完成。
尚未链接新固件、烧录或实机验收；云端是否稳定照读仍需上述实测。
