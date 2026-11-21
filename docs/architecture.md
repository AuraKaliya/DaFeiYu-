# 大肥鱼当前架构

大肥鱼本体通过 `Application` 装配主 Agent、内置模型、插件宿主和受信用户入口。MC 插件持有自己的陪玩循环、游戏知识和技能，使用宿主托管的独立子会话。终端和 MC 宿主使用正式入口。桌面探针提供只读运行摘要和固定样本房间预览。

## 架构图

箭头表示运行时调用或数据方向。终端、专用游戏宿主和桌面探针按各自入口创建应用实例。

```mermaid
---
config:
  flowchart:
    rankSpacing: 28
    nodeSpacing: 24
---
flowchart TB
    Terminal["终端 dafeiyu<br/>问答、插件与委托"]
    GameHost["游戏宿主<br/>dafeiyu --mc-partner"]
    Gui["桌面 dfy_ui_probe<br/>WebView2 / Vue"]

    subgraph Host["大肥鱼本体"]
        App["Application<br/>本体入口与业务命令"]
        Intake["Mailboxes / MailboxIntake<br/>直接对话、房间、插件各有地址"]
        Partner["CompanionSession<br/>计划板投影、委托与固定场景"]
        Routes["InteractionRoutes<br/>解析结果分发与原事项关联"]
        Ask["通用 ASK<br/>渠道、呈现、答复与时限"]
        Main["主 Agent Session"]
        Agent["通用 Session / RunLoop<br/>上下文、工具、审批、效果与退出"]
        Model["Model 异步客户端<br/>DeepSeek / Qwen"]
        Trace["局内 Trace<br/>模型、工具、事件、信箱及 ASK"]
        App --> Intake
        Intake --> Main
        App --> Partner
        Partner <-->|原插件收发地址| Intake
        App --> Routes
        Routes <-->|票据及渠道回执| Ask
        Routes <-->|原消息与答复| Intake
        Main -->|回合工具 ask / mail_reply| Routes
        Main --> Agent
        Agent --> Model
        Routes --> Trace
        Intake --> Trace
        Agent --> Trace
    end
    subgraph PluginHost["插件宿主"]
        Plugins["PluginManager<br/>依赖图、生命周期、活动调度"]
        Hooks["HostServices<br/>Hook 表、事件登记和解析器"]
        Child["Subagents<br/>插件子会话与授权范围"]
        Plugins --> Hooks
        Plugins --> Child
    end
    subgraph MC["MC 陪玩插件"]
        Mc["PartnerLoop<br/>收件、目标、待答事项、持续调度"]
        Skills["技能与原动作确认<br/>渐进展开、验证、执行读回"]
        Mc --> Skills
    end
    Agent <-->|Hook 控制、改写与原生工具执行| Hooks
    Hooks -->|带实例身份的已解析事件| Routes
    Mc -->|启动独立会话、提交本轮上下文| Child
    Child --> Agent
    Mc <-->|SDK C ABI v2| Plugins
    Mc <-->|插件入箱、原发箱| Intake

    Terminal --> App
    GameHost --> App
    Gui -->|只读运行摘要| App
    Model -->|Platform WinHTTP| Service["模型服务"]
    Skills <-->|Windows 命名管道| Bridge["独立 Node / Mineflayer 进程"]
    Bridge <-->|独立玩家连接| Minecraft["Minecraft 服务器"]
```

