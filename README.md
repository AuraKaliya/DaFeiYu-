# 大肥鱼

项目正在按最小 Agent 基线重新建设，采用 C++20、CMake 和 VS2022。

## 当前进度：R09i-3b C 组合数组访问已验收

R06 已接入生产 v2 模块加载及宿主能力，R07b 已提供 [实例控制器](plugins/runtime/include/dafeiyu/plugin_instance.hpp)。R08 使用独立纯 C 观察包验收了订阅、延后通知、反射读取、取消、关闭、卸载及重新加载，并覆盖部分启动失败和多实例故障隔离。

R09 已实现有界嵌套结构、可选字段与显式空值、标量数组、字符串键标量映射、闭合枚举及带标签联合，以及对应 C 反射接口。实际观察包已升级为 0.4.0，支持这些类型并限制消费深度和访问数量。任意组合容器元素仍待补齐。App／Agent 当前仍为同步问答，正式异步迁移留在 R15。

本次 Debug／Release 主工程及完整测试构建通过，完整 Debug、Release CTest 均为 49／49。宿主及 C 版本 2 已提供组合数组与完整元素描述，下一单元验证真实 C 消费及事件预算链，映射值随后扩展。范围和验收见 [开发指导](docs/foundation-development-guide.md)。

[v1 插件模块](plugins/runtime/include/dafeiyu/plugin_module.hpp)保留冻结的查询路径；[v2 插件模块](plugins/runtime/include/dafeiyu/plugin_module_v2.hpp)独立管理版本化能力、代码持有和活动调用。实例控制器负责 v2 实例的创建、启动、分阶段停止和失败保留；终端管理命令与安装尚未接入。

[ABI v1 契约](docs/plugin-abi.md)及查询示例保持原行为；[v2 契约](docs/plugin-abi-v2.md)和 [数据接口](sdk/include/dafeiyu/data_abi_v2.h)提供独立能力协商。普通数据由宿主管理，数据引用与 DLL 代码寿命分别计量。

[插件配置接口](plugins/runtime/include/dafeiyu/plugin_config.hpp)合并包默认配置与按插件 ID 隔离的用户设置，返回独立只读快照。配置读取不写文件、不修改模型配置，也不加载插件实例。

[插件目录注册表](plugins/runtime/include/dafeiyu/plugin_registry.hpp)支持显式扫描根目录，隔离坏包和重复 ID，并返回包含描述、诊断和变更记录的只读快照。当前不加载 DLL，也未接入终端管理命令。

[Agent 会话](agent/session.cpp)已接入独立的 [事件模块](events/include/dafeiyu/events.hpp)，发布问答开始、成功、失败和清空通知。通知用于观察生命周期，观察者失败不会改变模型结果；终端入口创建宿主总线，当前不注册观察者。

当前支持 DeepSeek、Qwen 的配置检查、单次调用和终端多轮问答。App 负责终端输入与展示，[Agent 会话](agent/session.cpp)维护当前上下文，[Model 模块](model/client.cpp)负责供应商协议，[Platform 模块](platform/winhttp.cpp)执行 HTTPS 请求。

Agent 通过独立的 [模型消息与调用接口](model/include/dafeiyu/model_client.hpp)同步请求完整文本，不处理供应商 JSON 或网络。会话只在完整回答成功后成对保存用户与助手消息，失败保持原上下文；本阶段没有工具循环和持久化记忆。

VS 解决方案当前按实际目标分组：

```text
dafeiyu
├── App
│   └── dafeiyu
│       └── main.cpp / chat.cpp / chat.hpp / console.hpp
├── Agent
│   └── dfy_agent
├── Model
│   └── dfy_model
├── Platform
│   ├── dfy_runtime
│   ├── dfy_events
│   ├── dfy_event_streams
│   ├── dfy_http
│   └── dfy_library
├── SDK
│   └── dfy_sdk
├── Plugins
│   ├── dfy_plugins
│   ├── dfy_query_example
│   └── dfy_observer
└── CMakePredefinedTargets
    ├── ALL_BUILD
    └── ZERO_CHECK
```

测试预设额外显示 `Tests` 下的模型、会话、事件流、运行时、插件和 SDK 验收目标，当前共 47 项 CTest；完整清单见 [测试构建](tests/CMakeLists.txt)。其他职责分组随实际功能逐步加入；JSON 依赖是头文件接口目标，不生成独立库项目。

