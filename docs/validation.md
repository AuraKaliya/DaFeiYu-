# v0.1 验证

验证日期：2026-11-09。平台：Windows x64，Visual Studio 2022 的 MSVC 19.44.35228.0，Windows SDK 10.0.26100.0，CMake 4.1.2-msvc8。

| 验证 | 结果 |
|---|---|
| Debug 构建 | 通过 |
| Debug CTest | 22 / 22 通过 |
| Release 构建 | 通过 |
| Release CTest | 22 / 22 通过 |
| SDK 独立安装、外部插件构建与加载 | Debug / Release 均通过，计入上述 CTest |
| 中文及空格状态目录、CLI 两进程 checkpoint 接续 | 通过，Run ID 与记忆快照保持，最终 delivery=sent |

测试用例是项目交付的一部分，可用 CTest 重新执行。未验证 Linux / macOS；未调用真实模型、QQ 或其它外部业务平台。

实际执行入口：

```powershell
cmake --preset windows -DBUILD_TESTING=ON
cmake --build --preset windows-debug --parallel 4
ctest --preset windows-debug --parallel 4
.\scripts\windows.ps1 -Action build -Configuration Release
.\scripts\windows.ps1 -Action test -Configuration Release
```

最初的分发测试暴露 Windows CMake 子进程输出解码问题，已为 JSON 输出指定 UTF-8；最终两种构建的完整测试均包含修正后的分发验证。

覆盖范围：动态插件加载、脚本闭环、checkpoint 接续、请求去重、权限/参数拒绝、未知发送、身份检查、取消、启动回滚、ABI 拒绝、依赖图、记忆唯一所有权、事件不可变、实例互斥、策略变化和轮数预算。

恢复集成测试由独立进程在真实 SQLite 提交后 `_Exit(73)`，第二个进程读取持久状态：写副作用不重放；模拟渠道已提交回执但核心尚未结算时，查询回执完成接续。

分发测试在新的安装目录运行宿主，再使用已安装 SDK 独立配置、编译并加载一个新插件。所有任务数据使用独立临时目录；测试不发送真实消息、不调用真实模型。
