# 摄像头 5 FPS 保守修正（2026-10-18）

本批修改当前 `C:/Users/Administrator/Desktop/try_PZF/head`，不修改上传副本、controller、Docker、Wi-Fi/热点配置、分辨率、心跳或1500ms整组发送预算。未编译、未烧录、未改变设备运行参数。源码与板上固件不是同一回事。

## 采用的修正

- 暂停8FPS试验，LOW/MID/HIGH恢复2/5/5FPS。不是保证实收5FPS，发送停顿和重连仍会降低实际吞吐。
- 用户FPS上限≤2时，在编码调用点强制Q15，不受BBA升档、PI或手动画质请求抬高。这是新的固定画质低负载模式；**帧大小仍会随真实画面变化，不是固定码率**。已在编码/发送中的一帧不会被设置请求撤销。
- 正常档PI范围收紧为LOW=15、MID=15–16、HIGH=15–18，自动调节不再超过各档基础画质。代价是暂时放弃高质量上探，优先限制负载；不是原5FPS快照的逐字还原。
- 目标JPEG载荷速率按实际档位节拍与用户上限缩放；实际反馈为“整组成功提交socket的JPEG载荷÷真实窗口时长”，不是空口吞吐，也不包括失败组的部分字节、WS头及链路开销。
- PI使用浮点EMA、输出时取整、按实际dt积分；静止场景不再跳过反馈，因为静止降帧本来就未启用。上限/档位/预算事件变化时重置PI并丢弃转移窗口，防止用旧流量计算新目标。
- 升档改为当前档位连续健康10秒后，再通过原有两次采样确认；坏窗口、样本不足、统计读取失败、换会话和换档都重计。预算超时仍立即LOW。恢复可能比旧版慢，这是有意收紧，不是未升档缺陷。
- 普通HTTP GET先确认已升级WebSocket，否则返回HTTP400，不占WS槽。真实重复WS连接仍可能先收到101再关闭，不伪称HTTP403；带query的`/camera`与`/algo`按实际路由识别。
- 摄像头fd锁内发布；整组发送锁内复查并捕获本次fd，发送/LRU/shutdown不再重读全局fd。部分发送仍立即shutdown、清摄像头槽；由HTTPD处理EOF完成物理关闭，不在本分支重复排队。已收到Close的成功及失败分支交回IDF cleanup，保留必需的`close(sockfd)`。

仅修应用层已确认路径，不宣称SDK所有异步关闭竞态已消失。IDF仍可能在某些控制队列/EOF时序下重复安排关闭；本批不修改IDF，也未证实现网命中过该路径。普通旧版text/binary接口未被整体重写；正常摄像头仍走pair接口。

低负载模式下画质设置接口返回当前控制值`quality`及请求值`requested_quality`，避免请求80、实际固定15却回报已应用80。正常自适应仍可能在随后采样更新画质。

## 已做的离线检查

`tests/test_camera_5fps_policy.py`：15项源码边界检查与数学模型通过；包括2/5/5表、固定Q编码点、真实窗口归一化、浮点EMA、恢复计时、HTTP拦截顺序、锁内fd捕获、残帧关闭和Close各失败出口。`git diff --check`通过。

这些检查**没有执行C代码**，不代替C编译、串口或真实视频验收。

## 上板后按这个顺序验收

1. 仅编译并烧录`head`到灯头COM4，保留串口日志。基座COM6本批不用烧；Docker不用重建。先核对物理设备，勿烧错板。
2. 新日志应含`Camera profile: conservative-5fps-v1`。没有这行，不先根据旧日志判本版效果。
3. 用head本次串口`Got IP`，不要沿用旧10.x/172.x地址。`/api/device/info`确认role=head，`/api/status`看控制状态；不要用普通GET `/camera`作日常探活。
4. 保持同一热点、算法、VPN状态和浏览器数量，先用2FPS固定Q15跑2分钟，再改5FPS跑至少10分钟。两次都记录真实镜头活动，不拿静止合成图代替。

```powershell
$headIp = '替换成head本次Got IP'
Invoke-RestMethod -Uri "http://$headIp/api/device/info" -TimeoutSec 5
Invoke-RestMethod -Uri "http://$headIp/api/camera/fps" -Method Post -ContentType 'application/json' -Body '{"fps":2}'
# 两分钟后恢复正常上限：
Invoke-RestMethod -Uri "http://$headIp/api/camera/fps" -Method Post -ContentType 'application/json' -Body '{"fps":5}'
```

5. 2FPS阶段Stats应为`actualQ=15 mode=fixed-low`，BBA仍可评估/换档但不能抬高实际编码Q；5FPS阶段为`mode=adaptive`，BBA可能因拥堵降到2FPS。默认上限可能仍为15，但表内最高只有5；API设置只影响当前运行，不能假定重启持久化。
6. 看相邻Stats累计成功帧数的增量÷实际秒数，同时对照Docker实收帧号；把断连空窗算进去，不用瞬时峰值冒充稳定帧率。记录`Camera group`原因/bytes/dur_ms、budget_total增量、重连数、最长无新帧间隔以及语音/灯光响应。

Stats的`actualQ`是最后记录帧的实际编码入参，`avg_size`是流启动以来成功JPEG载荷均值；WS耗时是当前窗口指标，不能当逐帧精确配对。需要逐帧关联时，源码已留DEBUG级`Frame: seq=... actualQ=... jpeg=... send_call_ms=... ret=...`；默认不刷每帧日志，避免串口本身影响对照。`send_call_ms`包含等发送锁时间，WS的`dur_ms`只覆盖组发送。

2FPS仍断连只说明此次减负不足以解决；不断连支持降低图像负载有帮助。两者都不能证明唯一起因。若本版仍频繁CLOSE，不继续加码PI/帧率，保留日志，走下面的昨日源码对照。

## 留存与回退

- 昨日留存保持不变：`../version_snapshots/2026-10-17_5fps_before_8fps/`。`head-source.zip`中的HIGH=5，仍含当时PI/BBA旧行为；它不是“已证明任何网络都稳定”的固件。
- 本批修改前备份：`../version_snapshots/2026-10-18_head_5fps_fix_before/affected-source.zip`，含camera_stream.c/.h与websocket_manager.c；`original/network_manager.c`另存了本次改前完整文件。`original/`内其余三文件由ZIP展开，便于差异比对。
- 若需要昨日控制逻辑对照：先保留本批文件，从昨日`head-source.zip`**只取回**`main/camera/camera_stream.c`及`components/websocket_manager/src/websocket_manager.c`，不要整包覆盖controller、Docker、用户配置或后续无关改动。同步取回该包的`camera_stream.h`说明，避免旧实现仍标注本批固定Q行为；画质API新增回报字段可保留，不改变控制逻辑。按同一环境重新编译/烧录head后对照。回退后本批固定Q、连续健康规则及15项新策略检查不再适用，不把它称为全工程逐字恢复。
- 仅设置API上限5可停止8FPS，但**不能撤销旧PI/关闭逻辑**；要代码回退需重新构建烧录。留存的既有bin未被证明等于昨日板上实际固件，不能仅凭日期直接刷回。

本批目标是恢复保守常用档并纠正确认的控制/接入缺陷，不承诺发送停顿的底层原因已定位或根治。
