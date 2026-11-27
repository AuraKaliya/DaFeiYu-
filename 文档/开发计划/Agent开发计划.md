# Agent开发计划

共用循环 0～4 和上下文装配 1～6 的审阅基线为 2026 年 11 月 26 日主线 `8fce20e`，领域状态与唤醒以一个 MC 玩家绑定为试点。11 月 27 日已确定本体集中检索和三类子 Agent 的身份边界，第一项离线实验见 [Agent-记忆-1](#agent-记忆-1)。[记忆设计](../设计/本体记忆与知识库设计.md) · [外部方案与评测资料](参考/记忆与知识库调研.md)

**进度入口**　[待办](#待办) · [已完成](#已完成) · [待决策](#待决策)

**立项与验收**　[立项理念与原始需求](#立项理念与原始需求) · [目标与范围](#目标与范围) · [参考与取舍](#参考与取舍) · [实施顺序与整体验收](#实施顺序与整体验收)

## 待办

当前记忆阶段 1 至 19 已接入并分别验收。下一轮优先推进跨领域观察接入和未预置问法的检索评测。

| 建议阶段 | 要解决的问题 | 可验收的结果 |
| --- | --- | --- |
| <a id="agent-记忆-20"></a>Agent-记忆-20 | 跨领域新渠道怎样签发可比较的观察？ | 先选两个业务无关的渠道，验证对象、单位、检查点及来源映射，不能确认可比性时保留疑问。 |
| <a id="agent-记忆-21"></a>Agent-记忆-21 | 词面和显式别称仍会漏掉哪些问题？ | 扩充未预置同义表达与大量相近对象的测试，对照召回、误配、延迟和字节，再决定是否引入语义检索或重排。 |

书房和 MC 的领域接入分别由[书房计划](内置书房开发计划.md#待办)和 [MC 计划](MC伙伴开发计划.md#待办)持有。

## 已完成

### 本体记忆与知识库

2026 年 11 月 27 日在本地完成一次全量离线回归。Debug 和 Release 各通过 340 项，其中核心及集成测试 306 项，桌面测试 34 项。下表中的送模数据来自固定模型后端实际收到的请求。真实模型面对新问法时如何选工具、提取参数和归纳，另列场景保留观察。

| 编号 | 已验收的行为 | 支持边界 | 验证入口 |
| --- | --- | --- | --- |
| Agent-记忆-1 | <a id="agent-记忆-1"></a>同题逐批加入过期、转述、越权及独立分歧材料，核对候选、来源、修订与总字节数 | 系统 SQLite 中的合成逻辑库，主对照预算 256 个 UTF-8 字节。前四阶段返回同一条 83 字节的证据，独立分歧加入后两条占 189 字节。另验历史时点、跨委托隔离、受众、撤权、完整往返行程及容量边界。主聊天原件接入见下一项。 | Debug、Release 各连续三次通过 `app.memory_recall_baseline`。构建目标 `dfy_memory_recall_tests`，执行 CTest 加 `-V` 可查看每阶段耗时。[查询](../../apps/host/tests/memory_recall/search.sql) · [固定语料](../../apps/host/tests/memory_recall/corpus.sql) · [验收程序](../../apps/host/tests/memory_recall/experiment.cpp) |
| Agent-记忆-2 | <a id="agent-记忆-2"></a>在主聊天收录短原话，重启后按主题或原会话号与消息号查询，并可撤回读取权 | 仅主人单独对话获得原件。单一数据档案内验证桌面来源、终端补收录、同号重试、房间及合并来信、跨档案编号、在途撤权、旧快照和撤权后再启动。 | Debug、Release 各连续三次通过 `app.main_memory`。每次在三个独立进程中将 15 份 `model.prepared` 与固定后端实际请求逐一核对。样例重启后按原号查回的原件项占 392 字节，整份编码请求占 3072 字节。[宿主入口](../../apps/host/src/main_memory.cpp) · [验收程序](../../apps/host/tests/main_memory.cpp) · [使用方法](../使用说明/构建与运行说明.md#交互命令) |
| Agent-记忆-3 | <a id="agent-记忆-3"></a>普通私聊可提议收录主人原话，依据不足时按单事实或多事实查回 | 仅主人私聊获得记忆工具。每轮检索一次，单事实一条，对照最多三条，证据数组共 2048 字节。核对完整请求中的原话、来源、顺序、缺失与撤权。 | Debug、Release 各连续三次通过 `app.dialogue_memory`。42 个通用场景每次核对 82 份实际后端请求。多事实超额请求送出 3 条、529 字节；累计字节受限时送出 2 条、1411 字节。真实模型在新措辞上的判断质量仅记录，不执行。[设计](../设计/本体记忆与知识库设计.md#普通聊天中的收录与查回) · [语料](../../apps/host/tests/memory_recall/dialogue.jsonl) · [验收](../../apps/host/tests/dialogue_memory.cpp) |
| Agent-记忆-4 | <a id="agent-记忆-4"></a>主人重复提到同一事项可为同版增加依据，明确改口形成新版本。私聊可分别引用当前结论与旧原话 | 对照本轮已查回的两份原件。按稳定身份追溯、按当前授权读取。覆盖旧档案迁移、迟到事件、同主题不同事件、重试、撤权和旧版追溯。 | `app.memory_reflection` 的 41 个通用场景逐一核对 90 份实际后端请求，并检查聊天投递中的结论、原话和原件号。月报样例先得到一版、两份依据；改口后查出新旧两版，各有独立出处。真实模型在新措辞上的归并判断仅记录，不执行。[设计](../设计/本体记忆与知识库设计.md#跨聊天的原话复盘) · [语料](../../apps/host/tests/memory_recall/reflection.jsonl) |
| Agent-记忆-5 | <a id="agent-记忆-5"></a>大肥鱼能结合后来的经历重新解释当时的选择，也能辨认相似条件下确实发生的改选。两类判断分别引用原场次 | 首批亲历为主人私聊中实际投递的选项答复。同一来信只计一次。自主改选需有明确委托、相同条件及相同选项。撤回任一必要依据后停用相关判断。 | `app.experience_reflection` 的 47 个通用场景核对 112 份实际送给固定后端的请求、当时的实选、事后解释、引用身份与最终投递。包括同轮主动复盘、重开档案、受命改选、转述、房间受众和撤权。[设计](../设计/本体记忆与知识库设计.md#本体怎样复盘自己的选择) · [通用语料](../../apps/host/tests/memory_recall/experience.jsonl) |
| Agent-记忆-6 | <a id="agent-记忆-6"></a>主聊天之外，本体能分别查回原计划、真实工具动作、退出回执和独立读回。任务退出时登记值得复盘的经历，空闲后按原件整理 | 包括只读、需确认的变更和读改读核验任务。以真实派发区分意图和执行，以原工作和前后采样区分工具效果与目标是否得到确认。待办跨启动保留，可按原号追溯与撤权。 | `app.task_memory` 以两个原生插件回放 17 个通用场景，每次核对 143 次私聊查回、347 份固定后端请求、Trace 中的原行动与回执、最后的聊天投递和复盘时机。[设计](../设计/本体记忆与知识库设计.md#工具任务结束后的亲历与复盘) · [通用语料](../../apps/host/tests/memory_recall/tasks.jsonl) |
| Agent-记忆-7 | <a id="agent-记忆-7"></a>连续空闲时，本体能比较多次直接工具任务，形成或加强解释。出现反例后，旧看法可被标记、收窄或修正 | 当前比较同一变更工具和读口的独立任务。以两次以上真实核验为依据；私聊可区分本人解释与原行动、回执，并按版本追溯。 | `app.task_understanding` 用展厅面板和登记计数器执行 47 步通用语料，每次核对 73 份任务请求、12 份空闲推断、49 份聊天请求，并检查原件展开、前台打断、撤权和旧档案。[设计](../设计/本体记忆与知识库设计.md#跨任务的解释与反例) · [语料](../../apps/host/tests/memory_recall/understandings.jsonl) · [验收](../../tests/application/task_understanding_cases.cpp) |
| Agent-记忆-8 | <a id="agent-记忆-8"></a>主人私聊可用一至三个主题或同义词查当前任务理解，线索同分时请主人澄清，版本号可查旧说法 | 以当前解释正文及有效目标、工具号排序，每组每词最多一票。迁移第七版档案，并沿原受众和字节预算装配。 | `app.understanding_recall` 在 32、512、4096 次合成观察上各回放 21 个问句，每档核对 39 份实际请求，5 个反例问题均首先得到反例。最大证据 1934 字节。[通用问句](../../apps/host/tests/memory_recall/understanding_queries.jsonl) · [验收](../../tests/application/understanding_recall.cpp) |
| Agent-记忆-9 | <a id="agent-记忆-9"></a>一次性委托、独立业务对象和分身都能在本档案内接续自己的记录。插件代次与持久身份分开 | 插件通过独立协商的 `dfy.agent.memory` 登记身份与事件。分身按插件、渠道、账号分别授予读、写、转述权，可明确交付一条主人的原话，撤回同时封住在途答复。 | `app.child_memory` 以两个真实插件、三次应用启动回放 84 步，核对 23 份实际请求和受众切换、撤权、原事件重试。[语料](../../apps/host/tests/memory_recall/child_memory.jsonl) · [代码](../../apps/host/src/child_memory.cpp) |
| Agent-记忆-10 | <a id="agent-记忆-10"></a>插件为持久子会话选择启动人格、结束报告和导出原件。本体通过实际信箱收件，按原信号签收 | 交付号跨启动；支持仅状态、摘要、逐字副本。身份来自宿主绑定，退出状态来自运行回执，报告和笔记保留子 Agent 作者。主人私聊可翻页、签收，房间与撤回来源受限。 | `app.child_letters` 的 76 步在五次应用启动间核对 50 份实际请求、12 个真实信箱收件回执，覆盖断点重投、未执行就关闭及排队取消。[通用语料](../../apps/host/tests/memory_recall/child_letters.jsonl) · [信件存储](../../apps/host/src/child_letters.cpp) |
| Agent-记忆-11 | <a id="agent-记忆-11"></a>大肥鱼依据本轮读到的原件提出知识候选，主人逐版审阅、发布、校正、撤下和授权给指定子库 | 发布核对当前基线与所有原件。子库只能读获准的当前版、适用条件和来源类别。旧版授权、在途答复和撤回的依据均受检。 | `app.knowledge_publication` 用 82 步核对主人原话、真实工具独立读回、子会话结束信、四次应用启动及 41 份聊天和子会话请求。[通用语料](../../apps/host/tests/memory_recall/knowledge.jsonl) · [存储](../../apps/host/src/knowledge_store.cpp) |
| Agent-记忆-12 | <a id="agent-记忆-12"></a>大肥鱼在固定条件下比较不同动作渠道的亲历，识别独立读回分歧，并可将解释修订为保留疑问 | 主人声明对象、度量、单位、条件和比较方法。事前绑定与事后归类分别标注。同一采样检查点内，读工具不同且可比值不同才认定有分歧。子 Agent 的结束信保留作者。 | `app.cross_tool_memory` 核对真实 DLL 读写、空闲复盘、原信、校正前后版本及撤权后的实际请求。[通用语料](../../apps/host/tests/memory_recall/comparisons.jsonl) · [证据判断](../../apps/host/src/case_review.cpp) |
| Agent-记忆-13 | <a id="agent-记忆-13"></a>子 Agent 可要求本体在空闲时整理结束报告。主人私聊按行动、报告结果、待确认项查回逐字摘录 | 处理接收方持有的整封信，选取最多三段。发信人、整理者、原文字节位置和是否节选分别标明。传输失败、容量不足及提议无效均可按原信回退。 | `app.letter_digest` 的 74 步跨 6 次应用启动，核对 45 份实际请求、16 次信箱收件及在途撤权。[语料](../../apps/host/tests/memory_recall/letter_digests.jsonl) · [原文定位](../../apps/host/src/letter_digest.cpp) |
| Agent-记忆-14 | <a id="agent-记忆-14"></a>主人可用对象、别称、条件和采样检查点找回跨工具比较，遇到同样符合的多组时要求澄清 | 每次选择一组仍有可读原件的比较。多个问词须同时符合，检查点本身不构成对象依据。主人可以添加、删除别称。 | `app.comparison_recall` 在 32、512、4096 个比较组中每档核对 29 个问句、57 份实际请求。6 个独立分歧查询均带回双方，证据数组最大 1198 字节。[语料](../../apps/host/tests/memory_recall/comparison_queries.jsonl) · [选择规则](../../apps/host/src/case_search.cpp) |
| Agent-记忆-15 | <a id="agent-记忆-15"></a>核对旧档案恢复的本体身份、主聊天与子库原件、已收信件和知识授权 | 从第 9 至 13 版结构重启，同一档案延续原号。两份档案可有相同的本地消息号和子库笔记号。插件缺席时主人仍可读已交付副本。异常身份或未来版本打开失败并回滚。 | `app.memory_archive_recovery` 用 89 步、13 次应用启动、47 份聊天及子会话请求和 5 份工具任务请求核对两份档案及撤权。[语料](../../apps/host/tests/memory_recall/archive_recovery.jsonl) · [旧版夹具](../../tests/application/archive_fixture.cpp) |
| Agent-记忆-16 | <a id="agent-记忆-16"></a>长期单组比较优先保留早期分歧、近期独立读回和报告，其余按页查出 | SQLite 汇总截止号内的索引。宿主保存至多六项优先见证，再取所需页面。数据库聚合与深页定位仍受历史长度影响。 | `app.comparison_history_budget` 在 64、1024、8192 次读回各加一封报告时，每档 6 题核对 12 份请求。8193 份材料的优先候选从原实现的 8193 行降到 4 行，证据数组最大 1155 字节。[语料](../../apps/host/tests/memory_recall/long_case_queries.jsonl) · [见证选择](../../apps/host/src/case_evidence.cpp) |
| Agent-记忆-17 | <a id="agent-记忆-17"></a>撤下旧版知识或其依据时，只封住仍能交出旧答复或沿用旧材料的子会话 | 只选新主题时，原答复仍受撤权保护。接纳明确换题或改受众的新请求后丢弃旧答复并强制重装上下文。同一知识主题继续原会话，共用原件仍触发撤权。 | `app.child_memory_context` 的 67 步核对主题切换、重复选择、排队、在途、转述与共享原件，并核对实际请求中的旧答复是否可见。[语料](../../apps/host/tests/memory_recall/child_context.jsonl) · [子会话](../../apps/host/src/subagents.cpp) |
| Agent-记忆-18 | <a id="agent-记忆-18"></a>同类工具、跨工具和长信整理轮流取得空闲机会，重启续接最近已结算的顺序 | 把下一类工作与该批游标同事务保存。前台插话使未提交解释作废，原材料仍可在下一次空闲重新处理。队列排空后停止请求。 | `app.memory_idle_schedule` 的 35 步跨 9 次启动、26 份主聊天、子会话及后台请求，另核对 20 份工具任务请求。[语料](../../apps/host/tests/memory_recall/idle_schedule.jsonl) · [调度](../../apps/host/src/task_reflection.cpp) |
| Agent-记忆-19 | <a id="agent-记忆-19"></a>分身寄出的委托在重试、恢复和结束时沿用启动时的共享原件 | 换共享原件要用新的委托号。同源已寄出的副本保持原出处，撤回其必要原件后停止读取。旧版异源结束信及其知识、摘要、跨任务解释退出检索。 | `app.letter_authority` 以 62 步、5 次启动核对已结束和未结束的重试、旧版升级、仍独立可读的控制组，含 35 份实际请求和 10 份工具任务请求。[语料](../../apps/host/tests/memory_recall/letter_authority.jsonl) · [收件约束](../../apps/host/src/child_letters.cpp) |

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
| Agent-决策-3 | <a id="agent-决策-3"></a>哪些新渠道可以证明大肥鱼自己的经历，哪些条件可以由宿主自动绑定？ | 直接工具的亲历与复盘已接入。知识由主人逐版发布、校正和撤下。跨工具比较目前采纳主人声明的对象与条件。新渠道的取证规则、可自动推断的范围、引入语义重排的测量标准仍待确定。[知识发布](#agent-记忆-11) · [跨工具比较](#agent-记忆-12) |
| Agent-决策-4 | <a id="agent-决策-4"></a>跨进程恢复哪些任务及在途工作？ | 需逐项确定持久身份、重启后实效查证、安全重试与过期授权。 |
| Agent-决策-5 | <a id="agent-决策-5"></a>接入实际社交渠道时，如何核验代表的账号、迁移业务身份和选择长信交付模式？ | 三种预设、分身读写与转述、子库按版读取知识、跨启动收件已经接入。新平台的默认授权、账号换绑时的迁移仍需逐渠道决定。[身份与交付](../设计/本体记忆与知识库设计.md#信箱与共享读取) |

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