| 模块 | 职责与源码入口 |
| --- | --- |
| **App** | 组装模型、插件和主会话，接纳终端工具授权、审批与状态读回。普通聊天、`--mc-partner` 和桌面探针共享应用服务代码。另有 `--complete` 直接调用模型的单次文本入口。[命令行](../app/main.cpp) · [Application](../app/application.cpp) · [Conversation](../app/conversation.cpp) |
| **Agent** | `Session` 持有当前运行和有界历史。每个 `RunLoop` 按工具快照、策略和预算推进模型、工具、计划与诊断。主 Agent 和插件子会话共享执行器并独立保存上下文。[Session](../agent/session.cpp) · [RunLoop](../agent/run_loop.cpp) · [子会话宿主](../app/subagents.cpp) |
| **Model / Platform** | Model 编解码请求并通过异步客户端接收完整响应。HTTP、控制台输入、DLL 装载和摘要算法属于 Windows Platform。默认 Provider 内置，空插件目录可直接问答。[模型客户端](../model/client.cpp) · [协议](../model/protocol.cpp) · [WinHTTP](../platform/winhttp.cpp) · [代码装载](../platform/code_module.cpp) |
| **Plugins / SDK** | 插件包使用同进程 v2 C ABI，安装后显式加载。宿主按依赖发布 Hook 顺序，独立接纳插件事件并调用对应解析器，公平推进插件活动步骤。卸载等原工作退出，再撤销地址与注册。[管理器](../plugins/runtime/manager.cpp) · [事件宿主](../plugins/runtime/host_routed_events.cpp) · [接口](../sdk/include/dafeiyu/routed_event_abi_v2.h) |
| **Runtime / Events** | Runtime 提供有界操作、预算和调度，Events 提供广播通知与定向信箱。信箱地址绑定发送方、接收方与独立 FIFO，类别和版本决定处理方式。接纳、业务结果和实际退出分别结算。[广播通道](../events/broadcast_channel.cpp) · [信箱](../events/mailboxes.cpp) · [本体收件](../app/mailbox_intake.cpp) |
| **ASK** | `AskService` 持有通用票据，`InteractionRoutes` 将声明式解析结果分流到渠道或主 Agent 信箱。插件消费原答复并裁定动作。[ASK 状态机](../app/ask.cpp) · [交互路由](../app/interaction_routes.cpp) |
| **Trace** | 主会话和插件子会话记录实际提交的统一模型请求、独立退出的模型响应、各工具及信件结果。事件、信箱、ASK 和游戏回执通过原身份关联。支持按回合查看，并用录制响应和工具回执离线重跑单轮。[模型与工具记录](../agent/include/dafeiyu/run_capture.hpp) · [本体落盘](../app/run_log.cpp) · [审阅与回放](run-guide.md) |
| **Desktop** | WebView2 主文档通过 `state.read` 获取当前或最近运行摘要，可按会话号和回合号筛选。桌面原生业务线程发布值副本，房间预览取固定样本。[业务线程](../app/gui/run_status_host.cpp) · [主文档桥接](../ui/host/probe_host.cpp) · [运行说明](ui-probe-run-guide.md) |

构建入口是 CMake 和 VS2022 x64，宿主采用 C++20，公开 SDK 支持 C 消费者。终端是默认启动目标，`BUILD_GUI` 和 `BUILD_TESTING` 控制探针和测试目标。[根构建](../CMakeLists.txt) · [预设](../CMakePresets.json) · [分发规则](../cmake/Distribution.cmake)

## Agent 会话与工具流程

```mermaid
---
config:
  flowchart:
    rankSpacing: 28
    nodeSpacing: 24
---
flowchart TB
    Input["普通聊天或工具委托"] --> Session["固定工具范围<br/>Session 接纳运行"]
    Session --> Mode{"启用计划？"}
    Mode -->|是| Prepare["模型生成候选<br/>宿主解析并固定首步范围"]
    Prepare --> Model["异步模型请求"]
    Prepare -->|失败或取消| Stop
    Mode -->|否| Model
    Model --> Response{"响应结果"}
    Response -->|工具调用| Check["工具名、参数、范围与策略校验"]
    Check -->|直接允许| Work["执行原生工作并等待实际退出"]
    Check -->|需确认| Approval{"原候选获批<br/>且重新校验通过？"}
    Approval -->|是| Work
    Approval -->|拒绝或失效| Rejected["记录候选未执行"]
    Check -->|不允许| Rejected
    Work --> Results["提交完整工具批次及效果回执"]
    Rejected --> Results
    Results --> Continue{"策略和预算允许继续？"}
    Continue -->|是| Model
    Continue -->|否| Stop["结算失败或取消<br/>保留已提交事实与已发生效果"]
    Response -->|完整文本| Plan{"属于计划步骤？"}
    Plan -->|是| Verify["等待宿主验证步骤"]
    Verify -->|下一步骤| Model
    Verify -->|全部通过| Done["以已提交的回答结算成功"]
    Verify -->|取消、超时或运行失败| Stop
    Plan -->|否| Done
    Response -->|模型失败、取消或超时| Stop
    Done --> Exit["发布终态通知<br/>实际退出后释放活动槽"]
    Stop --> Exit
```

