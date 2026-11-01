## 追加修补
参与度真实名称映射与HTTP栈修补见STABILITY_FIX.md；当前controller构建已更新。

# 专注环倒计时 · 2026-10-30

已实现并编译，HTTP栈修补固件尚未烧录；本机Docker名称映射修复已部署。

## 更新后会看到什么

进入专注后，灯头显示像素环和剩余时间，小屏读取同一个 controller 计时。
- 参与/高度参与：绿，1倍速；勉强参与：橙，0.5倍速；未参与：红，暂停。
- 等待、无人脸、无效或过期：灰，暂停；语音对话：蓝，暂停。
- 完成：00:00、空环、已完成，保持专注模式。手动退出恢复表情。
- 运行中重复开始返回409，不重置；完成后重新开始才重置。

120秒是演示参数，原有1–180分钟仍可配置。分心、等待或语音暂停会使实际耗时超过两分钟。
首次分类需要现有模型积累300个有效人脸样本；之后每30个样本更新分类。
本次没有改这些模型参数，也不把重复缓存当成新推理。防抖至少1秒且要有新分类确认，因此低采样率时切换会更慢。

## 使用顺序

请先确认灯头和基座的COM端口，关闭相应串口监视器。
以下COM4/COM6仅是此前示例，按当前实际端口替换。脚本只写0x10000应用，不写NVS、分区表、语音模型或LittleFS。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash-FocusRing.ps1 -HeadPort COM4 -ControllerPort COM6
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Update-Docker.ps1 -DockerRoot "D:\Docker_demo2\docker"
```

Docker脚本会备份源码并保留原运行镜像标签，只重建/重启main-client、focus-detector。
需要已经能运行原演示的Docker源码、模型和配置。此包不包含模型或账号凭据。
检查两个容器healthy后，保持摄像头和人脸可见，再通过网页/语音进入专注；选择2分钟演示。
也可用采集脚本启动2分钟专注并保存权威状态CSV（把地址替换成当前controller IP）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Test-FocusRing.ps1 -ControllerUrl "http://172.28.85.181"
```

手动退出：网页退出专注，或POST /api/mode，内容 {"mode":"idle"}。

## 已验证与待验证

已验证：
- 两块固件均编译成功；head 2,038,256字节，controller 4,051,424字节。
- 真实C计时状态机与controller API测试：三档速率、暂停、到期截止、重复缓存、完成保持、新会话、旧会话拒绝。
- 真实RGB565渲染器：六种状态预览、暂停/失联冻结、完成归零、新会话重置。
- 7项元数据测试、1项真实main-client接线测试、61项已有离线回归通过。

尚待实机：两屏总滞后≤1秒、连续两分钟一致性、真人参与度与语音切换、断线重连，以及30fps绘制耗时/PSRAM余量。
30fps是绘制任务调度目标，不能把编译或主机测试当成屏幕实测。
串口每约5秒打印[FOCUS_PERF]（绘制均值/峰值、内部RAM/PSRAM），controller打印[FOCUS]（session、seq、remaining_ms、速率、原因、age）。

## 数据规则

controller是唯一计时源；数字直接显示权威剩余值，圆环用150ms追随已收到进度，不预测未来。
两屏均500ms取状态；正常联网情况下的总差值仍需现场验证。
同一会话只减不加；暂停先按旧速率结算，过期只扣到本机截止时刻，恢复不补扣。
推理分类TTL默认15秒，新画面/传输TTL默认3秒，常量在focus_clock.h；应根据实际采样率复核。
重复epoch+frame不延长有效期；source_epoch只做身份，不比较大小。旧算法连接不能重新接管。
每段链路回显接收方自己的单调时钟，计算保守往返年龄，不拿两台设备的墙钟相减。
网页原有摄像头、算法卡片和五轴接口保持原路径；本次重点是物理两屏计时。

## 回退

Docker更新脚本打印备份目录，内有原源码和rollback-images.yml。需要回退时，在Docker根目录执行：

```powershell
docker compose -f docker-compose.yml -f "备份目录\rollback-images.yml" up -d --no-build --no-deps main-client focus-detector
```

恢复源码前先停止编辑，按备份文件逐个恢复；新增加的focus_metadata.py可保留，不会被旧main.py使用。
灯头改动前应用已另存于工作区version_snapshots/2026-10-30_before_focus_ring/mipi_dsi.bin。
本包没有完整的旧基座固件，不把其他日期的镜像当成本轮原版。
