# MCP 延迟结果最小验证（2026-10-27）

本批新增 `self.test.delayed_result`，验证普通 MCP 工具结果能否在后台完成后回到当前小智对话。只允许 `delay_ms=0` 或 `3000`，不接 Docker / 豆包，不是摄像头识图工具。

## 上板操作

只烧 **controller / COM6**，head 不需要烧录。当前代码保留真实小智服务配置，没有切到模拟服务器。

1. 在 ESP-IDF 终端进入 `C:\Users\Administrator\Desktop\try_PZF\controller`，执行 `idf.py -p COM6 flash monitor`。若串口已被监视器占用，先退出旧监视器。
2. 联网后正常唤醒，先确认普通对话仍能回答。
3. 说：“做一次通信延迟测试，延迟零毫秒，返回校验码7301，收到结果后读出来。”
4. 结束这轮回答后再说：“做一次通信延迟测试，延迟三千毫秒，返回校验码7301，收到结果后读出来。”
5. 保存包含 `MCP_PROBE`、MCP 调用和 TTS 起止的完整串口日志。若模型没有选择该工具，此次不算已测试，可明确说出 `self.test.delayed_result` 和参数。

预期：0 秒请求无需人为等待；3 秒请求在 worker 中等待后回包。云端接受结果后应通过原对话音色回答。**是否接受延迟回包、是否启动 TTS、喇叭是否出声，仍需上述实机验证。** 模拟端无法证明这三项。

## 实现边界

- 精确拦截本工具的普通 `tools/call`，其余 MCP 请求沿原 SDK 路径处理。工具声明不使用 MCP task 模式。
- 返回标准 JSON-RPC `CallToolResult`，不发送临时“已受理”结果，也不稍后发送第二个响应。
- 独立低优先级 worker；最多一项在途，忙则拒绝、不排队。同一代际同 ID 的在途重复调用不再启动 worker。
- 复制响应字节、原始 JSON 编码 ID、session ID；字符串/数字 ID 类型保留。
- 断线、重连、关闭音频会话、切换 session 均使旧代际失效。取到 WebSocket 共享发送/状态锁后再次检查归属，旧结果不发到重连后的连接。
- 关闭连接前由控制任务等实际发送返回再释放对象；事件回调只失效、不等待，不强制删除 worker。发送超时不是严格的整个调用截止时间。
- worker 栈为 6144 字节、优先级 2；任务结束释放动态缓冲。实际内部堆和栈余量需板端观察。
- 原音频发送 10 秒、普通 text 超时、心跳和重连参数保持原值。未改摄像头、光感、机械臂、网页或算法容器。

## 日志分段判读

1. `MCP_PROBE: request`：请求进入，含 ID / session / epoch / delay。
2. `worker ... ready_ms=...`：worker 完成等待，准备回包。
3. `response ... write=... current_after=...`：传输写入返回；**不是云端确认或出声确认**。
4. `discard ...`：连接或会话变化，旧任务放弃。
5. 另外对照连接日志、TTS START、音频播放日志和实际听感。前四项成立而没有 TTS，不能说“接收线程被堵”或“协议一定失败”。

## 本地验证结果

- `tests/test_mcp_delayed_probe.py`：11 项通过。实际编译运行生产 C worker，主机替代 RTOS / 发送回调；包括复制所有权、忙拒绝、断线同 session 重连、换会话、取发送锁后复查、等待在途发送、分配和启动失败、重复释放。
- 其中 localhost WebSocket 模拟使用 `tools/mcp_probe_fixture.py`：0 秒基线、3 秒延迟与并发 ping。一次观测为基线 0 ms、ping 16 ms、延迟 3031 ms。这是主机测试结果，接收端是 Python fixture，**没有执行 ESP-IDF 的真实 WebSocket 接收任务，也没有连接真实小智**。
- 原有播报生命周期 13 项、TTS 采集 26 项通过。这些包含源码约束、参考策略模型及主机 HTTP/格式测试，不等于实机音频验收。
- controller ESP-IDF 5.5.4 编译通过；未烧录、未调用真实云端。

本批产物：`build/controller.bin` 为 4,036,624 字节，app 分区剩余 36%。SHA256：

- BIN：`10c3d40842da6fd2732492e718c1d4841550db8762ddce42a53a8dca3da4080a`
- ELF：`08fe9deac269ed998021f71e49bfd3b18a28996ea0cbfb1dc3255e4b47aa6767`

主机测试（Windows）：使用已有 x86-64 clang，或在 `build/host_toolchain` 安装 `ziglang==0.13.0`；交叉编译器不能代替主机运行测试。Python 还需 `websockets`。

```powershell
python -m pip install --target build/host_toolchain ziglang==0.13.0
python tests/test_mcp_delayed_probe.py
```

## 可复用模拟端

```powershell
python tools/mcp_probe_fixture.py --host 127.0.0.1 --port 9011
```

默认只监听本机。若以后要实际控制器接入，需另行设置测试固件服务地址和电脑可达 IP，显式改为 `--host 0.0.0.0`；本批未修改服务地址、未开放防火墙、未执行此实机实验。模拟端发送 hello、initialize、tools/list、0 秒调用、3 秒调用和并发 ping，校验 ID 类型、session、结果文字和顺序。它不提供 TTS，不保存或转发上行音频，也不能验证原音色。

## 托管依赖与回退

- 本批托管组件差异保存为 `patches/c5_mcp_delayed_probe_20261027.patch`。它以本批修改前的现有依赖为基线；**不是直接对全新上游版本应用的补丁**。依赖刷新后先恢复此前本地补丁与会话作用域改动，再检查本补丁；应用层 `components/mcp_delayed_probe` 和 manager 集成也必须保留。
- 已在临时目录对留存基线执行 `git -c core.autocrlf=false apply --check`、实际应用并逐字节比对，10 个托管文件一致；没有在当前源码上重复应用。
- 修改前留存在 `C:\Users\Administrator\Desktop\try_PZF\version_snapshots\2026-10-27_before_mcp_delayed_probe\before.zip`，包含相关旧源码与旧 `build/controller.bin`。
- 回退源码时还需移除新增组件并恢复 manager CMake；回退固件可恢复快照中的旧 app。当前分区表 app 偏移 `0x10000`，使用当前 ESP-IDF 工程烧录流程，不执行擦除 NVS。
- 0 秒与 3 秒真实小智验证完成前，不把 10/20 秒试验、Docker 识图或网页任意文字朗读混进本批。
