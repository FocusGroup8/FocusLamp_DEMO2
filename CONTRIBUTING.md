# 贡献指南

## 分支管理策略

| 分支 | 用途 | 命名规则 |
|------|------|----------|
| `main` | 稳定发布分支，仅合并经过测试的代码 | `main` |
| `develop` | 开发集成分支，日常开发合并目标 | `develop` |
| `feature/*` | 新功能开发分支 | `feature/<功能名>` |
| `fix/*` | Bug修复分支 | `fix/<bug描述>` |
| `refactor/*` | 重构分支 | `refactor/<重构内容>` |
| `docs/*` | 文档更新分支 | `docs/<文档内容>` |

### 分支工作流
1. 从 `develop` 创建功能分支
2. 开发完成后提Pull Request到 `develop`
3. `develop` 定期合并到 `main` 发布版本

## 提交规范 (Conventional Commits)

### 格式
```
<type>(<scope>): <subject>

<body>

<footer>
```

### Type 类型
| 类型 | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug修复 |
| `docs` | 文档变更 |
| `style` | 代码格式调整（不影响功能） |
| `refactor` | 重构（既不是新功能也不是修bug） |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `chore` | 构建、工具、依赖等杂项 |
| `ci` | CI配置变更 |

### Scope 作用域
| 作用域 | 说明 |
|--------|------|
| `lcd` | 屏幕显示相关 |
| `touch` | 触控相关 |
| `gesture` | 手势识别 |
| `gui` | GUI库 |
| `game` | 触控游戏 |
| `config` | 配置文件 |
| `build` | 构建系统 |
| `core` | 核心初始化 |

### Subject 标题
- 使用祈使句（如"add"而非"added"）
- 首字母小写
- 不超过50字符
- 结尾不加句号

### Body 正文
- 解释"为什么"而非"做了什么"（代码已说明做了什么）
- 每行不超过72字符

### Footer 脚注
- BREAKING CHANGE: 描述破坏性变更
- Closes #issue: 关联issue

### 示例
```
feat(lcd): add ST7701S initialization sequence for KD034WXFID001

Implement the complete init command sequence from the panel spec
to support 480x480 resolution at 60Hz via MIPI DSI interface.

Closes #12
```

```
fix(gesture): resolve two-finger gesture misjudgment

Two-finger gestures were sometimes detected as single-finger swipes
due to race condition in touch point tracking. Added strict touch_cnt
check before processing single-finger gestures.
```

## 代码规范

### 格式化
- 使用 `.clang-format` 配置文件自动格式化
- 提交前运行: `clang-format -i main/*.c main/*.h`
- 基于 ESP-IDF 官方风格（4空格缩进，120字符行宽）

### 命名规范
- 函数/变量: `snake_case`
- 宏/常量: `UPPER_SNAKE_CASE`
- 静态变量前缀: `s_`
- 公共函数前缀: 模块名 (如 `gui_`, `gesture_`)
- 类型定义: `snake_case_t`

### 文件头
- 每个文件包含 SPDX 许可证头
- 模块文件包含功能说明注释块

### 日志规范
- `ESP_LOGE`: 错误（影响功能）
- `ESP_LOGW`: 警告（潜在问题）
- `ESP_LOGI`: 重要事件（初始化、检测结果）
- `ESP_LOGD`: 调试信息（开发时使用，生产关闭）
- `ESP_LOGV`: 详细追踪（极少量使用）
- 避免在循环中高频使用 `ESP_LOGI`，改用 `ESP_LOGD`
