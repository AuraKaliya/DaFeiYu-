# Agent 循环的提示词设计与大肥鱼的应用

`observe` 和 `craft` 是这份设计最初验证能力说明、入口校验和游戏回执的两个样例。MC 已从技能目录提供「就近取材」指引，主线已接入消息来源、当步工具契约和逐决策宿主状态。后续的事件关注按上下文装配第 5 项推进。[MC 技能目录](mc-companion-development-plan.md#插件-skill-的组织方式) · [上下文装配进度](agent-loop-core-design.md#上下文装配的实施顺序与验收)

这里的一个 Agent 回合从接纳用户任务开始，到交付答复、中断或失败结束。每次模型请求及其工具调用、结果回传属于其中一个步骤。在 MC 中，任务和已接纳的游戏工作可以跨多个回合推进。 [当前 Agent 流程](architecture.md#agent-会话与工具流程) · [MC 运行边界](architecture.md#运行边界)

## 三套工具在一次回合中考虑什么

| 维度 | Codex | DeepSeek Harness | Claude Code |
| --- | --- | --- | --- |
| 角色与工作方式 | 模型模板规定编码协作者的行为，包括自主推进、何时澄清、如何验证和汇报。 | Harness、部署角色和工具各自贡献指引。普通编码回合的快照包含角色及具体工具用法。 | `claude_code` 预设包含软件工程行为、工具指导和安全规则。官方循环包括收集上下文、采取行动和验证结果。 |
| 项目规则与环境 | 加载全局和项目 `AGENTS.md`。宿主还提供工作目录、协作模式及权限信息。 | 工作区指引插件可加载 `AGENTS.md`、`CLAUDE.md` 及本地补充文件。访问更深目录后可在下一步补入适用规则，运行状态也可更新。 | `CLAUDE.md`、路径规则、自动记忆和环境信息进入上下文。支持的版本可按配置回退到 `AGENTS.md` 或同时加载。 |
| 工具与结果 | 模板约束搜索、编辑和命令使用，宿主提供当前工具和参数定义。 | 工具提供具体用法和回执约定，例如检查 Bash 退出码、收集后台结果、把网页内容作为外部数据处理。 | 预设提供工具指导。模型根据工具反馈决定下一步，技能可以先提供简短描述，正文按需加载。 |
| 计划、审批与介入 | 协作模式可补充指令。宿主执行权限和沙箱检查，用户可以引导或中断正在执行的回合。 | Plan Mode 提供计划提交与审批规则。宿主管理审批，输入可进入下一步或下一回合。 | Plan Mode 支持先调查后提交方案。宿主通过权限模式、沙箱及 Hooks 检查调用，用户可中断并纠偏。 |
| 验证与交付 | 模板要求验证工作、给出进展，并在最终答复交代结果。宿主继续处理待接纳输入及工具后续。 | 根据退出码、失败原因和工作结果决定是否继续。工具批次、待接纳输入和取消由宿主管理。 | 验证属于循环本身。SDK 分别交付过程消息和最终结果，并支持调用方设置步数与费用上限。 |

### 指令怎样进入模型请求

- **Codex** 按模型提供基础模板。所对照的客户端把基础指令作为 `developer` 消息送入请求。`AGENTS.md` 随项目上下文进入历史，协作模式等通过单独片段注入。 [模型目录](https://github.com/openai/codex/blob/a1604a8c0554c09c1be45e5a121bb1889b4d9a50/codex-rs/models-manager/models.json#L4) · [基础指令角色](https://github.com/openai/codex/blob/a1604a8c0554c09c1be45e5a121bb1889b4d9a50/codex-rs/core/src/context/base_instructions.rs) · [项目规则](https://learn.chatgpt.com/docs/agent-configuration/agents-md) · [回合接口](https://learn.chatgpt.com/docs/app-server#turns)
- **DeepSeek Harness** 按 Agent 作用域和当前工具集合组装段落。初始指令写入 `system` 消息，后续步骤可追加段落变更，压缩后重建当前系统指令。当前工作目录等运行数据可以单独作为有来源的上下文更新。 [组装规则](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/system-prompt/README.zh.md) · [普通回合快照](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/snapshots/sdk/text-turn/system-prompt.expected.md) · [计划审批快照](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/snapshots/sdk/plan-review/system-prompt.expected.md) · [工作区规则](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/context/agent-instructions/README.md)
- **Claude Code** 的公开接口支持追加或替换预设。项目规则、可用技能、Hook 补充内容和文件变更提醒可以作为会话消息进入上下文。进行中的 Plan Mode 切换也可追加指令，复用前面未变的缓存前缀。此处以 Anthropic 对预设与请求结构的公开说明为准。 [预设与会话提醒](https://code.claude.com/docs/en/agent-sdk/modifying-system-prompts#context-claude-code-adds-outside-the-system-prompt) · [上下文与缓存](https://code.claude.com/docs/en/prompt-caching) · [项目规则](https://code.claude.com/docs/en/memory) · [Agent 循环](https://code.claude.com/docs/en/how-claude-code-works#the-agentic-loop)

三者都在模型提出调用后，由宿主检查工具权限、执行并回传结果。模型结束一个回合时，宿主还需要处理待接纳输入、停止原因和结果交付。业务目标是否达成，以可核对的验证结果判断。 [Codex 回合实现](https://github.com/openai/codex/blob/a1604a8c0554c09c1be45e5a121bb1889b4d9a50/codex-rs/core/src/session/turn.rs#L708) · [DeepSeek 回合与步骤](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/agent-loop/README.md#turn-and-step-flow) · [Claude SDK 停止条件](https://code.claude.com/docs/en/agent-sdk/agent-loop#handle-the-result)

## 大肥鱼的提示词与宿主职责

**从 Codex 借鉴指令与当前事实的分开装配。** 人格提供身份和表达风格。MC 根据本轮状态选取观察、追问、等待和发言指引。玩家原话、任务修订、待答事项、采样时间、在途工作和回执随正文提供。现有模型请求只在首条容纳 `system` 消息，可以沿用现有序列化，把来源和用途保留在组装处。 [角色装配](../apps/host/src/conversation.cpp) · [领域指引与正文装配](../plugins/mc/src/partner_loop.cpp) · [模型消息约束](../modules/model/src/messages.cpp)

**从 DeepSeek Harness 借鉴随工具提供操作指引。** 能力目录和送模契约从现有技能定义取值，分别描述技能是否实现、此轮是否可见、提交入口以及尚缺条件。当前任务已知需要的参数直接提供，其他分类按需展开。`queue_actions` 在目录和校验对齐后再缩减静态节点说明，保留当轮必要的入口、参数和依赖写法。候选接纳、授权、保护范围和执行前复核仍由宿主及插件按当前状态裁定。 [技能定义](../plugins/mc/src/partner_skills.hpp) · [计划准入](../plugins/mc/src/partner_plan.cpp) · [目录与渐进式披露](mc-companion-development-plan.md#渐进式披露)

**从 Claude Code 借鉴用结果推进下一步。** 模型决定何时补查、行动、询问或汇报。插件根据当前任务修订和原工作回执推进能确定的后继，在收到新原话或需要模型判断时开启决策。例如，挖掘已接纳、原木实际入包、木板实际新增、玩家实际收到发言，分别由对应证据确认。玩家途中问进度时，角色可依据最近一次读回作答，在途动作继续推进。显式 `reply` 与面向玩家的最终文本沿同一投递路径，使用原消息或回合与投递身份去重。 [验证与回合收尾](mc-companion-development-plan.md#4-回合结束不等于事情做完) · [信箱与唤醒](architecture.md#信箱优先级与唤醒)

**以实际送模请求核对变更。** `promptTemplate` 标签可以在人格叠加前生成，系统段随后还可能经请求 Hook 改写。定位某次误判时，按会话、运行、步骤和尝试编号查看 `model.prepared` 的消息及工具，再对照模型输出、宿主裁定、实际工作与渠道回执。这样可以区分送模内容缺失、参数选择偏差、准入拒绝和执行受阻。 [模板指纹](../apps/host/src/conversation.cpp) · [送模前改写](../modules/agent/src/run_loop.cpp) · [Trace 记录](../apps/host/src/run_log.cpp)

### 规则段顺序与来源

本轮设计让维护者阅读的顺序与程序渲染的顺序一致。每段有名字、顺序和版本。正文依次回答五个问题。

1. **职责与信息来源。** 当前入口承担什么任务，哪些是真人原话、宿主记录、插件观察或外部数据。
2. **授权与行动边界。** 哪些工具可以请求，哪些前置、范围和批准由宿主在执行前复核。
3. **判断与结算。** 依哪些事实继续、补查或等待，用什么回执确认结果与退出。
4. **输出与投递。** 谁会收到文本、工具发言或内部报告，怎样核对实际送达。
5. **人格与风格。** 身份、语气、详略和措辞如何适配受众。换人格时可以单独审查表达变化，真实性、权限与结算仍由对应规则段承载。

稳定规则说明怎样解释各类来源。当前原话、工具发现、采样与状态按其发生或适用的步骤进入轨迹。由宿主标明来源和作用域，适配器使用供应商支持的 Chat Template 角色及原生工具结果格式。状态栏可作为有来源的 `user` 数据消息放在合法尾部，用户原文中的同名标签仍归原发送者。语气对任务完成率的影响只记录假设。离线验收检查实际组装顺序、规则内容和投递行为。[状态栏契约](agent-loop-core-design.md#agent-status-bar-的字段与更新时机) · [开发与验收](agent-loop-core-design.md#上下文装配的实施顺序与验收)

## 首步方案与验收

首步用 `observe` 和 `craft` 验证技能定义、入口校验和游戏执行器的衔接，覆盖默认回合、显式展开和受限结算回合。实际 `model.prepared` 和行为结果为后续工具契约及 Skill 装配提供对照。 [首批样例](mc-companion-development-plan.md#首批样例) · [首步验收](mc-companion-development-plan.md#首步验收与延迟基线)

| 场景 | 核对项 |
| --- | --- |
| 默认回合与分类展开 | `observe` 的直调和计划节点各有明确参数说明。`craft` 显示为计划节点，目录展开后的 `model.prepared` 反映相应的能力范围。 |
| 受限结算回合 | 实际可调用工具符合本轮限定范围。目录或正文中提及的技能仍按宿主当轮权限处理。 |
| 观察参数与读回 | 固定响应验证直调的 `focus=objects`，再验证计划入口拒绝该值。场景观察保留原读数的来源和采样时间，同批重复请求引用原读数。 |
| 合成依赖与读回 | 等挖掘原工作确认拾取后推进合成。执行时按现场配方、库存和工作台条件核对，退出后按原工作号记录实际耗料、产出和未完成量。用条件变化和部分完成场景检查后继。 |

功能验证使用固定模型响应，对照 `mc.protocol`、`mc.partner_observe`、`mc.crafting` 和现有回放。隔离 Paper 核对真实采样、扣料和入包。真实模型的选工具、参数提取及判断表现只记录场景和预期。性能比较沿用首答、首次动作、完工和实际发言四个时点，以同一场景的 Trace 取证。 [测试入口](../tests/CMakeLists.txt) · [指向操作回归](../tests/mc-companion/README.md#指向操作回归) · [分段时序](../tests/mc-companion/README.md#观察与响应的分段时序)
