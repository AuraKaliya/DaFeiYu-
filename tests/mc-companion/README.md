# Minecraft 本地协议探针

本页的固定输入、台词断言与动作顺序用于回查现有探针。对真人试玩做定性评审时，参照[外部试玩评审方法](../../docs/mc-companion-development-plan.md#外部试玩怎样评)，记录现场、可能的解释与实际效果。

## 首次真人体验

准备好 G02 运行目录和观察端后，构建 Debug 目标 `dafeiyu`、`dfy_mc_companion`。在项目根目录通过 Windows PowerShell 5.1 或 PowerShell 7 启动，`-BuildDirectory` 指向实际的构建目录。

```powershell
.\tests\mc-companion\start-playtest.ps1 -FixtureRoot 'D:\DaFeiYu-MC-Test' -BuildDirectory '.\build\windows-tests'
```

正式入口使用配置的模型，从绑定玩家的在场事实或普通聊天发起回合。模型配置优先取 `-ModelConfig` 指定的 JSON 文件或 `DFY_MC_MODEL_CONFIG` 指向的路径，其次取体验根目录的 `partner-model.json`，缺省使用应用随附的 DeepSeek 配置。游戏连接与模型凭据分开保存。用 `api_key_env` 指定的环境变量向宿主提供凭据。

体验启动脚本及浏览器玩家启动脚本均可加 `-PlayerThinking low`。DeepSeek 玩家回合启用低强度思考，`off` 关闭，默认 `inherit` 使用宿主设置。独立部署在插件的 `settings.json` 中设置 `"playerThinking":"low"`。查看 `model.prepared` 的 `thinking` 和 `reasoningEffort` 可核对实际送模配置。玩家等待时间从发话计到收到有用反馈。

固定响应的两处开口场景可在 `playtest.cjs` 的 `--gap-group-probe` 后加 `low` 或 `off`。复用同一构建和场地分别运行，两局的 `player-feedback.json` 都记录实际送模档位、原消息、投递工作和玩家端首反馈耗时。

明确范围的两局还从首次送模摘要取出对应墙面的开口尺寸与泥土库存。在 `player-feedback.json` 中，`candidates` 关联原候选编号，`materials` 给出所需、已有及短缺数量。可将候选展开的坐标与游戏实际放置回执逐格比较。

### 动作调度和目标绑定基线

在已有 Debug 构建上核对本地的 `run-L5G5AB`、`run-LdgI0H` 原始记录。

```powershell
node .\tests\mc-companion\replay-action-baseline.cjs .\build\windows-tests\bin\Debug\dafeiyu.exe D:\DaFeiYu-MC-Test\evidence
```

脚本核对原件指纹、玩家原话、采掘结果、取消前置步骤与未绑定放置。可独立重跑的模型回合使用原录制响应，依赖外部控制或决策截止的路径按原 Trace 断言。

「跟随中挖一格」与「取材后补四格」的后续基线可传入两局证据目录：

```powershell
node .\tests\mc-companion\replay-action-baseline.cjs .\build\windows-tests\bin\Debug\dafeiyu.exe <run-FX9QKV目录> <run-rg8RdZ目录>
```

分别核对原跟随工作号、挖掘后的实际入包、取材确认过期或观察被新事件中断，以及两局均无放置回执。可独立重跑的六轮使用录制模型响应。外部事件导致的回合退出以原 Trace 与游戏回执为准。

### 实机比较思考模式

DeepSeek 配置可加布尔字段 `"thinking": true`，默认关闭。使用同一模型、同一场地和同一句「把你旁边那个木门拆下来，装到我指的这个缺口」，两次独立开局，只改变思考模式。分别记录玩家发话到首次观察、首次有效动作、完工和发言的耗时，检查原洞口是否保持、询问是否有必要。再用边走边聊确认模型处理期间跟随仍沿用原工作号。先比较思考模式，后续提示改动使用相同的对照条件单独比较。

真实模型只用于人工试玩。自动验收使用固定响应和延迟，核对工具回传、任务续行和 Trace。读取 `model.submitted`、`model.exited` 和 `mc.partner.action` 对照实际调用与游戏反馈。

1. 用 Java 1.21.4 直接连接 `127.0.0.1:25580`，或使用下方浏览器入口。两种方式都用独立玩家身份见到大肥鱼；原版客户端接受本服的[角色资源包](appearance.md)。
2. 进场后用普通聊天按下方主题自由交互。本机离线服上的 `/msg` 私聊缺少可核对的发送者身份。
3. 离服时，体验宿主给离场事件留出处理时间，再收尾。启动打印的 `DFY_TRACE` 指向模型与工具日志；同一宿主目录的 `board.txt` 记录实际发言，`game-actions.jsonl` 记录游戏执行结果。无人进服时，在宿主目录创建空文件 `stop` 可正常结束。

**定向验收**另需构建 `dfy_mc_playtest`。加 `-PartnerProbe` 以可控模型记录收到的入场、离场和玩家原话；两次发出相同插话，分别选择保留或中止正在执行的移动。核对动作回执及玩家所见发言与模型选择，服务故障查宿主状态。输入和决定的证据在 `host/model-events.jsonl`，游戏实效在 `host/game-actions.jsonl`。加 `-Probe` 仍跑旧的圆石授权与实带 16 块场景。两种验收均使用真实 Paper 和独立协议玩家，模型使用替身。自然语言理解质量留给真人游玩。

### 本轮真人验收

通过浏览器进入 Release 体验场。河边出生点附近，`(3, -60, 0)` 和 `(4, -60, 0)` 有两块泥土，`(5, -60, 1)` 有一根橡木原木。待看到大肥鱼入场后开始。

1. **指向观察与合成。** 对准原木，先问「我指的这块是什么，你旁边又有什么？」再对准它说「把这根原木挖下来做成四块木板」。核对挖的是哪一格，原木是否先入包，合成是否实际消耗原木并产出四块木板，以及游戏里是否只报一次完工。库存和工作号可从 `host/game-actions.jsonl` 核对。
2. **就近取材。** 走到两块泥土附近，说「从旁边取一块泥土，在你旁边垫一步，位置你选」。核对空位、实际取材和落块；若提出更换材料，留意原本可以继续的动作是否受到影响。
3. **受阻与复跟。** 让大肥鱼跟随，到台沿拉开高度后说「你挖俩方块垫一下」。取材开始后插一句「我在上面等你」。核对两次实际入包、按原委托继续放置和踩上，以及有路时原跟随是否恢复。若玩家下来后已能汇合，核对答复是否依据最新距离和高差。随后说「不用跟了」，观察实际停步。
4. **四格续补。** 在同一面墙留一个 2×2 缺口和一处旁边的门洞。指向缺口委托用泥土补好，明确保留门洞。选定四格后、放到上方前，用石头临时堵上方一格。等部分完成并报受阻，挪掉石头，说「把刚才剩下的补上」。核对只补余下格，已补格和门洞保持原样。
5. **填两处开口。** 在同一墙面留下两处相隔的竖向开口。分别试「把这两处缺口都补了」和范围含糊的「把缺口补好」。如果有追问，用「两边都补上吧」接续。核对是否只填获准的两处，实际扣掉相应泥土，按游戏读回报告完成。
6. **负坐标脚下定格。** 在负坐标的平坦区域，让大肥鱼站在草方块上并发话「挖你脚下的泥土」。核对她是否确认准确格和实际材质、保护当前落脚点。若改为允许采草方块，确认她移到安全位置并复核同一格后再动手。

遇到偏差时留下原话、实际方块或库存变化和本局 `DFY_TRACE` 路径。固定响应下的对应场景见[固定响应验收场地](#固定响应验收场地)。

### 首轮实机：接续我已经搭的墙

由用户操作。先准备一段有两处缺口的墙，一处用来补，另一处留作门洞。准备约定的材料。

- 说「你跟着我走」，边走边问一句「到哪了」。观察她是否边走边答，以及转弯、停下再起步时是否继续跟随。
- 在第一处缺口发话「过来帮我把墙的这个缺口填上」，随后走到门洞旁。核对她检查的是第一处缺口。现场有歧义时，观察她是否先确认，再按实际方格执行。
- 核对补墙后的实际方块、门洞和材料余量，记录关键原话和大致时间。结束后用玩家消息号、子回合和工作号串查实际上下文、回执与发言。 [日志审阅与离线回放](../../docs/run-guide.md)

### 实机故障基线 `run-4VDQ2M`

使用 `19f070c` 构建的 `dafeiyu`，将本局原始证据目录交给下列命令。仓库记录校验指纹与复现断言，原始模型请求、回复和游戏记录保存在体验目录。

```powershell
node .\tests\mc-companion\replay-playtest-baseline.cjs .\build\windows-tests\bin\Debug\dafeiyu.exe D:\DaFeiYu-MC-Test\evidence\run-4VDQ2M
```

工具核对 `host/trace/events.jsonl`、`host/game-actions.jsonl` 和 `console.log` 的原件。正式离线回放器用录制的模型响应和工具回执重跑 14 轮，逐次比较送模上下文、工具目录、调用及结果。游戏侧效果由原动作回执与控制台交叉核对。

| 审查项 | 基线证据 |
| --- | --- |
| 任务状态 | 子回合 3 和 11 共三次提交 `revision=1`，实际上下文和回执为 `revision=0`，共同目标始终为空。 |
| 工具范围 | 子回合 7 用错 `ask_player` 名称，实际目录里有该能力。子回合 12 调用未入目录的 `move`；`place` 下一轮才进入目录，整局没有提出放置调用。 |
| 跟随和位置 | 子回合 6 显式停止仍为 `in_range` 的工作 `1.67`。同轮按消息 5 观察时，原玩家位置和现场读到的实时位置已经不同。 |
| 确认等待 | 一个问题实际呈现两次，分别带 `649` 和 `1`。随后采集确认 `802` 等待期间，17:36:28 的「我给你了啊」在原确认过期后才入子回合 11，17:36:45 收到发言。子回合 10 回放重现 `source_denied`。游戏没有收到采集或放置工作。 |

### 固定响应验收场地

固定探针在独立回环 Paper 布置场地，由独立协议玩家读取初始方块，并把读数与坐标保存为 `arena.json`。

| 场景 | 设置与输入 | 通过条件 |
| --- | --- | --- |
| 边走边聊 | `--follow-probe` 执行 `arena.json` 中的四个路点。首点启动跟随，第一次插话让固定模型延迟并预输入一步，第二次插话清除该步，最后停止跟随并换到固定站位。 | 两次发言均在原跟随实际退出前。停步和重新追赶沿用同一工作号。被清除的预输入没有游戏回执，替代移动在原跟随真实退出后发生。 |
| 两处缺口 | `--gap-probe` 在 A `(3, -59, -3)`、门洞 B `(7, -60, -3)` 至 `(7, -59, -3)` 之外，布置一扇现成木门。玩家指 A 发话后移到 B。 | 角色走到原话位置附近同高度的站位，读原话视角后再以玩家现位置观察。现成门的上下两格归为同一个对象。后一次观察不影响绑定 A。A 实际补上，B 两格仍空，现成门保持原状。 |
| 两处缺口且指向不明确 | `--gap-ambiguous-probe` 从同一原位置看向别处。局内自然澄清后，角色库存不足，发出采集确认。玩家提供泥土并说「我给你土了」。 | 普通问题只出现一次，玩家按原话作答。读回新库存、撤回原采集并在等待期内发言。A 补齐，B 两格仍为空。再次直接采集和补发旧编号均未触发采集。 |
| 为原话补齐两处开口 | `--gap-group-probe` 从能观察两处开口的原位置发话，收到局内追问后答「都补」。 | 独立玩家读回两处共四格，角色背包实际扣四块泥土。Trace 串起原消息、原候选、同一追问答复和一次实际完工发言。 |
| 两个都补／只补左边 | `--gap-both-probe` 与 `--gap-left-probe` 从同一站位发话，两处开口共四格，初始库存两块泥土。 | 比对第一条材料反馈和实际放置格。都补时玩家补给两块后再放四格，只补左边时放两格、右侧留空。各局记录玩家端收发时点、原观察及投递工作。 |
| 一句话拆装木门 | `--door-probe` 指向两处缺口中的门洞，发出一句指令。 | 实际拆除原门两格，拾取一扇木门，装入原指向的两格并只发一次完工消息。另一处缺口保持原样。 |
| 取材后补四格 | `--fill-four-probe` 预置三块泥土，查询草方块掉落，用 `gather_ground` 补足库存，由 `fill_target` 放置原话绑定的四格。 | 一格草和一块实际入包泥土，四份放置读回，另一处门洞保持两格为空。最后只发送一次完工消息。 |
| 跟随中挖一格，再取材补四格 | `--follow-dig-fill-probe` 从零泥土起步。玩家边走边聊并指定挖一格，然后叫停，指向四格缺口。取材开始后玩家移到门洞，夹具临时阻挡一个目标格，随后移除。 | 跟随恢复沿用原工作号，实际挖一格入包，随后只取缺的三块。部分补墙回执保留已补两格，恢复后只补剩余两格，原门洞两格仍为空。实际送模可按本体人格、场景规则及原话追查。 |
| 场景观察与合成 | `--craft-probe` 从一根橡木原木起步。核对三个观察来源和现场方块，查看五格内的泥土掉落，再通过计划合成四块木板。 | 独立玩家核对掉落实体身份和两块泥土，另一种物品和半径外的泥土均被排除。库存确认扣一根、入包四块，实际发言一次。 |
| 拾取玩家指定的一堆物品 | `--ground-pickup-probe` 用计划捡三块的那堆。再把石头移到剩余两块泥土旁，通过能力目录展开独立拾取工具。 | 首次入包三块，旁边两堆保留原 UUID。第二次复查重叠后停在原地，双方库存和原实体保持可核对。玩家端首条入包回复的计时写入 `player-feedback.json`。 |
| 指定玩家询问备料并收货 | `--handoff-probe` 从共七十块的两堆泥土起步。先询问能否分出六十六块，再明确委托交物。使用本页只读观察端的当前版本。 | 备料时库存保持 70。实际投物为 64＋1＋1，服务端核对角色、实体入场及收件库存。玩家实得 66、角色剩 4，各收到一次对应答复。 |
| 普通服务器的交物回执 | `--handoff-unobserved-probe` 使用相同角色、场地和固定响应，桥接配置省略只读观察端。 | 玩家仍实际收到 66，伙伴答复可见库存减少并将玩家收取量保留为未知。 |
| 原工作复查迟到的收件 | `--handoff-late-probe` 将收件玩家的主背包和快捷栏填满。伙伴投出后先报告尚待收取。玩家腾出背包、实际拾取，再询问刚才那批。 | 原号重复提交复用回执，换号重投返回原工作。后一次只读查询沿原工作及来源上界核实实收 66，整个过程只投一次。 |
| 交物等收件期间插话 | `--handoff-progress-probe` 在满包玩家尚未收件、服务端已核实投出 66 时询问进展，记录玩家端收发时刻。 | 进展关联原投物工作，连续相同读数只发布一次。伙伴能回答已投出和仍待收取的数量，腾出背包后沿原号核对实际收件。 |
| 从零库存备料并交给玩家 | `--handoff-gather-probe` 在区块交界布置两格可采草方块。玩家先委托，原工作开挖后再询问进度。 | 原话首答和插话首答分别计时。原计划依次核对两次破坏与入包、会合脚位、玩家实收两块和一次完工发言，保留原委托与后续现场准入的消息号。 |
| 第三位玩家拾走交物 | `--handoff-other-probe` 中配对玩家背包已满，另一个独立协议玩家在投物落点等待。 | 核对旁人实收 66、原玩家实收 0，收件人 UUID 属于第三个连接。伙伴向原玩家报告实际去向。 |
| 本人生命与氧气 | `--vitals-probe` 在封闭水槽内查询同场伙伴刚受伤时的生命、饥饿与剩余空气，回到干燥区域再查一次。 | 独立玩家按同一实体号核对掉血、耗气和出水恢复至 300 游戏刻；实际回复与原操作回执可逐条对照。 |
| 采掘插话、他方拾取与满包 | `--pickup-probe` 在独立土坡布置一格草方块，用挖掘疲劳留出插话窗口。独立玩家在原工作实际开挖后询问进展，掉落出现后站到其脚下。随后复位目标，用石头填满角色的 36 个背包和快捷栏槽位，再次要求取材。 | 玩家端计时写入 `player-feedback.json`，关联原工作、插话和回复。首轮核对拾取包、双方库存及角色对拾取人的说明。第二轮核对零可用空间、零破坏和独立玩家读到的原状。 |
| 地面取材和部分完成 | `--gap-materials-probe` 只允许从地上挖。固定响应先提交墙格，再请求两块泥土，场上只有一格符合取材范围。 | 墙格未变。实际采下一格，库存增加一块。工具未采满的回执仍包含坐标与前后方块，重采样后仍能从任务状态读取。 |
| 零库存取材、垫高和复跟 | `--layered-resupply-probe` 先让玩家登上高台。伙伴没有泥土，观察自身附近的两个候选格，取材期间接收一句插话，入包后再绑定两个垫脚位。 | 独立协议玩家读回两块取材格和两次落块。两次入包解锁后继，插话得到实际答复，续行仍关联原委托。两次精确站位和扣料均得到结算，路径可达后恢复同一次跟随。 |
| 垫高后仍有高差 | `--layered-follow-probe` 预备两块泥土，高台与最后的垫脚位仍相差两格。 | 落块和站位均成功，原跟随继续受阻。玩家下来一格后，同一个跟随工作号恢复，叫停后收到实际退出回执。 |
| 渡水、聊天和无路 | `--water-probe` 在低一格的水道上检验持续跟随、干岸叫停、固定目标反向过水和密闭目标。 | 跟随期间可发言。普通移动按原工作号对照起步距离、实际位移、到岸脚位和干地读回。密闭目标留下本次无路原因及末次位置。 |

复测时在同一份证据中记录玩家消息、入轮、实际发言、动作接纳与读回时刻。这样可以分别定位通信等待、模型耗时和游戏动作耗时。

### 观察与响应的分段时序

构建 `dfy_mc_plugin_partner_tests` 后，运行固定响应和受控游戏桥接的定向用例。

```powershell
ctest --test-dir build/rebuild/windows-tests -C Debug -V -R '^plugins\.mc_partner_revision_response-timing$'
```

用玩家消息号 2 串起原话、首答实发和首次移动接纳。第一段移动在消息 3 的模型响应期间按原工作号读回抵达。消息 3 的实际回复在两段移动都完成前送达。第二段仍归原任务和原请求，完工后的发言以独立送达回执核对。

`-V` 打印 Trace 路径和五段相对耗时。起点采用宿主 Trace 中的原话投影，动作采用桥接接纳和实际抵达的回执，发言采用 `sent=true` 的桥接回执。夹具仅检查因果顺序，耗时用于查看本次回放。 [断言](native-world.cjs#L73)

固定响应的 `--gap-group-probe` 另从独立玩家发出“帮我把墙上的缺口补好”计到该玩家收到伙伴的私聊。此场景原话未指定哪处开口，确认两处是否都补是有效反馈。该局证据目录的 `player-feedback.json` 保存原话、玩家端单调时钟的收发样本、耗时与判定，完成核对后附上原消息号、送模回合和游戏发言工作号。用 `node --test tests/mc-companion/player-feedback.test.cjs` 核对五秒边界，原话已明确时的重复追问、空泛应答和截尾采样分别结算。

若从构建盘运行时宿主在送模前超时或无法创建 Trace，可把相同的可执行文件和插件包复制到本机临时目录运行该夹具。

```powershell
$build = (Resolve-Path 'build/rebuild/windows-tests').Path
$fixture = New-Item -ItemType Directory (Join-Path $env:TEMP ('dfy-response-timing-' + [Guid]::NewGuid().ToString('N')))
$package = New-Item -ItemType Directory (Join-Path $fixture.FullName 'mc.companion')
Copy-Item -LiteralPath "$build/tests/Debug/dfy_mc_plugin_partner_tests.exe" -Destination $fixture.FullName
foreach ($file in 'plugin.json', 'defaults.json', 'dfy_mc_companion.dll') {
  Copy-Item -LiteralPath "$build/plugins/mc/Debug/mc.companion/$file" -Destination $package.FullName
}
node .\tests\mc-companion\native-world.cjs (Join-Path $fixture.FullName 'dfy_mc_plugin_partner_tests.exe') `
  $package.FullName (Join-Path $fixture.FullName 'runs') partner-revision-response-timing
```

### 绕墙与「垫上来」回归

构建 `dfy_mc_plugin_partner_tests` 后运行下列定向用例。固定响应使用受控游戏读口和正式插件、队列与回合。用 CTest 的 `-V` 查看各场景的 `tests/mc-partner/world-*/trace/events.jsonl`，按消息号、任务修订和原工作号查回执。

```powershell
ctest --test-dir build/rebuild/windows-tests -C Debug --output-on-failure -R '^(mc\.partner_observe|plugins\.mc_partner_revision_(navigation(-mixed|-restart)?|layered(-blocked)?|repair-plan|attention))$'
```

下表中的场景名省略 `plugins.mc_partner_revision_` 前缀。绕墙和垫高共用按原号核对前台接纳、唯一退出、分层结算和后继放行的断言。被跳过的节点不得出现前台动作回执。恢复跟随时，新执行段仍归属原逻辑工作号。[共用回执断言](native-world.cjs#L12)

| 现场问题与追溯入口 | 要守住的行为 | 定向回归 |
| --- | --- | --- |
| 补墙途中说「现在绕着这个墙跑两圈」。任务切换见 `9cf0e97`，计圈见 `7e18a5e`。[固定原话](native-world.cjs) | 旧采掘按原号退出，保留已拾取的一块泥土。新任务重新核验墙和四个站位，单条路线只在实际退出后唤醒子 Agent。 | `navigation` 核对任务切换。`mc.partner_observe` 核对站位、闭环和半圈计数。 |
| 绕行与普通移动串联。计划合并见 `a537ea4`，在途回执见 `1546ce4`。 | 9 个检查点齐全但路线仍在途时，后继移动继续等待。 | `navigation-mixed` 保留到达起点、绕两圈、后继移动三层。 |
| 「中途重新绕」，见 `451b1f7`。[固定原话](native-world.cjs) | 旧路线带一圈按原号退出，旧追问撤下。新修订沿原墙从零开始，完成后恢复原跟随。 | `navigation-restart` 先返回仍在途的停止回执，再返回原号退出。 |
| 「你可以垫上来」后计划被拒却进入等待，见原始基线 `run-CA2yMm` 的回合 8、9。[历史回放](replay-follow-climb-baseline.cjs#L43) | 合法的放置和移动节点可接纳。无效计划在修订额度用完后明确结算。 | `layered` 验证合法计划接纳。`repair-plan` 验证两次拒绝后正常结算回合。 |
| 高台掉队后请求垫上来，见同一基线的跟随工作 `1.240`、`1.267`、`1.276`、`1.315`。[历史回放](replay-follow-climb-baseline.cjs#L22) | 受阻和恢复沿用原跟随身份。垫高每次放置或移动按原号结算。中途站位失败时保留已有方块，跳过依赖步骤。 | `attention` 覆盖异常与恢复。`layered` 和 `layered-blocked` 分别覆盖四层完成和第一步移动受阻。后者的站位失败由夹具注入。 |

`run-CA2yMm` 的原件由历史回放脚本按指纹核对，包括 `host/trace/events.jsonl`、`host/game-actions.jsonl` 和 `console.log`。绕墙的早期 Paper 对照从体验根目录下的 `evidence/mc-context-aafea37-mws05jz6/comparison.json` 追溯，现场工作号查该索引指向的运行记录。[历史对照记录](../../docs/mc-companion-development-plan.md#已验证的基线)

**Paper 验证。** 绕墙的 Paper 探针从上述对照索引追溯。实际验收核对脚位经过墙两侧和两端、返回起点，以及重开时从起点重新跑。垫高的两种场地使用表中的 `--layered-resupply-probe` 与 `--layered-follow-probe`。复盘时沿同一个跟随工作号检查受阻、判定可达、恢复与实际退出，另用采掘、放置及移动回执核对已经发生的效果。[垫高探针](playtest-check.cjs#L202)

### 指向操作回归

```powershell
ctest --test-dir build/rebuild/windows-tests -C Debug --output-on-failure -R '^plugins\.mc_partner_revision_(pointed.*|direct-place(-stale)?)$'
```

| 场景 | 核对项 |
| --- | --- |
| 敲掉石块上的泥土 | 视线命中石块时拒绝直接挖石和挖石计划，只允许通过核验的泥土目标。 |
| 原木追问后续接合成 | 其他聊天仍可回应，玩家指明后用原要求续接。方块挖掉但原工作仍在途时，合成继续等待。原号退出确认入包后，核对合成消耗一根原木并产出四块木板。未入包时保留挖掘实效，跳过合成。 |
| 缺口选项与旧答复 | `direct-place` 核对原问题在答复前保持等待，匹配答复只放行原修订获选格。`direct-place-stale` 在更换任务后送达同一句答复，原问题仍为已撤销状态，旧格无游戏工作。 |

`run-JKvHIw` 的历史故障可用原始 Trace 离线回放会话 52 的回合 11 和 36。旧门上下格见 Trace 第 244 行，发言称未找到见第 254 行。原话候选在第 1973 行读到，随后观察覆盖原读回，第 2026 行绑定失败。新回归用同一组门坐标检验合并，再用 `--gap-probe` 验证实际游戏观察和固定模型绑定。

用下方命令将模式换成 `--follow-probe` 可运行边走边聊的固定响应。两次发言分别通过显式工具和最终文本投递。

工程回归可在准备好的**独立 Paper 夹具**中运行模式 `--wall-probe`。沿用探针的 Java、Paper、Node 与素材设置，把 `DFY_MC_TEST_ROOT` 指向隔离目录，加载固定模型的 `dfy_mc_playtest` 和当前插件包。协议玩家离开原缺口后，核对错误候选被拦下、原缺口补齐、门洞在批准前保持两格为空、批准后读回门的上下两格。输出中的 `DFY_TRACE` 和同目录的 `game-actions.jsonl` 提供复盘材料。

```powershell
node .\tests\mc-companion\playtest.cjs .\build\windows-tests\tests\Debug\dfy_mc_playtest.exe .\build\windows-tests\plugins\mc\Debug\mc.companion --wall-probe
```

同一墙体夹具支持三种 ASK 验证，替换命令末尾的模式即可。每次单独启动一个本地 Paper 实例。协议玩家收到一次实际询问，按呈现的编号答复“靠河那段，留木门”，再确认原门洞候选。

| 模式 | 核对的路径 |
| --- | --- |
| `--ask-direct-probe` | 本体发 ASK 并把结果投到插件入箱，整局无主 Agent 模型回合。 |
| `--ask-main-probe` | 到件唤醒主 Agent，固定后端先调用 `ask` 选择游戏，再调用 `mail_reply` 转发原答案。 |
| `--ask-main-manual-probe` | 两封信都等待 `/mail review`，随后使用同一主 Agent 路径。 |

三种模式均核对后续实际送模上下文保留共同目标和原答复。协议玩家独立读回原墙未覆盖、缺格已补，以及批准后的上下门格。Trace 可按事件身份、ASK 编号、模型运行和游戏工作号逐项核对。

### 用浏览器进服

```powershell
.\tests\mc-companion\browser-player\start.ps1 -FixtureRoot 'D:\DaFeiYu-MC-Test' -BuildDirectory '.\build\rebuild\windows' -Configuration Release
```

命令用指定构建启动 Paper 与本机连接服务，以独立浏览器资料打开 WebMC 2.4.0。加 `-PrepareOnly` 可以只准备服务。当前体验服的构建和配置吻合时可直接复用。服务已启动时，打开入口并点击“进入河边体验”。[浏览器玩家入口](http://127.0.0.1:25582/) · [固定发布版本](https://github.com/zardoy/minecraft-web-client/releases/tag/v2.4.0)

你使用独立的本地玩家名 `DfyVisitor`。点击游戏画面后用 WASD 走动、鼠标转向，按 T 输入原话并回车。是否向你的玩家身份发言或继续行动，由大肥鱼结合上下文决定。浏览器按大肥鱼的离线 UUID 为其玩家模型加载已准备的皮肤。关闭游戏页面将离服，重开一轮可再运行启动脚本。此入口只连接本机 `127.0.0.1:25580` 的离线体验服。

进入后按真人验收清单操作。体验目录的 `mwc/*-paper.log` 记录本局证据目录和 `DFY_TRACE`；同一宿主目录下用 `trace/events.jsonl` 核对消息和工具，使用 `game-actions.jsonl` 核对游戏实际退出和效果。代理状态可从本机健康页查看。[真人验收清单](#本轮真人验收) · [代理健康页](http://127.0.0.1:25582/health)

G01 直接运行 `node --test tests/mc-companion/hillside.test.mjs`。样本解释器消费固定材料事件，不启动游戏或模型。

## G02 双角色只读探针

已验证的组合为 Minecraft Java 1.21.4、Paper 稳定构建 232、Java 21、Node 24.13.0 与锁文件中的 Mineflayer 4.39.0。游戏、JDK 和依赖均在 D 盘的独立测试目录。Paper JAR 的 SHA-256 为 `5ee4f542f628a14c644410b08c94ea42e772ef4d29fe92973636b6813d4eaffc`。[Paper 下载服务](https://docs.papermc.io/misc/downloads-service/)

1. 在 D 盘测试目录设置 `TEMP`、`TMP`、`npm_config_cache`，Java 启动时将 `java.io.tmpdir` 和 `user.home` 指向该目录。将 `plugins/mc/bridge/package*.json` 复制到其中的 `bridge` 子目录，运行 `npm ci --prefix D:/DaFeiYu-MC-Test/bridge --ignore-scripts --no-audit --no-fund`。
2. 使用独立空世界及已接受 EULA 的服务目录。配置 `server-ip=127.0.0.1`、`server-port=25579`、`online-mode=false`、`enforce-secure-profile=false`、`enable-rcon=false`、`enable-query=false`、`level-type=minecraft:flat` 和 `spawn-protection=0`。这种离线身份仅供本机隔离测试使用，线上玩家授权需要可信账号绑定。
3. 从该目录启动 `java -Xms256m -Xmx1G -jar <Paper文件> --nogui`，待就绪后在本地控制台执行 `gamerule spawnRadius 0` 和 `setworldspawn 0 -60 0`。会合区位于主世界，水平坐标各在 ±8 内，高度为 -61 至 -58。重跑时两个测试角色也要位于该区。
4. 设置 `NODE_PATH=D:/DaFeiYu-MC-Test/bridge/node_modules`，从项目根运行 `node tests/mc-companion/connection.cjs 25579`。最后可加一个尚不存在的 JSON 路径保存证据。验收读取两个玩家 UUID、实体、位置、会合区、真实脚下方块和背包，随后客户端离开。服务器用控制台 `stop` 正常关闭。

G02 使用两个独立的本机协议身份，没有接触个人账号。它证明两个玩家处于同一真实世界，尚不判断掉落来源或正式运行的授权关系。

**托管验证。** 在上述世界首次准备好并正常关服后，设置 `DFY_MC_TEST_ROOT`、`DFY_MC_JAVA`、`DFY_MC_PAPER_JAR` 为测试根、Java 可执行文件和 Paper JAR 的绝对路径。世界位于根目录的 `servers/stone-1.21.4`，根目录下创建 `evidence` 子目录。运行 `node tests/mc-companion/connection.cjs --managed`。夹具独占本次回环服进程，等待就绪、读取双端身份、保存证据，并在成功或失败后请求正常停服。每次证据存入新的 `evidence/run-*` 目录。

## G03 两种圆石来源

在已准备好的专用测试服停止时运行 `./tests/mc-companion/observer/build.ps1`。此脚本使用同一 JDK 和服务器自带的依赖，输出只读观察 JAR 到该实例。再运行 `node tests/mc-companion/origins.cjs`。测试控制台负责准备两个专用角色的物资和一个目标方块，观察插件只登记游戏事实。

重建试玩场地的交物观察端时，向同一脚本传 `-ServerName playtest-1.21.4`。观察端按同一世界的实体 UUID 对照生存丢物、入场、合堆、实际拾取人与其背包读回。工作回执保留观察流和序号，可以从服务端文件追溯投物的来源。

铁镐破坏后的服务端候选须与真实入世界的实体 UUID 对应，玩家主动丢物另有独立来源。两份圆石在场时关联客户端的物品网络编号。另用徒手实际破坏作零掉落负例。每次服务器启动使用独立观察流和从 1 开始的序号；`origins.json` 和 `no-drop.json` 分别保存两段结果。

## G04a 真实合堆与分取

重建观察 JAR 后运行 `node tests/mc-companion/pickups.cjs`。在专用场地分别验证两份独立掉落只捡自采、两份合堆后全取、合堆后两人各取一块。为了稳定进入指定分支，夹具控制台摆放已经真实生成的实体并移动角色；未替换物品、修改合堆半径或注入拾取事件。分取时令角色背包实际只剩一块容量。

观察插件记录合堆双方、拾取者、事前库存、游戏报告的地面余量及下一 tick 的地面和库存读回。客户端独立记录库存及实际收到的拾取通知。三个 JSON 文件各有两端证据。完整取走后可确认自采 1；两人分取后范围为 0～1。对原样重发及删除中间事实另作离线断言。

## G04b 返程持有量

运行 `node tests/mc-companion/return.cjs`。角色原有 5 块，在会合区外接收来源已确认的混合堆 2 块，从实际数量为 7 的单堆丢出 1 块，然后由 Mineflayer 控制真实角色步行回河边。测试控制台只能通过观察端的采样命令读取指定角色同一时点的位置、库存与堆叠数量，普通玩家不能调用。成功回执同时保存出发和到达两端的独立读数。到场有 6 块，自采入包曾确认 1，当前能证明实带 0～1；删去途中丢物证据则明确未核实。

## G05 游戏侧只读桥接

运行 `node tests/mc-companion/bridge-world.cjs`。以真实独立角色开启短名绑定的 Windows 本地管道。每条连接只接收一条有界 `state.read` 请求，回复回显原请求编号，给出当前出生代次、角色、维度、位置、圆石总数和距最近服务端信号的时长。库存总数未作来源归属。撤销后读回空世界，关闭桥接后连接失败，写方法不进入游戏。

## G08 游戏侧查看

运行 `node tests/mc-companion/owner-chat.cjs` 和 `node tests/mc-companion/inspect.cjs`。前者核对绑定玩家原话、来源和游标；后者在真实游戏里发出委托，并以私有本机管道请求查看。测试开场把角色放在河边，随后她通过实际寻路抵达山脚，视线读到圆石。负例覆盖“先别去”、旧世界、替换目的地和重复受理；旁路观察与客户端挖掘调用共同核对零采掘。此项仅验证游戏侧执行与回读。

## 正式应用和有界采掘

在独立测试服已关停时，运行 `node tests/mc-companion/player-session.cjs <dfy_mc_game_visit_tests.exe> <MC包目录> <变体>`。不加变体执行正式查看。`skip`、`tamper` 分别验证模型仅声称完成和替换参数。`decline` 在游戏中拒绝。`approve-game` 和 `approve-center` 分别验证两个入口的授权、原游戏接单与相冲突的迟到答复。`lost-receipt` 在游戏接单后丢回执，宿主只能回查；`lost-send` 在转交前断线，查明尚未接单后用原身份重投一次。

另运行 `node tests/mc-companion/inspect.cjs mine` 检查游戏侧的一块授权，验收未经接单、超量、旧世界、真实工具装备、实际采掘和重复回执。客户端动作返回后，独立观察端读到原玩家破坏及候选产出。运行 `node tests/mc-companion/inspect.cjs revoke` 验证开始前撤销保持零采掘。以上与入包或返程判定分别取证。

同一正式入口的 `pickup-one` 检查一块自采与 5 块旧库存，`collect-16` 检查 16 个独立来源。`return-16` 在河边读到库存 21、自采实带 16；`return-14` 从零旧库存开始，在返程途中通过**独立夹具**实际扣掉 2 块，应显示实带 14、数量不足。实际移动触发只读观察端在下一 tick 读取位置和库存。固定异常样本可用 `node --test tests/mc-companion/custody.test.cjs` 验证旧位置、错角色、错世界及混合库存范围。

## G14 探索受阻分析

查看行动期间，MC 适配器按游戏物理更新采样，把角色位置和八格视线内命中的方块各自映射到四格见方的区域。区域编号代表本次探索的采样粒度。角色必须实际离开并再进入才算一次返回；三次原地位置回报无法触发询问。缺块、位置校正、超过一秒的采样停顿或窗口容量耗尽时重新建立窗口。

宿主读取配对的 `search.progress` 用途，先核对世界、角色、查看消息号与同一连续窗口。三次返回仍有新增区域时直接继续；连续没有新增时再读一次，新的游戏采样仍无进展才询问原动作。游戏侧直接按目标和寻路器最近的结果／重算原因答复，移动可继续。只有后续新的可见区域才能标记探索恢复。普通 `state.read` 保持原结构。终端 `/companion status` 展示依据、问题、答复和验证；本步不调整游戏路线，也不弹出玩家审批。

用 `dfy_companion_review_tests` 运行 `app.review_exploring`、`app.review_blocked`、`app.review_recheck`、`app.review_disconnect`、`app.review_gap`。使用真实 DLL、正式宿主、固定游戏轨迹和固定模型，原始消息及判定保存在各测试目录的 `review.txt`。正常轨迹与受阻轨迹走相同区域，只改变实际看到的表面。`mc.exploration` 单独验证采样计数。真实回环服的 `inspect.cjs` 定向验证移动期间按原询问取得路线答复，并核对零采掘。
