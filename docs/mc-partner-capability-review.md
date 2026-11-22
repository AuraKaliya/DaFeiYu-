# MC 伙伴功能清单与参考项目复核

本页保留参考项目比较和待评审的能力细则。河边试玩的取证基线为 `4b6c880`，功能细则中的状态以该次取证为准。当前运行边界、已核验的修订和后续顺序见架构与开发计划。[当前流程](architecture.md#mc-陪玩插件) · [开发计划](mc-companion-development-plan.md)

## 河边这局暴露出的四个缺口

| 玩家看见的现象 | 源码和游戏记录能证明的事 | 对产品的要求 |
| --- | --- | --- |
| 她等人说话，随后又自行去采石 | 当时的进场问候由宿主发送。“你怎么没有动作”触发了去山脚及两次实际采石。她有执行能力，尚缺能跨回合维持的游戏目标及空闲唤醒规则。[当前伙伴入口](../app/companion_partner.cpp) · [开场与执行探针](../tests/mc-companion/playtest.cjs) | 管理中心能区分“等你商量分工”“已有目标，正在自己安排”“当前受阻”“玩家已接管”。她能主动提议，也能继续已约定的工作。 |
| 她说不能丢，又建议把东西放在脚边 | 正式技能表里仅有观察、移动、采集、回复。本机 Mineflayer 提供 `bot.toss`；上游的生存丢物走 `mc_act(toss)`。`mc_give` 是创造模式取物。我们缺游戏侧实现、对模型的注册和交付回执三个环节。玩家靠近不能从她背包里直接拾取。[本地技能表](../plugins/mc/native.cpp) · [上游动作定义][act] · [丢物实作][toss] | 能走到原玩家身边丢出约定数量。背包减少可回答“我丢在这里了”；有接收证据才回答“你拿到了”。普通陪玩无需为每块物资单独询问。 |
| 掉进坑后反复移动，最后找玩家帮忙 | “你能先上来吗”这一轮实际尝试了四次移动，也挖过一块泥土；四次移动均报受阻。当前观察能读注视方块、指定材料和坐标，但普遍存在的坑沿高度、头顶净空、可落脚面、连续无位移没有形成给伙伴的受困事件。旧 G14 的覆盖窗绑定特定查看任务。[通用技能](../plugins/mc/bridge/partner.cjs) · [旧探索判定](../app/companion_review.cpp) | 进入异常状态要能主动发现。由插件提供换路线、退一步、跳、挖出踏步或垫方块各自的可行条件，让模型选。求助时说清尝试过什么、还缺什么。 |
| 同一意思连续说两遍 | 这局泥土到手、求助、交付答复各双发一次。取证时模型可显式用 `reply` 发言，宿主也会发送最终文本；当时的去重只比较同一来源下的字面文本，计划板只显示最终答复。原探针故意返回同一句话，覆盖不到同义改写。[回复实现](../app/companion_partner.cpp) · [游戏侧去重](../plugins/mc/bridge/partner.cjs) · [原固定样本](../tests/mc_playtest.cpp) | 对一条待答消息只结算一次最终答复。独立的进度播报仍可发送。游戏和管理中心引用同一发送记录。 |

泥土这一局还验证了两个不同的数：她采到最初两块后确实在背包看见了两块；后来脱困又挖了泥土，出坑时说“四块”，下一次背包观察是五块。验收既要查玩家是否拿到了东西，也要查她报数用的是哪一次采样。原始记录在该局的 `console.log`、`host/game-actions.jsonl` 和 `host/board.txt`；下文的“验收”均是待验证场景。

## 四个直接参考各自解决了什么

| 固定参考版本 | 源码里的机制 | 对大肥鱼的取舍 |
| --- | --- | --- |
| **dsh-mc-agent** · `af7ea21` | `mc_auto` 有可改的常驻目标、空闲唤醒、靠近玩家时的唤醒间隔、退避、危险事件独立预算和决策记录。默认 `ai-led` 让事件唤醒模型。动作调度器声明移动、交互、背包等资源及可取消阶段，默认调度模式是观察记账。任务有步骤、实物进度、检查点及更换计划后的代次。[自主层][auto] · [看门狗][watch] · [动作调度][sched] · [任务结算][settle] | 最接近我们的独立玩家方案。借鉴“持续目标＋按事唤醒＋有回执的动作”。上游的默认通关目标、唤醒秒数、自动改地形策略不能代替玩家与大肥鱼的约定。 |
| **DeepClef** · `253f14a` | 多人聊天进入带游标的观察缓冲。对符合条件的在途任务，聊天检查点可将“动作仍在后台执行”交给模型，随后另送真实结果。普通聊天与明确替换指令有不同路径。AltoClef 有运动进展检查器、生存任务链和面向指定玩家的交付任务。[聊天检查点][dc-chat] · [进展检查][dc-progress] · [交付任务][dc-give] | 借鉴“边干边听”、主动作归属和任务组合。它运行在 Java 游戏客户端，我们用独立 Mineflayer 角色实现同一语义。它的交付任务会在缺货时尝试获取材料，我们应按当前分工决定是否获准补采。 |
| **dsh-sts2** · `3a29541` | 插件把带序号、发送者的待答消息交给正在运行的会话，支持时用 `steer`；通过 `sts2_chat_send(reply_to)` 把回答送到玩家真正看见的聊天窗口。状态、等待、自动推进和交还控制是独立工具。[消息注入][sts-plugin] · [工具注册][sts-tools] | 借鉴原话明确寻址、忙时插话以及无变化时等待。进展查询不应自动撤销采集权限。 |
| **STS2-AiTeammate** · `d598ac6` | 游戏端保留 pending/recent、消息序号和已答状态，拒绝已由同一路径回答的重复消息；允许内置后端补答后的外部迟答再展示。游戏状态有变化标记，决策通过游戏线程校验与执行；桥接在线与确实在驱动是两个状态。[聊天结算][sts-reply] · [状态与接管协议][sts-bridge] · [决策引擎][sts-decisions] | 借鉴“由游戏端记住哪条原话答过”，并自行确定迟答规则。大肥鱼是同一位说话者，首份最终答复已经发出后，迟到的同义答复应归档；确需纠正时明确发一条更正。 |

早期调研里还有三项跨领域经验。LeRobot RTC 的启发是算下一段时允许当前有效动作继续；在 MC 中可以在采集运行时读进展、处理聊天，并在替换动作时核对衔接点。UniAD 的启发是让感知服务于当前目标，例如“离开坑底”更需要坑沿与落脚面。Bench2Drive-Robust 将感知缺失、状态误差和控制延迟作为不同扰动；MC 可以分别测试晚到的拾取回执、旧位置和模型答复慢。这里迁移的是调度与评价方法。画面补充仍优先考虑 MC 的视线、地图和按需截图。[RTC](https://huggingface.co/docs/lerobot/rtc) · [UniAD](https://github.com/OpenDriveLab/UniAD) · [Bench2Drive-Robust](https://github.com/Thinklab-SJTU/Bench2Drive-Robust)

## 功能细则与验收场景

“已有”仅指在取证基线能找到本地入口。“部分”表示可复用其中一段。下表的动作举例用于说明技能能支持什么；试玩时允许模型选择其他合乎情境的做法。确定性检查核对系统是否忠实执行了她的选择。

### 陪伴、目标和并行处理

| 功能 | 当前缺口及建议职责 | 玩家可验证的结果 |
| --- | --- | --- |
| 常驻目标与分工 | 本体已有会话和计划基础。主 Agent 保留共同目标、范围、分工和暂停约定；游戏里的她可提议短期目标、在分工内自行排下一步。插件注册哪些事件值得唤醒。[常驻目标][auto] | “一起盖小屋”时她可以先提议备料；玩家同意后，采完一批能核对剩余工作。空闲陪伴也允许安静等候。 |
| 生命周期与完成条件 | 现有玩家回合和旧圆石任务各管一段。区分游戏会话、活动目标、原话、一次技能工作；记录等待、执行、受阻、暂停、交付、接管及恢复条件。[任务代次][tasks] | 一句话答完后能继续同一项备料工作。“会继续采”必须有可继续的任务；轮到等玩家时能说明在等什么。 |
| 唤醒与主动感知 | 已有状态轮询和单动作租期。MC 提供受伤、缺氧、跌落、停滞、目标变化、库存变化、玩家到场/离开等原始事实；模型结合目标决定继续、处理、提议或等待。[看门狗][watch] · [事件与空闲预算][auto] | 入场事件到达后能观察到她自主开口、行动或等待。反复采样同一堵墙时保留停滞证据；空闲退避不吞掉新的危险事件。 |
| 插话与动作所有权 | 取证基线以最新玩家消息号判原动作失效。提议由任务号持续持有动作，消息号用于寻址；是否修订目标交给模型。[聊天检查点][dc-chat] · [资源与优先级][sched] | 在同一工作中插入进度询问或分工建议，回看她听到了什么、怎样选择，以及动作是否真的维持或被替换。 |
| 让出控制、暂停与恢复 | 玩家在聊天中提出接手，模型结合当前工作和约定决定接受、拒绝或商量；适配器在确实收到执行请求后核对控制释放。第一步已移除旧版短语匹配。[本地实现](../plugins/mc/bridge/inspection.cjs) | “停，我来”可以得到“不要，我先做完”，也可以有其他回应。观察她是否听懂、说法与实际控制状态是否一致。 |
| 回复归属与可见性 | 游戏或管理中心的原话有待答状态；进度播报、提问、最终答复、更正分别记用途。一次最终发送只有一个负责方，工具已发送时宿主只结算。保留来源、频道、发送号与回执。[游戏侧回复表][sts-reply] | 用显式回复和最终文本做两种同义表述，玩家只收到一次最终答复；两条不同的原话仍能分别收到相同回答。跨入口回答或回执迟到也可追溯。 |
| 执行中问进度、失败与预算 | 已有动作退出结果、独立位置读取和模型调用预算。补动作进行中的采样时间、已挖、已入包、所处阶段；连续无进展才升级调查。时间预算耗尽区分已发生、未发生、未知。[任务结算][settle] · [状态等待][sts-bridge] | 可说“挖了两块，捡到一块”；数据过旧就重读。无变化不刷屏，未知效果不盲目重挖或再丢一次。 |
| 计划板和回放 | 现有对话板与游戏动作 JSONL。统一本局目标、玩家消息、观察、工作及发送回执的引用，记接收、开始、退出、发送时间；首页只展示进展和待决定事项。[本地宿主](../app/companion_host.cpp) | 从“为什么改采泥土”能追到玩家原话及旧动作效果；计划板可核对游戏实际说过的每句，也能拆分等待模型、寻路和拾取所花的时间。 |

### 在世界里观察、脱困、取物和交付

| 功能 | 当前缺口及建议职责 | 玩家可验证的结果 |
| --- | --- | --- |
| 世界与自身快照 | 部分已有坐标、维度、玩家、实体、注视方块及库存。补朝向、运动、着地、头顶净空、生命、饥饿、氧气、手持及关联的世界与采样时刻。[状态、背包工具][mc-index] · [生存任务链][dc-survival] | “她还在下落还是已经站稳”有读数。换维度或卸载区块后，旧坐标不能被当作当前位置。 |
| 地形、视野与目标指代 | 已能按注册名找可见块。补脚下层与上下层简图、坑沿高度、落脚面、掉落物、实体及可达/未知；“这块”关联玩家指向的目标。第一人称图、带图例的地图按需读取。[地图工具][map] · [第一人称观察][observe] · [掉落物][drops] | 站在两格深坑里可以比较两个方向；“采这棵树”丢失目标会重读或回问，“采一块原木”可换树。 |
| 导航、靠近、跟随和会合 | 已能走向地标、单次玩家位置及坐标。补精确站位、同高度、交互距离、持续跟随、避让、分段长途移动。插件返回实际位移和仍差多少。[移动意图][move] | “到我旁边”追踪移动的玩家；“先上来”以站稳于目标高度验收。仅在目标附近的坑底不能算上岸。 |
| 环境事件与通用受阻判断 | 旧查看场景已有“返回原区域且视野无新增”的复核；普通伙伴动作只有成功、受阻或中断等结果。推广距离、垂直进展、寻路重算原因、阻挡物、地形变化；保存已经试过的路线。[本地 G14](../app/companion_review.cpp) · [移动进展检查][dc-progress] | 原地转头或横向挪动不会把“上来”标成有进展；真实绕路仍能继续。重复同一失败目标时模型得到新证据。 |
| 自主脱困与求助 | 部分已有走、挖。引入可用出口、跳、退回上个稳定点、绕行、挖台阶、放置垫脚、游向岸边等候选。插件核对净空、支撑、库存和原范围；游戏里的她决定手段。[移动恢复][movement] · [地形交互][act] · [水下恢复][water] | 安全台阶可走就自行上来；需拆玩家建筑、没有可用方块或多个方案实际失败时再问。回报说明尝试和结果。 |
| 采掘与物品语义 | 已实现同类可替换与指定坐标两种采集。补材料目标与可能掉落物的对应、工具耐久、候选收益和可撤退性；保留破坏读回与服务端拒绝后的重读。[采掘工具][dig] · [通用名称][names] | 玩家要泥土时可评估草方块；锁定单格则不会改挖旁边。方块消失、满包或拿错工具都有明确结果。 |
| 地面拾取与库存核对 | 常规采集现在按背包净变化报告。补查询掉落实体、靠近并等候真实更新、部分拾取、满包、别人先捡。特殊圆石归属旁证继续在需要时使用。[掉落扫描][drops] · [本地采集](../plugins/mc/bridge/partner.cjs) | 挖空但东西还在地上时可以决定要不要追拾；脱困又挖一块后按新库存报数，没必要对普通陪玩逐块盘账。 |
| 当面交付 | 缺少模型可用的生存丢物能力。分清物品、数量、接收玩家、距离与可见性。复用现有观察、靠近和背包，再实现丢出及结果复核。DeepClef 将找玩家、靠近、丢出、拉开距离组合为任务。[生存丢物][toss] · [给指定玩家][dc-give] | “丢两块泥土给我”只交约定数量；玩家离开或库存不足时说明。可区分自己仍持有、已丢在地上、指定玩家已拾取、归属未知。 |
| 容器、背包与装备 | 有背包汇总及挖掘自动选工具。补槽位、装备/副手、耐久、可用空间、按名称与数量存取箱子，以及操作后双方库存读回。[库存][inventory] · [箱子][chest] | “剩下的木头放家里箱子”从可辨认的箱子入手；箱满或部分存入时只报实际数量，保留玩家预留物品。 |

### 可扩展的完整游戏范围

以下是能力地图里的后续范围。当前伙伴模型只拿到四项注册技能；表中的组合能力需要分别实现或验证。

| 功能 | 可借鉴的机制 | 独立验收示例 |
| --- | --- | --- |
| 配方、工作站与生产 | 查询可做配方、缺料与替代品；在工作台合成，熔炉核对原料、燃料、产物与等待；用库存变化结算。[合成][craft] · [熔炼][furnace] | 已有木头时能做木板；缺燃料能说明缺什么，等待熔炼期间可以交谈。 |
| 方块/实体交互、建设和拆改 | 看向、装备、使用、放置、拆除、搭桥与楼梯。建造可选区域或分层蓝图，先预演占地、清单与缺料，再分段施工并核对；改图保留已放方块。[原子交互][act] · [蓝图][build] | 合作盖屋能区分选址、备料、施工；改成石屋时不擅自拆已建部分。 |
| 日常生存和生活分工 | 睡觉/起床、照明与补给、食物/装备、耕作与补种、钓鱼、村民交易，结合时间、成熟度、物资及对方可用性。[生活与交易工具][mc-index] · [生存链][dc-survival] | “今晚在家等我”可以休息；“照看田地”能判断成熟度，种子不足时保留事实。 |
| 多地形移动、载具和远行 | 水域/氧气/上岸、乘船与下船；条件允许时扩展冰道、骑乘、鞘翅与着陆，按维度、装备、最短可行路段和返程条件组织。[水域移动][waternav] · [DeepClef 工具表][dc-tools] | 过河时能决定游、绕或坐船；缺船、氧气不足、玩家下船或目标失效时能停在合理位置。 |
| 游戏内危险、护卫与恢复 | 近身威胁、低血量、坠落、熔岩、水下风险、避险、游戏战斗/护卫、死亡重生、遗失物资及远行前整备。监测触发与是否代玩家开战分开配置。[危险观察][danger] · [游戏整备和战斗][mc-index] | 身边出现危险时能告知或按既定分工撤离；死亡后核对位置、装备及原目标。是否迎战由当前约定决定。 |
| 共享地标、规则与记忆 | 在本局保存家、箱子、工地、队友称呼、已答问题、资源保留量。跨局记忆和任务恢复涉及本体持久化，单独审查；重新入局先读世界。[上游工作区记忆][memory] · [任务账本][tasks] | 同一局说“回刚才的箱子”；重连后能区分所记位置与当前确认的位置。 |
| 身份、联机、皮肤与会话 | 已有独立账号、绑定玩家、世界代次、浏览器入口和指定皮肤。可补连服能力探测、角色在场/离线、多人发言范围、私聊可验证性、空闲接管和异常重连。[连接与能力声明][mc-index] · [组队桥接][sts-bridge] | 两个人一起说话时只按授权者修改分工；私聊保持原频道，不能核验来源时提示可用入口；重连无第二个身体控制者。 |
| 能力发现、扩展与诊断 | 已有插件技能枚举和独立配置。给模型动态说明当前可用工具、环境前提、禁用原因与真实成功标准；管理中心区分能力未注册、缺工具、地形不通、模型故障、回执不明。扩展工具分组，按需查询能力说明。[本地技能声明](../sdk/include/dafeiyu/companion_abi_v2.h) · [DeepClef 能力说明][dc-tools] | 询问“能把泥土递给我吗”时，答复和当前可执行动作一致。加一个 MC 技能不需给主 Agent 增加一句游戏口令。 |
| 只读旁观、主动托管和人工接管 | 观察、聊天与执行权限可分别启用；区分已连接、正在驱动、已暂停、真正释放；用状态版本与待答号处理迟到结果。管理工具、创造模式、服务器命令、文件发布等特权单独列能力。[接管协议][sts-bridge] · [上游工具分层][mc-list] | 旁观可问状态；接管期间可以回看结果。另一个控制者上线时不会复用旧动作或重复发送答复。 |
| 性能、异常与回归 | 按事件重要度合并/退避，模型慢时复用有效观察；分别记模型、游戏、传输与拾取延迟。使用协议替身与本地游戏覆盖输入变体、模型/游戏迟到、网络断开、时钟或观测落后。[决策节拍][auto] · [本地测试规范](../AGENTS.md) | 运行中查进度不打断动作；重试回复不双发；掉线前最后一块效果不明时先重读。真实模型的自然对话质量由真人试玩另记。 |

### 首批功能应写到什么粒度

| 能力 | 输入和前提 | 大肥鱼可以决定 | 插件/宿主必须给出的结果 |
| --- | --- | --- | --- |
| 无人发言时继续陪玩 | 本局目标、分工来源、允许范围、最近一次进展、是否明确要求等候。 | 继续已有步骤、换同类目标、靠近看看、提议或保持安静。在“等我回来”的约定下不擅自开工。 | 保留这轮唤醒的具体原因与目标归属。新目标可撤回；重复空闲事件合并。人工接管后停止自主发起动作。 |
| 受困时恢复 | 原目标（例如“到玩家所在的地表”）、最近位置/高度、路线失败原因、可站立邻格、身上可用物品及已试手段。 | 比较出口；可在原约定内改道、退一步、开阶；确需扩大破坏范围时回问。 | 每次尝试带动作号和前后位置。尚在掉落、仅横向挪动或仍在同一低层，只报告局部进展。失联时保留已发生效果，待重新观察。 |
| 将物品交给玩家 | 物品、请求数量、可动用数量、目标玩家、最后确认的位置。原话“丢给我吧”的指代由会话结合上下文判断。 | 找人、靠近、确认仍在场、分堆并丢。数量不清且影响分工时先问；库存不足可以报告实有数量。 | 关联丢前/丢后库存；可用时关联掉落实体和拾取者。明确“丢出 N”“对方确认收到 N”或“尚未核实”。中断后先看原回执及现场。 |
| 一条原话的一次最终答复 | 原消息号、原频道、是否已有最终回复、进度或最终用途。回复身份在发送前建立，重试沿用同一身份。 | 她可以先报关键进度、稍后再结算，也可以只给最终答复。文字可以自由组织。 | 游戏适配器保存“接纳、已发、失败或未知”；管理中心读同一记录。先用工具答过，再返回近义最终文本，只留一条玩家可见结论。断线后发送效果未知时不能承诺必达或无条件重发。 |

**可以直接用到工程里的其他做法。** 上游用简报层从工具回执中保留失败原因和进展，把逐块流水留给诊断；这适合让模型少读重复现场，同时让管理中心按需展开。上游还拆开工具总耗时、CPU 用量等指标，并将重计算线程池设成可选。这一局首答等了 46 秒，我们应先记录玩家原话进入、模型请求/返回、游戏工作开始/完成和实际发言的时间，再决定是改提示、减少工具往返、改寻路还是异步计算。[结果简报](https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/brief.mjs) · [性能采样](https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/perf-monitor.mjs) · [可选工作线程](https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/worker-pool.mjs)

## 工具覆盖索引

为便于查漏，下面按固定版本的注册入口列出名字。不同项目的同类能力在上表只列一次；每个注册名下还可能有多个动作模式。

| dsh-mc-agent 的注册组 | 名称及在本项目的用途 |
| --- | --- |
| 连接与控制 | `mc_status`、`mc_lan`、`mc_ping`、`mc_connect`、`mc_disconnect`、`mc_stop`、`mc_accounts`、`mc_sessions`、`mc_capabilities`。对应连接、状态、能力发现和控制权。 |
| 感知与诊断 | `mc_scan`、`mc_map`、`mc_inventory`、`mc_observe`、`mc_drops`、`mc_events`、`mc_watch`、`mc_diag`。分别解决查目标、读地形、看持有量、看现场、看掉落、回顾增量、订阅变化和查故障。 |
| 说话与行动 | `mc_say`、`mc_move`、`mc_act`、`mc_dig`、`mc_build`、`mc_sequence`。重点查 `mc_act` 的看向/靠近、放置/搭路、使用方块或手持物、装备/丢物、跳、睡/醒、农作、乘船/下船和重生子动作。 |
| 生存与生产 | `mc_craft`、`mc_furnace`、`mc_chest`、`mc_fish`、`mc_trade`、`mc_prep`、`mc_station`、`mc_attack`、`mc_shoot`。按游戏版本、身份和分工逐项启用。 |
| 自主与配置 | `mc_task`、`mc_auto`、`mc_config`。对应目标/任务、何时醒来，以及本局行为配置。 |
| 特殊权限与辅助 | `mc_give`、`mc_command`、`mc_admin_config`、`mc_kit_memory`、`mc_kit_image`、`mc_kit_express`。分别是创造取物、服务端命令、管理配置、记忆、图片加工和发布；图片与发布可由管理中心承接。 |

合计 41 个静态注册名。其中根入口 35 个，扩展另注册 6 个。说明文字也提到 `mc_skin`；该版本的同名扩展文件提供 PNG 解析、缓存和皮肤采样，核对注册时没有把它另算成第 42 个模型工具。[上游工具表][mc-list] · [皮肤实现][skin]

**DeepClef 的 20 个模型工具。** 读取类有 `altoclef_status`、`altoclef_inventory`、`altoclef_list_resources`、`altoclef_capabilities`、`altoclef_screenshot`、`altoclef_look`、`altoclef_entities`、`altoclef_block_at`、`altoclef_equipment`；协作/控制类有 `altoclef_chat`、`altoclef_stop`、`altoclef_execute`；游戏动作有 `altoclef_walk`、`altoclef_break`、`altoclef_place`、`altoclef_boat`、`altoclef_swim`、`altoclef_elytra`、`altoclef_smithing`、`altoclef_combat`。`execute` 还能调底层任务库，所以 20 是模型入口数，功能量还包括组合任务。[工具定义][dc-tools]

**尖塔两项目的跨游戏能力。** 可迁移的有会话状态、全量/增量快照、等待变化、待决项与可选动作、记录战术要求、回复归属、截图、开始/继续/暂停/恢复、自动推进、单一控制者、兜底和交还。出牌、弃牌、用药、领取奖励、事件选项与地图投票提供的是“由游戏列合法选项，按最新状态验证，回传结果”的范例。在 MC 中对应实体/方块候选、可走目的地或可执行配方。房间托管、昵称、存档、退局各自对应联机、角色形象和生命周期，需按 MC 自身权限实现。[插件工具][sts-tools] · [游戏端协议][sts-bridge] · [决策校验][sts-decisions]

| 尖塔侧接口 | 在 MC 中对应的设计问题 |
| --- | --- |
| `sts2_status`、`sts2_state`、`sts2_wait`、`sts2_read`、`sts2_note`、`sts2_screenshot` | 先取摘要，再读取细节或等有意义的变化；让重要约定和视觉证据进入下一次决策。 |
| `sts2_chat_poll`、`sts2_chat_send`、`sts2_set_nickname`、`sts2_decide` | 分开待答原话、发言身份、待决选项和决定回执；私聊、行动和改昵称各自有独立权限。 |
| `sts2_takeover`、`sts2_host`、`sts2_start_run`、`sts2_continue_run`、`sts2_pause`、`sts2_resume`、`sts2_save`、`sts2_quit_to_menu`、`sts2_abandon`、`sts2_autoplay`、`sts2_handoff` | 分清入局、接管邀请、暂离、保存、主动执行、归还和放弃任务；需要恢复时重新校验当前世界和玩家。 |
| `sts2_play_card`、`sts2_play_cards`、`sts2_claim_rewards`、`sts2_use_potion`、`sts2_discard_potion`、`sts2_end_turn`、`sts2_choose`、`sts2_select_cards`、`sts2_proceed` | 单一动作、批次、不可逆消费、等待他人和多选约束各有不同回执。对应 MC 的放置、材料清单、补给、分工确认和计划推进。 |

## 河边试玩后的能力扩展示例

1. **先把一次交谈做对。** 定消息号、任务号和动作号的关系，确定发言由谁投递、迟到如何结算。复现“三组同义双发”，再向进行中的同一次采集发送不同原话，评议她理解、回应和实际行动之间的关系。
2. **让一个目标在没有新聊天时仍然成立。** 商定闲时自主范围，接通目标、进展、完成条件和事件唤醒。以“备两块泥土交给我”为第一条玩家可见的闭环，贯通采、捡、会合、丢出和结果确认。
3. **用同一目标验收受困恢复。** 布置有出口的坑、可自行开阶的坑、必须拆玩家建筑才有路的坑。分别验证自行走出、在范围内改地形、带依据回问。记录目标高度、实际高度和用过的办法。
4. **按共同活动扩充。** 下一条可以是“一起盖小屋”，自然引入箱子、配方、工具、放置与蓝图。生存、远行、生产和游戏战斗按上表逐项评审。每项补齐输入、可用条件、资源冲突、成功/部分/未知回执、取消以及独立验收，再宣称对玩家可用。

待对齐的核心产品选择是**未分工时可以主动做到哪一步**。建议默认允许观察、非破坏性陪伴和主动提议；共同分工成立后，在范围内自行行动。玩家也可以明确授予更宽的自主范围。这个选择决定空闲唤醒后给她哪些工具和约定，不能只靠调整模型调用频率解决。

[mc-index]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/index.js
[mc-list]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/插件说明.md#L229
[act]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/index.js#L4285
[move]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/index.js#L4144
[dig]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/index.js#L4687
[build]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/index.js#L4632
[map]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/index.js#L3846
[toss]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/inventory.mjs#L593
[inventory]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/inventory.mjs
[tasks]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/task-runner.mjs
[settle]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/task-report.mjs#L64
[watch]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/watchdog.mjs
[auto]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_auto.mjs#L65
[sched]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/action-scheduler.mjs#L63
[movement]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/movement.mjs
[water]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/water-recovery.mjs
[waternav]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/water-navigation.mjs
[danger]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/danger.mjs
[names]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/generic-names.mjs
[observe]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_observe.mjs
[drops]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_drops.mjs
[chest]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_chest.mjs
[craft]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_craft.mjs
[furnace]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_furnace.mjs
[skin]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/extensions/mc_skin.mjs
[memory]: https://github.com/blueziii/dsh-mc-agent/blob/af7ea21d203e3d975b3e7a99152a35561e813e1a/src/memory.mjs
[dc-chat]: https://github.com/Cymew/DeepClef/blob/253f14aa83801ec3b2c6e1332da8e048e33e5790/src/main/java/adris/altoclef/util/DeepSeekAgent.java#L200
[dc-tools]: https://github.com/Cymew/DeepClef/blob/253f14aa83801ec3b2c6e1332da8e048e33e5790/src/main/java/adris/altoclef/util/DeepSeekAgent.java#L1409
[dc-give]: https://github.com/Cymew/DeepClef/blob/253f14aa83801ec3b2c6e1332da8e048e33e5790/src/main/java/adris/altoclef/tasks/entity/GiveItemToPlayerTask.java
[dc-progress]: https://github.com/Cymew/DeepClef/blob/253f14aa83801ec3b2c6e1332da8e048e33e5790/src/main/java/adris/altoclef/util/progresscheck/MovementProgressChecker.java
[dc-survival]: https://github.com/Cymew/DeepClef/blob/253f14aa83801ec3b2c6e1332da8e048e33e5790/src/main/java/adris/altoclef/chains/WorldSurvivalChain.java
[sts-plugin]: https://github.com/YuWan886/dsh-sts2/blob/3a2954127ddf7e49cdbb24952c7b6d41b468ece6/index.js#L466
[sts-tools]: https://github.com/YuWan886/dsh-sts2/blob/3a2954127ddf7e49cdbb24952c7b6d41b468ece6/index.js#L752
[sts-bridge]: https://github.com/YuWan886/STS2-AiTeammate/blob/d598ac63d4ac21cf8ba82b478a1307eb9c923053/docs/dsh-agent-bridge.md
[sts-decisions]: https://github.com/YuWan886/STS2-AiTeammate/blob/d598ac63d4ac21cf8ba82b478a1307eb9c923053/docs/decision-engine.md
[sts-reply]: https://github.com/YuWan886/STS2-AiTeammate/blob/d598ac63d4ac21cf8ba82b478a1307eb9c923053/AiTeammateCode/Agent/AgentChatRelay.cs#L295
