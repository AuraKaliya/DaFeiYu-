# 大肥鱼

最小 Agent 基础阶段已完成，采用 C++20、CMake、VS2022 和 Windows x64。开发指导中的 R01～R16 已形成独立提交及验收记录，供统一审查。

随后已完成对这 35 个提交的[专项审查及修复](docs/review-2026-11-14.md)，修复 5 处可复现问题，并再次通过 Debug／Release 各 79／79 全量及 20／20 跨配置检查。[当前架构图](docs/architecture.md)按实际装配绘制，区分正式问答链路与可选基础接口。

最终从全新目录构建 Debug、Release，两种配置的完整 CTest 均为 **79／79**；另通过 **20／20** 项 Debug 宿主调用 Release DLL 的检查。DeepSeek 与 Qwen 均完成真实单次调用，以及空插件、加载观察包、卸载后的多轮上下文验收。详情见 [最终验收记录](docs/final-acceptance-2026-11-14.md) 和 [开发指导](docs/foundation-development-guide.md)。

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

离线测试使用可控模型或传输替身，真实线程、Windows 管道、符号链接和 DLL 仍实际执行。真实供应商验收通过 [verify-live.ps1](scripts/verify-live.ps1) 明确触发，需要 PowerShell 7，不加入 CTest；测试日志仅保留非敏感提示和普通输出。

第三方版本、归档校验和许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。开发过程和中文注释要求见 [AGENTS.md](AGENTS.md)。基础阶段后的新能力另行讨论与审查。
