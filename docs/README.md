# 文档导航

每个主题在 `docs/` 原位维护最新版本。已结束的阶段记录、被替代的方案和旧图稿通过 Git 历史查阅。阅读当前实现从架构、运行说明和源码开始，待实现的设计由各方案注明。

| 想做什么 | 阅读入口 |
| --- | --- |
| 看当前实现、架构图和流程图 | [项目架构](architecture.md) |
| 评审主 Agent、插件子 Agent、信箱与 Hook 的架构调整 | [架构调整方案](architecture.md#架构调整方案) |
| 运行大肥鱼、选择模型、查询终端命令 | [运行说明](run-guide.md) |
| 运行桌面只读探针和房间预览 | [UI 探针运行说明](ui-probe-run-guide.md) |
| 和 MC 伙伴实际进服 | [MC 插件](../plugins/mc/README.md) · [本机或浏览器游玩](../tests/mc-companion/README.md#首次真人体验) |
| 评审“玩家原话／游戏事件 → 模型决定 → 行动与发言”的设计 | [MC 伙伴通用交互流程](mc-companion-development-plan.md) · [能力清单和参考项目](mc-partner-capability-review.md) |
| 评审桌面主面板与内置房间 | [主面板与内置房间](main-agent-room-development-plan.md) · [UI 框架](ui-framework-design.md) · [技术选型](ui-technology-decisions.md) |
| 开发插件或核对接口 | [SDK 指南](../sdk/README.md) · [v2 契约](plugin-abi-v2.md) · [冻结的 v1 契约](plugin-abi.md) |

通过 `git log -- docs` 查看变更。清理前的阶段索引可用 `git show 343d5ed:docs/archive/README.md` 读取。
