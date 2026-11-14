# 大肥鱼运行说明

支持 Windows 10／11 x64。保留解压目录中的程序、运行库 DLL、配置和示例目录，直接运行 `dafeiyu.exe` 即可使用内置 DeepSeek 配置。程序无需安装插件即可问答。

## 模型与启动

在 Windows 用户或系统环境变量中配置对应服务的凭据，更新后新开终端：DeepSeek 使用 `DEEPSEEK_API_KEY`，Qwen 使用 `DASHSCOPE_API_KEY`。配置文件只保存环境变量名称，不保存密钥。运行期间不自动读取其他进程刚修改的环境。

在运行目录中执行：

```powershell
.\dafeiyu.exe
.\dafeiyu.exe --chat .\config\qwen.example.json
.\dafeiyu.exe --check-config .\config\deepseek.example.json
.\dafeiyu.exe --complete .\config\qwen.example.json .\prompt.txt
```

`prompt.txt` 使用 UTF-8。配置检查只检查文件格式，真实服务可用性由实际请求验证。可以复制示例配置，修改服务地址、模型、凭据环境变量名称或超时，再用 `--chat` 指定副本。

不带参数时，程序从自身所在目录寻找默认配置；从其他工作目录启动也不影响默认资源定位。明确给出的相对配置、包或数据路径以当前工作目录为准。

## 会话操作

| 输入 | 行为 |
|---|---|
| 普通文字 | 空闲时开始一轮问答 |
| `/cancel` | 取消当前问答，保留此前已提交上下文 |
| `/clear` | 空闲时清空当前上下文 |
| `/exit` | 请求取消当前问答，等待实际调用和插件停止后退出 |

网络等待期间仍可处理本地管理命令。新的普通问答会明确报告忙碌，不会进入隐藏队列。取消先确定问答结果，已经送出的远程调用仍须等待系统调用退出；取消不承诺撤销服务端已执行的请求。默认不自动重发失败请求。

只有完整成功回答才与本轮提问成对提交；失败和取消保持原历史。当前上下文正文总量上限为 1 MiB，达到限制后可 `/clear` 开始新的上下文。当前没有持久化会话，退出后不会恢复历史。重定向输入正常结束时，程序会等待已经接纳的回答完成。

## 插件管理

插件是同进程可信原生 DLL。默认包目录为程序目录下的 `plugins`；安装和发现都不会自动运行插件。分发目录的 `examples/plugins/example.observer` 是独立观察示例，默认不装入包目录。

以下安装命令中的 `D:\DaFeiYu` 请替换为实际运行目录：

```text
/plugins list
/plugins install "D:\DaFeiYu\examples\plugins\example.observer"
/plugins refresh
/plugins load example.observer
/plugins unload example.observer
/plugins remove example.observer
```

默认用户设置位于 `%LOCALAPPDATA%\DaFeiYu\plugins\<id>\settings.json`，为 JSON 对象。包内默认设置和用户设置按顶层键覆盖。移除包会保留该用户设置，不把它复制进其他包。

需要独立运行环境时指定目录：

```powershell
.\dafeiyu.exe --chat .\config\deepseek.example.json --plugins "D:\应用数据\插件" --data "D:\应用数据\用户设置"
```

本地安装只接受普通本地目录，拒绝链接、重解析点、已有 ID 和覆盖更新；默认上限为 4096 个子项、512 MiB 文件数据及 32 层目录。完整检查和暂存复核后才发布，坏包单独报告，不阻止本体问答。

卸载明确区分完成、正在停止、忙碌、仍被使用和失败保留。等待下一事件的观察插件可正常停止；仍在执行的调用或仍被其他实例使用的代码不会被强行释放。宿主建立了运行时依赖时，可以使用 `/plugins unload <id> --linked` 请求先停止使用者。

`remove` 先尝试卸载，未完成时保持包文件；已卸载包先退出发现目录，再清理文件。只读或权限问题造成部分清理时，会显示私有残留路径。修复文件属性或权限后，再执行相同 `/plugins remove <id>` 继续清理。不要把“正在停止”或“清理未完成”当作已经全部释放。

## 分发内容

| 位置 | 内容 |
|---|---|
| `dafeiyu.exe`、同目录运行库 DLL | Release 程序及编译器提供的运行库 |
| `config` | 不含密钥的 DeepSeek／Qwen 示例 |
| `plugins` | 用户明确安装的包，初始为空 |
| `examples/plugins/example.observer` | 可安装的观察示例包 |
| `sdk` | C ABI 头文件、开发说明及独立可构建示例 |
| `licenses`、`THIRD_PARTY.md` | 第三方版权和许可证说明 |

程序运行不依赖源码目录、`Old` 或测试目录。SDK 构建方式见 `sdk/README.md`。Windows 系统组件提供 WinHTTP 和 UCRT；分发附带的是编译器提供的 Release 运行库，不包含开发用 Debug 运行库。
