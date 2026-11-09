# v0.4 架构收敛验证

验证日期：2026 年 11 月 9 日。Windows x64，Visual Studio 2022 MSVC 19.44.35228.0，Windows SDK 10.0.26100.0，CMake 4.1.2-msvc8。

| 验证 | 结果 |
|---|---|
| Debug 构建与完整 CTest | 69 / 69 通过 |
| Release 构建与完整 CTest | 69 / 69 通过 |
| 仅核心与 SDK 构建及安装 | 通过，安装包没有示例模块或 profile |
| 仅最小示例的 Release 构建 安装与运行 | 通过，五个模块、一个只读动作、零活动；结果 13 且交付确认 |
| 测试开启且示例不安装 | 构建通过；最小运行、公开边界及独立 SDK 消费三项定向检查通过 |
| SDK 独立构建与真实调用 | Debug / Release 均通过，计入完整 CTest |
| 正常分发的夹具排除 | 通过，带示例安装恰好五份清单 |
| 旧核心数据版本拒绝 | v1、v2 均拒绝打开，原版本字段保持不变 |

## 本轮新增验证

保留上一批的 49 项验证，新增 20 项：

- 7 项路由：两个渠道同名会话、入队后默认渠道改变、账号改变、授权撤销再授予、实例改名再恢复、冒用其它服务入站，以及跨渠道补充意图拒绝。
- 11 项调用树：只读中转写拒绝、资源收窄、跨域复合操作、根动作撤销、资源撤销、额外读委派不能扩张写范围、绕过交付协调器拒绝、共享调用额度、父期限继承、父正常结束与迟到写回执。
- 1 项硬重启额度：在额度持久预留后终止宿主，重启同一 Run 不获得新额度。
- 1 项旧核心存储 v2 拒绝检查。

测试都使用最小角色或显式装配的假提供方。没有接入真实模型、QQ 或业务平台，没有验证 Linux/macOS。资源权限仍以命名域表达，调用额度按次数计量，不代表真实供应商 token 或费用预算。

## 恢复与生命周期证据

渠道在输入接纳时固定账号、会话和交付绑定，队列恢复不改投新的默认渠道。撤权、移除或改名保留授权代次；恢复旧配置也不能恢复旧交付资格。

父调用结束或超时后，普通子调用失去业务资格。异步调用可通过 terminal_only 错误收束；已经发生的写效果仍记录迟到回执。根操作不会把已确认子写入误记为没有副作用，未知效果继续等待核对。

交付、未知写和 Job 的既有硬终止恢复用例继续通过。C ABI 仍为 2；profile 和核心存储升级为 3，策略/渠道契约为 3，RunView 为 2，插件私有数据库仍为 2。没有执行旧数据迁移或清理用户实例目录。

## 可复现入口

```powershell
.\scripts\windows.ps1 -Action build -Mode Test
.\scripts\windows.ps1 -Action test -Mode Test
.\scripts\windows.ps1 -Action build -Mode Test -Configuration Release
.\scripts\windows.ps1 -Action test -Mode Test -Configuration Release
.\scripts\windows.ps1 -Action build -Mode Core -Configuration Release
.\scripts\windows.ps1 -Action install -Mode Core -Configuration Release
.\scripts\windows.ps1 -Action build -Mode Minimal -Configuration Release
.\scripts\windows.ps1 -Action demo -Mode Minimal -Configuration Release
```

测试开启、示例不安装的组合使用 windows-tests 预设，定向检查 runtime.minimal、contracts.public_boundary、distribution.install_and_sdk。构建复用本机已有的固定版本依赖源码缓存，没有改变依赖版本。示例状态目录为 .runtime/minimal-v4；测试状态均位于 build 下。

## 仍未完成的范围

实例/会话级 Scope 调用和注册释放、范围化订阅、动作执行封装、策略失败路由、活动反馈视图、完整内部状态所有权及调度索引仍有评审待办。默认示例与安装包没有增加业务插件，当前测试通过不表示整份收敛设计已经实现。
