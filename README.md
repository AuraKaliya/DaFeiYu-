# 大肥鱼

最小 Agent 基础阶段已完成，采用 C++20、CMake、VS2022 和 Windows x64。开发指导中的 R01～R16 已形成独立提交及验收记录，供统一审查。

随后已完成对这 35 个提交的[专项审查及修复](docs/review-2026-11-14.md)，修复 5 处可复现问题，并再次通过 Debug／Release 各 79／79 全量及 20／20 跨配置检查。[当前架构图](docs/architecture.md)按实际装配绘制，区分正式问答链路与可选基础接口。

最终从全新目录构建 Debug、Release，两种配置的完整 CTest 均为 **79／79**；另通过 **20／20** 项 Debug 宿主调用 Release DLL 的检查。DeepSeek 与 Qwen 均完成真实单次调用，以及空插件、加载观察包、卸载后的多轮上下文验收。详情见 [最终验收记录](docs/final-acceptance-2026-11-14.md) 和 [开发指导](docs/foundation-development-guide.md)。

[大肥鱼管理中心 UI 框架设计方案 V0.2](docs/ui-framework-design.md) 覆盖房间首页、组件库、UI 接口和两级插件界面规范；[技术选型](docs/ui-technology-decisions.md) 采用 Win32／WebView2、Vue 3／TypeScript 与独立来源的受限 iframe。[U01 独立验证窗口](docs/ui-u01-validation.md) 已完成离线、Frame 授权、协议输入／焦点及资源回收的自动验收，Debug／Release 各 86／86。正式问答仍使用终端，U02 业务界面接入尚未开始。

## 运行与构建

内置 DeepSeek、Qwen 返回一次完整文本，空插件目录也能问答。凭据分别来自 `DEEPSEEK_API_KEY`、`DASHSCOPE_API_KEY`，示例配置只包含环境变量名称。更新环境后请新开终端。

```powershell
cmake --preset windows
cmake --build --preset windows-release
.\build\rebuild\windows\bin\Release\dafeiyu.exe
```

不带参数使用程序目录下的 DeepSeek 配置；指定 Qwen 或独立目录：

```powershell
.\build\rebuild\windows\bin\Release\dafeiyu.exe --chat .\config\qwen.example.json
.\build\rebuild\windows\bin\Release\dafeiyu.exe --chat .\config\deepseek.example.json --plugins "D:\应用数据\插件" --data "D:\应用数据\用户设置"
```

支持 `--check-config FILE`、`--complete CONFIG PROMPT_FILE` 和 `--help`。配置检查不联网；单次提示词文件使用 UTF-8。默认资源按程序所在位置定位，明确指定的相对路径以工作目录为准。

生成独立运行目录：

```powershell
cmake --install build/rebuild/windows --config Release --prefix "$PWD/build/package/DaFeiYu"
```

分发包含程序、Release 运行库、无密钥配置、初始空插件目录、独立示例包、SDK 和第三方许可，不依赖源码、`Old` 或测试目录。详见 [运行说明](docs/run-guide.md) 与 [SDK 开发说明](sdk/README.md)。

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
| App：启动、终端及管理命令 | [main.cpp](app/main.cpp)、[chat.cpp](app/chat.cpp)、[plugin_commands.cpp](app/plugin_commands.cpp) |
| Agent：唯一正式会话及历史提交 | [Session](agent/include/dafeiyu/session.hpp)、[实现](agent/session.cpp) |
| Model：异步接纳及供应商协议 | [AsyncClient](model/include/dafeiyu/async_model.hpp)、[完整文本调用](model/client.cpp) |
| Events：不可变交付与处理 | [广播](events/include/dafeiyu/broadcast_channel.hpp)、[处理](events/include/dafeiyu/processing.hpp)、[控制边界](events/include/dafeiyu/hooks.hpp) |
| Runtime：预算、调度、结果及重试 | [预算](runtime/include/dafeiyu/budget.hpp)、[调度](runtime/include/dafeiyu/owner_scheduler.hpp)、[结果](runtime/include/dafeiyu/result_group.hpp)、[重试](runtime/include/dafeiyu/retry_controller.hpp) |
| Platform：系统输入、HTTPS 与代码寿命 | [输入](platform/include/dafeiyu/line_input.hpp)、[传输](platform/winhttp.cpp)、[CodeModule](platform/include/dafeiyu/code_module.hpp) |
| Plugins：目录、实例、包操作与依赖 | [管理器](plugins/runtime/include/dafeiyu/plugin_manager.hpp)、[实例](plugins/runtime/include/dafeiyu/plugin_instance.hpp)、[依赖](plugins/runtime/include/dafeiyu/plugin_dependencies.hpp)、[包操作](plugins/runtime/include/dafeiyu/plugin_package.hpp) |
| SDK：独立 C 接口 | [v2 契约](docs/plugin-abi-v2.md)、[公开头与示例](sdk/README.md) |
| Tests：离线与真实 DLL 验收 | [构建清单](tests/CMakeLists.txt)、[单进程集成](tests/application_lifecycle.cpp) |

VS 按 App、Agent、Model、Platform、SDK、Plugins、可选 Tests 分组，只建立已实现目标。独立同步 `Bus` 保留为早期基础及其测试，正式 Agent 不再依赖它。`Old/` 保留重建前工程，只供参考，不参与新根工程构建。

## 验证方式

开发中按影响范围构建目标、运行相关测试；全量只在最终验收提交时执行。完整验收可复现为：

```powershell
cmake --preset windows-tests
cmake --build build/rebuild/windows-tests --config Debug
ctest --test-dir build/rebuild/windows-tests -C Debug --output-on-failure
cmake --build build/rebuild/windows-tests --config Release
ctest --test-dir build/rebuild/windows-tests -C Release --output-on-failure
```

所有开发、回归和最终验收测试均使用可控模型响应或传输替身，不调用远程或本地真实模型。真实线程、Windows 管道、符号链接和 DLL 按功能需要实际执行；UI 不采用 Computer Use 检查。依赖真实模型表现的事项仅记录、不执行，不计入测试通过率或验收门槛。旧 [verify-live.ps1](scripts/verify-live.ps1) 及已有真实服务记录只保留历史用途，不纳入当前验证流程。

工具循环、受控自动规划与“观察＋行动”建设顺序见 [Agent 基础能力开发计划](docs/agent-capability-development-plan.md)。已完成模型结构化协议、工具目录与执行基础、原生插件工具适配，并开发至 A05a-2 的独立上下文事务；按用户要求暂停。默认 CLI 仍为单次问答，完整工具循环尚未接入。当前完成边界和框架见 [进度与架构快照](docs/agent-progress-2026-11-15.md)。

第三方版本、归档校验和许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。开发过程和中文注释要求见 [AGENTS.md](AGENTS.md)。基础阶段后的新能力另行讨论与审查。
