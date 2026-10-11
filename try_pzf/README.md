# try_PZF 集成代码快照

这是 `try_PZF` 中 controller/head 当前集成代码的独立副本，位于
`feature/try-pzf-demo-integration` 分支。仓库原有根工程保持不变；不要在
仓库根目录构建这两个工程。本地 controller 没有 Git 历史，因此这里交付
可构建源码快照，而不将每个文件都宣称为本次新写的代码。

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
版本不匹配时停止，不默默覆盖新版本。`patches/` 中已有 C0/C2 历史补丁仅供
审阅，当前源码及覆盖已经包含其效果，不需要重复应用。

## 验证证据与限制

近期 head Wi-Fi 管理、Wi-Fi Remote 接口、WebSocket 修改已通过使用本机
compile_commands 的 ESP GCC 语法检查。用户日志确认有效 MAC、DHCP 获取 IP、
controller/head 自动发现和新 WebSocket 启动标记。上传副本只做配置脱敏、
目录整理和依赖恢复接线，未对这个独立副本执行完整编译或实机烧录。
原工程中的功能仍以对应 docs 验收记录为准。
