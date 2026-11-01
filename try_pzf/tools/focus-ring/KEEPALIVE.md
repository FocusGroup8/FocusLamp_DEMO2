# 专注转发连接复用 · 2026-11-01

本包仅更新灯头应用，不更新基座或模型。保留上一版 focus_sync 和 engagement_diagnostics 诊断字段。

## 依据及改动

真实同一样本记录见 tools/diagnostics/focus-diag-live-20261101/matched-age.json：基座增加的样本年龄与其transit_ms一致，说明该部分来自保守链路计龄，不是重复缓存被当作新样本。链路抖动和转发等待使部分样本到基座时只剩很短TTL余量。

此前exchange_path每次调用均init/perform/cleanup，无法复用HTTP连接。现改为单个串行worker保留成功连接。对端地址变化、HTTP传输失败、响应溢出时释放并重建；GET/POST每次重设URL、method、body及Content-Type；返回前清除栈上的body/user_data指针。不自动重发失败样本。

没有修改：900ms超时、3秒样本TTL、15秒分类TTL、0/0.5/1倍计时规则、分类顺序或模型阈值。

## 使用

确认COM4是灯头，关闭其串口监视器：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash-HeadKeepalive.ps1 -HeadPort COM4
```

只写0x10000应用，不擦NVS，不写分区表、LittleFS或语音模型。ESP32-P4芯片校验无法区分两块板角色。

烧录后恢复画面与专注会话，读取两板已有诊断比较相同样本的sent_sample_age_ms、sample_age_ms/transit_ms及失败计数。不能仅凭HTTP200判全部正常。

## 验证与限制

已通过实际exchange_path函数的离线C测试：连续请求复用、POST后GET不残留请求体、失败后重建、地址变化关闭旧连接、溢出后重建、栈指针清理；灯头固件构建成功。

尚未烧录或实机验证性能改善。此改动减少重复建连开销，不能承诺消除无线抖动或所有过期。

模型方面：已查本地原仓库FocusGroup8/FocusLamp_DEMO2及其算法目录和交接说明，未找到训练标签、归一化及采样时基约定。真实输入非零且有变化，模型选择类别3；目前没有证据允许翻转标签。模型诊断已部署于focus-detector，计时仍会按“未参与”暂停，不能把本包当作已解决模型准确性问题。
