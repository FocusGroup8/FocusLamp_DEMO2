# controller HTTP 响应修复

本批只改 `mipi_dsi_bridge` 的 HTTP 收集与两处查询解析；不改久坐播报、
音频清队列、自动重试、发现组件、head、sdkconfig 或分区表。

## 改动

- GET/POST 共用事件收集器：响应头在回调内拷贝，响应体分段累加。
- 不再在 `perform()` 后 `read()`，不再把请求头当响应头。
- 检查传输返回值、HTTP 200、网络响应完整性。
- 本地缓冲溢出：查询返回错误；控制命令保留 HTTP 成功语义，但清空
  截断响应、不解析也不进行 CRC 比较。
- 响应 CRC 缺失可接受；存在时只作诊断，错误不改变返回值。
- 两处查询按 head 的实际 JSON 格式解析：`code=0`，字段在 `data` 内。
  无效 JSON、错误 code、缺失或类型错误的字段会打印告警。
- 不自动跟随重定向，避免将多个响应混入一次收集；3xx 返回错误。

## 本机验证

修改后的整个 `mipi_dsi_bridge.c` 已用现有 IDF 构建目录的真实 ESP32-P4
编译参数编译通过（额外加入新增 JSON 依赖的头文件目录）。

模拟 HTTP 测试夹具直接提取生产函数，覆盖分段、chunked、空响应、
恰好填满、溢出、传输失败、不完整响应、非 200、CRC 缺失/正确/错误/
格式错误、初始化失败等场景。运行：

```powershell
./tools/test_mipi_http.ps1 -CompileOnly
```

本机 Espressif clang 不支持 Windows 原生目标，当前只通过了夹具编译，
**断言尚未执行**。脚本可通过 `-Clang`、`-Linker` 指定原生 Windows LLVM
后运行断言。

完整 `idf.py build` 被现有虚拟环境阻断：它指向已不存在的 Python 3.10。
本批没有修改 Python 安装。**组件编译通过不等于新固件已生成。**

## 上板验收（先在可用 IDF 环境完整 build，再 flash）

1. head/controller 连同一热点并完成动态发现。
2. 调用 `self.mipi_dsi.eyes.get_expression`：应返回 head 当前表情，不再
   因空响应固定返回 unknown；日志显示 received/stored 非零、overflow=0、complete=1。
3. 调用 `self.mipi_dsi.touch.wake`：按实际 pending 状态返回；head 的该
   接口读取后会清 pending，第二次返回 false 可以是正常行为。
4. 摄像头、头灯、表情控制命令仍按原来执行；无响应 CRC 不产生错误。
5. 查询溢出、损坏 JSON 或缺少字段时明确告警，不使用截断内容。

尚未烧录或完成实机验收；久坐固定播报问题尚未在本批解决。
