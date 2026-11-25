# 文档导航

每个主题在 `docs/` 原位维护最新版本。已结束的阶段记录、被替代的方案和旧图稿通过 Git 历史查阅。阅读当前实现从架构、运行说明和源码开始，待实现的设计由各方案注明。

| 想做什么 | 阅读入口 |
| --- | --- |
| 核对各模块做到哪一步，查看架构图和流程图 | [当前开发进度](architecture.md#当前开发进度) · [架构图与模块职责](architecture.md#架构图) |
| 核对主 Agent、插件子 Agent、信箱与 Hook 的运行边界 | [当前边界](architecture.md#当前边界) |
| 比较 Agent 回合提示词与上下文设计，评审大肥鱼的首步应用 | [Agent 循环的提示词设计与大肥鱼的应用](agent-loop-prompt-design.md) |
| 查阅基础 Agent 循环设计、实施记录及下一轮三线分工 | [基础 Agent 循环核心设计方案](agent-loop-core-design.md) · [主线实施记录](agent-loop-core-design.md#主线实施记录) · [下一轮开发计划](agent-loop-core-design.md#九下一轮开发计划) |
| 运行大肥鱼、选择模型、查询终端命令 | [运行说明](run-guide.md) |
| 运行桌面主面板、只读探针和房间预览 | [桌面运行说明](ui-probe-run-guide.md) |
| 和 MC 伙伴实际进服 | [MC 插件](../plugins/mc/README.md) · [本机或浏览器游玩](../tests/mc-companion/README.md#首次真人体验) |
| 在本机浏览器复盘 MC 伙伴 | [MC Trace 查看器](mc-trace-viewer.md) |
| 评审 MC 陪玩的功能范围、实施顺序和通用交互 | [按方向的功能开发列表](mc-companion-development-plan.md#按方向的功能开发列表) · [实施顺序与跨方向验收](mc-companion-development-plan.md#实施顺序与跨方向验收) · [参考项目复核](mc-partner-capability-review.md) |
| 审核管理中心首版的界面、后台本体、消息、配置与插件扩展 | [完整开发方案](ui-framework-design.md) · [实施顺序与验收](ui-framework-design.md#10-分步实施与验收) · [UI 技术选型](ui-technology-decisions.md) |
| 核对原生书房、身体、感知与动作的开发进度 | [主面板与内置房间](main-agent-room-development-plan.md) |
| 开发插件或核对接口 | [SDK 指南](../sdk/README.md) · [v2 契约](plugin-abi-v2.md) · [冻结的 v1 契约](plugin-abi.md) |

通过 `git log -- docs` 查看变更。清理前的阶段索引可用 `git show 343d5ed:docs/archive/README.md` 读取。
