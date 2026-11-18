# Minecraft 本地协议探针

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
