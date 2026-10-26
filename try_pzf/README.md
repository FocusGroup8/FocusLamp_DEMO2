# try_PZF 集成代码快照

这是 `try_PZF` 中 controller/head 当前集成代码的独立副本，位于
`feature/try-pzf-demo-integration` 分支。仓库原有根工程保持不变；不要在
仓库根目录构建这两个工程。本地 controller 没有 Git 历史，因此这里交付
可构建源码快照，而不将每个文件都宣称为本次新写的代码。

## 2026-10-26 更新

本批在同一分支更新当前 controller/head 源码，不覆盖仓库原有根工程。

- 九条欢迎语及久坐提醒已换为采集的小智云端音色 WAV，触发时仍在基座本地播放，
  不等待云端；实际文案和资源哈希见 `controller/main/audio/cloud_voice_assets.json`。
- 包含云端对话录音采集接口、电脑采集/格式转换工具及对应测试。采集原始目录、
  私有转换工具环境不上传；编译需要的十段固定 WAV 已包含在工程中。
- 包含机械臂位置只读采样、手动控制/校准与限位接口、对应测试和验收说明。
- head 灯光 PWM 频率已改为 20 kHz；保留当前摄像头及其他源码状态。
- 本批仅同步代码，不更改运行工程、不自动编译或烧录，也不宣称语音上传间歇超时、
  摄像头断连或所有机械臂动作已经完成实机验收。
- `sdkconfig.defaults` 与 `sdkconfig.defaults.snapshot` 的 Wi-Fi SSID 已替换为占位值，
  密码清空。首次构建前需要在本机重新填写热点；依赖覆盖恢复流程保持下文说明。
- 范围仍为 controller/head，不包含旧 base/voice、Docker 运行工程、版本留存包、
  IDE 目录、串口/采样记录、构建产物及实际热点凭据。
- 本次脱敏副本验证：controller Python 测试 137 项通过，其中 3 项因未安装本机
  FFmpeg 转换依赖跳过；head Python 测试 37 项通过。未在上传副本重新构建固件，
  未烧录或执行硬件动作；测试不代表已解决语音和摄像头间歇故障。

## 2026-10-21 历史更新

本次同步当前 `try_PZF/controller` 与 `try_PZF/head`，保留原有仓库根工程及
分支结构；不是将旧 base/voice 工程覆盖到当前基座。

- 拍头欢迎语改为基座内置 9 条 PCM16 WAV，按 1→9→1 循环；重启从第 1 条开始。
  连拍至多保留一条待播，长按取消当前及待播欢迎语。head 原来的三句 HTTP
  请求作为触发信号，欢迎语不再等待云端 TTS。两板仍需局域网互通。
- 本地欢迎语与原久坐提醒复用一个播放任务；播报期间隔离扬声器与上行，结束
  后按原云端状态恢复。普通语音对话仍走小智服务。本地欢迎语使用慧慧音色，
  不是小智云端音色；用户已确认可播放，完整轮播、取消和唤醒回归按验收单执行。
- 包含有界异步语音上传、会话生命周期与诊断、光感缓存与呼吸灯策略、机械臂
  点头 ±150/500ms，以及 head 当前保守 5 FPS 摄像头、自适应与会话管理代码。
- 新增/更新测试、九条录音生成脚本、相关验收说明；保存当前三个托管依赖覆盖。
  托管组件本体、构建产物、串口日志、热点密码与本机备份不上传。

当前欢迎语说明见 [local_welcome9_acceptance.md](controller/docs/local_welcome9_acceptance.md)。
较早的云端欢迎语验收文档保留历史诊断，不代表拍头请求仍走旧注入路径。

## 范围

- controller：本地灯带、机械臂、光感、雷达/触摸、模式倒计时、陪伴/久坐行为，
  语音与唤醒同步、OPUS 内存处理、LCD 软件驱动和相关验收说明。
- head：发现/动态桥接、摄像头传输、算法结果入口、WebSocket 会话管理，
  摄像头数据与控制帧发送串行化，以及 Wi-Fi MAC/DHCP 防护与诊断。
- 保留构建依赖锁、分区表及 head 的 LittleFS 输入资源。
- 不包含 base/voice 工程、电脑上的 `D:/Docker_demo2` 算法、构建产物、串口日志、
  IDE 设置、密钥和实际热点凭据。

这不是“全部 Demo 验收通过”的发布：LCD 尚未完成实机点亮验收；摄像头在
代理关闭和朝向调整后已收到真实图像，但长期稳定性仍需测试；真实手势到
机械臂、久坐播报、手机/电脑识别按对应验收文档继续验证。

## 构建（ESP-IDF 5.5.4 / ESP32-P4）

两个工程分别构建，只烧录对应板子。`sdkconfig.defaults.snapshot` 保存实际
配置快照，SSID 已替换为 `YOUR_WIFI_SSID`，密码已清空。创建本机配置后用
`idf.py menuconfig` 填入实际热点，**不要提交包含凭据的 sdkconfig**。

以 head 为例，在已加载 IDF 的 PowerShell 中：

```powershell
cd try_pzf/head
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.snapshot' reconfigure
& ../tools/Restore-DependencyOverrides.ps1 -Project head
idf.py menuconfig
idf.py build
idf.py -p <HEAD_COM_PORT> flash monitor
```

controller 同理，将目录和脚本参数改成 controller。不要使用 erase-flash，
不要混淆两块板子的串口。首次下载依赖的 reconfigure 完成后、build 之前
必须恢复下列覆盖文件；更新依赖或删除 managed_components 后需重新执行。

## 本地依赖覆盖

依赖通常由 IDF 下载，不整体上传。为保留当前工程行为，以下改动以完整
单文件源码覆盖形式存放在 `dependency_overrides/`，保留原许可证头：

| 工程 | 依赖及版本 | 文件 |
|---|---|---|
| head | esp_wifi_remote 1.6.2 | esp_wifi_remote_net2.c |
| controller | esp_xiaozhi 0.1.1 | src/esp_xiaozhi_websocket.c |
| controller | esp_websocket_client 1.7.0 | esp_websocket_client.c |

恢复脚本先检查所有版本，再复制，保存原文件到被忽略的 dependency_backups。
版本不匹配时停止，不默默覆盖新版本。`patches/` 中已有 C0/C2/C3/C4 历史补丁仅供
审阅，当前源码及覆盖已经包含其效果，不需要重复应用。

## 验证证据与限制

近期 head Wi-Fi 管理、Wi-Fi Remote 接口、WebSocket 修改已通过使用本机
compile_commands 的 ESP GCC 语法检查。用户日志确认有效 MAC、DHCP 获取 IP、
controller/head 自动发现和新 WebSocket 启动标记。上传副本只做配置脱敏、
目录整理和依赖恢复接线，未对这个独立副本执行完整编译或实机烧录。
原工程中的功能仍以对应 docs 验收记录为准。

本次同步前运行工程的 controller 构建已通过，app 大小 `0x442540`，6 MiB
app 分区约剩余 29%；运行源码的测试结果为 controller 56 项、head 37 项通过。
这些检查以源码约束、独立策略模型及实际音频格式检查为主；C fixture 为
编译检查，不冒充 FreeRTOS/硬件运行验收。上传副本会再次运行 Python 检查；
本次上传不自动烧录任何板子。
