# MC伙伴使用说明

面向玩家和服主。个人试玩可从本机隔离体验服开始；已有服务器可独立部署桥接并配对自己的玩家身份。正式游戏回合使用配置的模型。固定响应开发回归见[MC 本地验收说明](MC本地验收说明.md)。

**目录**　[首次真人体验](#首次真人体验) · [用浏览器进服](#用浏览器进服) · [连接自己的服务器](#连接自己的服务器) · [伙伴能力与工具](#伙伴能力与工具) · [本体委托与确认](#本体委托与确认) · [固定圆石场景](#固定圆石场景) · [回顾一项委托](#回顾一项委托)

## 首次真人体验

按[隔离场地准备](MC本地验收说明.md#准备隔离场地)配置本机 Paper 并构建[服务端观察插件](MC本地验收说明.md#两种圆石来源)后，用 `windows-tests` 预设构建 Debug 目标 `dafeiyu-cli`、`dfy_mc_companion`。在项目根目录通过 Windows PowerShell 5.1 或 PowerShell 7 启动，`-BuildDirectory` 指向实际的构建目录。

```powershell
.\tests\mc-companion\start-playtest.ps1 -FixtureRoot 'D:\DaFeiYu-MC-Test' -BuildDirectory '.\build\rebuild\windows-tests'
```

正式入口使用配置的模型，从绑定玩家的在场事实或普通聊天发起回合。模型配置优先取 `-ModelConfig` 指定的 JSON 文件或 `DFY_MC_MODEL_CONFIG` 指向的路径，其次取体验根目录的 `partner-model.json`，缺省使用应用随附的 DeepSeek 配置。游戏连接与模型凭据分开保存。用 `api_key_env` 指定的环境变量向宿主提供凭据。

体验启动脚本及浏览器玩家启动脚本均可加 `-PlayerThinking low`。DeepSeek 玩家回合启用低强度思考，`off` 关闭，默认 `inherit` 使用宿主设置。独立部署在插件的 `settings.json` 中设置 `"playerThinking":"low"`。查看 `model.prepared` 的 `thinking` 和 `reasoningEffort` 可核对实际送模配置。玩家等待时间从发话计到收到有用反馈。

1. 用 Java 1.21.4 直接连接 `127.0.0.1:25580`，或使用[浏览器入口](#用浏览器进服)。两种方式都用独立玩家身份见到大肥鱼；原版客户端接受本服的[角色资源包](第三方软件与素材许可.md#mc-体验素材)。
2. 进场后用普通聊天与伙伴交互。本机离线服上的 `/msg` 私聊缺少可核对的发送者身份。
3. 离服时，体验宿主给离场事件留出处理时间，再收尾。启动打印的 `DFY_TRACE` 指向模型与工具日志；同一宿主目录的 `board.txt` 记录实际发言，`game-actions.jsonl` 记录游戏执行结果。无人进服时，在宿主目录创建空文件 `stop` 可正常结束。

体验服使用独立的本机身份 `DfyCompanion`。1.21.4 客户端按该离线 UUID 的 Java 哈希在 18 个默认外观中选中 slim/zuri。体验包只覆盖这一格，游戏协议的玩家身份和权限仍取原 UUID。恰好分在同一默认格的其他无自定义皮肤玩家也会看到相同外观。进服时接受服务器资源包即可使用，离服即卸下。使用正式账号时可将同一 PNG 选为细臂皮肤，无需这份体验包。

如需对照一次对话、动作与实收，保留控制台显示的 `DFY_TRACE` 以及运行目录。

## 用浏览器进服

```powershell
.\tests\mc-companion\browser-player\start.ps1 -FixtureRoot 'D:\DaFeiYu-MC-Test' -BuildDirectory '.\build\rebuild\windows-tests' -Configuration Release
```

命令用指定构建启动 Paper 与本机连接服务，以独立浏览器资料打开 WebMC 2.4.0。加 `-PrepareOnly` 可以只准备服务。当前体验服的构建和配置吻合时可直接复用。服务已启动时，打开入口并点击“进入河边体验”。[浏览器玩家入口](http://127.0.0.1:25582/) · [固定发布版本](https://github.com/zardoy/minecraft-web-client/releases/tag/v2.4.0)

你使用独立的本地玩家名 `DfyVisitor`。点击游戏画面后用 WASD 走动、鼠标转向，按 T 输入原话并回车。是否向你的玩家身份发言或继续行动，由大肥鱼结合上下文决定。浏览器按大肥鱼的离线 UUID 为其玩家模型加载已准备的皮肤。关闭游戏页面将离服，重开一轮可再运行启动脚本。此入口只连接本机 `127.0.0.1:25580` 的离线体验服。

进入后可在河边使用普通聊天。体验目录的 `mwc/*-paper.log` 记录本局证据目录和 `DFY_TRACE`；同一宿主目录下用 `trace/events.jsonl` 核对消息和工具，使用 `game-actions.jsonl` 核对游戏实际退出和效果。代理状态可从本机健康页查看。[代理健康页](http://127.0.0.1:25582/health)

需要 Release 的 `dafeiyu-cli` 和 `dfy_mc_companion`，或将命令中的 `-Configuration` 改为已构建的 `Debug`。

## 连接自己的服务器

在 D 盘等指定运行目录准备 Node 22 或以上环境，并按源码根目录下的 `plugins/mc/bridge/package-lock.json` 安装依赖。若将依赖装在源码目录外，设置 `NODE_PATH` 指向对应的 `node_modules`。为自己的连接在该运行目录创建 JSON 配置，例如隔离的本机测试服使用：

```json
{"channel":"companion","host":"127.0.0.1","port":25579,"username":"DfyCompanion","auth":"offline"}
```

从项目根运行 `node plugins/mc/bridge/start.cjs <配置文件>`。个人服务器连接可用 `auth: "microsoft"` 选择大肥鱼自己的玩家账号，登录缓存位于配置文件旁的 `profiles` 目录。开发验收使用本机测试身份。通道短名必须与 DLL 的独立设置一致，默认值为 `companion`。

要在适配器接收玩家原话，可在本地连接配置中给出 `owner`（玩家 UUID）和 `bridgeKey`（随机 32 字节的小写十六进制）。私有 `chat.read` 查询还须绑定该键和角色当前世界；普通世界查询不返回聊天。适配器只接受源于该 UUID 的游戏协议消息，线上要求通过签名校验。系统广播和角色自己的文字不构成玩家指令。保留窗口外重读会返回缺口，每批有消息数与字节上限。

构建目标 `dfy_mc_companion` 会在对应配置下生成 `mc.companion` 包目录。使用终端的 `/plugins install "<包目录>"`、`/plugins load mc.companion` 显式装载，或由共享应用服务接入。普通模型问答保持独立。定向验收见 [MC 探针](MC本地验收说明.md)。

在用户数据目录的 `plugins/mc.companion/settings.json` 保存与适配器相同的 `channel` 和 `bridgeKey`。用 `dafeiyu-cli --chat <模型配置>` 打开终端，加载插件后输入 `/companion open mc.companion`。用 `/companion status` 查看执行入口，`partner ready` 表示当前实例已取得角色控制权。每次技能调用绑定原世界和事件号。玩家回合可以通过 `reply` 或可交付的最终文本发言，两者共用游戏投递和回执。委托回合的最终文本返回委托方，内部结算保持静默。本机离线服请用普通聊天，`/tell` 的协议数据缺少可验证的玩家 UUID。[控制权](../../plugins/mc/bridge/controller.cjs) · [回合结算](../../plugins/mc/src/partner_loop.cpp)

## 伙伴能力与工具

默认伙伴模式按游戏游标保存原话、局内目标及最近实际回执。插件优先使用 `dfy.agent@3` 的独立子会话，沿用本体所选供应商和模型。`playerThinking` 控制面向玩家的回合，设为 `inherit` 时可兼容第 2、1 版子会话接口。玩家消息、已核实的现场变化和任务状态都能唤醒下一轮。`mc.companion/world` 返回角色 UUID、出生代次、维度、坐标、背包圆石总数及最近游戏信号的年龄。世界撤销时可读到 `null`，连接失败时工具返回失败。[子会话协商与回合装配](../../plugins/mc/src/partner_loop.cpp)

地标使用 `"places":{"hillside":{"label":"山脚","dimension":"overworld","x":12,"y":-60,"z":0}}` 的形状。观察工具提供世界、玩家和背包，并可按坐标网格查看玩家或角色附近已加载的方块。移动可用地标、玩家或附近坐标。采集接受游戏注册表中的方块名和数量，也可锁定一格坐标。放置使用背包里的实际建材，指定一个空格，门的坐标取下半格。回执记录核实的目标格和材料读数。

普通玩家回合可直接观察、发言、跟随、挖指定单格或按库存取材。`skills` 列出十二个分类以及各入口已支持的动作，一次可选至多三类，在下一轮展开对应契约。物品栏类支持按原观察拾取指定整堆、按约定数量向配对玩家投物，合成类支持按配方把原木加工为木板。[完整分类与契约](../../plugins/mc/src/partner_skills.hpp)

## 本体委托与确认

`/companion consult "目标或问题"` 给主 Agent 一轮与当前伙伴通信的权限。主 Agent 可以委托、询问当前任务的进度，或读取已收到的回执。信箱接纳后可用 `/companion board 会话号 [任务号]` 查原请求及后来回执。插件独立处理委托，源自本体的请求会使用受限工具范围，采集需要玩家另行批准。进度查询使用已核实的游戏采样，可以在子会话运行时返回。

游戏动作可在候选参数中请求确认。`approval=game` 在游戏向配对玩家询问，`approval=main` 经通用 ASK 交由主 Agent 选择终端或游戏渠道。终端用 `/ask answer 编号 approve` 或 `/ask answer 编号 deny`，游戏内通过普通聊天回复 `确认 编号` 或 `拒绝 编号`。首个符合身份和时效的答复决定原候选结果，当前确认期限为 25 秒。批准后仍需依据原世界、玩家和最新游标通过动作校验。本体委托的采集强制进入同一确认流程。

`ask_player` 默认在局内提问。同批原话中可用 `sourceMessage` 指定要澄清的那条消息。玩家按选项或用普通聊天补充，到期后插件保留原事项和等待范围。显式选择 `direct` 或 `main` 才进入本体 ASK，游戏渠道会提示玩家以 `答复 编号 内容` 回答。终端用 `/mail wake manual` 切换到手动收件，再用 `/mail review` 处理。`/mail wake auto` 恢复到件唤醒空闲主会话。呈现、答复与动作实效分别记录。[局内待答](../../plugins/mc/src/partner_question.cpp) · [宿主与子会话职责](../设计/Agent循环与上下文设计.md)

## 固定圆石场景

旧采石流程使用适配器设置 `"mode":"scenario"` 和终端命令 `/companion scenario mc.companion`。下面的查看、显式审批和自采归属判定用于这一场景。

要使用采集返程，另配置本世界的 `riverside` 会合地标。当游戏角色私聊询问时可回复“采 16 块带回来”，也可在受信终端用 `/companion approve 会话号 问题号 修订 请求号 数量` 成立同一委托。包按原消息绑定接单，角色使用背包中的合适镐，在原地标附近只挖看得见、够得着的圆石，按授权上限执行并回会合区。当前接入支持明确数量 1～64。

对于本地可控服务器，可在**适配器**配置中加 `observer`，其 `directory` 是只读观察端输出目录的绝对路径，`world` 是该服务器世界 UUID，`dimension` 是对应游戏维度。观察记录只用于判断实际拾取与带回。未配置或证据断档时仍可移动、采集，管理中心或终端只能据实展示已知库存和归属范围。查看、采掘和返程的验收分开保存。[本地验收说明](MC本地验收说明.md)

终端的 `/companion where 会话号` 可直接采用足够新的进展，或说明最后一次确认的位置并开启读取。`/companion where 会话号 meet` 用于现场会合，会返回新读取的编号，已有在途查询可以复用。读取期间游戏角色继续当前动作，查询本身不调用模型。

## 回顾一项委托

“停，我来”与其他玩家原话使用同一入口。宿主操作者可通过终端 `/companion stop 会话号` 显式请求回收。回收状态根据实际退出更新。

使用 `/companion board 会话号` 查看最近的玩家委托及可查任务号，使用 `/companion board 会话号 任务号` 回顾指定的查看。可从到岸回执沿采集委托、原问题和答复修订追到玩家原话与山脚观察。刚授权时显示“已授权，暂无投递回执”，投递后再显示游戏是否接单。G14 的介入与答复也按原任务关联。

共享应用的 `companion_board(session, task, after_message)` 在原宿主线程返回可信值副本。业务快照每次完整返回，游标只筛选该任务的主子消息，消息修订及缺口以整个会话的有界窗口为准。当前查看之外保留最近八项被接替的查看，超出范围时给出裁剪水位。查询不启动模型、刷新游戏或投递命令。

`dafeiyu-cli --mc-partner CONFIG PACKAGE RUN_DIR` 启动的独立宿主将计划板写到 `board.txt`，并保存 `game-actions.jsonl` 和 `DFY_TRACE` 指向的宿主日志。模型回合故障出现在计划板的系统状态中。[MC 轨迹查看器](MC轨迹查看器说明.md)
