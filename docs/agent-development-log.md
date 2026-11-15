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

## A01c 当前文本上下文投影复核

状态：当前文本范围复用既有实现，不建立重复历史容器。`Session::begin` 从已提交历史生成本次输入副本；`advance` 只将成功的完整问答成对提交，失败／取消的尝试记录不会混入模型消息。普通通知与运行诊断不携带正文、不参与上下文生成；提交前检查 1 MiB 正文边界，清空只影响当前上下文，不改写已交付运行事实。

这些约束已由本轮 `agent.async_session` 的成功／失败历史、取消、提交超量及清空场景覆盖，不为本次文档复核重复运行。下一步 A02 将结构化模型消息接入同一通路；A04／A05 必须另行保留已发生或未知的动作事实并投影到后续上下文，不能把当前文本失败整轮丢弃规则直接用于副作用工具。

## A02a-1 统一模型消息与请求校验

状态：已完成类型和离线校验入口，尚未接入 Provider 工具请求。新增系统消息、工具调用和结果、请求定义、结束原因及可选用量；旧文本消息聚合初始化保持兼容。校验完整对话中的调用 ID 配对、重复调用、角色顺序、参数对象和容量，拒绝未闭合调用后再次请求模型。正文与工具元数据合计最多 1 MiB，1024 条消息、64 个工具和每条最多 64 项调用。

Debug 必要目标 `dfy_model_messages_tests`、`dfy_model_client_tests`、`dfy_async_session_tests` 构建成功；`^(model\.(messages|complete)|agent\.async_session)$` 3／3 通过。新测试仅使用固定内存消息，原协议与会话使用既有替身。下一步实现完整响应校验，再接两家 Provider 的结构化编解码。

## A02a-2 响应完整性边界

状态：已完成。统一响应最多 8 MiB、64 项工具调用；校验角色、结束原因、唯一调用 ID、合法名称和完整对象参数。只允许 `tool_calls` 结束原因交付调用，长度截断、过滤或普通文本结束夹带调用均拒绝；无调用的截断和过滤结果保留明确原因，后续运行控制器据此停止。

Debug 仅重建并运行 `model.messages`，1／1 通过，覆盖完整调用、重复、截断、空文本、容量及结束原因；未运行其他测试或真实模型。下一步 A02b 增加纯协议转换，HTTP 仍由原传输边界负责。

## A02b 供应商结构化编解码

状态：已完成纯协议转换。依据 [DeepSeek Tool Calls](https://api-docs.deepseek.com/guides/tool_calls/) 与 [Qwen Function Calling](https://help.aliyun.com/zh/model-studio/qwen-function-calling) 的工具定义、调用及结果格式，增加 `encode_request`／`decode_response`。两家思考关闭字段仍独立选择；只读可选用量，缺失时保持未知，负数拒绝。工具执行和重试不进入协议层，暂不启用思考或多模态。

Debug 仅构建 `dfy_model_protocol_tests` 并运行 `^model\.protocol$`，1／1 通过。固定样本覆盖两家请求方言、工具结果回填、普通文本兼容、用量缺失／非法、截断和损坏响应。供应商在线兼容性仍属 M05“仅记录，不执行”，本单元没有读取密钥或访问模型服务。下一步把统一协议连接到现有 HTTP 和异步生命周期入口。

## A02c-1 结构化请求复用传输

状态：已完成同步连接。`complete_response` 在读取凭据前校验、编码请求，复用原单次 HTTPS、状态检查和凭据校验，再解码完整响应。旧 `complete` 接口继续只接受文本；新增工具字段传入旧接口时明确拒绝，避免静默丢弃。

`dfy_model_client_tests` 仍独立编译协议与校验实现，并以原 `post_json` 替身替代传输，不链接 WinHTTP。Debug `^model\.complete$` 1／1 通过，覆盖双服务结构化调用、非法输入不进入传输和原文本／错误行为；使用虚构测试凭据，未读取真实凭据。下一步复用原异步操作与退出边界。

## A02c-2 结构化异步入口与 A00b 固定序列

状态：已完成。`AsyncClient` 增加显式 `Backend` 和 `submit_request`，原 Future、结果／退出 Operation、取消优先、保留槽与诊断继续复用；`ModelRequest::response()` 借用完整响应。旧文本调用仅做适配，不支持的结构化字段在提交前拒绝；空结构化后端明确拒绝，不能因遗漏测试注入而回退到真实服务。

A00b 随本单元复用原异步测试加入固定响应序列：工具调用 → 工具结果 → 最终文本，以及迟到响应取消、空后端和不支持字段检查，不建立另一套测试框架。Debug 必要构建后 `^(model\.async|agent\.async_session|app\.async_chat)$` 3／3 通过；修正测试中可选无符号值比较的编译警告后相关目标重建干净。下一步 A03 工具描述、参数校验与授权目录。