- **普通问答**每轮接收一次完整文本。成功时提交整轮对话。失败或取消后，仍保留先前已经提交的对话与工具事实。[历史提交](../agent/run_context.cpp) · [会话推进](../agent/session.cpp)
- **终端委托**中，`/run` 授予一个只读工具，`/run-confirmed` 允许具体变更候选进入审批。`/task` 固定同一个插件实例上的首读、变更、末读，宿主核对步骤顺序、实际回执和期望值。原候选的批准与实际调用分开记录。[应用入口](../app/application.cpp) · [工具批次](../agent/tool_batch.cpp) · [读回验证](../app/tool_verification.cpp)
- **原生观察与计划控制**通过显式宿主入口接纳。观察触发默认只读，变更分支要求声明条件、可信当前采样和确认。计划步骤支持有界重规划、目标修订和等待验证时换入新观察。诊断和离线比较沿用运行身份和版本信息。[观察控制](../agent/observation_controller.cpp) · [计划与观察入口](../agent/session.cpp) · [离线场景](../tests/offline_scenario.cpp)

## 接续我已经搭的墙

```mermaid
---
config:
  flowchart:
    rankSpacing: 28
    nodeSpacing: 24
---
flowchart TB
    Game["玩家：接着搭我围的墙<br/>下一次聊天：我在墙边上"] --> Loop
    Loop["MC PartnerLoop<br/>收玩家和现场，保存目标、修订与动作回执"] -->|新事实或任务可继续| Turn
    Turn["宿主子会话<br/>运行模型与已注册 Hook"] --> Look["MC 观察原墙，留下待答事项"]
    Look --> Local["MC 先问玩家，局内等待"]
    Local -->|收到有效答复| Resurvey
    Local -->|等待期满| Event["插件发出待答事项超时事件"]
    Event --> Decode["宿主按类型和版本调用插件解析器"]
    Decode -->|直接 ASK| Ask["本体 ASK<br/>呈现原问题，等首份有效答复"]
    Decode -->|交给主 Agent| Box["插件登记的主信箱<br/>优先收件或手动审查"]
    Box --> Main["主 Agent 读取原信<br/>通过 ask 选择渠道"]
    Main --> Ask
    Ask -->|直接路径| In["MC 原实例入箱"]
    Ask -->|主 Agent 路径| Back["原主信箱再收答复<br/>主 Agent 调用 mail_reply"]
    Back --> In
    In --> Resurvey["MC 恢复共同目标，重新读现场"]
    Resurvey --> Expand["MC 选择放置分类<br/>宿主为下一轮固定工具范围"]
    Expand --> Gate["MC 校验原格、支撑和材料<br/>门洞候选需要玩家批准"]
    Gate -->|允许且现场仍有效| Work["Mineflayer 补墙或放门<br/>读回精确方格与库存"]
    Gate -->|拒绝、到期或失效| Receipt
    Work --> Receipt["回填动作结果与实际效果"]
    Turn -. 本轮退出 .-> Loop
    Receipt --> Loop
    Loop -->|暂无新事实且任务在等待| Wait["继续运行，等待玩家、现场或原答复"]
    Wait --> Loop
```

