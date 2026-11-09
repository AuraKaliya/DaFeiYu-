# v0.2 验证记录

验证日期：2026-11-09。平台：Windows x64，Visual Studio 2022 MSVC 19.44.35229.0，Windows SDK 10.0.26100.0，CMake 3.31.6-msvc6。

| 验证 | 结果 |
|---|---|
| Debug 构建 | 通过，无编译警告 |
| Release 构建 | 通过，无编译警告 |
| Debug CTest | 41 / 41 通过 |
| Release CTest | 41 / 41 通过 |
| 已安装 SDK 的独立插件构建与实际执行 | 两个配置均通过，包含在 CTest 内 |
| 常驻宿主、中文及空格状态路径、在线命令、重启恢复 | 两个配置均通过，包含在 CTest 内 |
| 硬终止后的交付、写动作、后台 Job 恢复 | 两个配置均通过，包含在 CTest 内 |
| 源文件编码、JSON 清单、Markdown 本地链接及差异格式 | 检查通过 |

## 验证范围

36 项运行时用例覆盖连续任务、输入去重、持久队列、行为期限、策略复核、服务替换、记忆子操作、未知效果与资源限制、授权撤销、取消、意图修订、模型阻塞期间查询、截止时间、预算、后台执行、活动暂停、重复终态、嵌套写拒绝、伪造作用域、启动回滚、降级状态、schema、契约冲突、ABI 拒绝、依赖环、实例锁、存储版本拒绝和命令回执。

补充用例验证插件通过受控命令调整行为、旧交付不能结束新意图、对已结束任务的补充被拒绝但实例继续运行、渠道启动期输入持久接纳、未更新 revision 的配置变化仍被指纹识别，以及无关提供方变化不阻断旧 Run。

4 项宿主集成用例启动真实宿主进程，经本地管理传输交互。常驻用例验证休息模式下输入跨重启保留；三个恢复用例在提供方已保存记录后硬终止进程，再启动同一实例，分别验证原交付回执查询、未知写效果不重放、同一 Job 的查询与接管。

分发用例创建新的安装目录，使用已安装 SDK 独立配置和构建外部插件，再由已安装宿主通过模型、外部动作和渠道走完闭环，检查实际结果为 16。

## 可复现入口

以下命令在最终代码上执行；本地使用串行测试以减少独立 SQLite FULL 同步数据库的 I/O 争用。

```powershell
cmake --preset windows -DBUILD_TESTING=ON
cmake --build --preset windows-debug --parallel 4
cmake --build --preset windows-release --parallel 4
ctest --preset windows-debug --parallel 1 --output-on-failure
ctest --preset windows-release --parallel 1 --output-on-failure
```

CMake 未加入 PATH 时，可使用 scripts/windows.ps1 的 build/test 入口。此次最终测试总耗时约为 Debug 212 秒、Release 191 秒；这些是本机测试记录，不是运行时性能承诺。

测试状态都位于 build 下的独立目录，不使用用户实例数据。验证期间曾遇到 Windows 应用程序控制阻止新编译程序；用户处理系统拦截后重新执行，以上结果来自能够实际启动程序的完整测试。

## 未验证与后续范围

没有运行 Linux/macOS 构建，没有真实网络模型、消息平台或外部业务副作用。模拟搜索、渠道和后台提供方验证的是框架协议与恢复行为，不代表任何真实平台具备相同幂等或查询保证。

当前不包含子 Agent、热替换、MCP/Skills 桥接、进程沙箱、完整日历/事实触发调度、供应商 token 成本预算或旧数据迁移。实现边界见 architecture.md；resident-runtime-design.md 中的后续验收条件不计入本轮已通过项目。
