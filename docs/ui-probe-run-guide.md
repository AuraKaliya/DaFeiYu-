# 大肥鱼 U01 验证窗口运行说明

`dafeiyu-ui-probe.exe` 提供桌面主面板、房间预览和原生容器探针。终端问答使用 `dafeiyu.exe`。

## 桌面主面板

在 GUI 构建的可执行文件旁打开 PowerShell，运行 `.\dafeiyu-ui-probe.exe --main-panel`。默认使用同目录 `config/deepseek.example.json`；也可通过 `--main-panel` 后的 JSON 文件路径指定已有配置。模型凭据沿用配置的环境变量。

标题下方的「书房 · 书桌旁」由当前应用实例的房间和身体快照提供。打开或刷新面板会读取位置，第一次模型请求由用户主动发送消息触发。

状态显示「可以聊天」后，输入消息并发送。回复期间仍可编辑下一条草稿，或用输入框旁的「停止回复」停止本轮。取消先结算时，页面显示「正在收尾」，原工作实际退出后恢复发送。空闲时选择「清空对话」并确认，主会话从空历史继续，输入框保留草稿。关窗重开会创建新会话，当前进程内最多显示最近 16 轮，正文合计至多 2 MiB。

回复期间，输入区默认「本轮补充」。选择「追加本轮」后可查看它何时进入下一次实际模型请求。取消或超限留下的补充可以加入草稿，再由用户发送。选择「下一轮」再发送，原话会加入独立的待发列表。正常完成后按顺序逐条发送，停止或失败后列表显示「已暂停」。可以移除其中一条，也可以选择「继续发送」。刷新会恢复本进程的补充和待发项。空闲清空只清会话历史，待发项与当前草稿会保留。

同一窗口刷新会保留草稿。正式面板使用独立的本地来源，原命令留在当前历史项中。重载先查询业务线程的命令水位，已登记的操作查回执，尚未登记的操作按原编号、原参数补投一次。关闭窗口会回收独立配置。

`ui.main_panel` 用空插件目录和固定响应，在隐藏 WebView2 中核对来源、原消息号、独立停止、实际退出与清空后的模型输入。窄窗口验收覆盖排队、移除、刷新、暂停和继续发送。[主面板验收](../tests/ui_main_panel.cpp) · [待发队列验收](../tests/ui_dialogue_queue.cpp) · [刷新边界验收](../tests/ui_dialogue_recovery.cpp)

## 房间首屏预览

在同一 GUI 构建的可执行文件旁运行 `dafeiyu-ui-probe.exe --room-preview idle` 或 `dafeiyu-ui-probe.exe --room-preview busy`。右上角可以切换两份固定样本。空闲时可直接输入；忙时可选择追加本轮或排到下一轮，独立停止入口保留排队消息。对话旁展开待决定事项可查看依据。输入、决定和停止只改变当前预览样本，切换样本会复位。预览不开启 Application，宿主关闭该页面的 Web 消息入口。

定向 CTest 为 `ui.room_preview_idle` 和 `ui.room_preview_busy`，均使用隐藏 WebView2 和固定浏览器协议检查真实页面。

## 运行环境

需要 Windows x64 和 Microsoft Edge WebView2 Evergreen Runtime。运行目录已包含前端静态资产及 MSVC Release 运行库，不需要 Node.js、源码目录或页面服务器。

若提示 WebView2 Runtime 不可用，从 [微软官方下载页](https://developer.microsoft.com/en-us/microsoft-edge/webview2#download-section) 获取 x64 Evergreen Standalone Installer；提前下载该完整安装器后可离线安装。Edge 浏览器与 WebView2 Runtime 是不同组件。本验证包不自动安装或修改系统运行库。

## 打开界面

直接运行 `dafeiyu-ui-probe.exe`。主窗口使用随程序分发的默认模型配置组装业务宿主，可刷新查看当前或最近退出的运行状态。输入区和独立来源页面用于验证组合输入、对话框和 Frame 消息。组合结束次数与浏览器可信标记分开显示。

关闭窗口会继续回收创建回调、Frame 和浏览器进程，然后清理本次独立临时配置。临时配置位于系统临时目录的 `DaFeiYu-U01` 下，不复用用户浏览器配置。

## 自动验证

自动模式使用不激活的隐藏窗口。交互验收窗口由 Windows 桌面合成器隐藏，以便 WebView2 生成可接收浏览器键盘事件的首帧。宿主核对隐藏标记及前台窗口。输入使用固定浏览器协议。

| 参数 | 验证内容 |
|---|---|
| `--smoke` | 本地资产、外部资源拒绝、Frame 编码与授权 |
| `--offline` | 浏览器离线模拟状态下，从全新配置加载本地页面并完成 Frame 检查 |
| `--interaction` | 等待首帧后验证中文组合输入、Tab／Shift+Tab、Enter、Escape、焦点返回与独立 Frame 回显 |
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
