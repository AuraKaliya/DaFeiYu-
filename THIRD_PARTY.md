# 第三方依赖

| 依赖 | 固定版本 | 许可证 | 用途 |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json/tree/v3.12.0) | 3.12.0 | [MIT](https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT) | 模型配置、请求与响应、插件清单及插件配置的 JSON 处理 |

CMake 首次配置时下载并校验归档 SHA-256。依赖保存在构建目录，未复制到项目源码，也不引用 `Old/`。分发包含该依赖的产物时须随附其版权和许可证声明。

HTTPS 使用 Windows SDK 的 WinHTTP 接口，链接系统 `winhttp` 库；无需额外下载 HTTP 库，当前网络实现仅支持 Windows。