加载与查询测试另外提供 `Tests/dfy_library_tests`、`Tests/dfy_code_module_tests`、`Tests/dfy_plugin_module_tests` 及专用 DLL 夹具目标。测试构建需要 VS2022 的 x86 和 x64 C/C++ 工具；Win32 DLL 仅用于验证架构拒绝，不改变本体与 SDK 的 x64 目标。

运行时基础另有 `Tests/dfy_runtime_ids_tests`、`Tests/dfy_runtime_handles_tests`、`Tests/dfy_runtime_values_tests`、`Tests/dfy_runtime_struct_descriptors_tests`、`Tests/dfy_runtime_types_tests`、`Tests/dfy_runtime_struct_values_tests`、`Tests/dfy_runtime_executor_tests`、`Tests/dfy_runtime_operations_tests`、`Tests/dfy_runtime_notifications_tests`、`Tests/dfy_runtime_resources_tests`；v2 契约另有查询、引用、操作和通知四个测试目标。`dfy_runtime` 的范围及逐步验收见 [开发指导](docs/foundation-development-guide.md#31-宿主签发域标识r02a-1)。

各目标的自有源码统一按模块内实际目录展示，不额外套一层 `Source`；公开头文件保留 `include/dafeiyu` 路径。测试目标借用的业务实现保留 `agent/`、`app/`、`model/` 来源路径。职责分组是 IDE 虚拟分组，不改变磁盘目录；Qt Creator 仍可能同时显示目录中的 CMake 配置入口。

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

## 会话事件与同步重入保护

宿主先创建 `events::Bus`，再以 `Session(config, bus)` 注入同一个总线实例，并保证其覆盖会话的使用期。会话创建和操作均限同一宿主线程；会话不可复制或移动。`id()` 返回进程内非零且不复用的会话标识，`message_count()` 允许观察者读取已经提交的消息数量，不向事件中放入对话正文。

| 事件 | 发布时机 | 回合标识 |
|---|---|---|
| `turn.started` | 通过忙碌与编号余量检查、接受回合后，构造请求和调用模型前 | 从 1 开始，每个接受的回合递增 |
| `turn.completed` | 完整问答已成对提交到历史后 | 与本轮开始相同 |
| `turn.failed` | 本轮准备、模型调用或新历史构造失败，原历史保持不变时 | 与本轮开始相同，与成功事件互斥 |
| `session.cleared` | 清空成功后；已空历史也发布一次 | 固定为 0，不创建问答回合 |

失败回合占用自己的序号；清空不重置序号。会话或回合标识耗尽时明确拒绝，避免回绕复用。会话忙碌期间，以及共享总线正在分发任何事件时，`ask` / `clear` 抛出 `session: busy`，不创建回合、不改变历史或通知报告；因此回调操作绑定同一总线的另一会话也会被拒绝。操作的忙碌保护持续到终态或清空通知结束，模型失败后自动恢复空闲。

`last_notifications()` 在操作返回或抛出后给出最近一次已接受操作的通知报告，分别保留 `started` 和 `finished` 的成功数、失败数与首个失败订阅归属。清空只有 `finished`，其 `started` 为零计数。观察者抛错不会阻断其他订阅者，不会改变成功回答，也不会替换原模型异常；报告由宿主按需读取，不通过事件再次发布诊断。`--complete` 直接调用模型，不产生会话事件。

## 验证配置、调用与会话

测试使用隔离构建目录和生成的配置文件，不需要 API Key 或网络模型服务：

```powershell
cmake --preset windows-tests
cmake --build --preset windows-tests
ctest --preset windows-tests
```

配置测试覆盖两个服务示例、字段边界、地址规范化和非法文件。调用测试用替身传输验证供应商参数隔离、UTF-8 消息、凭据与超时传递、完整回答、HTTP 错误、超时传播、截断与畸形响应，以及非法输入在联网前被拒绝。离线测试不验证真实供应商凭据、模型可用性或 WinHTTP 网络连接；真实联通需使用自己的有效密钥运行上述命令。

会话测试替代模型服务，验证成功问答的消息顺序、失败回滚、清空、独立会话，以及终端命令、中文输入、错误后继续和流故障；事件部分使用真实总线，验证通知时的历史状态、调用顺序、唯一终态、关联标识、清空后编号连续、模型调用与回调中的重入拒绝，以及观察者失败不覆盖模型异常。模型协议测试验证历史消息的角色映射与内容总量边界。

会话阅读顺序：[Session 接口](agent/include/dafeiyu/session.hpp) → [会话实现](agent/session.cpp) → [终端循环](app/chat.cpp) → [启动入口](app/main.cpp) → [模型消息映射](model/client.cpp) → [会话测试](tests/agent_session.cpp)。

## 独立异步观察插件

[观察插件](plugins/examples/observer/observer.c)是仅依赖公开 SDK 的 C11 DLL，构建后形成 `build/rebuild/windows/plugins/examples/observer/<Debug或Release>/example.observer/` 目录包，包含 DLL、清单及空默认配置。该包目前由集成测试显式装配；App 尚未提供加载命令。

在项目根目录复现实际包的完整链路：

```powershell
cmake --build --preset windows-tests
ctest --test-dir build/rebuild/windows-tests -C Debug -R '^plugins.observer$' --output-on-failure
cmake --build build/rebuild/windows-tests --config Release
ctest --test-dir build/rebuild/windows-tests -C Release --output-on-failure
```

[集成测试](tests/observer.cpp)验证真实模块卸载、排队通知撤销、取消等待、重新加载、多实例失败隔离及组合事件的有界消费。数组和映射当前仅包含六种基础值及独立允许的 null；它们不能容纳其他组合类型。详细类型约束、引用寿命及错误结果见 [生产数据契约](docs/plugin-abi-v2.md#生产数据能力与组合访问)。

## 独立同步事件基础

`dfy_events` 只依赖标准库，不依赖 Agent、模型、网络或 DLL。每个 `Bus` 独立保存订阅，单线程同步发布；每个订阅者接收全部事件，可按 `Kind` 自行选择处理。事件只含种类、会话标识和回合标识，由 Agent 按上述规则分配标识并发布。

- `subscribe` 接收调用方分配的非零所有者标识和非空回调，返回本总线内不复用的订阅标识。以后不同插件实例应使用不同所有者标识。
- `unsubscribe` 返回是否取消了有效订阅；`unsubscribe_owner` 返回该所有者被取消的有效订阅数。未知、重复或零标识不会取消其他订阅。
- 按注册顺序通知；分发中新订阅下次生效，被取消而尚未调用的订阅本次立即跳过。当前回调可以取消自身并继续执行，取消项在本次发布结束前集中销毁；空闲时取消立即销毁。
- `publish` 返回固定大小的报告，不传播回调异常。同一总线嵌套发布返回 `reentrant` 且不通知；正常遍历返回 `completed`，成功数和失败数分别统计，失败时记录首个失败订阅及其所有者，不保存异常对象或文本。
- `publishing()` 提供只读分发状态，供会话在改动业务状态前拒绝嵌套通知。
- 事件引用只在回调内有效；总线须活到发布结束，回调对象的析构不得重入总线。取消保证不再调用，不能将取消数量等同于回调资源已释放或 DLL 可安全卸载。

事件测试覆盖空总线、非法订阅、顺序与回调状态保持、独立总线、重复取消、所有者隔离、分发中增删、自取消资源生命周期、不同异常隔离、嵌套发布拒绝及错误后继续使用。

事件基础阅读顺序：[事件接口](events/include/dafeiyu/events.hpp) → [分发实现](events/bus.cpp) → [契约测试](tests/events.cpp)。

## 插件清单

`plugins::load_manifest(目录)` 只读取该目录中的 `plugin.json`，返回不依赖 JSON 类型的包描述和规范化绝对路径。清单使用 UTF-8（支持 BOM），总大小含 BOM 最多 64 KiB；拒绝重复字段、未知字段、错误类型及无效 JSON。完整目录示例见 [开发指导](docs/foundation-development-guide.md)。

| 字段 | 当前规则 |
|---|---|
| `manifest_version` | 必填整数，固定为 `1` |
| `id` | 必填，最多 64 字节；格式 `[a-z][a-z0-9]*([._-][a-z0-9]+)*`，拒绝 Windows 保留设备名 |
| `name` | 必填 UTF-8 文本，最多 256 字节 |
| `version` | 必填 UTF-8 文本，最多 64 字节；仅作描述，不作版本依赖求解 |
| `abi_version` | 必填整数，范围 1～4294967295；此处只记录，兼容性留待加载阶段检查 |
| `entry` | 必填包内相对文件路径，最多 1024 字节，扩展名须为 `.dll`（不区分大小写） |
| `default_config` | 可省略；提供时须为包内相对文件路径，最多 1024 字节 |

文本不允许空串、纯 ASCII 空格或 ASCII 控制字符。路径支持中文、空格和子目录；拒绝绝对路径、`.` / `..` 分量、Windows 特殊字符、保留设备名及以点或空格结尾的分量。包目录、清单及引用文件路径不接受符号链接或目录联接；入口和已声明的默认配置必须是现有普通文件。默认配置内容由下述配置接口读取，DLL 的结构、导出和 ABI 在后续加载步骤检查。

路径校验反映读取当时的文件状态，不保证防止并发替换；后续安装和加载应重新核对边界。单包解析不检查跨包 ID 冲突，由下述注册表处理；输入目录名不必等于 ID。宿主模型调用不依赖该模块。

清单测试创建真实文件、文件符号链接和目录符号链接，运行账户需具备创建链接权限；测试入口是普通文本文件，解析无需可加载的 DLL。测试覆盖字段与长度边界、中文路径、版本、文件缺失及越界和链接拒绝。

清单阅读顺序：[清单接口](plugins/runtime/include/dafeiyu/plugin_manifest.hpp) → [解析实现](plugins/runtime/manifest.cpp) → [清单测试](tests/plugin_manifest.cpp)。

## 插件发现与注册表

`Registry(root)` 固定非空根目录的绝对路径，初始快照为空；只有显式调用 `refresh()` 才扫描。注册表采用单线程操作，不创建目录、不递归发现，也不执行插件代码。缺失或空根目录产生空结果；根目录为文件、链接或扫描失败时抛出错误，保留上一次完整快照。

只检查根目录的直接子项：忽略普通文件，点开头的子项及其子树保留给宿主暂存用途。其余子项按路径排序，分别调用清单解析器；坏包、链接或不可读子项记录独立诊断，不妨碍有效包被发现。所有有效清单按 ID 检查冲突，同一 ID 的全部冲突包都被排除并各自记录诊断，不选择扫描顺序中的第一个包。

`snapshot()` 返回 `shared_ptr<const Catalog>`，包含以下内容；旧快照可保留到刷新之后，也可在注册表销毁后继续读取：

| 字段 | 含义 |
|---|---|
| `packages` | 按 ID 排序的有效且无冲突的包描述 |
| `diagnostics` | 按候选路径排序的坏包及冲突原因；成功刷新时替换，不累计历史错误 |
| `changes` | 相对上一次成功刷新的可用目录差异，按 ID 排序，类型为新增、移除或描述变更 |

首次发现有效包记为新增；未变化的刷新产生空差异。包失效或新增同 ID 冲突会使原可用 ID 记为移除，修复后重新记为新增。描述变更只比较清单字段与规范化路径，不比较 DLL、默认配置文件内容或修改时间；每次仍会重新校验引用文件是否存在且位于包内。目录信息与未来加载实例的状态分开保存，当前快照不含 DLL 句柄或有效配置。

发现测试覆盖缺失和空目录、坏包隔离、链接拒绝、保留目录、多个同 ID 包、冲突解除、显式更新及新增移除、旧快照存活和根目录故障恢复。

注册表阅读顺序：[注册表接口](plugins/runtime/include/dafeiyu/plugin_registry.hpp) → [刷新实现](plugins/runtime/registry.cpp) → [发现测试](tests/plugin_registry.cpp)。

## 插件配置快照

`plugins::load_config(manifest, user_data_root)` 接收清单或注册表提供的描述，以及宿主指定的非空用户数据根目录。先重新校验清单及引用文件；描述发生变化时要求刷新目录，避免沿用旧 ID 或旧配置路径。相对用户根路径在本次调用时转为绝对路径，App 后续装配时应按程序位置确定默认根目录。

| 来源 | 规则 |
|---|---|
| 包默认配置 | 读取清单声明的 `default_config`；未声明时为 `{}`，声明后缺失则报错 |
| 用户配置 | 读取 `<user_data_root>/plugins/<id>/settings.json`；目录或文件尚不存在时为 `{}`，不自动创建 |
| 合并结果 | 先取默认对象，再用用户对象的同名顶层键覆盖；嵌套对象、数组整体替换，`null` 保留为值 |

每份文件须为 UTF-8 JSON 对象（支持 BOM），含 BOM 最多 64 KiB，容器嵌套最多 64 层。拒绝任何对象层级内的重复键、无效 JSON、错误根类型及过深结构；错误不回显配置内容。宿主不限制插件自有字段，字段含义待插件初始化时自行校验。用户路径逐段检查目录与普通文件，拒绝符号链接、目录联接、错误文件类型及越出预期目录的定位。

结果为 `shared_ptr<const EffectiveConfig>`，保存插件 ID 和合并后的 UTF-8 JSON 对象文本，公开接口不依赖 JSON 库。重新读取产生新快照，成功或失败都不修改旧快照。注册表仍只保存包描述，刷新不会自动读取配置；未来加载实例将持有自己的配置快照，暂不提供在线配置回调。默认与用户文件之间的读取不构成文件系统事务，仍需避免并发替换。

配置测试覆盖空配置、单侧配置、覆盖顺序、嵌套对象及数组替换、显式 `null`、插件隔离、快照保持、BOM、大小与嵌套边界、重复键、缺失默认文件、过期描述及真实路径链接拒绝。

配置阅读顺序：[配置接口](plugins/runtime/include/dafeiyu/plugin_config.hpp) → [读取与合并](plugins/runtime/config.cpp) → [配置测试](tests/plugin_config.cpp)。

## 插件 C ABI

首版 SDK 面向 Windows x64，头文件可由 C11 和 C++20 消费，仅包含固定宽度整数、UTF-8 字节视图、不透明句柄和函数表。查询入口固定为 `dfy_plugin_query`，接口表明确大小、ABI 版本和架构。错误文本写入调用方持有的固定缓冲区，避免跨模块分配和释放。调用线程、借用期、资源归属、失败恢复及订阅规则见 [完整契约](docs/plugin-abi.md)。

`dfy_query_example` 只提供查询验证，创建实例会明确返回 `DFY_UNSUPPORTED`；它没有可安装运行的 observer 包，也不实现插件生命周期。普通 Debug / Release 构建会生成该 DLL，本体不链接或加载它。

`sdk.abi` 使用 C++ 测试宿主按导出名加载纯 C DLL，检查版本拒绝、完整函数表、重复查询和错误缓冲区寿命，并释放本次模块引用。C/C++ 消费者分别验证布局及头文件打包设置恢复，均只依赖 SDK，不继承本体字符集选项。另已使用 Debug 测试宿主查询 Release DLL，验证该组合下的 ABI 与错误输出。

ABI 阅读顺序：[契约](docs/plugin-abi.md) → [公开头文件](sdk/include/dafeiyu/plugin_abi.h) → [查询示例](plugins/examples/query/query.c) → [查询测试](tests/sdk_abi.cpp)。

## 受控 DLL 加载

`platform::DynamicLibrary` 独立于 HTTP、SDK 和插件业务逻辑，按单线程使用，只拥有一次 Windows 模块引用。构造要求绝对 `.dll` 路径且不能含内嵌 NUL；加载前规范化路径并确认是普通 `.dll` 文件。上层仍负责清单、包边界及可信来源检查。

| 操作 | 当前行为 |
|---|---|
| 构造 | 以规范化完整路径加载，文件系统依赖搜索配置为 DLL 所在目录与 System32 |
| `symbol(name)` | 按非空、无内嵌 NUL 的名称取得导出地址，调用方必须按真实函数签名转换；本步不校验 ABI |
| `close()` | 释放对象持有的这一份引用，成功后不再允许查找；重复关闭无操作，失败保留句柄并抛错 |
| 析构 | 正常或异常离开作用域时尽力释放本次引用，不抛出异常；需要释放诊断时先显式关闭 |

加载期间临时设置当前线程的错误模式，避免系统加载错误对话框阻塞流程，正常或异常退出均恢复原模式。加载、导出查找和释放错误保留 Win32 错误号，常见缺失依赖、错误架构或无效映像、缺失导出有明确类别。搜索配置与线程错误模式分别依据 [LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw) 和 [SetThreadErrorMode](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setthreaderrormode)。

私有依赖应与主 DLL 放在同一目录并使用唯一文件名；不启用当前目录或 PATH 的额外文件搜索，仍遵循 Windows 的已加载模块等规则，不承诺同进程内任意同名库隔离。关闭只释放本对象取得的引用，不能据此断言其他引用已释放或模块已完全卸载，参见 [FreeLibrary](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary)。

导出查找失败时，引用仍由对象管理；异常退出其作用域会触发释放。借用地址、活动调用和未来插件实例都必须在关闭前结束。平台封装不处理插件停止或忙碌保护，ABI 校验由下述 `PluginModule` 完成；两者都不替代后续实例安全卸载流程。

`platform.library` 使用真实 DLL 验证中文路径、多个持有者、重复关闭、异常退出释放、缺失文件与导出、真实 x86 架构不匹配和无效映像；同时验证只有工作目录中的依赖不会被拾取，同目录依赖可正常加载，并检查最后一份测试引用释放后模块不再存在。

平台加载阅读顺序：[接口](platform/include/dafeiyu/dynamic_library.hpp) → [实现](platform/library.cpp) → [加载测试](tests/dynamic_library.cpp) → [夹具构建](tests/fixtures/library/CMakeLists.txt)。

## ABI 查询与模块持有

`plugins::PluginModule(manifest)` 接收清单或注册表中的包描述。在执行 DLL 代码前拒绝宿主不支持的声明 ABI，重新读取清单并要求描述保持一致；清单或入口定位变化时应先刷新目录。通过平台加载器取得模块后，调用固定导出 `dfy_plugin_query`，请求与清单一致的 ABI v1。

查询校验依次检查错误长度、返回状态、成功结果、表大小、ABI 版本、架构、保留字段及全部必需函数。查询返回非零状态时不解引用其表指针，也不回显模块的错误正文；成功必须返回非空表和零错误长度。先确认表大小，再访问其余字段；只有全部通过后才复制函数表到宿主内存。

构造成功后，`api()` 返回只读接口副本，函数地址仍依赖对象持有的 DLL 引用。`close()` 成功后使接口失效，重复关闭无操作；释放失败保留接口及引用供重试。构造失败或正常析构时，由平台成员回收本次引用，不影响其他持有者。调用方须在关闭前结束所有借用及调用，本步不调用 `create`、`start`、`stop` 或 `destroy`，也不替代实例停止和忙碌保护。

`plugins.module` 使用真实 DLL 覆盖 16 种查询结果，包括成功、状态失败、空表、大小过小或过大、版本及架构不匹配、非零保留字段、四个必需函数分别缺失、错误长度越界及成功时附带错误。还验证了失败状态附带无效表指针时不读取该指针、未知状态码、过期清单、声明 ABI 不支持、缺失导出、关闭与重新加载。测试分别持有控制端和被测引用，确认失败后无引用残留且未调用实例生命周期函数。

上述校验面向遵守 C ABI 内存与异常约定的可信 DLL；同进程原生代码的非法指针、越界写入或跨 ABI 异常仍不具备故障隔离。

模块阅读顺序：[接口](plugins/runtime/include/dafeiyu/plugin_module.hpp) → [查询实现](plugins/runtime/module.cpp) → [查询测试](tests/plugin_module.cpp) → [故障夹具](tests/fixtures/query/query.c)。第 07 步整体验收保留；R01a／R01b／R01c-1 已增加 v2 查询、句柄借用及操作状态契约，独立 v2 加载和实例控制已在 R06／R07b 实现，当前下一审查项为 R08 独立观察插件与完整链路验收。

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
- [初始基线参考的 pi 版本](https://github.com/earendil-works/pi/tree/b2aabe02966a6b4528c7498a3bb406c14396932d)

## 后续审查顺序

剩余基础开发的模块边界、建议契约、分步审查点及验收条件，见 [剩余基础开发指导](docs/foundation-development-guide.md)。其中明确区分已确认约束、当前代码能力和待实现建议。

归档 → 新构建骨架与 VS 分组 → 模型配置 → 单次真实调用 → 最小 Agent 终端流程 → 事件机制 → 插件发现 → SDK 与加载 → 安全卸载 → 本地包管理 → 联通验收。

这些是逐次审查点，复杂步骤继续拆分。每次只提交一个可审查的功能点，完成后停止，等待用户审查再继续。开发及注释要求见 [AGENTS.md](AGENTS.md)。
