# 大肥鱼

项目正在按最小 Agent 基线重新建设，采用 C++20、CMake 和 VS2022。

## 当前审查点：最小 Agent 终端流程

当前支持 DeepSeek、Qwen 的配置检查、单次调用和终端多轮问答。App 负责终端输入与展示，[Agent 会话](agent/session.cpp)维护当前上下文，[Model 模块](model/client.cpp)负责供应商协议，[Platform 模块](platform/winhttp.cpp)执行 HTTPS 请求。

Agent 通过独立的 [模型消息与调用接口](model/include/dafeiyu/model_client.hpp)同步请求完整文本，不处理供应商 JSON 或网络。会话只在完整回答成功后成对保存用户与助手消息，失败保持原上下文；本阶段没有工具循环和持久化记忆。

VS 解决方案当前按实际目标分组：

```text
dafeiyu
├── App
│   └── dafeiyu
│       └── Source
│           └── main.cpp / chat.cpp / chat.hpp / console.hpp
├── Agent
│   └── dfy_agent
├── Model
│   └── dfy_model
├── Platform
│   └── dfy_http
└── CMakePredefinedTargets
    ├── ALL_BUILD
    └── ZERO_CHECK
```

测试预设额外显示 `Tests/dfy_model_config_tests`、`Tests/dfy_model_client_tests` 和 `Tests/dfy_agent_tests`。其他职责分组随实际功能逐步加入；JSON 依赖是头文件接口目标，不生成独立库项目。

## 构建与运行

需要 CMake 3.28 或更新版本，以及安装了 C++ 桌面开发工具的 VS2022。在项目根目录执行：

```powershell
cmake --preset windows
cmake --build --preset windows-debug
.\build\rebuild\windows\bin\Debug\dafeiyu.exe
```

在 VS2022 中打开 `build/rebuild/windows/dafeiyu.sln`；默认启动项目为 `dafeiyu`。Release 构建使用 `cmake --build --preset windows-release`，产物位于 `build/rebuild/windows/bin/Release/`。

新预设使用独立的 `build/rebuild/windows/`，避免复用旧工程缓存和解决方案。首次配置会下载并校验固定版本的 JSON 依赖，参见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 模型配置

一份 JSON 文件描述当前选用的一个模型服务。分别提供 [DeepSeek 示例](config/deepseek.example.json)和 [Qwen 示例](config/qwen.example.json)，可复制为同目录的 `*.local.json` 后修改，本地副本已加入 Git 忽略规则。示例仅含环境变量名称，不含密钥。

| 字段 | 校验规则 |
|---|---|
| `provider` | 必填，`deepseek` 或 `qwen` |
| `base_url` | 必填，`https://域名[:端口][/前缀]`；域名和路径使用示例中的 ASCII 形式；端口为 1～65535；拒绝用户信息、查询参数、片段和未替换的占位符 |
| `model` | 必填非空文本，不限制模型名称清单，避免模型升级时修改程序 |
| `api_key_env` | 必填环境变量名称，格式为 `[A-Za-z_][A-Za-z0-9_]*`；离线检查仅验证名称，调用时读取凭据 |
| `timeout_ms` | 可选整数，范围 1～600000，缺省为 60000 毫秒；分别用于解析、连接、发送和接收阶段，不是请求总时限 |

文本字段不能含 ASCII 空白或控制字符；拒绝重复字段、未知字段、错误类型和非法 JSON。基址会去掉尾部斜杠，应填写接口前缀，不要包含 `/chat/completions`。错误提示定位字段或错误类别，不回显配置原文。

Qwen 示例使用百炼北京地域仍受支持的通用域名；使用其他地域或业务空间专属域名时，请按官方控制台调整 `base_url`，并在真正调用时使用对应地域的密钥。模型名称均可自行修改，离线检查不会查询供应商模型目录。

在项目根目录验证示例：

```powershell
.\build\rebuild\windows\bin\Debug\dafeiyu.exe --check-config config/deepseek.example.json
.\build\rebuild\windows\bin\Debug\dafeiyu.exe --check-config config/qwen.example.json
```

