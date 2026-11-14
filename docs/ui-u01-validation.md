# U01 原生 UI 容器验证

基线：`0e59334`。用户授权完成 U01，并要求小步开发、每步只运行必要检查。本阶段验证离线启动、Frame 授权、中文输入、焦点及资源回收；当前窗口是验证程序，不代表 U02～U09 的业务界面或插件 UI 已完成。

## 运行入口

GUI 是独立可选目标，终端不依赖 WebView2。安装微软 WebView2 Evergreen Runtime 后，可构建验证窗口：

```powershell
cmake --preset windows-ui
cmake --build --preset windows-ui-debug
.\build\rebuild\windows-ui\bin\Debug\dafeiyu-ui-probe.exe
```

`--smoke` 隐藏窗口，在本地页面导航成功后关闭，用退出码报告结果。退出码 4 表示运行库不可用，1 表示容器或资产失败。完整程序交互验证仍需实际窗口，不能用隐藏导航代替中文输入或焦点验收。

## U01a 独立离线容器

固定 WebView2 SDK 1.0.4314.35 及官方 NuGet 归档 SHA-256，按 `BUILD_GUI` 条件引入；静态加载器与 CMake 配置不进入终端目标。窗口在 STA 消息循环中创建浏览器，程序目录下的 `ui` 映射到本地 HTTPS 来源，不依赖当前工作目录或页面服务器。异步创建回调仍未完成时保留窗口责任；迟到控制器在关闭路径回收。

本机实际运行库为 157.0.4341.46。构建 Debug `dfy_ui_probe`；真实 WebView2 的隐藏导航与关闭通过，缺少本地资产时明确失败。另定向构建未开启 GUI 的终端并通过离线配置检查。官方 SDK 归档下载后核对校验值；本次构建复用其解包目录和已验证的 JSON 依赖，没有引用 `Old`。下一步增加来源与 Frame 授权校验；本单元尚未宣称完成浏览器进程退出或中文输入验证。
