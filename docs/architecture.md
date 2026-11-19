# 当前项目架构

大肥鱼的正式程序把会话、模型调用和插件生命周期放在共享应用服务。终端问答和独立游戏角色都使用配置的模型。管理中心已有原生容器、运行状态只读桥接和房间首屏预览，完整的游戏交互面板仍在设计中。

| 从哪里看 | 当前怎样工作 |
| --- | --- |
| [正式入口](../app/main.cpp) · [共享应用服务](../app/application.cpp) | 终端可独立问答，也可显式加载插件、授权工具、审批、读回和查询运行。专用游戏入口创建使用所选配置的 Application。 |
| [Agent 工具循环](../agent/run_loop.cpp) · [模型调用](../model/client.cpp) | Session 持有会话，上层按本轮范围提供工具和角色引导。模型给出文本或工具调用，宿主执行后把结果交回模型。 |
| [原生插件管理](../plugins/runtime/manager.cpp) · [SDK](../sdk/README.md) | 本机可信 DLL 按包发现、安装、加载和卸载；正式实例使用 v2 ABI，可选能力由插件注册，仍有活动工作时须等其退出。 |
| [游戏会话](../app/companion_partner.cpp) · [MC 技能](../plugins/mc/bridge/partner.cjs) | MC 插件通过本机管道连接独立 Mineflayer 角色；当前注册观察、移动、采集、回复。通用技能回执与固定圆石场景的严格旁证各有适用范围。 |
| [桌面状态桥接](../app/gui/run_status_host.cpp) · [UI 验证窗口](ui-probe-run-guide.md) | Win32/WebView2 容器用受限来源交互；实际状态查询和空闲/忙碌首屏预览可以分别验证。 |

如需核对一次游戏行为，从同一局的玩家原话、技能回执与游戏聊天入手。[MC 插件运行说明](../plugins/mc/README.md)列出当前入口；[通用交互设计](mc-companion-development-plan.md)列出已知差距及拟议流程。

基础阶段的架构 PNG、SVG、可编辑结构图和当时的验收记录见[架构基线](archive/foundation/architecture-baseline.md)及[历史资料](archive/README.md)。
