# 文档导航

当前设计从[设计文档](../文档/设计文档.md)进入。迁移期间，下表保留原章节的直接入口。已结束的阶段记录、被替代的方案和旧图稿通过 Git 历史查阅。[文档规范化计划](../文档/开发计划/项目文档规范化计划.md)

| 想做什么 | 阅读入口 |
| --- | --- |
| 定位 CMake 目标、模块测试与集成场景 | [工程构建树](architecture.md#cmake-构建树) · [一键构建与个人预设](../README.md#运行与构建) |
| 核对主线验收版本、各模块已交付的范围与下一项 | [当前开发进度](architecture.md#当前开发进度) · [架构图与模块职责](architecture.md#架构图) · [Agent 会话与工具流程](architecture.md#agent-会话与工具流程) |
| 核对主 Agent、插件子 Agent、信箱与 Hook 的运行边界 | [当前边界](architecture.md#当前边界) |
| 核对 Agent 回合中的规则顺序、消息来源与状态声明 | [Agent 循环的提示词设计与大肥鱼的应用](agent-loop-prompt-design.md) · [规则段顺序与来源](agent-loop-prompt-design.md#规则段顺序与来源) · [状态栏注册契约](agent-loop-core-design.md#agent-status-bar-的字段与更新时机) |
| 核对逐决策快照、审批唤醒、工具计数、待办和领域事实 | [六项开发顺序](agent-loop-core-design.md#上下文装配的实施顺序与验收) · [第 4 项快照](agent-loop-core-design.md#第-4-项逐决策状态与按版本查询) · [第 5 项审批回放](agent-loop-core-design.md#第-5-项审批唤醒与一次性展示) · [第 6 项读回证据](agent-loop-core-design.md#第-6-项待办环境工具计数与实效) |
| 查阅基础 Agent 循环设计、已修复的运行问题及后续分工 | [基础 Agent 循环核心设计方案](agent-loop-core-design.md) · [主线实施记录](agent-loop-core-design.md#主线实施记录) · [长会话与回执复查](agent-loop-core-design.md#长会话与回执复查) · [下一轮开发计划](agent-loop-core-design.md#九下一轮开发计划) |
| 运行大肥鱼、选择模型、查询终端命令 | [运行说明](run-guide.md) |
| 运行桌面主面板、只读探针和房间预览 | [桌面运行说明](ui-probe-run-guide.md) |
| 和 MC 伙伴实际进服 | [MC 插件](../plugins/mc/README.md) · [本机或浏览器游玩](../tests/mc-companion/README.md#首次真人体验) |
| 在本机浏览器复盘 MC 伙伴 | [MC Trace 查看器](mc-trace-viewer.md) |
| 评审 MC 陪玩的功能范围、实施顺序和通用交互 | [按方向的功能开发列表](mc-companion-development-plan.md#按方向的功能开发列表) · [实施顺序与跨方向验收](mc-companion-development-plan.md#实施顺序与跨方向验收) · [参考项目复核](mc-partner-capability-review.md) |
| 审核管理中心首版的界面、后台本体、消息、配置与插件扩展 | [完整开发方案](ui-framework-design.md) · [实施顺序与验收](ui-framework-design.md#10-分步实施与验收) · [UI 技术选型](ui-technology-decisions.md) |
| 核对书房感知如何进入主聊天、身体如何移动及抵达 | [房间知情范围](main-agent-room-development-plan.md#4-首间房与感知规则) · [步骤 01～13 和后续计划](main-agent-room-development-plan.md#7-分步开发顺序) |
| 开发插件或核对接口 | [SDK 指南](../sdk/README.md) · [v2 契约](plugin-abi-v2.md) · [冻结的 v1 契约](plugin-abi.md) |

通过 `git log -- docs` 查看变更。清理前的阶段索引可用 `git show 343d5ed:docs/archive/README.md` 读取。
