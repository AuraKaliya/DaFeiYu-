# Minecraft 本地协议探针

## 首次真人体验

准备好 G02 运行目录和观察端后，构建 Debug 目标 `dafeiyu`、`dfy_mc_companion`，运行 `./tests/mc-companion/start-playtest.ps1 -FixtureRoot D:/DaFeiYu-MC-Test`。正式入口使用配置的模型，仅在收到玩家消息后发起回合。模型配置优先取 `-ModelConfig` 指定的 JSON 文件或 `DFY_MC_MODEL_CONFIG` 指向的路径，其次取体验根目录的 `partner-model.json`，缺省使用应用随附的 DeepSeek 配置。游戏连接与模型凭据分开保存。用 `api_key_env` 指定的环境变量向宿主提供凭据。

1. 用 Java 1.21.4 直接连接 `127.0.0.1:25580`，或使用下方浏览器入口。两种方式都用独立玩家身份见到大肥鱼；原版客户端接受本服的[角色资源包](appearance.md)。
2. 收到“我在河边”后，在普通聊天里随意说话。例如先打个招呼，再说“大肥鱼，帮我采俩泥土”。你可以临时改主意。要立即接管时说“停，我来”。本机离线服上的 `/msg` 私聊缺少可核对的发送者身份。
3. 接管或离服会结束本场。启动时打印的宿主目录中，`board.txt` 记录游戏对话，`game-actions.jsonl` 按世界、玩家消息号和工作号记录游戏侧实际执行的参数与结果。无人进服时，在该目录创建空文件 `stop` 可正常结束。

**定向验收**另需构建 `dfy_mc_playtest`。加 `-PartnerProbe` 会以固定响应验证自由原话进入模型、前文可用、四种技能、已有回复去重、途中改口和服务故障反馈。加 `-Probe` 仍跑旧的圆石授权与实带 16 块场景。两种验收均使用真实 Paper 和独立协议玩家，模型使用替身。自然语言理解质量留给真人游玩。

### 用浏览器进服

运行 `./tests/mc-companion/browser-player/start.ps1 -FixtureRoot D:/DaFeiYu-MC-Test`。脚本验证并展开 WebMC 2.4.0 的官方发布包，启动体验服及本机连接服务，用 D 盘独立浏览器资料打开入口。加 `-PrepareOnly` 可以只准备服务。已在运行时可直接打开 [浏览器玩家入口](http://127.0.0.1:25582/)，点击“进入河边体验”。[上游项目与协议兼容范围](https://github.com/zardoy/minecraft-web-client) · [固定发布版本](https://github.com/zardoy/minecraft-web-client/releases/tag/v2.4.0)

你使用独立的本地玩家名 `DfyVisitor`。点击游戏画面后用 WASD 走动、鼠标转向，按 T 输入原话并回车。大肥鱼会向你的原玩家身份回复，你可继续用普通聊天答复或叫停。浏览器按大肥鱼的离线 UUID 为其玩家模型加载已准备的皮肤。关闭游戏页面将离服，重开一轮可再运行启动脚本。此入口只连接本机 `127.0.0.1:25580` 的离线体验服。

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
