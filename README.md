# 大肥鱼

项目正在按最小 Agent 基线重新建设，采用 C++20、CMake 和 VS2022。

## 当前审查点：构建骨架与 VS 分组

当前只有一个可执行目标 `dafeiyu`，入口位于 [app/main.cpp](app/main.cpp)。启动后显示提示，输入 `/exit` 或结束标准输入即可退出；其他非空输入会收到未知命令提示。模型调用在后续审查点实现。

VS 解决方案当前按实际目标分组：

```text
dafeiyu
├── App
│   └── dafeiyu
│       └── Source
│           └── main.cpp
└── CMakePredefinedTargets
    ├── ALL_BUILD
    └── ZERO_CHECK
```

Agent、Model、Platform、SDK、Plugins、Tests、ThirdParty 分组随对应功能和目标逐步加入，不预建空库。根构建仅引用 `app/`。

## 构建与运行

需要 CMake 3.28 或更新版本，以及安装了 C++ 桌面开发工具的 VS2022。在项目根目录执行：

```powershell
cmake --preset windows
cmake --build --preset windows-debug
.\build\rebuild\windows\bin\Debug\dafeiyu.exe
```

在 VS2022 中打开 `build/rebuild/windows/dafeiyu.sln`；默认启动项目为 `dafeiyu`。Release 构建使用 `cmake --build --preset windows-release`，产物位于 `build/rebuild/windows/bin/Release/`。

新预设使用独立的 `build/rebuild/windows/`，避免复用旧工程缓存和解决方案。构建不需要下载第三方依赖。

## 旧工程归档

- 旧工程基线为 `f7a4fddf4272e8d5295db86584c8d13d44077232`。
- 该基线的全部 130 个受版本管理文件已原样保存到 [Old](Old/README.md)，包括旧配置、示例、测试和设计记录。
- 根目录保留忽略规则和通用中文注释规范；旧版本的对应文件也保存在 `Old/` 中。
- 旧构建缓存和运行数据保留在原位置，未归入版本控制。旧缓存包含原源码路径，应使用新的构建目录验证归档工程。
- `Old/` 不参与新根工程构建；如需验证旧工程，应将 `Old/` 作为独立 CMake 源码目录。

阅读旧实现请从 [旧 README](Old/README.md) 或 [旧架构说明](Old/docs/architecture.md) 开始；旧设计不作为新框架的强制需求。

## 已确定的新基线

1. Agent 只依赖独立模型接口；默认 Provider 随程序发布，空插件目录也能运行。
2. 首批覆盖 DeepSeek 和 Qwen 模型服务，先实现一次性完整文本响应。供应商差异留在模型层，端点、模型名称和凭据引用可配置。
3. 参考 pi 的模型层、Agent 核心和扩展层分工，不直接移植其完整功能集。
4. 基础设施包含运行时插件管理和最小事件系统；插件采用同进程可信 DLL 与 C ABI，忙碌时拒绝卸载。
5. 插件定义及默认配置随包发布，用户配置按插件分开，运行时信息汇总在内存中。
6. 记忆、工作等本体固有能力在基础框架通过审查后逐项添加。

DeepSeek 和阿里云百炼 Qwen 都提供兼容的 Chat Completions 接口，可复用基础请求与响应处理；具体兼容边界在接入阶段逐项验证。Qwen 的服务地域、端点和凭据须匹配，届时通过配置明确选择，不将地址或模型版本写死在 Agent 中。

- [DeepSeek 官方接口说明](https://api-docs.deepseek.com/)
- [Qwen 官方 Chat 接口兼容说明](https://help.aliyun.com/zh/model-studio/compatibility-of-openai-with-dashscope)
- [本次参考的 pi 版本](https://github.com/earendil-works/pi/tree/b2aabe02966a6b4528c7498a3bb406c14396932d)

## 后续审查顺序

归档 → 新构建骨架与 VS 分组 → 模型配置 → 单次真实调用 → 最小 Agent 终端流程 → 事件机制 → 插件发现 → SDK 与加载 → 安全卸载 → 本地包管理 → 联通验收。

这些是逐次审查点，复杂步骤继续拆分。每次只提交一个可审查的功能点，完成后停止，等待用户审查再继续。开发及注释要求见 [AGENTS.md](AGENTS.md)。
