# 大肥鱼运行说明

支持 Windows 10／11 x64。保留解压目录中的程序、运行库 DLL、配置和示例目录，直接运行 `dafeiyu.exe` 即可使用内置 DeepSeek 配置。程序无需安装插件即可问答。

## 模型与启动

在 Windows 用户或系统环境变量中配置对应服务的凭据，更新后新开终端：DeepSeek 使用 `DEEPSEEK_API_KEY`，Qwen 使用 `DASHSCOPE_API_KEY`。配置文件只保存环境变量名称，不保存密钥。运行期间不自动读取其他进程刚修改的环境。

在运行目录中执行：

```powershell
.\dafeiyu.exe
.\dafeiyu.exe --chat .\config\qwen.example.json
.\dafeiyu.exe --check-config .\config\deepseek.example.json
.\dafeiyu.exe --complete .\config\qwen.example.json .\prompt.txt
```

`prompt.txt` 使用 UTF-8。配置检查只检查文件格式，真实服务可用性由实际请求验证。可以复制示例配置，修改服务地址、模型、凭据环境变量名称或超时，再用 `--chat` 指定副本。

不带参数时，程序从自身所在目录寻找默认配置；从其他工作目录启动也不影响默认资源定位。明确给出的相对配置、包或数据路径以当前工作目录为准。

## 会话操作

本体默认采用内置的「大肥鱼」人格。可在本体用户数据目录放置 UTF-8 的 `persona.json`，重启后直接对话、房间点名和 MC 陪玩使用同一档案。终端默认目录是 `%LOCALAPPDATA%/DaFeiYu`，也可用 `--data` 指定。MC 体验入口的该目录为 `RUN_DIR/user`。档案有三个字段。`id` 使用不超过 32 字节的小写字母、数字、点、连字符或下划线，`revision` 是正整数，`instructions` 是不超过 2048 字节的人格正文。修改正文时可递增修订号，Trace 同时记录正文摘要。工具权限和游戏目标由各自入口和插件决定。

```json
{
  "id": "dafeiyu.home",
  "revision": 1,
  "instructions": "你叫大肥鱼。平常简短自然地说话，和对方一起做事。"
}
```

