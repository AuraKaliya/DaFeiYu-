# 插件 SDK 0.5 与 ABI 2

## 公开边界

SDK 的数据层目标为 dafeiyu::contracts，包含 value.hpp、schema.hpp、contracts.hpp 和 RunView。插件作者目标 dafeiyu::plugin_sdk 在其上提供 abi.h 与 plugin.hpp。两者不持有核心实例或数据库连接。核心公共头文件只依赖数据层，内部插件加载和 SQLite 实现不通过该头文件传播。

contracts.hpp 是内建服务方法 schema 的唯一来源。清单声明服务 id、contract、version 即可引用内建定义；若同时显式提供 methods，必须与 SDK 定义完全一致。核心在 ready 前校验 model、strategy、channel、memory、context 槽位以及活动绑定的角色。领域服务继续显式声明自己的方法，核心不枚举领域名称。

策略契约为版本 4，渠道/活动契约为版本 3，动作执行器为版本 1，其余内建契约仍为版本 2。ABI 保持版本 2，profile 与核心存储为版本 4，插件私有存储仍为版本 2。旧核心数据拒绝打开且不改写；没有自动迁移或旧服务契约适配。

RunView v3 提供公开身份、ConversationKey、意图修订、进度、输入、候选、结果、人格快照、原准入动作、用量、observation、model_bindings、error、wait_reason、active_operation、activity_id 和 goal_origin。预算视图含 model_attempts、strategy_steps、nested_calls。策略请求与插件 read_state(run) 使用该视图，不包含绑定闭包、原始 Grant、内部请求、策略私有状态或调度字段。admitted_actions 表示原准入集合，实际调用仍重新检查当前授权。view 不签发权限，SDK C++ 类型也不直接穿过二进制边界。

## 二进制入口与生命周期

唯一导出为 dfy_plugin_query。宿主检查 ABI、struct_size、插件身份与发行版本，得到 create/start/on_event/quiesce/stop/destroy 函数表。当前只支持 ABI 2，无 ABI 1 适配器。

跨模块只传递 C 函数表、UTF-8 指针与长度、整数和借用回调。调用返回前输入已复制；字符串及错误 sink 不可保留。host 表直到 destroy 前有效。同平台、同架构、相同 SDK 与匹配工具链是支持基线。

生命周期和 on_event 在每插件的宿主执行线程串行运行。start 成功且声明与注册一致后，服务和动作才发布。失败会 stop/destroy 并撤销注册；optional 插件失败可使实例 degraded，必需插件启动失败则拒绝上线。stop 必须收束插件工作线程，destroy 必须不抛异常。

## 清单与服务

清单包含 id、version、abi_version、library、requires、services，以及可选 actions、activities、discovery_prefix。模块路径必须位于插件目录内。requires 使用命名绑定、契约 ID 与版本，profile 将绑定解析为唯一服务。

services 每项包含 id、contract、version；内建契约由 SDK 补全 methods，领域契约必须显式提供 methods。每个方法描述 input/output schema、effects 和 resource，可声明查询方法 query。相同契约与版本的方法定义必须一致。

effects 为 read、local-write 或 external-write。resource 是主资源域，resources 可显式列出完整资源集合且必须包含主域。delegates 可声明后续调用的效果类别与资源上限；默认允许 read 和本方法的效果类别，资源默认为本方法的资源集合。只读方法不能声明写委派，local-write 方法不能声明 external-write 委派。写方法使用 ActionOutcome 报告效果；渠道使用 sent/not-sent/unknown 回执。声明不提供 OS 沙箱，提供方必须如实实现。

实际子许可取祖先、方法声明与当前授权的交集。额外委派的读资源不能扩大写入范围：write_resources 始终限制在当前方法资源与祖先写资源之内。根动作、效果或资源撤销后拒绝后续子调用，已确认的部分副作用仍保留。动作自己的参数、结果、效果、资源与委派独立于执行器信封，权限按经过接纳的动作定义计算。

## 渠道身份与交付绑定

profile 的渠道插件声明 channels 映射，例如 channel.simulator 对应 {instance: simulator.primary, account: simulation-account, allow_reply: true}。每个已注册渠道服务必须有身份声明；instance 在实例内唯一。账号标识是稳定的配置身份，不是凭据，也不能替代真实平台的认证检查。

Host.input(channel_service, key, input) 必须指明来源服务。宿主检查该服务属于发起插件、接受输入已启用，并把外部 actor 映射到获准主体。ConversationKey 由实例、渠道实例、账号和外部会话组成；相同外部会话名不会跨渠道合并。

接纳时记录 DeliveryBinding，包括具体服务与账号、目标、回复关联、提供方版本/配置指纹及授权代次。入队后重启或默认渠道改变，不修改该绑定。身份与权限配置变化推进持久代次；渠道实例移除或改名也记录撤销，重新授予或恢复旧名不能恢复旧绑定资格。渠道实际发送须核对自己的服务/账号与绑定一致，并保留稳定交付 ID。

