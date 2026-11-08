# 大肥鱼原生 Harness · 0.1.0

C++20 核心、CMake 工程和版本化插件 SDK。参考 DSH 与现有大肥鱼的业务逻辑，不依赖其 TypeScript、Node.js 或 Python 运行时。

本版本是可构建、可重启的第一版闭环：模拟入站 → 人格快照 → 脚本模型 → 示例动作 → 模型结果 → 模拟交付。四个能力都是实际动态加载的 DLL / 共享模块。

## 构建

需要 CMake 3.28+、支持 C++20 的编译器。Windows 使用安装了“使用 C++ 的桌面开发”的 Visual Studio 2022；也提供 Ninja 预设。首次配置下载固定版本的 nlohmann/json 和 SQLite，并校验 SHA-256。SQLite 是原生 C 依赖；宿主、SDK 封装和插件实现均为 C++。

在 Visual Studio 开发者 PowerShell 或已配置 CMake 的终端中：

```powershell
cmake --preset windows
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug
```

若 CMake 尚未加入 PATH，仓库提供一个只在本次进程查找工具的 Windows 入口：

```powershell
.\scripts\windows.ps1 -Action build
.\scripts\windows.ps1 -Action test
.\scripts\windows.ps1 -Action demo
```

使用 `-Configuration Release` 选择发布构建。脚本不会修改系统 PATH 或安装开发工具。

Linux / macOS 的构建入口为 `ninja-debug`，需要 Ninja 与本机编译器；平台实现包含 POSIX 动态库和文件锁支持。实际验证的平台见 `docs/validation.md`，不把提供预设视为跨平台验证通过。

离线构建可用 CMake 的 `FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON`、`FETCHCONTENT_SOURCE_DIR_SQLITE_SOURCE` 指向对应固定版本的已解包源目录。

## 运行闭环

```powershell
$app = '.\build\windows\run\Debug\bin\dafeiyu.exe'
& $app plugins --state '.runtime\demo'
& $app run --state '.runtime\demo' --request-id demo-1 --message '运行示例加法任务'
& $app status --state '.runtime\demo' --request-id demo-1
& $app events --state '.runtime\demo' --request-id demo-1
```

默认 profile 中脚本固定调用 `action.add(a=6,b=7)`，结果为 13。它用于验证 Harness，不理解任意自然语言，也不会访问真实模型或发送真实消息。修改 `profiles/demo.json` 后重新配置 CMake 可更新构建目录内的示例配置。

stdout 为 JSON，进度写入 stderr。`execution`、`goal` 和 `delivery` 分别表示执行状态、脚本模型的目标判断和渠道交付状态；`goal=met` 不代表程序已经验证任意用户目标。

## 检查点与重启

```powershell
& $app run --state '.runtime\checkpoint' --request-id task-1 --pause-after-action
# 第一个进程退出；已结算的动作、人格快照和任务身份已保存。
& $app resume --state '.runtime\checkpoint' --request-id task-1
```

`run` 对相同 request-id 与正文去重，只返回原记录；接续必须显式使用 `resume`。相同 request-id 的不同正文会被拒绝。已经确认的动作与发送不重放；待确认的写操作保持 unknown；发送中断先查询渠道回执，无法确认时等待核对。

模拟渠道的 `delivery_mode` 支持 `sent`、`not-sent`、`unknown`。后两种不会自动重发。本版尚无人工核对/重新授权发送命令。改变运行授权集合、服务版本或固定底色后，旧 Run 的接续会被拒绝；不会静默套用新规则。

每个数据目录只允许一个宿主进程持有锁。`status` / `events` 是离线查询，需要当前运行进程已退出。

## 安装与 SDK

```powershell
cmake --install build/windows --config Debug --prefix out/dafeiyu
& '.\out\dafeiyu\bin\dafeiyu.exe' run --state '.runtime\installed' --request-id install-1
```

安装目录包含宿主、四个插件、清单、profile、固定底色、SDK 头文件和 CMake package。实例数据目录位于安装包外。

独立插件工程通过 `find_package(dafeiyu 0.1 CONFIG REQUIRED)` 和 `dafeiyu::plugin_sdk` 使用 SDK。完整的独立消费示例在 `tests/sdk_consumer/`；分发测试实际安装项目、单独构建此插件，再由已安装的宿主加载。

- [职责和恢复规则](docs/architecture.md)
- [插件 ABI 与方法协议](docs/plugin-sdk.md)
- [验证记录与当前限制](docs/validation.md)
- [第三方依赖](THIRD_PARTY.md)

## 当前范围

实现单宿主、串行 Run、同步且可协作取消的 ABI v1；模型生成允许多条输出事件。模型请求和结果持久记录，但 CLI 当前显示阶段进度和最终 JSON，不提供实时 token 界面。

最小记忆插件只有版本化初始人格与历史快照校验，不实现自主成长。任务的多轮意图修订、后台活动、管理服务、真实模型、真实渠道、通用进程工具、热替换和不受信插件隔离留给后续版本。实例授权目前是本地模拟 owner 与动作白名单，不能用于对外认证。
