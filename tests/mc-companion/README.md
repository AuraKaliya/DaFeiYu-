# Minecraft 本地协议探针

G01 直接运行 `node --test tests/mc-companion/hillside.test.mjs`。样本解释器消费固定材料事件，不启动游戏或模型。

## G02 双角色只读探针

已验证的组合为 Minecraft Java 1.21.4、Paper 稳定构建 232、Java 21、Node 24.13.0 与锁文件中的 Mineflayer 4.39.0。游戏、JDK 和依赖均在 D 盘的独立测试目录。Paper JAR 的 SHA-256 为 `5ee4f542f628a14c644410b08c94ea42e772ef4d29fe92973636b6813d4eaffc`。[Paper 下载服务](https://docs.papermc.io/misc/downloads-service/)

1. 在 D 盘测试目录设置 `TEMP`、`TMP`、`npm_config_cache`，Java 启动时将 `java.io.tmpdir` 和 `user.home` 指向该目录。将 `plugins/mc/bridge/package*.json` 复制到其中的 `bridge` 子目录，运行 `npm ci --prefix D:/DaFeiYu-MC-Test/bridge --ignore-scripts --no-audit --no-fund`。
2. 使用独立空世界及已接受 EULA 的服务目录。配置 `server-ip=127.0.0.1`、`server-port=25579`、`online-mode=false`、`enforce-secure-profile=false`、`enable-rcon=false`、`enable-query=false`、`level-type=minecraft:flat` 和 `spawn-protection=0`。这种离线身份仅供本机隔离测试使用，线上玩家授权需要可信账号绑定。
3. 从该目录启动 `java -Xms256m -Xmx1G -jar <Paper文件> --nogui`，待就绪后在本地控制台执行 `gamerule spawnRadius 0` 和 `setworldspawn 0 -60 0`。会合区位于主世界，水平坐标各在 ±8 内，高度为 -61 至 -58。重跑时两个测试角色也要位于该区。
4. 设置 `NODE_PATH=D:/DaFeiYu-MC-Test/bridge/node_modules`，从项目根运行 `node tests/mc-companion/connection.cjs 25579`。最后可加一个尚不存在的 JSON 路径保存证据。验收读取两个玩家 UUID、实体、位置、会合区、真实脚下方块和背包，随后客户端离开。服务器用控制台 `stop` 正常关闭。

G02 使用两个独立的本机协议身份，没有接触个人账号。它证明两个玩家处于同一真实世界，尚不判断掉落来源或正式运行的授权关系。
