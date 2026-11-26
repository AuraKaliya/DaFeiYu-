# Agent开发计划

实现审阅基线为 2026 年 11 月 26 日主线 `8fce20e`。共用循环 0～4 和上下文装配 1～6 已完成各自承诺的范围。工具发现、领域状态与唤醒的跨领域验证目前以一个 MC 玩家绑定为试点。本轮先讨论[记忆与本地知识库](参考/记忆与知识库调研.md)，随后讨论主 Agent 与子 Agent 的分工。

**进度入口**　[待办](#待办) · [已完成](#已完成) · [待决策](#待决策)

**立项与验收**　[立项理念与原始需求](#立项理念与原始需求) · [目标与范围](#目标与范围) · [参考与取舍](#参考与取舍) · [实施顺序与整体验收](#实施顺序与整体验收)

## 待办

当前主线已有编号的两组计划均已完成。下一批扩展保留在「待决策」，待确定具体对象、工具授权和可验收的实效后排入开发。同一宿主能力在书房和 MC 的接入工作由各领域计划持有。[书房待办](内置书房开发计划.md#待办) · [MC 待办](MC伙伴开发计划.md#待办)

## 已完成

### 共用循环与事实投影

| 编号 | 已验收的行为 | 支持边界 | 交付提交与验证证据 |
| --- | --- | --- | --- |
| Agent-循环-0 | <a id="agent-循环-0"></a>建立普通决策、规划、修订和 MC 的固定输入基线 | 固定回放 7 份实际请求、1 次库存工具分派，分别定位授权排列、旧请求缺失库存回执和回复 Hook 后文本。隔离游戏另核对真实入包及玩家实收。 | `88e8945`，[请求与调用对照](../../modules/agent/tests/loop_projection.cpp) · [游戏端读回](../../tests/mc-companion/playtest-check.cjs) |
| Agent-循环-1 | <a id="agent-循环-1"></a>同一冻结输入旁路比较有序工具、消息和首个差异 | 主会话与同一时刻一个 MC 绑定；比较不产生额外执行或玩家发言。 | `5015d70`，[工具排序](../../modules/agent/src/tool_catalog.cpp) · [固定投影](../../modules/agent/tests/loop_projection.cpp) |
| Agent-循环-2 | <a id="agent-循环-2"></a>正式普通请求、计划和修订带入原库存、目标、步骤证据及实际送达文本 | 提议步骤零个可执行工具。Hook 改写后，以 SQLite 实际记录的可见答复供下轮引用。 | `fe6af78`、`669c478`，[规划与修订](../../modules/agent/tests/loop_projection.cpp) · [真实 DLL 与投递](../../tests/plugins/plugin_mailbox_dll.cpp) |
| Agent-循环-3 | <a id="agent-循环-3"></a>单绑定事实基线、增量及原工作回执按号接纳和查回 | 通过可选 ABI 选择一个配对玩家。连续插话可保留同一在途工作，重传、丢序、新基线失败及旧代次迟到结算有界。 | `dfe0dce`、`0baea64`，[绑定事实](../../apps/host/src/binding_facts.cpp) · [插件固定回放](../../tests/mc-companion/mc_plugin_partner.cpp) |
| Agent-循环-4 | <a id="agent-循环-4"></a>预留输出和整批工具回执空间，按安全边界归档或切换／回退 | 足额的 16 个工具调用可执行，批次或小模型窗口超限均在首次分派前受阻。非选定玩家继续使用原入口。 | `e1e8f4a`、`22c5eaa`、`6e2705c`，[批次与预算](../../modules/agent/tests/loop_projection.cpp) · [两种入口切换](../../tests/plugins/plugin_mailbox_dll.cpp) |

### 上下文装配

| 编号 | 已验收的行为 | 支持边界 | 交付提交与验证证据 |
| --- | --- | --- | --- |
| Agent-上下文-1 | <a id="agent-上下文-1"></a>工具裁定给出可操作的原因，区分无进展、待审批和实际效果 | 参数路径与枚举、范围外、原审批号、过期观察、目标变更、迟到实效及动作后取消。重复尝试号不会重置无进展上限。 | `1e71ae2`、`0cd58cc`、`ec23dd9`，[执行前复核](../../modules/agent/tests/session_action_guard.cpp) · [MC 原例](../../tests/mc-companion/mc_plugin_partner.cpp) |
| Agent-上下文-2 | <a id="agent-上下文-2"></a>按真实来源和原时间编码消息，稳定渲染五段规则 | 主聊天、子会话、规划、两种供应商传输。真人写入同名状态标签仍为外部原话，只换人格措辞时授权与投递独立。 | `79af3af`、`66e9b51`、`b333fe6`、`ceea5e2`，[规则](../../modules/agent/src/prompt_rules.cpp) · [重启与原时间](../../apps/host/tests/message_recovery.cpp) |
| Agent-上下文-3 | <a id="agent-上下文-3"></a>在原位置记录工具契约，依据该步范围装配真实可调用定义 | 宿主和 MC 试点。分类展开在原回执增加 5 项契约，同回合重复展开零新增，后续授权回合才有相应直调定义。 | `6f329d6`、`31717e1`，[跨轮契约追踪](../../apps/host/tests/tool_contract_trace.cpp) · [原轨迹回放](../../tests/mc-companion/mc_plugin_partner.cpp) |
| Agent-上下文-4 | <a id="agent-上下文-4"></a>注册状态 schema，逐决策发布冻结尾注，获准后按版本读回 | 主聊天、工具续行、规划／修订和选定 MC 绑定。隐式项、同值重采、到期、超窗、撤权、换绑、压缩均有独立判定。 | `dcac5ffe`，[7 次决策固定回放](../../modules/agent/tests/loop_projection.cpp) · [主聊天发言前后两版](../../apps/host/tests/persistent_dialogue.cpp) |
| Agent-上下文-5 | <a id="agent-上下文-5"></a>按任务持续关注或按事件展示次数，合并重复回调 | 第一项领域唤醒覆盖配对玩家审批未执行的终态。实际展示并收到有效模型响应才扣次，原事件与快照仍可查。 | `726c468`、`1c0e320`、`9163655`、`8e62507`，[关注计次](../../modules/agent/tests/decision_status.cpp) · [信箱与模型请求](../../tests/mc-companion/native-world.cjs) |
| Agent-上下文-6 | <a id="agent-上下文-6"></a>在同一快照读取待办、开发或游戏环境、工具计数和最近核实实效 | 测得 3 个候选、2 个接纳、1 个派发。游戏余量须匹配原工作与原实体；原堆与旁堆有独立玩家实物及三版原快照证据。 | `d7ab6f1`、`dea9fbb`、`a8f6d4a`、`0f87047`、`ef2efb5`，[计数](../../modules/agent/tests/tool_counts.cpp) · [隔离游戏实收](../../tests/mc-companion/playtest-check.cjs) |

### 从运行问题补齐的验收

| 编号 | 已验收的行为 | 支持边界 | 交付提交与验证证据 |
| --- | --- | --- | --- |
| Agent-恢复-1 | <a id="agent-恢复-1"></a>写库待定按原投递补写，新增原话阻止尚未分派的变更 | 已有独立只读操作可继续。规划和修订保留原观察与其时效。 | `9658c29`、`01f263b`，[SQLite 写锁](../../apps/host/tests/message_recovery.cpp) · [插话](../../modules/agent/tests/turn_input.cpp) |
| Agent-恢复-2 | <a id="agent-恢复-2"></a>普通对话和工具思考会话可归档已经完成的原轮次 | 两类会话各完成 515 轮，近期原话、工具调用结果配对及原件索引保留。计费按真正编码的字段。 | `38a2c48`、`5f009ff`，[容量回放](../../modules/agent/tests/loop_projection.cpp) |
| Agent-恢复-3 | <a id="agent-恢复-3"></a>满额工作索引与换绑后的迟到实效沿原号结算 | 回归做到第 45 次修订、第 390 个工作；全部 128 槽未决时拒绝新动作，旧工作仍能收尾。游戏断线重连后原玩家实收只记一次。 | `4bc1ccf`、`c9f7f94`、`462d59e`，[原索引](../../apps/host/tests/binding_works.cpp) · [原绑定回执](../../tests/mc-companion/mc_plugin_partner.cpp) |
| Agent-重放-1 | <a id="agent-重放-1"></a>重放恢复录制时实际工具目录顺序 | Debug、Release 各核对三份原游戏回合，记录中的模型输入、工具和退出可对照。 | `8fce20e`，[离线重放](../../apps/host/src/trace_replay.cpp) · [入口回归](../../tests/application/application_lifecycle.cpp) |

## 待决策

| 编号 | 要决定的问题 | 选项、影响或所缺依据 |
| --- | --- | --- |
| Agent-决策-1 | <a id="agent-决策-1"></a>摘要缺了细节时，哪些会话可以让大肥鱼自己查回当时的原话或工作回执？ | 例如主人问“刚才从左箱拿了几块”，摘要只写了“已取到材料”。宿主能按原会话和消息位置取原文，MC 试点能查绑定内的原工作。待定的是哪些入口给模型哪些原件、换绑后怎样隔离。可先讨论本会话原话和当前玩家的原工作。[会话原件](../../modules/agent/src/session.cpp#L372) · [绑定内的工作](../设计/Agent循环与上下文设计.md#插件事实与工作) |
| Agent-决策-2 | <a id="agent-决策-2"></a>一件事还在进行时，大肥鱼能否要求以后每次作决定都看到某项状态？ | 例如过河时关注自己的空气，后续决策随附最新读数，上岸即解除。现有任务可持有关注，唤醒另由事件规则决定。待定的是模型能否主动申请、占用多少展示空间，以及谁能设置跨任务常开项。[现有关注与释放规则](../设计/Agent循环与上下文设计.md#关注与唤醒) |
| Agent-决策-3 | <a id="agent-决策-3"></a>经历怎样沉淀为人格，已检验的本地知识如何收录、检索和修订？ | 已定记忆分层、不设自动遗忘，知识与记忆共用检索。待定具体分层、人格生效条件、原件的可见范围、知识发布与冲突修订的权责。建议从经历如何形成长期倾向谈起。[已采纳的职责](../设计/项目架构设计.md#记忆与知识库的职责) · [方案调研与实例](参考/记忆与知识库调研.md) |
| Agent-决策-4 | <a id="agent-决策-4"></a>跨进程恢复哪些任务及在途工作？ | 需逐项确定持久身份、重启后实效查证、安全重试与过期授权。 |
| Agent-决策-5 | <a id="agent-决策-5"></a>记忆系统讨论之后，本体唯一的主会话怎样与插件子会话分工和通信？ | 以现有定向信箱为起点，届时讨论谁见证经历、谁能提交记忆候选、主会话可见哪些内容，以及委托取消、回信入箱、实际收件和业务实效如何核对。主子各自持有当轮上下文。[信箱现状](../设计/项目架构设计.md#事件与信箱) |

## 立项理念与原始需求

同一玩家问进度时，模型既要知道原目标，也要知道当时实际入包多少、还有哪项在等人。原固定回放曾观察到规划读不到已经存在的库存、同组授权顺序改变请求、Hook 后用户实收文本与下轮引用不一致。计划据此拆开事实采集、决定送什么、执行和实际投递。让主会话和领域子会话共享可验收的执行语义，领域事实保留原所有者。

## 目标与范围

冻结每次逻辑决策的作用域和采样截止，提供可复算的消息与有序工具。明确模型所知、程序准入、实际效果、渠道实收和实际退出各自的证据。首批领域只切换一个 MC 绑定，普通聊天、规划、旧插件和非选定玩家仍走各自授权的正式入口。[Agent循环与上下文设计](../设计/Agent循环与上下文设计.md)

## 参考与取舍

以下是立项时比对的版本或资料，采用的是各行列出的机制。当前大肥鱼的契约由本项目实现和离线验收决定。

| 依据 | 可核对的做法 | 在大肥鱼中的取舍 |
| --- | --- | --- |
| Codex | 工作目录变化时追加 `user` 环境消息，权限变化时追加 `developer` 指令。工具搜索按当前步骤的工具策略筛选候选。 | 环境和权限各自保留来源、角色与版本。稳定前缀有利于复用，工具按当步范围提供。[Agent 循环说明](https://openai.com/index/unrolling-the-codex-agent-loop/) · [工具搜索实现](https://github.com/openai/codex/blob/a1604a8c0554c09c1be45e5a121bb1889b4d9a50/codex-rs/core/src/tools/handlers/tool_search.rs) |
| Claude Code 与 Claude API | Claude Code 的提醒随模型采用带标签的 `user` 消息或独立 `system` 消息。Claude API 的工具搜索在历史中保留 `tool_reference`，服务端按请求里的工具定义展开引用。 | 标签标明来源和用途。发现事件留在原位置，适配器仍按协议传递可调用工具的定义。[Claude Code 提醒](https://code.claude.com/docs/en/agent-sdk/modifying-system-prompts#context-claude-code-adds-outside-the-system-prompt) · [Claude 工具搜索](https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-search-tool) · [前缀缓存](https://code.claude.com/docs/en/prompt-caching) |
| DeepSeek Harness | `ctx.sessionProjections.register()` 登记状态 schema、版本及按事件更新状态的函数，可选 `wire` 供客户端读取。`ctx.systemPrompt.context()` 登记模型可见的动态上下文。时间插件用私有投影保存事实，在符合条件的步骤注入带来源的快照。 | 大肥鱼借鉴事实维护与送模装配的分工。下文的关注模式由本项目定义。可见工具仍按请求组装。压缩后结合原作用域重建基线。[会话投影注册表](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/session/session-projection/README.zh.md) · [上下文与工具装配](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/system-prompt/README.zh.md) · [时间插件](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/context/time-context/README.zh.md) · [压缩策略](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/compaction/compaction-basic/README.zh.md) |
| 《AI Agents in Depth》 | Agent Status Bar 把次数、进度和待确认项等隐式状态显式化，可随工具结果或单独消息交给模型。 | 宿主按已接纳事实计算一份短状态，在决策末尾标明已核实进展、阻塞与可继续事项，支持按号取证。 [作者对 Agent 状态栏的说明](https://bojieli.github.io/ai-agent-book/book/chapter2/#agent-状态栏通过元信息增强-agent-轨迹管理) |

Codex 的资料比对固定在 `a1604a8`，DeepSeek Harness 固定在 `4b399c3`。Claude Code 的预设、可追加规则以及 Claude API 的工具引用分开比较。协议角色、当前工具清单与是否真正派发均由本项目各自核对。[Codex 模型模板](https://github.com/openai/codex/blob/a1604a8c0554c09c1be45e5a121bb1889b4d9a50/codex-rs/models-manager/models.json) · [DeepSeek Harness 指令装配](https://github.com/deepseek-ai/deepseek-harness/blob/4b399c39a6d1479cd84ca591b4d7c88e24d38a96/packages/core/system-prompt/README.zh.md)

## 实施顺序与整体验收

1. 共用循环先录 0 的请求和玩家实收，1 旁路比较同一事实截止号，2 改正式请求与投递，3 只切一个协商成功的绑定，4 验证容量、回退及迟到结算。每次切换检查空插件普通问答、只读观察、执行前撤权。
2. 上下文各步依实际决策顺序验收。用参数错、范围外及原审批区分裁定；用用户输入的同名标签和不同人格核对来源；用 MC 工具发现、冻结状态读回、一次性审批唤醒与原物品堆实收检查 3～6。所有结论须能按原消息、回合、工作及投递身份回查。
3. 实际功能基线 `88d8324` 的 Debug、Release 各通过 321 项离线 CTest。随后 `8fce20e` 定向复验原 Trace 的工具排列。隔离游戏用固定响应和独立协议玩家，从同一玩家端时钟计量原话、首次有依据的答复、首次动作、完工和实际收件。原固定场景以首条有依据的反馈不超过 5 秒为目标，缺一个端点就只报告能够核实的分段。模型自主判断、使用真实供应商的缓存或时延均“仅记录，不执行”。[离线测试入口](../../tests/CMakeLists.txt) · [固定游戏场景](../使用说明/MC本地验收说明.md)
