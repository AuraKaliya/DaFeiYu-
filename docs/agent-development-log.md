# Agent 基础能力开发记录

开发起点：`cb293f1`。用户于 2026-11-15 认可 [计划](agent-capability-development-plan.md)，并授权从 A00a／A01a 持续开发到该计划完成；每个独立功能分别验证与提交。所有模型测试使用替身，真实模型表现仅记录、不执行。U01 已完成，本轮仅按依赖衔接应用服务，不包含整套 UI 实现。

## A00a 离线测试入口复核

状态：已完成，复用现有替身，不新增测试框架或改动产品模型路径。

- `tests/model_client.cpp` 提供 `dfy::http::post_json` 替身，使用测试专用虚构凭据和 `example.invalid` 地址；`dfy_model_client_tests` 单独编译协议实现，不链接 `dfy_http`／WinHTTP。
- `model::AsyncClient` 与 `agent::Session` 已支持显式 `Completion` 注入。现有 `model.async`、`agent.async_session` 和 `app.async_chat` 均注入返回、异常或受控延迟响应；不会依赖真实 Provider。产品省略注入时仍使用默认 Provider，因此后续测试必须继续显式注入，不允许把省略注入作为测试配置。
- 本次实际只构建 Debug `dfy_model_client_tests`、`dfy_async_model_tests`，运行 CTest 正则 `^model\.(complete|async)$`，2／2 通过；未读取真实凭据、未运行真实模型或全量测试。

本机 CMake／CTest 使用 VS2022 附带的 `C:/Program Files (x86)/Microsoft Visual Studio/build_tools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`，复用构建目录 `build/rebuild/windows-tests`。后续记录中的目标与正则均对应此目录，除非单独注明。

下一步为 A01a：运行与步骤身份，保持现有单次问答行为及 Operation 状态来源。
