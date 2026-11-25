# 第三方依赖

| 依赖 | 固定版本 | 许可证 | 用途 |
|---|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json/tree/v3.12.0) | 3.12.0 | [MIT](https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT) | 模型配置、请求与响应、插件清单及插件配置的 JSON 处理 |
| [Microsoft.Web.WebView2](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4314.35) | 1.0.4314.35 | 包内 LICENSE.txt 与 NOTICE.txt | 可选 GUI 的 C++ SDK 与静态加载器；Evergreen Runtime 单独安装 |
| [Vue](https://vuejs.org/) | 3.5.45 | MIT | 本地管理中心验证页面 |
| [Reka UI](https://reka-ui.com/) | 2.11.0 | MIT | 对话框、焦点及可访问性交互基础 |

CMake 首次配置时下载并校验归档 SHA-256。依赖保存在构建目录，未复制到项目源码，也不引用 `Old/`。分发包含该依赖的产物时须随附其版权和许可证声明。

WebView2 仅在 `BUILD_GUI=ON` 时获取，归档 SHA-256 为 `90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4`。GUI 分发附带 SDK 原始许可证和 NOTICE；不会把 Edge 浏览器当作运行库，也不把 WebView2 依赖加到终端目标。

前端工具和传递依赖由 `apps/desktop/web/package-lock.json` 固定，Node.js 只用于构建。Vite 构建按实际进入页面的模块收集原始许可证，随 UI 的 `licenses/` 目录和 `packages.json` 发布；页面运行不使用 CDN 或 npm 服务。

HTTPS 使用 Windows SDK 的 WinHTTP 接口，链接系统 `winhttp` 库；无需额外下载 HTTP 库，当前网络实现仅支持 Windows。

主会话消息库使用 Windows SDK 的 `winsqlite/winsqlite3.h` 和导入库 `winsqlite3.lib`。安装包通过 Windows 自带的 `winsqlite3.dll` 读写 SQLite 数据库。

Release 运行目录通过 CMake `InstallRequiredSystemLibraries` 收集当前 VS2022 工具链提供的 x64 Release 运行库，原样附带检测到的 MSVC CRT／C++ 运行库 DLL，不分发 Debug 运行库。Windows 10／11 提供系统 UCRT、WinHTTP、Shell32 和 Ole32。微软运行库保留原有二进制版权信息，其来源为本机 Visual Studio 的 `VC/Redist/MSVC` 目录。

独立分发目录包含 `licenses/nlohmann-json-LICENSE.MIT`，保留 nlohmann/json 的原始完整版权和 MIT 许可文本。源码树中的第三方构建产物不作为新根工程之外的运行依赖。
