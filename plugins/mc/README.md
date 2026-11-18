# Minecraft 游戏陪伴插件

当前包提供显式启用的 `mc.companion/world` 世界查询。读取返回角色 UUID、出生代次、维度、坐标、背包圆石总数和最近游戏信号的年龄。世界撤销时可读到 `null`，连接失败时工具返回失败。总库存只描述当前持有量。

在 D 盘等指定运行目录准备 Node 22 或以上环境，并按 `bridge/package-lock.json` 安装依赖。若将依赖装在源码目录外，设置 `NODE_PATH` 指向对应的 `node_modules`。为自己的连接在该运行目录创建 JSON 配置，例如隔离的本机测试服使用：

```json
{"channel":"companion","host":"127.0.0.1","port":25579,"username":"DfyCompanion","auth":"offline"}
```

从项目根运行 `node plugins/mc/bridge/start.cjs <配置文件>`。个人服务器连接可用 `auth: "microsoft"` 选择大肥鱼自己的玩家账号，登录缓存位于配置文件旁的 `profiles` 目录。开发验收使用本机测试身份。通道短名必须与 DLL 的独立设置一致，默认值为 `companion`。

要在适配器接收玩家原话，可在本地连接配置中给出 `owner`（玩家 UUID）和 `bridgeKey`（随机 32 字节的小写十六进制）。私有 `chat.read` 查询还须绑定该键和角色当前世界；普通世界查询不返回聊天。适配器只接受源于该 UUID 的游戏协议消息，线上要求通过签名校验。系统广播和角色自己的文字不构成玩家指令。保留窗口外重读会返回缺口，每批有消息数与字节上限。

构建目标 `dfy_mc_companion` 会在对应配置下生成 `mc.companion` 包目录。使用原终端的 `/plugins install "<包目录>"`、`/plugins load mc.companion` 显式装载，或由共享应用服务接入。普通模型问答保持独立。定向验收见 [MC 探针](../../tests/mc-companion/README.md)。
