# 大肥鱼 U01 验证窗口运行说明

`dafeiyu-ui-probe.exe` 是管理中心的原生容器验证程序，提供运行状态只读查询、本地输入、对话框和独立 Frame 页面。正式问答使用 `dafeiyu.exe`。

## 运行环境

需要 Windows x64 和 Microsoft Edge WebView2 Evergreen Runtime。运行目录已包含前端静态资产及 MSVC Release 运行库，不需要 Node.js、源码目录或页面服务器。

若提示 WebView2 Runtime 不可用，从 [微软官方下载页](https://developer.microsoft.com/en-us/microsoft-edge/webview2#download-section) 获取 x64 Evergreen Standalone Installer；提前下载该完整安装器后可离线安装。Edge 浏览器与 WebView2 Runtime 是不同组件。本验证包不自动安装或修改系统运行库。

## 打开界面

直接运行 `dafeiyu-ui-probe.exe`。主窗口使用随程序分发的默认模型配置组装业务宿主，可刷新查看当前或最近退出的运行状态。输入区和独立来源页面用于验证组合输入、对话框和 Frame 消息。组合结束次数与浏览器可信标记分开显示。

关闭窗口会继续回收创建回调、Frame 和浏览器进程，然后清理本次独立临时配置。临时配置位于系统临时目录的 `DaFeiYu-U01` 下，不复用用户浏览器配置。

## 自动验证

以下模式都使用不激活的隐藏窗口，不进行 Computer Use 或系统键鼠注入，也不开放远程调试端口。

| 参数 | 验证内容 |
|---|---|
| `--smoke` | 本地资产、外部资源拒绝、Frame 编码与授权 |
| `--offline` | 浏览器离线模拟状态下，从全新配置加载本地页面并完成 Frame 检查 |
| `--interaction` | 43 个固定协议步骤：中文组合输入、Tab／Shift+Tab、Enter、Escape、焦点返回与 Frame 回显 |
| `--lifecycle` | 20 轮 Frame 创建、销毁、对象归还及旧身份拒绝 |
| `--close-early` | 环境创建中请求关闭，等待回调责任归还 |
| `--close-controller` | 控制器创建中请求关闭，等待实际进程退出 |
| `--missing-runtime` | 使用固定不存在的独立运行库路径验证不可用反馈，预期退出码 4 |

启用 `BUILD_TESTING` 的 GUI 构建可运行 CTest `ui.run_status`。它在构建目录内创建独占临时配置，传入真实状态面板包，用固定模型响应检查空闲、待审、无效请求拒绝和读回成功。真实主文档只发起查询，提交和批准由原生验收序列执行。独立 Frame 的越权检查由 `ui.smoke` 覆盖。

例如，在 PowerShell 中等待离线检查并保存回执：

```powershell
$probe = Start-Process .\dafeiyu-ui-probe.exe -ArgumentList '--offline' -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput result.json -RedirectStandardError errors.log
$probe.ExitCode
```

普通成功检查返回 0，失败返回 1，非法参数返回 2，运行库不可用返回 4。JSON 回执的 `processes_exited` 与 `profile_cleaned` 应为真，`pending_callbacks`、`active_frames`、`live_channels` 应为零，`protocol_pending` 应为假。运行失败会保留未能安全清理的配置并报告，不通过强制卸载伪造成功。

中文验证使用真实 WebView2 中的浏览器协议模拟，覆盖组件事件和提交结果；不包括物理键盘、Windows 输入法候选窗或不同机器上的人工体验。浏览器离线模拟只作用于本验证环境，不会关闭电脑网络。