内建 channel.deliver 仅由核心交付协调器派发，通用嵌套 request 不得绕过交付检查。嵌套 query_delivery 只允许查询本 Run 的原交付操作和原服务。撤权后已发生效果仍能通过受控核对保留，但不能继续外发。

actions 定义模型可见 ID、说明、revision、服务方法绑定、输入输出、效果及资源域。静态动作由清单发布；发现型插件可在 create/start 使用 Host.action 注册获准命名空间中的动作。普通运行阶段不允许新增动作定义；visibility=scoped 的启动期定义可通过 register_scoped 发布与撤销其范围内可见性。activities 声明活动身份、版本和所属 ActivityProvider 服务，profile 提供部署策略。

配置选择项包含 manifest、revision、config，可显式声明 required=false、输入主体映射、允许观察的事实或可提交的核心命令。config 应保存非敏感行为参数和凭据引用；变更行为时更新 revision，宿主也比较配置指纹。

## 事件式调用

on_event 收到 call 时包含 call_id、service、method、input 与描述性 context。插件可立即 complete/fail，也可发起 request 后返回，在 reply 事件中继续。不能阻塞等待同一执行线程上的 reply。

| SDK 方法 | 语义 |
|---|---|
| provide、action | 启动期服务与发现型动作注册 |
| complete、fail、progress | 报告当前票据的结果、结构化错误或可丢弃进度 |
| request | 按声明的命名依赖发起子调用，可指定更短 timeout_ms；以 reply 返回 |
| input | 指明渠道服务并提交平台输入，以 input_receipt 表示已持久接纳 |
| register_job、job_result | 启动前登记后台工作，随后提交终态与接收 job_receipt |
| open_scope、close_scope | 请求范围资格或关闭本人范围，以 scope_reply 返回 |
| register_scoped、release | 登记上下文、动作或逐项释放 |
| request_scoped、read_scoped | 范围内只读依赖请求或公开状态查询 |
| subscribe、acknowledge | 在 Scope 内订阅事件，以登记句柄确认游标 |
| command、command_receipt | 提交获准的行为/活动命令，或查询当前操作的命令回执 |
| read_state | 读取调用所属 Run 或行为状态的副本 |
| cancelled | 查询调用是否取消或过期 |

post 返回 OK 只表示已复制进队列。input、Job 和管理修改必须以相应持久回执为依据。BUSY 表示入口背压，CLOSED 表示资格或生命周期已结束。普通进度不具有崩溃恢复保证；尚无远程实时进度界面。

宿主按回调所属插件和有效 call_id 签发的作用域路由请求。JSON context 不能转授权限。普通只读调用不得生成未计量模型请求；模型由策略提案进入核心流程。嵌套写入登记子操作，以 parent operation + request key 去重；相同步骤键不能对应不同计划。

子 Call 记录父票据；截止时间不得晚于父调用或宿主上限。limits.nested_calls 是同一 Run 的持久共享额度，已接纳的调用尝试在派发前扣减，派发失败或硬重启不清零。模型/策略计数和效果核对仍分别管理，不把此额度称为供应商 token 或费用预算。

父调用正常结束、超时或取消后，普通后代失去继续业务的资格，插件必须协作收束。异步父调用可能收到 terminal_only=true 的 reply 错误；它只用于释放资源和报告原调用终态，不能触发重试或新的业务调用。迟到物理结果仍可记录操作效果。Call 的逻辑结束不释放仍在执行的模块，Job 仅通过显式登记与移交获得独立结果资格。

## 主要契约

| 契约 | 方法 |
|---|---|
| dafeiyu.model | generate，返回 events 数组，其中恰好一个 tool_call 或 final 候选 |
| dafeiyu.strategy | propose、review_finish，返回提案及候选策略状态，或复核 accept |
| dafeiyu.channel | deliver、query_delivery，输入包含稳定 id 和不可变计划 |
| dafeiyu.memory | snapshot、validate、retrieve；最小契约只读 |
| dafeiyu.context | collect，贡献项包含 source、revision、scope、content 和可选 expires_at |
| dafeiyu.activity | evaluate_trigger、on_settlement |
| dafeiyu.action-executor | execute；capabilities 可选 readiness、query、jobs；query 对应 query_operation |
| 后台工作服务 | query_job、reattach_job、cancel_job，使用原工作键与提供方绑定；当前由测试夹具实现 |

记忆写入不是 dafeiyu.memory 的必需方法。测试记忆提供方在同一数据所有者下另行注册 test.memory.changes，提供 commit_change/query_change，消费方显式声明该依赖；最小示例不会以空实现冒充写能力。

模型、策略与一般领域方法的 JSON 业务结构分别约定；ABI 版本和契约版本独立。策略可返回 request_model、invoke_action、wait、finish，复核可 accept 或返回非结束提案。核心执行预算、授权和结束检查，插件不直接写 Run 状态。

