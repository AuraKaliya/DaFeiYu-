# Minecraft 游戏陪伴插件

当前包提供独立角色的世界查询、受控查看及采集返程。`mc.companion/world` 返回角色 UUID、出生代次、维度、坐标、背包圆石总数和最近游戏信号的年龄。世界撤销时可读到 `null`，连接失败时工具返回失败。总库存只描述当前持有量。

在 D 盘等指定运行目录准备 Node 22 或以上环境，并按 `bridge/package-lock.json` 安装依赖。若将依赖装在源码目录外，设置 `NODE_PATH` 指向对应的 `node_modules`。为自己的连接在该运行目录创建 JSON 配置，例如隔离的本机测试服使用：

```json
{"channel":"companion","host":"127.0.0.1","port":25579,"username":"DfyCompanion","auth":"offline"}
```

从项目根运行 `node plugins/mc/bridge/start.cjs <配置文件>`。个人服务器连接可用 `auth: "microsoft"` 选择大肥鱼自己的玩家账号，登录缓存位于配置文件旁的 `profiles` 目录。开发验收使用本机测试身份。通道短名必须与 DLL 的独立设置一致，默认值为 `companion`。

要在适配器接收玩家原话，可在本地连接配置中给出 `owner`（玩家 UUID）和 `bridgeKey`（随机 32 字节的小写十六进制）。私有 `chat.read` 查询还须绑定该键和角色当前世界；普通世界查询不返回聊天。适配器只接受源于该 UUID 的游戏协议消息，线上要求通过签名校验。系统广播和角色自己的文字不构成玩家指令。保留窗口外重读会返回缺口，每批有消息数与字节上限。

设置中的地标使用 `"places":{"hillside":{"label":"山脚","dimension":"overworld","x":12,"y":-60,"z":0}}` 形状。首例游戏侧支持明确的“去山脚看看”“请去山脚看看”或“帮我去山脚看看”。它用对应的原玩家消息、世界代次和地标约束一次寻路；其他表达仍可作对话，不能仅因包含地名就取得移动许可。工具请求不能自行指定任意坐标。查看时关闭挖掘、垫方块与开门；到达后从角色视线确认圆石。动作租期需要调用方持续查询，撤销时等路径实际退出。

构建目标 `dfy_mc_companion` 会在对应配置下生成 `mc.companion` 包目录。使用原终端的 `/plugins install "<包目录>"`、`/plugins load mc.companion` 显式装载，或由共享应用服务接入。普通模型问答保持独立。定向验收见 [MC 探针](../../tests/mc-companion/README.md)。

在用户数据目录的 `plugins/mc.companion/settings.json` 保存与适配器相同的 `channel` 和 `bridgeKey`，再加载包。启动游戏适配器后，在终端输入 `/companion open mc.companion`。等 `/companion status` 显示 `listening`，绑定的玩家即可在游戏里发出“去山脚看看”。从收件开启时已有的聊天水位之后开始接纳，旧聊天不会自动补作委托。`/companion status` 可查看实际验收状态，`/companion stop 会话号` 向指定原会话请求停止。

要使用采集返程，另配置本世界的 `riverside` 会合地标。当游戏角色私聊询问时可回复“采 16 块带回来”，也可在受信终端用 `/companion approve 会话号 问题号 修订 请求号 数量` 成立同一委托。包按原消息绑定接单，角色使用背包中的合适镐，在原地标附近只挖看得见、够得着的圆石，按授权上限执行并回会合区。当前接入支持明确数量 1～64。

对于本地可控服务器，可在**适配器**配置中加 `observer`，其 `directory` 是只读观察端输出目录的绝对路径，`world` 是该服务器世界 UUID，`dimension` 是对应游戏维度。观察记录只用于判断实际拾取与带回。未配置或证据断档时仍可移动、采集，管理中心或终端只能据实展示已知库存和归属范围。查看、采掘和返程的验收分开保存。[本地验收说明](../../tests/mc-companion/README.md)

终端的 `/companion where 会话号` 可直接采用足够新的进展，或说明最后一次确认的位置并开启读取。`/companion where 会话号 meet` 用于现场会合，会返回新读取的编号，已有在途查询可以复用。读取期间游戏角色继续当前动作，查询本身不调用模型。
