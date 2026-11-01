# 开机角色恢复 · 2026-11-01

## 实测反馈后的初始化顺序修复

用户反馈断电仍蓝眼、联网后才恢复。源码确认display_system_start先启动眼睛，
network_manager_init随后才通过wifi_manager调用nvs_flash_init；先前face_boot读取会在NVS未初始化时失败，而且没有日志。
已将nvs_flash_init放到app_main中system_manager_init之前；失败时输出原因并停止启动，不自动擦除NVS。
眼睛模块在动画任务之前读取选择，并记录[FACE_BOOT] Restored role=... style=...；缺记录、格式不支持或读取失败均输出日志。
角色持久化输出Selection committed或Saved selection already matches。
本次构建已通过，尚未执行物理断电测试。镜像head/build/mipi_dsi.bin已更新，旧应用备份在version_snapshots/2026-11-01_before_nvs_boot_order。
只更新应用，不写NVS；已有有效记录应直接恢复。验收应关闭工作台或断网后断电重启，不能用联网后后台下发作为验收。

灯头收到带role/style的成功切换请求后，将组合选择写入NVS face_boot/selection。
相同选择不重复写入，普通九态表情不写NVS。开机在启动动画任务前恢复选择。
若持久化失败，接口明确返回错误：画面可能已改变，但不宣称已保存开机状态。

工作台后台新增role-head-sync：每10秒只读云端人设与音色的联合指纹，
按当前agent下的已确认指纹→灯头角色映射核对实际灯头role/style。
只有不匹配才发送neutral+optimized角色切换；匹配时不重播、不重置当前表情。
映射由启用角色后登记，也由角色库启动后的已确认历史补登记，保存于后台role-head-mappings.json。
后台可在关闭浏览器标签后继续运行；但需要后台进程、有效授权、真实PZF已连接且控制未锁定。
不自动连接设备、不绕过停止锁定、不修改云端音色或角色说明。
未知云端指纹、读失败或没有映射时保留当前灯头；不能仅凭同名或音色猜角色。

断网或后台未运行时只能恢复上次保存角色，无法保证知道云端离线期间的新修改。
本次头端已编译，28项相关模拟测试及浏览器启用检查通过，未烧录、未实际断电验证。
需要重新烧录head/build/mipi_dsi.bin并重启原带授权的工作台启动服务。
仍使用flash-head-app.ps1，只写应用、不擦NVS；首次从旧固件升级后，自动对齐成功会写入开机选择。