使用 MC 独立角色时，正式入口为 `dafeiyu --mc-partner CONFIG PACKAGE RUN_DIR`。需要已准备的 MC 包、游戏连接和本局运行目录。源码仓库中的[插件说明](../plugins/mc/README.md)和[本机试玩说明](../tests/mc-companion/README.md#首次真人体验)提供完整步骤。

### Trace 审阅与回放

MC 体验宿主在 `RUN_DIR/trace/events.jsonl` 保存模型请求、原始响应、改写结果和运行退出摘要。重复使用运行目录时，后续日志保存在新的 `trace-N` 子目录。启动输出中的 `DFY_TRACE` 给出实际文件位置。普通终端会话可用 `--chat CONFIG --trace DIR` 开启同一格式。文件包含本局对话与工具正文。模型记录用 `thinking` 标记模式，DeepSeek 返回的 `reasoningContent` 随本地记录保存，用于还原同一工具链的实际上下文。玩家可见输出仍取正文和实际发言回执。

Minecraft 伙伴可在浏览器按回合核对模型调用、计划、游戏回执和实际发言。 [MC Trace 查看器](mc-trace-viewer.md)

在 `model.prepared` 中，`personaProfile` 标识本体人格，`promptTemplate` 标识额外场景规则的版本或摘要。`input.messages` 与 `input.tools` 保留本轮实际送模的正文和工具定义，可与 `model.exited`、`tool.exited` 及游戏回执交叉核对。比较体验时分别计算收件到首次实际发言、收件到动作接纳、接纳到游戏退出，并单列动作接纳后无实效或仅部分完成的情况。

工具记录区分候选、接纳和退出，保存原调用编号以及有效参数和实际结果。MC 信箱记录保留游戏代次、消息序号和原工作编号。使用 `invocation` 对照 Agent 工具回执，使用 `bridgeWork` 对照 `game-actions.jsonl` 的 `work`。业务成功、实际退出和发送回执各自取证。

陪玩移动接纳后用 `mc.partner.motion` 记录开始及状态变化，用同一工作编号查对应的 `mc.partner.action` 终态。后续模型上下文的 `activeMotion` 记录正在运行的动作。查看墙体时，用 `sceneAtMessage` 核对发话现场，用 `task.target` 核对所选方格，再用放置回执的 `sourceMessage` 和 `taskRevision` 对照原任务。

用 `mc.partner.question` 核对局内自然追问的实际呈现和到期。呈现记录带 `bridgeWork`，到期记录沿用原 `item`，下一条玩家原话送模时可看到 `pendingQuestion.status=expired`。同回合再次请求发言时，工具会返回 `already_requested`。

用同一组 `eventDomain` 和 `event` 核对 `event.captured`、`event.decoded`、`event.dispatched` 和 `ask.opened`。随后用 `ask` 关联 `ask.presented`、`ask.answer` 和 `ask.transition`。游戏渠道用呈现记录里的 `body.bridgeWork` 核对游戏发言。最后用 `mailbox` 和 `message` 核对 `interaction.delivered` 的入箱与 `interaction.acknowledged` 的收件确认。子 Agent 的后续 `model.prepared` 记录收到的 `questionResult` 和原目标。

主 Agent 路径中的 `mail.dispatched` 记录真正收取信件的 `session` 和 `run`。该回合的 `model.prepared` 显示本轮工具及实参范围。`reply.intent` 将 `mail_reply` 的 `callId` 与原事项身份、信件序号和运行身份关联。回合退出时没有意图会记录 `reply.review_required`，可用 `/mail review` 主动取回待复审结果。

使用 `dafeiyu --review-trace FILE` 列出运行及关联的玩家消息，附加 `SESSION RUN` 查看原始记录和同一子请求的游戏回执。读取时校验文件版本和连续序号。未正常关停时仍可读取已落完整的行，并提示记录缺尾。

使用 `dafeiyu --replay-trace FILE SESSION RUN` 离线重跑一轮已退出的直接运行。正式 Agent 协调器读取起始上下文，用固定模型响应、已记录的响应和参数改写以及工具结果重跑，逐次核对送模消息、工具定义、调用、回执和最终结果。工具回执可包含 MC 业务确认后的游戏实效。若新游戏消息导致原回合的下一次送模被拒，回放会按原记录重现停止。执行只读取本地文件和内存替身。取消、Agent 宿主审批、规划、观察绑定和最终答复 Hook 改写仍通过原日志审阅。

### 交互命令

| 输入 | 行为 |
|---|---|
| 普通文字 | 空闲时开始一轮问答 |
| `/mail wake auto`、`/mail wake manual` | 切换插件信箱的自动唤醒 |
| `/mail review` | 主会话空闲时主动审查一个信箱队首 |
| `/ask answer 编号 "答案"` | 向已在终端实际呈现的 ASK 提交原答复 |
| `/run 包ID/工具ID "目标"` | 空闲时把一个已加载的只读工具明确授予本轮，返回运行标识 |
| `/run-confirmed 包ID/工具ID "目标"` | 授予本轮一个指定工具，具体状态变更等待人工确认 |
| `/confirmations` | 查询本轮待确认动作、参数、请求号和修订 |
| `/approve 请求号 修订`、`/deny 请求号 修订` | 登记对原动作的批准或拒绝 |
| `/task 读工具ID 改工具ID 期望状态 "目标" [--request 请求号]` | 启动同一实例内的单步读、改、读任务，由宿主核对实际状态 |
| `/task-status 请求号` | 在同一宿主进程内查回原接纳和仍保留的运行状态 |
| `/verification` | 查看当前或最近一轮验证任务的证据判断 |
| `/diagnostic export "文件"` | 将当前或最近运行的无正文诊断保存为新文件 |
| `/status` | 查看当前回合；空闲时查看最近一轮的状态、耗时和通知接纳结果 |
| `/cancel` | 取消当前问答，保留此前已提交上下文 |
| `/clear` | 空闲时清空当前上下文 |
| `/exit` | 请求取消当前问答，等待实际调用和插件停止后退出 |

网络等待期间仍可处理本地管理命令。新的普通问答会明确报告忙碌，不会进入隐藏队列。取消先确定问答结果，已经送出的远程调用仍须等待系统调用退出；取消不承诺撤销服务端已执行的请求。默认不自动重发失败请求。

普通问答在完整成功后成对提交。工具运行将已结算的调用和结果成对保留，后续回答失败或取消时仍可关联已执行的工作。`/run` 只接纳已加载实例中的具体只读工具，包含空白的目标用双引号括起。普通对话可以带入一封经审查的待收信件，若同轮信件关联一项 ASK，仅授予处理原事项所需的工具。当前上下文正文总量上限为 1 MiB，达到限制后可 `/clear` 开始新的上下文。会话保留在进程内。重定向输入正常结束时，程序会等待已经接纳的回答完成。

批准前核对 `/confirmations` 显示的工具及参数。回执表示已登记决定，实际执行和退出继续通过 `/status` 查询。最近 64 条决定的值回执在当前进程内可重复查询，动作退出后也可重发原命令。输入结束后，等待人工确认的运行协作取消。

`/status` 不调用模型、不推进或重试业务。`Question` 是问答结果，`Model call` 是后台调用事实，`exit=confirmed` 才表示宿主确认调用退出；取消期间可能同时显示 `Question=cancelled`、`Model call=running` 和 `exit=waiting`。时间均为单调时钟相对模型提交的毫秒偏移；后台可能在宿主完成回合接纳前开始，横线表示该阶段尚未发生，最终耗时不会随再次查询增长。

通知中的 `accepted (queued only)` 只表示通道接纳，不能证明插件已经处理。开始、终态或清空通知未接纳会独立提示一次，不改变问答或清空结果，也不会自动补发。`/status` 保留开始和终态回执及成功接纳的事件身份，便于关联会话／回合／模型请求。终端只保留最近一轮的固定大小快照，`/clear` 不清除该诊断；进程结束后丢弃。诊断不包含提示词、回答、凭据或异常正文。

导出需要明确的新文件路径，并拒绝覆盖。使用 `dafeiyu.exe --review-diagnostic "文件"` 离线查看历史缺口、实际退出、效果计数及公开版本。只读回顾无需启动模型或加载插件。

构建标签随二进制记录编译器、平台和配置。构建者核对源状态后可在配置 CMake 时用 `-DDFY_APPLICATION_COMMIT=完整提交号` 声明源码版本。未提供时显示未知，运行时从二进制读取标签。

## 插件管理

插件是同进程可信原生 DLL。默认包目录为程序目录下的 `plugins`；安装和发现都不会自动运行插件。分发目录的 `examples/plugins/example.observer` 是独立观察示例，默认不装入包目录。

从运行目录启动终端后，可以安装并读取独立的状态面板示例。面板模式随当前实例存活，初始为 `idle`。

```text
/plugins install "examples/plugins/example.status"
/plugins load example.status
/run example.status/read "读取状态面板的当前模式"
```

下一轮可交代大肥鱼切换到专注模式并验证。

```text
/task example.status/read example.status/set_focus focus "把状态面板切换到专注模式"
```

首次读取完成后，用 `/confirmations` 查看待审调用，按显示的请求号和修订输入 `/approve` 或 `/deny`。批准后，`/verification` 显示重读结果是否与期望值一致。该路径要求同一插件实例的两个无参工具、三次单独的顺序调用和原变更的实际退出。目标为空、没有初读、读取不匹配或证据丢失时不能结算为成功。可将 `set_focus focus` 换为 `set_idle idle` 切回空闲。

需要重发或重新接入时，用 `--request 1` 开始给任务编号，后续的新编号依次递增。相同请求号及相同参数返回原运行，参数冲突会被拒绝。`/task-status 1` 可查原接纳。当最近运行已换成其他任务时，旧请求仍可在最近 16 项回执内查到原身份，详细状态可能已超出保留范围。

以下安装命令中的 `D:\DaFeiYu` 请替换为实际运行目录：

```text
/plugins list
/plugins install "D:\DaFeiYu\examples\plugins\example.observer"
/plugins refresh
/plugins load example.observer
/plugins unload example.observer
/plugins remove example.observer
```

默认用户设置位于 `%LOCALAPPDATA%\DaFeiYu\plugins\<id>\settings.json`，为 JSON 对象。包内默认设置和用户设置按顶层键覆盖。移除包会保留该用户设置，不把它复制进其他包。

需要独立运行环境时指定目录：

```powershell
.\dafeiyu.exe --chat .\config\deepseek.example.json --plugins "D:\应用数据\插件" --data "D:\应用数据\用户设置"
```

本地安装只接受普通本地目录，拒绝链接、重解析点、已有 ID 和覆盖更新；默认上限为 4096 个子项、512 MiB 文件数据及 32 层目录。完整检查和暂存复核后才发布，坏包单独报告，不阻止本体问答。

卸载明确区分完成、正在停止、忙碌、仍被使用和失败保留。等待下一事件的观察插件可正常停止；仍在执行的调用或仍被其他实例使用的代码不会被强行释放。宿主建立了运行时依赖时，可以使用 `/plugins unload <id> --linked` 请求先停止使用者。

`remove` 先尝试卸载，未完成时保持包文件。已有移除残留先分批清理；仍有未处理批次或清理失败时，当前包保持可见并明确显示“Package kept”。旧残留结束后，当前包先退出发现目录，再清理文件。只读或权限问题会显示具体残留路径；修复后再次执行相同 `/plugins remove <id>` 继续。不要把“正在停止”或“清理未完成”当作已经全部释放。

## 分发内容

| 位置 | 内容 |
|---|---|
| `dafeiyu.exe`、同目录运行库 DLL | Release 程序及编译器提供的运行库 |
| `config` | 不含密钥的 DeepSeek／Qwen 示例 |
| `plugins` | 用户明确安装的包，初始为空 |
| `examples/plugins/example.observer` | 可安装的观察示例包 |
| `examples/plugins/example.status` | 可安装的状态面板及读写工具 |
| `sdk` | C ABI 头文件、开发说明及独立可构建示例 |
| `licenses`、`THIRD_PARTY.md` | 第三方版权和许可证说明 |

程序运行不依赖源码目录、`Old` 或测试目录。SDK 构建方式见 `sdk/README.md`。Windows 系统组件提供 WinHTTP 和 UCRT；分发附带的是编译器提供的 Release 运行库，不包含开发用 Debug 运行库。