检查成功返回 0，配置或命令行错误返回 1。不带参数时仍进入最小终端，输入 `/exit` 或结束标准输入退出。

## 发起一次模型请求

准备 UTF-8 提示词文件（支持 BOM、中文路径和多行文本，正文最多 1 MiB），在启动进程的环境中设置配置引用的 API Key。例如在项目根目录使用 PowerShell：

```powershell
$env:DEEPSEEK_API_KEY = [System.Net.NetworkCredential]::new('', (Read-Host 'DeepSeek API Key' -AsSecureString)).Password
[IO.File]::WriteAllText((Join-Path $PWD 'build/提示词.txt'), '请用一句话介绍你自己。', [Text.UTF8Encoding]::new($false))
.\build\rebuild\windows\bin\Debug\dafeiyu.exe --complete config/deepseek.example.json 'build/提示词.txt'
```

Qwen 使用同一入口，将环境变量改为 `DASHSCOPE_API_KEY`，配置改为 `config/qwen.example.json`。命令向 `base_url/chat/completions` 发送一条用户消息，设置 `stream: false`，成功时只输出回答并返回 0；错误写入标准错误并返回 1。

本阶段固定关闭思考：DeepSeek 使用 `thinking.type: disabled`，Qwen 使用 `enable_thinking: false`。选择支持非流式、非思考文本调用的模型；仅思考或仅流式模型不在本次接入范围。参数依据 [DeepSeek 请求说明](https://api-docs.deepseek.com/api/create-chat-completion/)和 [Qwen 思考模式说明](https://help.aliyun.com/zh/model-studio/deep-thinking)。

缺失或含空白、非 ASCII 字符的密钥会在联网前被拒绝。网络层保留系统证书校验和系统代理行为，不跟随 HTTP 重定向，不自动重试；接收正文最多 8 MiB。HTTP 错误仅报告状态码（如 401/403 检查凭据与权限，429 检查限流或额度，5xx 检查服务状态），不回显服务正文。无效 JSON、缺失文本、空回答和非 `stop` 结束原因均视为失败，截断内容不会当作完整回答返回。

`--check-config` 仍完全离线；其成功不代表凭据、模型权限、地域或服务可用性已经验证。

## 启动 Agent 终端会话

在当前进程环境中设置相应 API Key 后，运行以下任一命令：

```powershell
.\build\rebuild\windows\bin\Debug\dafeiyu.exe --chat config/deepseek.example.json
.\build\rebuild\windows\bin\Debug\dafeiyu.exe --chat config/qwen.example.json
```

每行普通文本触发一轮同步请求：携带本会话此前成功的问答，等待完整回答，再保存本轮上下文并展示。空白行忽略；`/clear` 清空上下文但保留模型配置；`/exit` 或输入结束退出；其他以 `/` 开头的内容按未知命令处理，不发送给模型。一次输入为一行，多行提示词仍可使用 `--complete` 文件入口。

模型调用失败时显示错误，继续等待输入，失败提问不写入历史。会话消息正文总量（历史加本轮提问）上限为 1 MiB，超过时在联网前拒绝，不自动裁剪；可用 `/clear` 开始新上下文。供应商的 token 上限仍由服务校验。历史仅保存在进程内，退出即丢弃，也不会跨会话共享。

当前为单线程同步流程，等待模型响应期间不处理新输入，命令在请求结束后处理。正常 `/exit` 或输入结束返回 0，即使之前某轮请求失败；启动配置错误或输入输出故障返回 1。

Windows 控制台在运行期间临时切换 UTF-8，正常退出时恢复原编码；管道和文件重定向仍要求 UTF-8 字节。`--chat` 的输出包含提示符和状态提示，需要只获取回答文本时使用 `--complete`。

## 验证配置、调用与会话

测试使用隔离构建目录和生成的配置文件，不需要 API Key 或网络模型服务：

```powershell
cmake --preset windows-tests
cmake --build --preset windows-tests
ctest --preset windows-tests
```

配置测试覆盖两个服务示例、字段边界、地址规范化和非法文件。调用测试用替身传输验证供应商参数隔离、UTF-8 消息、凭据与超时传递、完整回答、HTTP 错误、超时传播、截断与畸形响应，以及非法输入在联网前被拒绝。离线测试不验证真实供应商凭据、模型可用性或 WinHTTP 网络连接；真实联通需使用自己的有效密钥运行上述命令。

会话测试替代模型服务，验证成功问答的消息顺序、失败回滚、清空、独立会话，以及终端命令、中文输入、错误后继续和流故障。新增模型协议测试验证历史消息的角色映射与内容总量边界。

本次阅读顺序：[Session 接口](agent/include/dafeiyu/session.hpp) → [会话实现](agent/session.cpp) → [终端循环](app/chat.cpp) → [启动入口](app/main.cpp) → [模型消息映射](model/client.cpp) → [会话测试](tests/agent_session.cpp)。后续审查点为最小事件机制。

## 旧工程归档

- 旧工程基线为 `f7a4fddf4272e8d5295db86584c8d13d44077232`。
- 该基线的全部 130 个受版本管理文件已原样保存到 [Old](Old/README.md)，包括旧配置、示例、测试和设计记录。
- 根目录保留忽略规则和通用中文注释规范；旧版本的对应文件也保存在 `Old/` 中。
- 旧构建缓存和运行数据保留在原位置，未归入版本控制。旧缓存包含原源码路径，应使用新的构建目录验证归档工程。
- `Old/` 不参与新根工程构建；如需验证旧工程，应将 `Old/` 作为独立 CMake 源码目录。

阅读旧实现请从 [旧 README](Old/README.md) 或 [旧架构说明](Old/docs/architecture.md) 开始；旧设计不作为新框架的强制需求。

## 已确定的新基线

1. Agent 只依赖独立模型接口；默认 Provider 随程序发布，空插件目录也能运行。
2. 首批覆盖 DeepSeek 和 Qwen 模型服务，先实现一次性完整文本响应。供应商差异留在模型层，端点、模型名称和凭据引用可配置。
3. 参考 pi 的模型层、Agent 核心和扩展层分工，不直接移植其完整功能集。
4. 基础设施包含运行时插件管理和最小事件系统；插件采用同进程可信 DLL 与 C ABI，忙碌时拒绝卸载。
5. 插件定义及默认配置随包发布，用户配置按插件分开，运行时信息汇总在内存中。
6. 记忆、工作等本体固有能力在基础框架通过审查后逐项添加。

DeepSeek 和阿里云百炼 Qwen 都提供兼容的 Chat Completions 接口，可复用基础请求与响应处理；具体兼容边界在接入阶段逐项验证。Qwen 的服务地域、端点和凭据须匹配，届时通过配置明确选择，不将地址或模型版本写死在 Agent 中。

- [DeepSeek 官方接口说明](https://api-docs.deepseek.com/)
- [Qwen 官方 Chat 接口兼容说明](https://help.aliyun.com/zh/model-studio/compatibility-of-openai-with-dashscope)
- [本次参考的 pi 版本](https://github.com/earendil-works/pi/tree/b2aabe02966a6b4528c7498a3bb406c14396932d)

## 后续审查顺序

剩余基础开发的模块边界、建议契约、16 个小步审查点及验收条件，见 [剩余基础开发指导](docs/foundation-development-guide.md)。其中明确区分已确认约束、当前代码能力和待实现建议。

归档 → 新构建骨架与 VS 分组 → 模型配置 → 单次真实调用 → 最小 Agent 终端流程 → 事件机制 → 插件发现 → SDK 与加载 → 安全卸载 → 本地包管理 → 联通验收。

这些是逐次审查点，复杂步骤继续拆分。每次只提交一个可审查的功能点，完成后停止，等待用户审查再继续。开发及注释要求见 [AGENTS.md](AGENTS.md)。
