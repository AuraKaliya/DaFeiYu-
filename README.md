# 大肥鱼架构验证工程 0.4.0

C++20 核心、版本化 C ABI 和独立插件 SDK。当前工作集中在架构边界与最小验证，暂不扩展业务插件。

核心维护身份、调度、授权、状态和恢复；SDK 提供无运行状态的契约与公开视图；五个最小示例角色完成确定性闭环；服务组合和故障场景位于测试夹具中。

## 构建范围

需要 CMake 3.28+ 和 C++20 编译器。Windows 使用 Visual Studio 2022 的 C++ 工具链；脚本会查找本机 CMake。原生依赖仍为固定版本的 nlohmann/json 和 SQLite。

| 配置 | 内容 | 安装结果 |
|---|---|---|
| 默认 CMake 配置 | Host、核心、SDK | 不带示例模块与默认 profile |
| DFY_BUILD_EXAMPLES=ON | 额外构建最小示例 | 五个模块、minimal profile、固定底色 |
| BUILD_TESTING=ON | 额外构建契约测试和隔离夹具 | 夹具不安装 |
| windows 开发预设 | 示例与测试均启用 | 安装仍只包含核心、SDK和最小示例 |

普通构建默认关闭示例和测试。Windows 脚本的默认模式也为 Core：

```powershell
.\scripts\windows.ps1 -Action build -Configuration Release
.\scripts\windows.ps1 -Action install -Configuration Release
```

输出在 build/windows-core 和 out/dafeiyu-core-v4。核心安装没有默认插件配置；启动业务实例需要显式提供自己的 profile。

## 最小示例

```powershell
.\scripts\windows.ps1 -Action build -Mode Minimal
.\scripts\windows.ps1 -Action demo -Mode Minimal
.\scripts\windows.ps1 -Action install -Mode Minimal
```

最小组合位于 examples/minimal，配置位于 profiles/minimal.json，只包含脚本模型、默认策略、模拟渠道、只读记忆快照和加法动作。脚本固定计算 6 + 7，不理解自然语言，不调用真实平台，不默认启动周期活动、后台工作或记忆写入。

常驻示例与单次 demo 使用同一个运行时：

```powershell
$app = '.\build\windows-minimal\run\Debug\bin\dafeiyu.exe'
& $app serve --state '.runtime\minimal-v4'
# 在另一个终端提交或查询：
& $app request input.accept --state '.runtime\minimal-v4' --json '{"key":"task-1","text":"运行最小闭环"}'
& $app request run.list --state '.runtime\minimal-v4'
& $app request host.shutdown --state '.runtime\minimal-v4'
```

管理入口仍使用本地文件邮箱，受状态目录权限保护。已受理命令和输入以持久回执为准；重试管理修改时使用同一个 --command-id。执行、目标评价和交付分别记录。

## 测试与分发

```powershell
.\scripts\windows.ps1 -Action build -Mode Test
.\scripts\windows.ps1 -Action test -Mode Test
```

开发预设 windows 的构建目录为 build/windows。夹具输出在 tests/runtime，最小示例输出在 run，两者分开；每个场景从最小 profile 加入自己需要的提供方，不再加载全部旧示例。分发测试在新安装目录检查模块清单，并独立编译和执行 SDK 消费插件。

另有 windows-core、windows-minimal、windows-tests 预设分别验证“仅核心”“仅示例”“测试开启且示例不安装”。这些目录互相独立。旧构建或安装目录可能仍有历史文件，发布验证应使用新的安装目录。

若首次依赖下载受网络影响，可通过 FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON 和 FETCHCONTENT_SOURCE_DIR_SQLITE_SOURCE 指向对应固定版本的已解包源目录；不会修改依赖版本或校验规则。

## 核心与 SDK

- dafeiyu::contracts 提供数据类型、schema、内建服务定义和 RunView，不包含插件运行对象。
- dafeiyu::plugin_sdk 在契约之上提供 ABI 和插件作者封装。
- 核心公共头文件仅依赖数据契约；SQLite 与插件执行封装是私有依赖。
- 内建服务使用 SDK 的统一定义，错误槽位、必需方法或契约版本在 ready 前拒绝。
- 策略与插件 read_state(run) 获取 RunView，不读取完整的核心持久投影。

独立插件使用 find_package(dafeiyu 0.4 CONFIG REQUIRED) 与 dafeiyu::plugin_sdk。C ABI 仍为版本 2；profile 与核心存储升级为版本 3，渠道与策略契约为版本 3，RunView 为版本 2，其余内建契约及插件私有库仍为版本 2。旧实验接口没有适配层；旧核心数据库拒绝打开并保留原数据，演示使用新的 minimal-v4 目录。

渠道实例和账号在 profile 的插件 channels 字段声明，输入必须标明其来源服务。接纳时保存会话与交付绑定，恢复不改投默认渠道。子调用记录父关系、收窄后的许可和继承期限；limits.nested_calls 是持久的 Run 级调用次数上限，硬重启不清零。取消后保留已派发工作的实际结果，不恢复原业务资格。

## 文档

- [收敛设计与实施状态](docs/architecture-convergence.md)
- [实际架构](docs/architecture.md)
- [SDK 契约与插件开发](docs/plugin-sdk.md)
- [验证记录](docs/validation.md)
- [v0.2 评审依据](docs/architecture-review-v0.2.md)
- [长期常驻设计参考](docs/resident-runtime-design.md)
- [第三方依赖](THIRD_PARTY.md)

本轮已补齐渠道归属与 Run 内调用树的权限、截止时间、取消和共享调用额度。实例/会话级 Scope 注册释放、范围化订阅、动作封装、策略失败路由、内部状态所有权与调度索引仍按评审继续处理。默认示例仍只有五个角色，没有新增业务插件。
