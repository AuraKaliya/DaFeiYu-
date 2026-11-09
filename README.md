# 大肥鱼常驻 Harness · 0.2.0

C++20 常驻核心、版本化 C ABI 和独立插件 SDK。核心维护身份、调度、授权、状态与恢复；插件提供决策策略、模型、渠道、记忆、上下文、动作及领域能力。

这一版面向框架验证。模型和渠道使用可重复的模拟实现；搜索有两个可替换的本地提供方。当前协议为 ABI 2、profile 2、存储 2，不保留旧接口适配、旧 CLI 或数据迁移器。

## 构建与测试

需要 CMake 3.28+、C++20 编译器。Windows 使用 Visual Studio 2022 的 C++ 工具链；仓库脚本会查找其 CMake，不修改系统 PATH。

```powershell
.\scripts\windows.ps1 -Action build
.\scripts\windows.ps1 -Action test
.\scripts\windows.ps1 -Action build -Configuration Release
.\scripts\windows.ps1 -Action test -Configuration Release
```

也可以使用 windows-debug、windows-release 和 ninja-debug 预设。依赖固定版本的 nlohmann/json 与 SQLite，下载时校验 SHA-256。实际验证范围见[验证记录](docs/validation.md)。Windows 本地测试默认串行，避免同时创建大量 FULL 同步数据库造成 I/O 争用。

## 常驻运行

在第一个终端启动实例：

```powershell
$app = '.\build\windows\run\Debug\bin\dafeiyu.exe'
& $app serve --state '.runtime\resident-v2'
```

在另一个终端向在线实例提交命令：

```powershell
$app = '.\build\windows\run\Debug\bin\dafeiyu.exe'
& $app request health --state '.runtime\resident-v2'
& $app request input.accept --state '.runtime\resident-v2' --json '{"key":"task-1","text":"search:harness"}'
& $app request run.list --state '.runtime\resident-v2'
& $app request events --state '.runtime\resident-v2'
& $app request host.shutdown --state '.runtime\resident-v2'
```

input.accept 返回持久接纳记录；通过 input.get 的 id 查询关联 Run，再用 run.get 查询任务。相同来源键与正文返回原记录，不会创建第二项任务。任务结束后实例及插件继续运行。

管理入口使用状态目录下的本地文件邮箱，适合当前单机框架；请求和响应使用原子改名发布，实例启动有独立 epoch。客户端不打开核心数据库，不争抢实例锁。它依赖目录的操作系统访问权限，仅面向本机受信用户。

需要重试可能已提交的管理修改时，显式使用同一个 --command-id。核心会返回原命令回执；相同 ID 的不同参数会被拒绝。控制请求超时不表示修改未发生。

## 示例能力

```powershell
& $app demo --state '.runtime\demo-v2'
& $app demo --state '.runtime\demo-v2' --message 'search:插件契约'
& $app demo --state '.runtime\demo-v2' --message 'remember:先记录来源，再核对执行结果'
& $app demo --state '.runtime\demo-v2' --message 'background:计算'
```

demo 是使用相同实例运行时的单次验证入口，完成后关闭实例。普通文本运行加法示例；search:、remember:、background: 是脚本模型的测试语法，不代表自然语言理解。unknown: 用于演示无法确认的写效果，任务会等待核对。

修改 profile 中的 search 绑定即可选择 search.secondary，搜索动作的代码不变。strategy_default 的 verify 配置开启结束前复核，会通过同一模型调用流程再取一次结果。

默认注册“整理经历”活动，每五分钟评估一次，经相同的策略与操作流程提交记忆，不对外发送。此示例提交固定配置的候选，用于验证流程；真实经历理解需要模型插件。activity.set 调整启停策略；behavior.set 支持 available、focused、quiet、resting 和到期时间。两类修改都要求 expected_revision，暂停状态跨重启保留。

## 管理命令

| 命令 | 用途 |
|---|---|
| health、plugins | 实例健康、行为、服务与动作目录 |
| input.accept、input.get | 持久输入及接纳回执 |
| run.list、run.get、run.steer | 任务查询与关联任务的补充输入 |
| run.cancel、run.resume | 取消或显式接续；未结束的原生调用不被强制卸载 |
| behavior.set、activity.list、activity.set | 行为模式和活动策略 |
| policy.set | 修订动作、效果、资源授权；撤销在调用前重新检查 |
| operation.list、operation.query、operation.reconcile | 操作证据、原身份查询和带证据的人工核对 |
| job.list、job.cancel | 后台执行状态和取消请求 |
| events | 按游标、Run、类型分页读取已提交事实 |
| host.shutdown | 停止接纳、收束调用并关闭实例 |

## SDK 与项目审阅

```powershell
.\scripts\windows.ps1 -Action install
```

安装包含宿主、示例插件、清单、profile、头文件和 CMake package。独立插件使用 find_package(dafeiyu 0.2 CONFIG REQUIRED) 与 dafeiyu::plugin_sdk；[独立工程](tests/sdk_consumer/CMakeLists.txt)实际被安装测试构建并执行，不链接核心。

- [实际架构与模块职责](docs/architecture.md)
- [ABI 2 和插件开发协议](docs/plugin-sdk.md)
- [目标设计与阶段边界](docs/resident-runtime-design.md)
- [验证记录](docs/validation.md)
- [第三方依赖](THIRD_PARTY.md)

当前没有真实模型/平台适配、MCP 或 Skills 桥接、子 Agent、动态热替换和进程隔离。原生插件是受信同进程代码；逻辑权限、超时与协作取消不构成操作系统沙箱。后续扩展应从现有契约接入，并补上对应行为验证。
