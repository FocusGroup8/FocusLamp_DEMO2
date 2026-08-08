# 开发任务执行规范

在执行开发任务前，若当前工作环境中已存在分支，需首先通过交互工具向用户发起询问，确认是否需要新建分支并切换至该新分支进行后续操作。整个开发流程需严格按照分阶段方式执行，每个阶段完成后，必须通过交互工具向用户提问以获取该阶段实际执行结果和详细反馈信息，在得到用户明确确认后，方可进入下一阶段。

当面临多方案选择的情况时，必须通过交互工具向用户清晰呈现各方案的具体优缺点、适用场景及实施难度等关键信息，由用户自主决策后，再依据用户选择继续执行相应操作。整个过程中，所有提问、确认环节及信息展示均必须通过交互工具进行，严禁使用纯文字形式进行沟通。对于任何未明确提供的信息，禁止进行假设，必须通过交互工具询问用户并获得确认。应合理使用子代理以实现高效的批量查找与操作。

所有操作必须基于本地 ESP-IDF SDK（例如位于 C:\\Software\\Espressif\\.espressif\\v6.0.1\\esp-idf 目录）和本项目中 managed_component 目录内组件的实际情况进行。严禁修改 sdkconfig(.defaults) 文件以及 managed_component 目录下的任何内容，若开发过程中需要进行相关配置，必须指导用户通过 menuconfig 进行配置并保存，并通过交互工具询问用户配置是否已完成。

由于 Agent 不具备 idf.py 运行环境，对于 idf.py build、idf.py flash、idf.py monitor 等指令，必须明确指导用户在其本地环境中自行运行相应命令，并通过交互工具询问用户命令是否执行成功。

根据开发需求，应优先在 ESP Component Registry（ idf.py add-dependency *** ，并指导用户运行 idf.py reconfigure 命令将所需组件实际下载至本地开发环境。

任何代码变更在提交 git 之前，必须经过实际烧录测试验证通过。具体流程为：代码修改完成后，先指导用户在本地环境执行 `idf.py build && idf.py flash monitor` 编译烧录固件，并由用户实际运行测试确认功能正常、性能达标、无崩溃或回退后，方可执行 `git add` 与 `git commit`。严禁在未经验证的情况下直接提交代码。若测试未通过，需根据用户反馈的问题继续修复，直至测试通过后再提交。

修改代码或文档时，必须同步修改相关注释、设计文档或其他说明性信息（如函数说明、参数说明、示例、README 等），确保注释与文档始终与代码现状保持一致，禁止出现注释或文档与实现脱节的情况。

## 问题排查与诊断规范

排查问题时，优先在相关代码路径添加串口调试信息（关键状态、坐标、失效区域、事件时序等），以获取可观测证据；同时指导用户描述或注意相关现象（如残留位置、颜色、触发时机、消失条件），据此缩小根因范围后再实施修复。

若某问题经过诊断修复三轮仍未修复，须生成后续任务及提示词（包含问题现象、已尝试方案、候选方向），供用户在新对话或本对话的后续继续执行任务；给出相应内容后结束当前对话，避免在同一对话中无限迭代。

## 日志输出规范

1. **临时调试日志**：为排查突发问题而临时添加的调试日志（关键状态、坐标、失效区域、事件时序等），必须在代码中以注释明确标注其临时调试用途与所针对的问题现象；问题修复后必须删除该日志，禁止遗留至 git 提交。
2. **常驻监控日志**：周期性输出的监控类日志（如系统资源监控），必须通过 Kconfig 配置开关控制且**默认关闭**；Kconfig 中必须为对应开关添加 help 帮助信息，说明其作用与影响。用户可通过 menuconfig 开启/关闭该开关。

## 开发资源查找优先级

在实现项目需求时，开发人员应遵循以下资源查找与使用优先级顺序：

1. 优先参照 ESP-IDF SDK 官方提供的例程（examples 目录）进行开发；
2. 若官方例程中未找到相关实现，应从 ESP Component Registry 查找合适的组件，并提供具体的安装命令指导用户获取；
3. 若前两种途径无法满足需求，可在 GitHub 或其他开源平台搜索相关项目的例程及可复用代码片段；
4. 若以上资源均无法获取可参考内容，需主动询问用户是否能提供相关例程或参考资料；
5. 仅当所有上述途径均无法获得有效参考时，才进行独立设计与实现。