- **局内状态。** `PartnerLoop` 由宿主活动调度持续推进。插件保存当前共同目标、修订、已知与待核实事项、最近行动回执。一次模型回合结算后，插件按新玩家事实、任务继续、私有现场变化或目录展开选择何时开下一轮。玩家事件优先，每批新玩家事实最多补充四轮自主决策额度。离场、换世界和关闭会回收局内任务。[插件循环](../plugins/mc/partner_loop.cpp) · [任务状态](../plugins/mc/partner_task.cpp) · [配对读口](../plugins/mc/partner_feed.cpp)
- **工具快照。** 基础回合收到观察、交流、目录、任务更新和登记局内待答事项。目录列出 12 类及各类当前实现，查询后最多选三类进入后续回合。宿主每轮固定实际工具契约，游戏侧检查世界、来源、范围和动作前置条件。[技能注册表](../plugins/mc/partner_skills.hpp) · [宿主授予](../app/subagents.cpp) · [游戏实现](../plugins/mc/bridge/partner.cjs)
- **放置读回。** 观察返回已加载的方块体积和坐标原点。放置检查精确目标格、支撑面、占用及材料，再依据回执里的实际方块与库存判断效果。门上下两格分别核对。旧圆石路径由独立 `scenario` 入口承接显式委托、拾取和返程。[现场取样](../plugins/mc/bridge/block-survey.cjs) · [游戏放置](../plugins/mc/bridge/placement.cjs) · [宿主回执校验](../plugins/mc/world_protocol.cpp)

## 当前边界

信箱去重窗口、局内任务和业务确认驻留进程内。局内 Trace 写本地证据并按运行关联。桌面通过值副本读取状态，房间生产者和伙伴面板按桌面方案接入。原生 Hook 在宿主线程同步返回，活动任务通过 `dfy.activity` 推进。Agent 宿主审批和 MC 业务确认有各自的状态和生命周期。[运行诊断](../agent/diagnostic_snapshot.cpp) · [MC 交互方案](mc-companion-development-plan.md) · [桌面主面板方案](main-agent-room-development-plan.md)

## 事件注册与解析

`dfy.event.routes@1` 在插件加载时登记事件名、版本、闭合字段契约和原生解析器。运行中的插件按实例序号提交事件。宿主校验并入队时记录 `event.captured`，后续取件调用原解析器并记录 `event.decoded`。应用分派器把 ABI 投影转换为类型化指令，再记录 `event.dispatched` 或拒绝。普通信件、直接 ASK、交主 Agent 的提问、渠道反馈和插件收件确认分别进入对应处理分支。MC 的待答事项及超时规则由 MC 插件持有。[插件 SDK](../sdk/include/dafeiyu/routed_event_abi_v2.h) · [宿主解析](../plugins/runtime/host_routed_events.cpp) · [应用分派](../app/event_dispatch.cpp)

## 信箱优先级与唤醒

直接对话、房间和每个注册插件分别使用受信地址。每箱独立 FIFO，信箱 ID 锚定发送方与收件方，分类及版本决定处理方式。`dfy.mailbox.rules@1` 声明同一插件信箱的多个分类，按箱设优先级、按类别设唤醒。高优先和普通档各自轮转，两档都有待办时最多连续取三封高优先件。主回合忙碌时，到件留队等待；审查通过的消息在回合接纳后出队，被过滤的事实由收件器消费。[信箱网络](../events/mailboxes.cpp) · [取件调度](../app/mailbox_intake.cpp)

到件可按类别唤醒空闲主 Agent。用户也可用 `/mail review` 主动审查，或在发起普通对话时带入一封已审查的待收信件。`/mail wake manual` 暂停插件信箱的自动唤醒。消息接纳、ASK 呈现、真人答复及 MC 动作读回分别结算，均可通过 Trace 关联。[房间信箱](../app/room_mailbox.cpp) · [通用交互路由](../app/interaction_routes.cpp)

## 通用 ASK

ASK 票据关联原事件、事项和修订。原事件声明可用的游戏渠道，宿主按运行入口报告能呈现和接收答复的终端渠道。主 Agent 的 `ask` 工具只枚举可用渠道，实际调用时复核。网页陪玩宿主由后台管道启动，MC 问题只有游戏渠道。渠道实际呈现后，在答复截止前接纳配对的第一份有效答复。直接路径把结果送回原插件入箱。主 Agent 路径把结果放入原主信箱，由 `mail_reply` 转发。已接纳的答复在本次宿主运行内跨越答复截止继续保留，直至同一插件实例按原身份确认收件。Trace 分别记录答复接纳、信箱入队和 `interaction.acknowledged`。普通信件可作为上下文入轮，提问工具仅在对应的提问信件回合中开放。[ASK](../app/ask.cpp) · [分流和回信](../app/interaction_routes.cpp)

