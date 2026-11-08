# 插件 SDK v0.1

## ABI 与生命周期

`dafeiyu/abi.h` 为 C 兼容二进制接口；宿主、四个示例插件和 `plugin.hpp` 封装均用 C++20 编写。唯一导出入口为 `dfy_plugin_query`，通过 ABI 版本与 struct_size 验证后得到 create/start/invoke/stop/destroy 函数表。ABI、服务接口、插件发行版本与持久 schema 版本分别管理。

本版以同平台、同架构、相同 SDK 和匹配工具链构建为支持基线；不承诺任意编译器二进制兼容。不得跨边界传递 STL 对象、异常或需要对方释放的内存。字符串为 UTF-8 指针+长度；call、input、context、sink 及 emit 参数只在对应同步调用期间有效。host 表与其中数据目录/实例字符串直到插件 destroy 前有效。

使用 `DFY_PLUGIN(MyPlugin)` 导出 C++ 适配器。构造函数中调用 `host.provide(service_id)` 注册清单声明的服务。start 成功前不暴露能力。stop 必须停止接纳新工作、收束已有资源；destroy 必须不抛异常。C++ 适配器在所有 ABI 入口截获异常。构造失败由 RAII 回收成员；start 部分失败仍会收到 stop。

插件不得在 DllMain 内做复杂初始化，也不得保留 output 回调、在 invoke 返回后继续输出或在卸载后运行自己的线程。v1 同步接口中的 deadline/cancellation 是协作式的，使用 `call.checkpoint()` 响应。已经发生的副作用要先返回回执，不能用事后取消覆盖。

所有注册由宿主按插件实例持有；v1 不提供动态撤销单个服务或热重载，停止实例时统一撤销。`requires` 是服务 ID，不是插件加载顺序；宿主验证缺失依赖和环。当前宿主只允许一个服务 ID 的一个提供方。

## 清单

每个模块有相邻 JSON 清单：id、version、abi_version、library、requires、services。library 必须解析在插件目录内。插件导出的 ID 与发行版本必须与清单一致；服务的 kind 和 version 也必须受宿主支持。清单声明、实际注册必须一致。

`services[].kind` 支持 model/channel/memory/action。action 额外声明 effects、input_schema；effects 为 read/local-write/external-write。描述能力不授予权限，实例 profile 独立控制 allowed_actions 与 allowed_effects。

输入 schema v1 是 JSON Schema 的明确受限子集：根为 type=object、additionalProperties=false、properties、required；字段只支持 integer/string/boolean，integer 可声明 minimum/maximum。其它关键字或类型在加载阶段拒绝。内置示例为 `plugins/action_add/`。

## 方法

### model.generate

输入：foundation、memory、input、results、actions、turn。依次输出带 type 的 JSON 事件。支持 text_delta、usage，以及恰好一个 tool_call 或 final。

- tool_call：action 为服务 ID，arguments 为对象。
- final：text 为最终文本；goal 可为 met/partial/unmet/uncertain，省略保持 uncertain。
- usage：用量事实或未知值；脚本模型不估算真实供应商 token。

宿主限制每次调用最多 256 个输出事件、合计 1 MiB。invoke 返回失败或取消时不会使用不完整模型决策。当前只保存完整返回后的事件，不把硬崩溃前尚未提交的流声称为持久事实。

### action.readiness / action.execute

readiness 返回一个 `{available, reason?}` 对象；不授予权限。execute 输入通过清单 schema 校验，输出一个对象：status、effects、coverage、data、sources、receipts。三项状态的取值见 `sdk` 及 `core/runtime.cpp`；动作结果、覆盖范围与外部副作用分别表达。

### memory.snapshot / memory.validate

snapshot 返回 revision、instance_id、persona、sources。validate 接收已保存快照，返回 `{valid: bool}`。最小插件只在新库中写入 seed_persona，重启或配置变化不覆盖已有版本；尚无成长提交方法。

### channel.ingest / channel.deliver / channel.query_delivery

ingest 输入 request_id/text，返回 request_id/text/actor/conversation；核心验证 actor 是否为 profile 的模拟 owner。该接口只用于本地模拟，不具备真实平台认证。

deliver 接收稳定的 id/target/text，返回 id/status 和已发送时的 receipt_id。status 为 sent/not-sent/unknown。相同 ID 的不同计划必须拒绝。query_delivery 按 id 返回持久回执，无法确认时返回 unknown。未知效果绝不能按“没有查到回执”转换为“可以重发”。

## 独立编译

先安装宿主 SDK，在独立 CMake 工程中：

```cmake
find_package(dafeiyu 0.1 CONFIG REQUIRED)
add_library(my_plugin MODULE plugin.cpp)
target_link_libraries(my_plugin PRIVATE dafeiyu::plugin_sdk)
```

SDK 导出头文件与 nlohmann_json 依赖；不要求链接核心或复制核心状态。`tests/sdk_consumer/` 包含完整清单、CMake 和 C++ 代码，并由分发测试独立编译和加载。