## Host 单测规范

纯逻辑组件（不依赖 ESP-IDF API、仅依赖 C 标准库与项目内其它纯算法组件）的单元测试采用 **Host 侧独立 C 程序**方式，不引入 unity/CMock 等框架：

1. **文件位置与命名**：测试源文件置于 `components/<组件>/test/`，命名为 `test_<被测对象>_host.c`；被测对象须保证零 ESP-IDF 头依赖（`sudoku_*` 组件返回自有 `sudoku_err_t` 枚举，随机源以弱符号/覆盖注入）。
2. **构建与运行**：从仓库根目录用宿主 gcc 直接编译（示例）：
   ```bash
   gcc -O2 -Wall -I components/<组件>/include \
       [-I components/<依赖组件>/include ...] \
       components/<组件>/src/<源>.c \
       [components/<依赖组件>/src/<源>.c ...] \
       components/<组件>/test/test_<对象>_host.c -o test_<对象>
   ./test_<对象>          # Windows: test_<对象>.exe
   ```
   依赖其它纯算法组件时须一并编译其源文件（如 `sudoku_game` 依赖 `sudoku_solver` + `sudoku_puzzle`）。
3. **断言与结果约定**：使用文件内 `CHECK(cond, msg)` / `CHECKF(cond, fmt, ...)` 宏累加 `s_checks`/`s_failures`；退出码 0 表示全部通过，失败打印 `FAIL 文件:行号: 信息`。
4. **运行时机**：被测组件源码变更后、提交 git 前，须在本地运行对应 host 测试并确认全部通过。
5. **产物规范**：`test_<对象>.exe` 等 host 构建产物为临时文件，**禁止提交 git**；可在 `.gitignore` 中排除或仅保留于本地工作区。

## 文档规范

Markdown 文档（.md 文件）中的所有示意图（架构图、流程图、数据流图、状态图等）必须使用 Mermaid 格式绘制，禁止使用 ASCII 字符画或图片占位。唯一例外：目录/文件结构树（tree）保留为代码块文本形式。

- 示意图统一使用 ```mermaid 代码块
- 流程图优先使用 `flowchart`，时序图使用 `sequenceDiagram`，状态图使用 `stateDiagram-v2`
- 文档内容与代码现状必须保持一致，参数值、端点、模块名等以实际代码为准
- 源码注释（.h/.c）保持 Doxygen 英文风格，项目设计文档使用中文

## Git 提交规范

项目通过 `scripts/hooks/install-hooks.sh` 安装 git hooks（`pre-commit` + `commit-msg`），提交时必须满足以下约束，否则会被 hook 拦截（`git commit` 返回非零退出码，无输出或仅报 `cannot spawn .git/hooks/<hook>`）：

1. **提交信息格式（commit-msg hook）**：遵循 Conventional Commits，首行 `类型(可选作用域): 主题`，首行总长度 **≤ 72 字符**；类型限 `feat|fix|refactor|docs|chore|test|style|perf|ci|build|revert`，作用域为小写字母/数字/`-`/`_`。首行超长或不匹配格式即被拒绝。
2. **代码格式（pre-commit hook）**：暂存的 `main/` 与 `components/` 下 `.c/.h` 文件必须通过 `clang-format --dry-run --Werror` 检查。提交前先本地执行 `clang-format -i <文件>` 并 `git add` 后再提交。
3. **hook 可执行权限**：git 在 Windows 上仅 spawn 带可执行位（755）的 hook 文件；若 hook 文件缺失执行位，git 报 `error: cannot spawn .git/hooks/pre-commit: No such file or directory`。新建/重装 hook 一律使用 `bash scripts/hooks/install-hooks.sh`（源文件 755，`cp` 保留权限），**禁止**用 IDE 直接新建 hook 文件（默认 644 无法被 git 执行）。
4. **PowerShell 提交提示**：hook 的 stderr 输出在 PowerShell 下会显示为 `git : <hook 输出>` 的 CLIXML 错误记录，这是 PowerShell 将原生命令 stderr 渲染为错误流的正常现象，**并非 git 故障**。提交失败时应以 hook 的具体输出为准定位原因（消息格式 / clang-format / 权限），而不是误判为环境问题。
5. **紧急绕过**：仅限紧急情况使用 `git commit --no-verify`，绕过前必须确认变更已经过烧录验证。