采集与放置共享 MC 的原候选确认。局内确认可由插件自行处理，交给主 Agent 的确认复用上述 ASK。原候选的确认窗口为 25 秒。MC 插件收件时按候选时限、玩家、世界和游戏游标裁定，候选失效也确认收件，Trace 将其标为 `stale`。动作执行前再检查现场与参数。Agent 运行时的工具权限审批继续处理自己的候选、修订和调用权限。[MC 动作确认](../plugins/mc/partner_confirm.cpp) · [宿主审批](../agent/confirmation.cpp)

主侧结果信件按原消息序号和实际接纳它的回合绑定 `mail_reply({})`。模型的结构化工具调用经过通用执行器后接纳转发意图，Trace 记作 `reply.intent`。主回合已退出而尚未提交意图时，原答复进入待复审，用户调用 `/mail review` 可再次取件。复审信件使用被动类别。[回信工具](../app/mail_reply_tool.cpp) · [复审与意图处理](../app/interaction_routes.cpp)

## Hook 注册与执行

插件载入时声明 Hook 点和适用范围，范围可含整个主会话。宿主校验完整依赖图并检测环，再按稳定拓扑顺序固定各 Hook 表。依赖已满足的插件按注册顺序取用，同一插件内按处理器注册顺序取用。每次触发持有当前顺序快照，相关工作实际退出后才能释放插件模块。

`dfy.hooks@1` 对模型请求、工具启动和工具结果提供控制，工具启动支持等待人工批准。`dfy.hook.text@1` 支持消息入轮、模型请求发出前、模型响应收到后、工具参数和最终回复投递的字段改写。独立提案基于原字段，不同结果产生冲突。声明接续且依赖先前改写者的处理器可读到前一结果。工具使用最终参数进行授权、校验和审批，实际效果保留原回执。回复投递改写与会话历史各自保存。[C ABI](../sdk/include/dafeiyu/hook_abi_v2.h) · [正式循环](../agent/run_loop.cpp)

## 实施和验收

| 交付 | 验收标准 |
| --- | --- |
| 实际上下文记录与离线回放 | 按原会话、回合、步骤和调用身份读到送模消息、详细工具、模型输出、工具回执与最终结果。固定证据重跑得到相同的模型输入和结果。 |
| 整理测试 | 功能回归采用固定模型、传输替身或隔离 Paper。真人操作仅保留已搭墙体主题。 |
| 插件局内循环 | 一轮模型结束后，插件仍收取世界和玩家事实。固定样本验证无新聊天时自主继续、现场变化触发、插话、退出和回收。 |
| 分类展开和通用候选确认 | 初始工具范围保持精简，展开类别后核对正式送模契约。采集与放置共用确认状态机，批准与实际执行分别取证。 |
| 放置及首次架构审查 | 隔离游戏由独立协议玩家核对原墙、补上的一格、审批前的空门洞和批准后的上下门格。用户审查本页后进行实际模型体验，并按日志讨论。 |
| 事件注册与解析 | 拒绝无对应版本、非法载荷、冲突重投和未登记的信箱分类；实例停止后回收原登记。 |
| 优先级、主动收件与 ASK | 检查两档取件、主回合接纳后出队、主动对话带入待收件、呈现后首答、到期及重复、迟到答复。 |
| 两条墙体路径 | 固定后端和隔离 Paper 验证直接 ASK、主 Agent 自动唤醒及手动审查；检查同一答复进入子 Agent 后续上下文并继续原墙。 |

子会话使用主程序选定的模型配置。长期记忆、跨会话保存和真实房间面板另行审查。首次真人体验采用同一正式伙伴入口。[首轮实机设计](../tests/mc-companion/README.md#首轮实机接续我已经搭的墙)
