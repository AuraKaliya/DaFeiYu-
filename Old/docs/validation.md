# v0.5 架构收敛验证

验证日期：2026 年 11 月 10 日。Windows x64，Visual Studio 2022 MSVC 19.44.35228.0，Windows SDK 10.0.26100.0，CMake 4.1.2-msvc8。

本轮以前一轮检查点 `92f2bc7` 为起点，继续完成 Scope、注册释放、范围订阅、动作封装、策略失败路由、活动反馈视图、内部状态所有权与调度索引。未新增业务插件。

| 验证 | 结果 |
|---|---|
| Debug 构建与完整 CTest | 93 / 93 通过 |
| Release 构建与完整 CTest | 93 / 93 通过 |
| 仅核心与 SDK 构建及安装 | 通过，安装包没有示例模块或 profile |
| 仅最小示例的 Release 构建、安装与运行 | 通过，恰好五份清单、零活动；动作结果 13，交付 sent |
| 测试开启且示例不安装 | 构建通过；最小运行、公开边界、会话 Scope 和独立 SDK 消费定向检查通过 |
| SDK 独立构建与真实调用 | Debug / Release 均通过，计入完整 CTest |
| 分发的夹具排除 | 通过，核心安装无插件，示例安装仅五个最小角色 |
| 旧核心数据版本拒绝 | v1、v2、v3 均拒绝打开，原版本字段保持不变 |
| 源码空白检查 | git diff --check 通过 |

## 新增契约与生命周期验证

保留 v0.4 的 69 项检查，新增 24 项：22 项 Scope / 动作 / 策略场景、1 项所有权与索引检查、1 项旧核心存储 v3 拒绝检查。

Scope 测试覆盖实例、会话、Run、Call 生命周期；逐项释放、显式关闭、会话关闭后重新接纳、授权撤销；相同外部会话名位于两个渠道时的隔离；配置变化拒绝恢复、运行期旧句柄拒绝、未确认订阅重投、调用额度与 Run 取消。正常关闭时未完成 Run 保留恢复资格，重启后重新签发 Scope，已预留模型额度继续累计；完成后登记全部释放。

同一假执行器通过启动期发现注册两种动作：整数只读动作和文本本地写动作。测试验证各自 schema、效果声明与共享执行器信封；参数类型错误在执行前拒绝。独立 SDK 工程使用相同信封，在已安装宿主中完成真实动态加载和调用。

模型测试覆盖提供方错误、无效结果与超时，由策略选择冻结集合中的备用绑定；未授权绑定与累计预算耗尽由核心拒绝。默认最小策略仍在模型失败时等待用户，不加入业务模型路由。活动夹具严格校验 RunView v3，检测 bindings、grants、request、limits、strategy_state 等私有字段泄漏，并由测试断言成功反馈。

## 状态与调度证据

StateWriter 检查唯一所有者、具体 system 键范围、事务边界和核心线程。测试验证越权写拒绝、外层回滚和嵌套 SAVEPOINT 回滚；回滚不发布投影、事实或变更通知，外层合法写仍能提交。

索引测试向 Run、输入、操作、Job 四类各填入一万条已结束历史，另留少量待处理记录。调度查询只解码 7 条记录，SQLite VM 总步数低于 2500；测试同时验证遗漏通知后的持久结算可检索、未知效果资源约束、历史分页及禁止全历史 all 查询。测试曾发现状态多值查询和资源关联仍遍历历史索引，已改为明确使用活动状态索引及按未知操作查资源。

事件循环不再每 10 ms 扫描历史，按消息、最近期限、到期点与有界核对期限等待。触发 defer / 失败保存 retry_at；反馈成功或失败均落事实，不立即无限重试。正常任务仍有用户优先与单决策资格；没有为了测试向正式核心加入故障开关。

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

windows 开发预设启用测试和示例；windows-tests 验证测试开启而示例不安装。分发测试在临时新目录安装，独立 SDK 工程只使用安装后的头文件和 CMake 包。默认 CMake 的 BUILD_TESTING / DFY_BUILD_EXAMPLES 仍均为 OFF。

本机日志位于 build/v05-debug.log、build/v05-release.log、build/v05-test-only.log、build/v05-minimal-demo.json；这些构建产物不提交。核心安装为 out/dafeiyu-core-v5，示例安装为 out/dafeiyu-minimal-v5，新示例状态为 .runtime/minimal-v5。没有迁移或清理用户旧实例。

## 验证范围

C ABI 仍为 2；profile / 核心库为 4，策略为 4，渠道 / 活动为 3，RunView 为 3，动作执行器为 1，插件私有数据库仍为 2。管理历史列表改为带 cursor 的分页；run.get 是本地诊断投影，run.view 和插件查询使用公开视图。

全部结果来自最小示例和测试夹具。尚未验证 Linux / macOS，也未接入真实模型或业务平台。资源权限以命名域表达，调用预算以次数计量；实例总费用预算、文件路径级权限、DLL 热替换和不受信原生代码隔离不在本阶段范围。