ActionOutcome 包含 status、effects、coverage、data、sources、receipts。效果为 none、confirmed 或 unknown；未知写效果进入核对。后台移交使用 type=job_started 与 job_id，既不等于业务成功，也不释放 Job 的结果通道。

## Schema 子集

支持空 schema（任意 JSON）、object、array、string、integer、number、boolean、null；支持 properties、required、布尔 additionalProperties、items、enum、minimum/maximum、minItems/maxItems、minLength/maxLength。字符串长度按 UTF-8 码点计数。schema 深度最多 16，值校验深度最多 32；不支持的关键字在注册时拒绝。

## 独立编译

```cmake
find_package(dafeiyu 0.5 CONFIG REQUIRED)
add_library(my_plugin MODULE plugin.cpp)
target_link_libraries(my_plugin PRIVATE dafeiyu::plugin_sdk)
```

从 sdk::Plugin 继承，构造期间 provide 声明的服务，实现 on_event，并用 DFY_PLUGIN(MyPlugin) 导出。完整独立例子见 tests/sdk_consumer；跨插件异步组合、记忆提交和受控命令分别位于 tests/fixtures/service_consumer、memory_commit 和 command，仅随测试构建。

## Scope 消息与恢复

部署选择项显式给出 scope_permissions，例如：

```json
{"scope_permissions":{"instance":{"resources":["instance"],"registrations":["context","subscription"]},"conversation":{"resources":["instance"],"registrations":["action","context","subscription"]}},"observe":["run.completed"]}
```

open_scope(key, kind, parent_call=0) 支持 instance/conversation/run/call。除 instance 外必须传有效的本人父调用；派生资格还要与父许可相交。scope_reply.result.id 是运行期资格句柄。request_scoped(scope, key, binding, method, input, timeout_ms) 只允许已声明依赖的只读方法，以 scoped_reply 返回；模型、动作执行、写服务均须经 Run 的核心流程。

register_scoped(scope, key, "context", {service}) 或 register_scoped(scope, key, "action", {action}) 返回登记 id；对象必须属于当前插件。release(key, registration_id) 只撤销本人登记，close_scope(key, scope_id) 释放整个范围。相同范围/key 的相同登记可重试，参数改变拒绝。过期范围和登记不能派发新调用；终态/迟到结果仍按既有结算规则记录。

subscribe(scope, key, types, filter={}) 返回登记 id；filter 可包含 run_id 或 conversation_id，只能进一步收窄。facts 包含 key、registration_id 和 page；acknowledge(registration_id, page.cursor) 只能确认本次已发送游标。最多一页在途，每页最多 32 条。先提交消费者自己的事务，再确认；重复事件按事实 ID 去重。

重启以稳定的范围/登记身份恢复，签发新的句柄。scope_restored.result 是新范围描述，registration_restored.result 含新登记 id、scope_id、kind 和 value。插件可重建本地句柄表，或以相同 key 幂等登记。未确认页从已确认游标重新查询，页可能包含新到事件，因此不要依赖原分页边界。关闭范围会停止投递并发送 scope_closed（含 last_sequence）；需要跨 Run 保存观察进度的插件使用部署授权的实例或会话范围。

逻辑关闭不会强行卸载仍在执行的原生模块。生命周期错误只允许释放资源；terminal_only=true 不可用来继续业务。limits.max_scopes、max_registrations 限制活跃登记数量；scope_calls 限制实例/会话每次激活的已接纳调用次数，Run 请求共享 nested_calls 的持久额度。

## 执行器与失败观察

一个执行器可以处理多个独立定义的动作，无需为每个参数 schema 定义一个服务。最小清单仅声明 {id, contract:"dafeiyu.action-executor", version:1}；capabilities 可选 readiness、query、jobs。不支持的能力不要声明，也不必实现空方法。

```cpp
const dfy::sdk::Call call(event);
const dfy::sdk::ActionInvocation invocation(call);
// 已核对 operation_id 与调用上下文一致。
const auto& arguments = invocation.arguments;
// 根据 action_id 与 definition_revision 处理已冻结的定义。
```

execute/readiness/query_operation 均接收 {action_id, definition_revision, arguments, operation_id}。ActionInvocation 校验信封与调用身份一致。execute 返回动作定义约束的 ActionOutcome，或在登记 Job 后返回 job_started；最终 Job 结果同样验证动作输出。查询原操作的资格不依赖已释放的原业务 Scope，以便继续保留已发生效果。

RunView.observation 在失败时包含 kind=model_failed、stage、binding、attempt_id、error；成功时为 model_succeeded。request_model 的 binding 默认为 model，也可选择 view.model_bindings 中的已冻结别名。失败与备用尝试均消耗同一 model_attempts，非法别名、预算耗尽、撤权和未知效果不能被策略改写。最小策略在失败后等待用户；resume 或输入修订允许重新决策。

activity.on_settlement 接收 {activity_id, run: RunView}，不能依赖核心的 bindings、grants、request、limits 或 strategy_state。核心记录反馈成功/失败并完成通知标记；该回调不是核心状态写入口。
