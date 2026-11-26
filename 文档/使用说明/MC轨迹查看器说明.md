# MC轨迹查看器说明

供开发者或复盘玩家在本机浏览器核对 Minecraft 伙伴的宿主 Trace 与游戏回执。浏览器直接读取所选文件，复盘可以在游戏和模型服务已关闭时进行。

**目录**　[打开日志](#打开日志) · [从回合定位原始证据](#从回合定位原始证据) · [按原工作号核对游戏实效](#按原工作号核对游戏实效) · [验收路径](#验收路径)

## 打开日志

在项目的 `apps/desktop/web` 下使用与本体网页相同的 Node.js 24 工具链。新检出先运行 `npm ci`，然后构建并启动。

```powershell
npm run build:mc-trace
npm run start:mc-trace
```

用当前版 Edge 或 Chrome 打开 [本地查看器](http://127.0.0.1:4174/)。启动命令提供独立静态产物 `apps/desktop/web/dist-mc-trace`。主面板、Minecraft 网页和游戏代理使用各自的入口。

1. 选择 `DFY_TRACE` 写出的 `events.jsonl`。体验证据包中通常位于 `host/trace/events.jsonl`。
2. 对照实际挖掘、落块、背包或聊天时，附加同局的 `host/game-actions.jsonl`。也可以同时选择或拖入两份文件。
3. 写入中或非正常退出的日志可以打开。当前视图对应已读取的快照。日志继续增长时重新选择文件，或者优先打开已归档的证据包。

## 从回合定位原始证据

左侧按会话号与回合号列出 `mc.companion` 的宿主接纳。已出现可识别的 Minecraft 送模但缺少接纳记录时显示接纳待确认。即使正式接纳后无模型响应，回合仍可打开。搜索支持玩家原话、触发码和回合号。

| 触发分类 | 来自插件的回合原因 |
| --- | --- |
| 游戏输入 | `game.event`，同一次送模的多条事件留在一页，局内自然语言答复也可经同一入口 |
| 工作结算 | `motion.finished`、`plan.finished`、`task.ready` |
| 现场变化 | `follow.attention`、`scene.changed` |
| 追问与审批 | `question.result`、`approval.result` |
| 续行与能力 | `task.continue`、`skills.expanded` |
| 委托与复核 | `main.delegation` 或有原在途动作和新旧事件的复核回合 |

在中栏选择模型步骤与尝试。步骤卡片归组装配、提交、响应和可能的改写。悬停阶段标签可看相对回合时间。需要核对交错的接纳、工具和信箱时，进入“原始记录”查看文件的物理顺序。

- **实际送模**读取 `model.prepared`。可查场景事实、原有顺序的消息正文、工具目录及参数定义。同页内可对照上一次的消息、工具契约和 MC 事实字段。
- **本步宿主快照**在“实际送模”内显示快照号、原步骤、消息位置及状态项来源。展开状态项可看 `fact`、对象和原值，点击原行按钮可回到这一次装配。查看器按原行顶层的 `messageEvidence` 与 `input.messages` 对齐，只识别末条来源为 `host_status` 的 `user` 消息和其中的 v1 宿主尾注。玩家或工具传来的同文 JSON、旧日志及残缺尾注仍可在“模型消息”中查看原文。
- **响应与裁定**分开显示提交到开始、模型响应、完成到宿主读回。确认响应文本和工具提议后，继续核对工具接纳、退出、领域状态以及回合退出行的程序裁定。
- **MC 实效**按观测、计划与动作、追问与审批、聊天送达等用途筛选。可打开观测的视角和覆盖范围、逐格变化、合成与背包、原计划和后继回合。检查发言时以带送达状态的回执为据，宿主记录与游戏桥记录各自保留来源。
- **原始记录**按文件物理行序浏览，可按行号、事件类型或工作身份定位。点击原行锚点可读取和复制该行。“全局日志”可核对其他 Agent、外部事件以及无法关联到 MC 回合的领域记录。

宿主以自己的单调时钟记录模型阶段。游戏样本按记录中的游戏时钟呈现。部分旧文件没有完整回执或计时字段，缺少的数据显示为未知或未见记录。导入上限沿用宿主审阅的每文件 512 MiB、单行 16 MiB。损坏记录之前的有效前缀可查看。

## 按原工作号核对游戏实效

MC 信箱记录保留游戏代次、消息序号和原工作编号。使用 `invocation` 对照 Agent 工具回执，使用 `bridgeWork` 对照 `game-actions.jsonl` 的 `work`。业务成功、实际退出和发送回执各自取证。

陪玩移动接纳后用 `mc.partner.motion` 记录开始及状态变化，用同一工作编号查对应的 `mc.partner.action` 终态。后续模型上下文的 `activeMotion` 记录正在运行的动作。查看墙体时，用 `sceneAtMessage` 核对发话现场，用 `task.target` 核对所选方格，再用放置回执的 `sourceMessage` 和 `taskRevision` 对照原任务。

用 `mc.partner.question` 核对局内自然追问的实际呈现和到期。呈现记录带 `bridgeWork`，到期记录沿用原 `item`，下一条玩家原话送模时可看到 `pendingQuestion.status=expired`。同回合再次请求发言时，工具会返回 `already_requested`。

用同一组 `eventDomain` 和 `event` 核对 `event.captured`、`event.decoded`、`event.dispatched` 和 `ask.opened`。随后用 `ask` 关联 `ask.presented`、`ask.answer` 和 `ask.transition`。游戏渠道用呈现记录里的 `body.bridgeWork` 核对游戏发言。最后用 `mailbox` 和 `message` 核对 `interaction.delivered` 的入箱与 `interaction.acknowledged` 的收件确认。子 Agent 的后续 `model.prepared` 记录收到的 `questionResult` 和原目标。

## 验收路径

| 场景 | 在页面内核对 |
| --- | --- |
| 实机历史 `run-z65R6G` | 有 10 个 MC 回合和 23 次模型调用。会话 57 回合 7 的工具原始第 164 行同时保留宿主 `succeeded` 和 `invalid_plan`。 |
| 补墙追问及四格结算 | 从 `plan.finished` 跳到原计划和原追问，展开桥接回执，核对四个变化坐标和实际发言。 |
| 先部分受阻再续补 | 在两个原工作下分别看到各两格变化，首轮仍显示受阻及原原因。 |
| 多视角观测与合成 | 查看伙伴自身、玩家当下和消息发生时的视野范围及脚下格，再核对合成原料、产物和库存变化。 |
| 截尾或错误文件 | 查看首个未完成或损坏的行以及仍有效的记录。可在选择了不匹配回执时用“全局日志”检查未关联的记录。 |

开发时运行 `npm run test:mc-trace` 检查离线索引、来源关联及计时。可从 [宿主 Trace 审阅与回放](构建与运行说明.md#trace-审阅与回放) 继续复查终端的原生审阅和固定响应回放。
