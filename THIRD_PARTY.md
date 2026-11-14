# 第三方依赖

| 依赖 | 固定版本 | 许可证 | 用途 |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json/tree/v3.12.0) | 3.12.0 | [MIT](https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT) | 模型配置、请求与响应、插件清单及插件配置的 JSON 处理 |

CMake 首次配置时下载并校验归档 SHA-256。依赖保存在构建目录，未复制到项目源码，也不引用 `Old/`。分发包含该依赖的产物时须随附其版权和许可证声明。

HTTPS 使用 Windows SDK 的 WinHTTP 接口，链接系统 `winhttp` 库；无需额外下载 HTTP 库，当前网络实现仅支持 Windows。

Release 运行目录通过 CMake `InstallRequiredSystemLibraries` 收集当前 VS2022 工具链提供的 x64 Release 运行库，原样附带检测到的 MSVC CRT／C++ 运行库 DLL，不分发 Debug 运行库。Windows 10／11 提供系统 UCRT、WinHTTP、Shell32 和 Ole32。微软运行库保留原有二进制版权信息，其来源为本机 Visual Studio 的 `VC/Redist/MSVC` 目录。

独立分发目录包含 `licenses/nlohmann-json-LICENSE.MIT`，保留 nlohmann/json 的原始完整版权和 MIT 许可文本。源码树中的第三方构建产物不作为新根工程之外的运行依赖。
