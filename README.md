# 大肥鱼

大肥鱼默认打开 Windows 桌面管理中心。主面板与普通聊天终端连接同一个后台主 Agent，空插件目录也能问答。桌面可以续读主聊天、查看房间位置和信箱，在设置页加载或卸载已安装的插件。可选终端还支持受控工具调用和结果读回。MC 陪玩插件使用宿主托管的独立子会话，通过桥接操控游戏角色。项目使用 C++20、CMake 和 VS2022 x64。

- **想玩或运行程序**，见[运行说明](docs/run-guide.md)、[MC 插件](plugins/mc/README.md)与[本机试玩入口](tests/mc-companion/README.md#首次真人体验)。
- **想审当前设计**，从[设计文档](文档/设计文档.md)进入项目架构、Agent、管理中心、内置书房和 MC 伙伴。
- **想了解工程进展**，可从各开发线的交付范围和下一步读到对应源码与验收记录。[当前开发进度](docs/architecture.md#当前开发进度) · [架构图](docs/architecture.md#架构图) · [文档导航](docs/README.md)

## 运行与构建

内置 DeepSeek、Qwen 返回一次完整文本，空插件目录也能问答。凭据分别来自 `DEEPSEEK_API_KEY`、`DASHSCOPE_API_KEY`，示例配置只包含环境变量名称。更新环境后请新开终端。

安装 VS2022 的 x64 C++ 工具、CMake 3.28 或更高版本和前端所需的 Node.js 24.13 及以上 24.x 版本（含 npm）。双击 [Debug 构建](build-scirpt/build-debug.cmd) 或 [Release 构建](build-scirpt/build-release.cmd) 即可编译。首次构建会按锁文件安装前端依赖。双击 `build/bin/Debug/dafeiyu.exe` 或 `build/bin/Release/dafeiyu.exe` 打开桌面管理中心。窗口会连接或启动同目录的 `dafeiyu-host.exe`。

```powershell
.\build-scirpt\build-debug.cmd -Target dfy_model -Jobs 8
.\build-scirpt\build-release.cmd
.\build\bin\Release\dafeiyu.exe
```

根目录的 [CMakePresets.json](CMakePresets.json) 随项目提交。项目预设默认并行调度 4 个构建任务，自有 MSVC C++ 目标各用至多 2 个编译进程。单次运行的 `-Jobs` 优先于所选构建预设的 `jobs`。

本机需要单独设置时，将[个人预设模板](build-scirpt/CMakeUserPresets.json)复制到项目根目录作为 `CMakeUserPresets.json`。已有个人预设时可合并模板条目。编辑 `jobs`、`DFY_MSVC_COMPILE_JOBS` 或本机工具链路径后，运行 `.\build-scirpt\build-debug.cmd -Profile local-windows`（Release 同理）。模板使用单独的 `build/local-windows/`，本机文件由 Git 忽略。

脚本用 `-Profile <名称>` 选择配置预设。默认 `windows` 构建桌面与后台。需要终端时运行 `.\build-scirpt\build-debug.cmd -Profile windows-cli`，然后启动 `build/rebuild/windows-cli/bin/Debug/dafeiyu-cli.exe`。测试配置 `windows-tests` 构建 CLI，包含本体和原生托盘测试。`windows-ui` 增加 WebView2 集成测试，其一键入口定向构建桌面。运行全量测试前按下文「验证方式」构建全部目标。[构建预设](CMakePresets.json)

从根目录直接使用 CMake 也能构建相同的程序。

```powershell
cmake --preset windows
cmake --build --preset windows-release
```

桌面默认使用程序目录下的 DeepSeek 配置。可以指定模型，也可以为本次启动选择独立的数据目录。

```powershell
.\build\bin\Release\dafeiyu.exe --main-panel .\modules\model\resources\config\qwen.example.json
.\build\bin\Release\dafeiyu.exe --data "D:\应用数据\用户设置"
```

可选的 `dafeiyu-cli.exe` 提供终端聊天、`--check-config FILE`、`--complete CONFIG PROMPT_FILE`、`--review-diagnostic FILE` 和 `--help`。配置检查及诊断回顾可以离线执行。单次提示词文件使用 UTF-8。默认资源按程序所在位置定位，明确指定的相对路径以工作目录为准。

生成独立运行目录：

```powershell
cmake --install build --config Release --prefix "$PWD/build/package/DaFeiYu"
```

分发包含程序、Release 运行库、无密钥配置、初始空插件目录、独立示例包、SDK 和第三方许可，安装目录可以独立运行。[运行说明](docs/run-guide.md) · [SDK 开发说明](sdk/README.md)

## 会话与本地命令

会话通过后台模型调用与不可变事件队列推进。只有完整回答和新历史都准备成功后才成对提交；失败和取消保留原上下文，观察失败不改写问答结果。历史正文上限 1 MiB，保留回合结果也有容量限制。

| 命令 | 用途 |
|---|---|
| `/status` | 查看当前或最近一轮的状态、调用耗时、退出确认和通知接纳结果 |
| `/clear` | 空闲时清空上下文 |
| `/cancel` | 请求取消本轮，等待实际调用退出 |
| `/exit` | 停止输入并有序结束 |
| `/plugins list` | 同时查看包目录与运行状态 |
| `/plugins refresh` | 显式刷新目录，不自动加载 |
| `/plugins install "目录"` | 暂存校验并发布本地包 |
| `/plugins load <id>` | 明确创建并启动实例 |
| `/plugins unload <id> [--linked]` | 卸载，或明确选择先停止依赖方 |
| `/plugins remove <id>` | 实际卸载后移除，保留用户设置，可重试清理残留 |
| `/run`、`/run-confirmed` | 为本轮明确授予一个已加载工具，变更候选需要确认。开发委托可在目标后追加 `--workspace "目录"`，按发起终端的位置解析路径。本轮决策可见已核实的项目目录、Windows 系统、采样时间和可读时区。 |
| `/task`、`/task-status` | 接纳读、改、读任务，或按进程内请求号查原运行 |
| `/confirmations`、`/approve`、`/deny` | 查看原候选并提交决定 |
| `/verification`、`/diagnostic export` | 查看宿主读回判定，或导出有界无正文诊断 |

安装并加载可选的状态面板，从终端提交一次切换任务。

```text
/plugins install "examples/plugins/example.status"
/plugins load example.status
/task example.status/read example.status/set_focus focus "把状态面板切换到专注模式" --request 1
```

首读完成后，输入 `/confirmations` 查看待确认动作，按显示的请求号和修订批准或拒绝。用 `/verification` 查询读回判定。示例状态随当前插件实例存活。完整操作见 [运行说明](docs/run-guide.md)。

模型等待期间仍可使用本地命令，新的普通问答明确报告忙碌。正常输入 EOF 等待已接纳回答完成；明确退出请求取消当前问答。取消决定与后台实际退出分开，迟到回答不能恢复已取消结果。模型默认不自动重发失败请求。

`dfy.session.event@1` 记录 `kind`、`session`、`turn`、`messages`，不包含凭据或对话正文。种类为 `turn_started`、`turn_completed`、`turn_failed`、`turn_cancelled`、`session_cleared`；成功事件在历史提交后接纳，观察者始终延后运行。

单次问答诊断将会话、回合、模型请求和通知事件身份关联，并记录单调时间及失败阶段。`/status` 仅查询有界内存快照，不调用模型；取消结果与实际退出分别显示。通知未接纳会独立提示一次，不改写问答结果，也不自动补发。诊断不含正文、凭据或异常内容，终端仅保留最近一轮，清空上下文后仍可查看，退出后丢弃。详细字段解释见 [运行说明](docs/run-guide.md)。

## 插件与运行时

插件采用可信同进程原生 DLL 和版本化 C ABI。默认包目录在程序目录的 `plugins`，用户设置在 `%LOCALAPPDATA%/DaFeiYu/plugins/<id>/settings.json`，可用启动参数分别覆盖。描述及默认配置随包发布，用户配置独立保存，运行时关系仅在内存中汇总。

[观察包 0.7.0](plugins/examples/observer/observer.c) 只依赖公开 SDK，支持独立订阅或宿主明确授予的组成员消费。它可从分发 SDK 独立构建。包安装不加载 DLL，不覆盖已有 ID；默认限制 4096 个子项、512 MiB、32 层目录，拒绝链接和重解析点。

卸载区分忙碌、依赖占用、正常停止等待和失败保留。实际退出前保持实例与代码；重新绑定先准备新关系，旧视图继续保持旧提供方。包移除先退出发现目录再清理，失败报告残留并允许按同 ID 重试。

运行时已实现有界类型与引用、完整组合数据、广播和指定投递、消费者组、顺序域与显式等待环、处理结果及实际退出、分层预算、所有者加权与优先级份额、控制预留、结果汇总、观察／控制边界和显式安全重试。默认 Provider 与 Agent 行为不依赖插件。

## 阅读入口

| 职责 | 入口 |
|---|---|
| App：共享服务、终端及原生插件管理 | [Application](apps/host/src/application.hpp)、[Conversation](apps/host/src/conversation.hpp)、[管理命令](apps/host/src/commands/application_commands.cpp)、[插件命令](apps/host/src/commands/plugin_commands.cpp) |
| Agent：会话、运行与历史提交 | [Session](modules/agent/include/dafeiyu/session.hpp)、[实现](modules/agent/src/session.cpp) |
| Model：异步接纳及供应商协议 | [AsyncClient](modules/model/include/dafeiyu/async_model.hpp)、[完整文本调用](modules/model/src/client.cpp) |
| Events：不可变交付与处理 | [广播](modules/events/include/dafeiyu/broadcast_channel.hpp)、[处理](modules/events/include/dafeiyu/processing.hpp)、[控制边界](modules/events/include/dafeiyu/hooks.hpp) |
| Runtime：预算、调度、结果及重试 | [预算](modules/runtime/include/dafeiyu/budget.hpp)、[调度](modules/runtime/include/dafeiyu/owner_scheduler.hpp)、[结果](modules/runtime/include/dafeiyu/result_group.hpp)、[重试](modules/runtime/include/dafeiyu/retry_controller.hpp) |
| Platform：系统输入、HTTPS 与代码寿命 | [输入](modules/platform/include/dafeiyu/line_input.hpp)、[传输](modules/platform/src/winhttp.cpp)、[CodeModule](modules/platform/include/dafeiyu/code_module.hpp) |
| Plugins：目录、实例、包操作与依赖 | [管理器](modules/plugin_host/include/dafeiyu/plugin_manager.hpp)、[实例](modules/plugin_host/include/dafeiyu/plugin_instance.hpp)、[依赖](modules/plugin_host/include/dafeiyu/plugin_dependencies.hpp)、[包操作](modules/plugin_host/include/dafeiyu/plugin_package.hpp) |
| SDK：独立 C 接口 | [v2 契约](docs/plugin-abi-v2.md)、[公开头与示例](sdk/README.md) |
| Tests：离线与真实 DLL 验收 | [构建清单](tests/CMakeLists.txt)、[单进程集成](tests/application/application_lifecycle.cpp) |

源码按模块组织，VS 目标按职责分组。修改事件存储时，可在 Events 模块内找到实现和单模块测试，从根目录按目标构建。涉及宿主加载插件或跨进程投递的场景在顶层测试树中。[CMake 构建树](docs/architecture.md#cmake-构建树) · [Events 测试](modules/events/tests/CMakeLists.txt) · [跨模块测试](tests/CMakeLists.txt)

## 验证方式

开发中按影响范围构建并运行相关测试。例如，使用 `windows-tests` 构建预设可定向构建 `dfy_event_store_tests`，再用 `ctest --preset windows-tests -R '^events\.store$'` 运行。完整离线验收使用包含桌面目标的 `windows-ui` 配置，分别构建两种模式的全部目标。

```powershell
cmake --preset windows-ui
cmake --build build/rebuild/windows-ui --config Debug --parallel 4
ctest --test-dir build/rebuild/windows-ui -C Debug --output-on-failure
cmake --build build/rebuild/windows-ui --config Release --parallel 4
ctest --test-dir build/rebuild/windows-ui -C Release --output-on-failure
```

构建 Release 程序及示例后，可在 PowerShell 7 中运行独立安装目录检查。

```powershell
.\scripts\verify-agent-package.ps1 -Build .\build\rebuild\windows-ui
```

开发、回归和最终验收使用固定模型响应或传输替身，按功能使用真实线程、Windows 管道、符号链接、DLL 和隔离 Paper。真实模型选工具、理解现场等表现按「仅记录，不执行」处理。游戏试玩和固定响应场地分别有独立入口。旧真实服务脚本及结果可从 Git 历史回查。[玩家试玩](tests/mc-companion/README.md#首次真人体验) · [固定响应场地](tests/mc-companion/README.md#固定响应验收场地)

原生观察规则默认只读，宿主可配置明确数据契约和标量条件，在命中后发起一个待审批变更。终端的主动委托与原生观察入口共用会话和工具授权。[Agent 会话与工具流程](docs/architecture.md#agent-会话与工具流程)

第三方版本、归档校验和许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。开发过程和中文注释要求见 [AGENTS.md](AGENTS.md)。基础阶段后的新能力另行讨论与审查。
