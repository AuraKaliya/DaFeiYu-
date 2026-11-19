# 真人体验服的角色外观

首轮选用 LittleSkin 公共皮肤库的 **《DeepSeek大肥鱼》**（材质 926524，上传者 Lemon_Cat_Tea）。该文件是原版 Java 支持的 64×64 Alex 细臂皮肤，可以直接穿在联机玩家模型上。皮肤取自上传者的公开材质页，仓库仅提供来源与本机准备脚本，项目的 MIT 许可仅适用于项目自身代码。[素材页](https://littleskin.cn/skinlib/show/926524) · [上游材质元数据](https://littleskin.cn/texture/926524)

运行 `./tests/mc-companion/prepare-appearance.ps1 -FixtureRoot D:/DaFeiYu-MC-Test` 会核对原 PNG 的 SHA-256（`f72db0a64b13e31225ec8280f712cf496fb044d5a4b3b46d0d812028880de1c0`），把 PNG 和本地体验资源包写入该目录的 `appearance`。已有且匹配的皮肤可离线使用。

体验服使用独立的本机身份 `DfyCompanion`。1.21.4 客户端按该离线 UUID 的 Java 哈希在 18 个默认外观中选中 slim/zuri。体验包只覆盖这一格，游戏协议的玩家身份和权限仍取原 UUID。恰好分在同一默认格的其他无自定义皮肤玩家也会看到相同外观。进服时接受服务器资源包即可使用，离服即卸下。使用正式账号时可将同一 PNG 选为细臂皮肤，无需这份体验包。
