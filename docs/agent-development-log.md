# Agent 基础能力开发记录

开发起点：`cb293f1`。用户于 2026-11-15 认可 [计划](agent-capability-development-plan.md)，并授权从 A00a／A01a 持续开发到该计划完成；每个独立功能分别验证与提交。所有模型测试使用替身，真实模型表现仅记录、不执行。U01 已完成，本轮仅按依赖衔接应用服务，不包含整套 UI 实现。

## A00a 离线测试入口复核

状态：已完成，复用现有替身，不新增测试框架或改动产品模型路径。

- `tests/model_client.cpp` 提供 `dfy::http::post_json` 替身，使用测试专用虚构凭据和 `example.invalid` 地址；`dfy_model_client_tests` 单独编译协议实现，不链接 `dfy_http`／WinHTTP。
- `model::AsyncClient` 与 `agent::Session` 已支持显式 `Completion` 注入。现有 `model.async`、`agent.async_session` 和 `app.async_chat` 均注入返回、异常或受控延迟响应；不会依赖真实 Provider。产品省略注入时仍使用默认 Provider，因此后续测试必须继续显式注入，不允许把省略注入作为测试配置。
- 本次实际只构建 Debug `dfy_model_client_tests`、`dfy_async_model_tests`，运行 CTest 正则 `^model\.(complete|async)$`，2／2 通过；未读取真实凭据、未运行真实模型或全量测试。

本机 CMake／CTest 使用 VS2022 附带的 `C:/Program Files (x86)/Microsoft Visual Studio/build_tools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`，复用构建目录 `build/rebuild/windows-tests`。后续记录中的目标与正则均对应此目录，除非单独注明。

下一步为 A01a：运行与步骤身份，保持现有单次问答行为及 Operation 状态来源。

## A01a 运行、步骤与尝试身份

状态：已完成。新增 [RunId／StepId／AttemptId](../agent/include/dafeiyu/run_identity.hpp) 值类型：会话域和原回合号组成运行身份，步骤和尝试序号分别从一开始。`TurnRequest` 与诊断直接返回同一份身份，不另建操作状态；当前仍是每轮一个步骤、一次模型请求。原通知结构和单次问答行为保持兼容。

复用 `tests/async_session.cpp` 的模型替身，补充跨会话同序号不碰撞、拒绝不消耗身份、清空后递增、失败／取消／迟到返回／宿主销毁后身份稳定，以及原结果和退出操作不被替换的检查。Debug 只构建 `dfy_async_session_tests`、`dfy_async_chat_tests`，运行 `^(agent\.async_session|app\.async_chat)$`，2／2 通过；公开头 UTF-8 BOM 与差异检查通过。未运行全量或真实模型。

阅读顺序：身份值类型 → [Session 接口](../agent/include/dafeiyu/session.hpp) → [会话接纳与诊断](../agent/session.cpp) → [相关测试](../tests/async_session.cpp)。下一单元为 A01b：有界运行事实与只读快照。

## A01b 单次运行事实与快照

状态：已完成当前单次问答范围。`TurnRequest::snapshot()` 返回诊断和按宿主修订排序的事实值：接纳、首次取消请求、模型结束、业务结算、实际退出。五种事实各有固定预留槽，收尾不分配内存，重复推进不追加；后台发生时间与宿主观察顺序分开。多步工具事实在 A04／A05 接入时扩展，不把本单元写成已支持通用执行日志。

Debug 仅构建 `dfy_async_session_tests`，`^agent\.async_session$` 1／1 通过。覆盖查询不推进、通知背压后仍有完整事实、取消后迟到返回、修订连续、快照不占用结果槽及槽容量拒绝。差异检查通过。下一单元 A01c 整理当前文本上下文投影，为结构化消息接入保留同一约束入口。
