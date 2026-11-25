# 大肥鱼原生插件 SDK

此目录提供 Windows x64 的 C ABI。插件为同进程可信原生 DLL，推荐使用 VS2022、C11 和 CMake 3.28 或更新版本。C++ 插件同样可以使用这些头，但不能跨边界传递 C++ 对象、异常或分配器所有权。

## 独立构建观察示例

本目录的 `examples/observer` 自带独立构建文件，只依赖相对路径下的 `include`：

```powershell
cmake -S .\examples\observer -B .\build-observer -G "Visual Studio 17 2022" -A x64
cmake --build .\build-observer --config Release
```

生成包位于 `build-observer/Release/example.observer`。在运行中的宿主输入 `/plugins install "生成包的绝对路径"`，再明确 `/plugins load example.observer`。示例名称和配置都在包中，宿主没有固定必需插件清单。

## 独立构建状态面板示例

在已安装的 SDK 目录中，用相同的公开头编译纯 C 工具生产方。

```powershell
cmake -S .\examples\status -B .\build-status -G "Visual Studio 17 2022" -A x64
cmake --build .\build-status --config Release
```

生成包位于 `build-status/Release/example.status`。用 `/plugins install` 和 `/plugins load example.status` 明确启动。面板演示无参只读工具加可确认的幂等状态变更，输出 `idle` 或 `focus`，可接宿主的 `/task` 做实际读回验证。结果可先于退出到达，票据仅在实际退出后释放。

## 入口与生命周期

公开头文件位于 [include/dafeiyu](include/dafeiyu)。[plugin_abi.h](include/dafeiyu/plugin_abi.h) 的 v1 查询接口保持冻结；正式实例使用 [plugin_abi_v2.h](include/dafeiyu/plugin_abi_v2.h) 的独立 v2 导出及精确版本查询。定义 `DFY_PLUGIN_BUILD` 以导出入口。

每次查询均核对名称长度、精确版本、公共头大小和全部必需函数。查询表只在相应模块持有期间有效。未知能力返回不支持并清空输出，不通过强制转换读取未知布局；主 ABI 与各能力自己的版本分别协商。

[lifecycle_abi_v2.h](include/dafeiyu/lifecycle_abi_v2.h) 规定创建、启动、停止请求、停止轮询和单次销毁。停止请求成功只表示接受请求，`STOPPED` 才表示插件自己的任务已退出；停止失败不能继续释放仍被使用的代码。实例由创建方销毁，不能跨 CRT 释放对方分配的内存。

插件清单可以声明 `"dependencies": ["example.provider"]`。宿主加载前检查所选包的传递依赖及环路，直接提供方须已显式加载。启动成功后绑定实际实例，该关系进入 Hook 排序及卸载检查。

## 宿主能力

| 能力 | 当前版本和职责 |
|---|---|
| `dfy.references` | 1：独立持有和释放，完整域／编号／代次检查 |
| `dfy.types`、`dfy.values`、`dfy.structures` | 1 为冻结基础契约，2 提供完整结构及组合访问 |
| `dfy.arrays`、`dfy.maps`、`dfy.constraints` | 1 为冻结标量约束，2 提供完整组合元素约束 |
| `dfy.enums`、`dfy.unions` | 1：闭合枚举、显式稳定标签及有界反射 |
| `dfy.reader` | 1：类型化异步读取、结果及载荷 |
| `dfy.operation.state`、`dfy.operation.cancel`、`dfy.operation.notify` | 1：目前以完整读取请求为宿主对象，不能传入任意值或处理句柄 |
| `dfy.subscriptions` | 1：独立订阅接纳、关闭和状态 |
| [信箱](include/dafeiyu/mailbox_abi_v2.h) | `dfy.mailbox@1`：启用后可接入本体，按独立地址交付、去重及读取 |
| [信箱文本](include/dafeiyu/mailbox_abi_v2.h) | `dfy.mailbox.text@1`：从已收信件读取分类、版本和单文本正文 |
| [事件解析与主信箱策略](include/dafeiyu/routed_event_abi_v2.h) | `dfy.event.routes@1` 登记事件契约和解析器，`dfy.mailbox.rules@1` 声明主信箱的分类、优先级及唤醒 |
| [子会话](include/dafeiyu/agent_abi_v2.h) | `dfy.agent@1` 提供独立会话、排队提交与退出读回。第 2 版支持标记面向玩家的回合以装配本体人格，第 3 版支持为单次请求指定低强度或关闭思考。`dfy.agent.facts@1` 接纳绑定事实和原工作回执，版本 2 增加有序接纳、最终实效与原号读回。 |
| [后台推进](include/dafeiyu/activity_abi_v2.h) | 插件可公开 `dfy.activity@1`，宿主公平调度非阻塞步骤，每个实例最多排队一步 |
| [Hook](include/dafeiyu/hook_abi_v2.h) | 插件可公开 `dfy.hooks@1` 控制模型请求、工具启动、工具结果，以及 `dfy.hook.text@1` 改写指定阶段的文本字段 |
| `dfy.groups` | 1：宿主明确授予组时加入、退出和查看本成员状态 |
| `dfy.processing` | 1：宿主明确提供处理记录时的开始、业务结算、实际退出、取消和载荷 |

各能力以对应头文件的结构和函数注释为准：[数据](include/dafeiyu/data_abi_v2.h)、[事件](include/dafeiyu/event_abi_v2.h)、[组](include/dafeiyu/group_abi_v2.h)、[处理](include/dafeiyu/processing_abi_v2.h)。普通观察实例默认获得订阅和读取能力，不自动承担必要业务参与者职责，也不会自动获得组或处理授权。

所有宿主服务只在创建它的宿主线程、有效调用绑定内使用。后台线程不得直接调用宿主表。完成通知延后交付，发布位置不会内联调用观察者；释放客户端引用不等于取消业务或结束宿主责任。

字符串和字节视图按明确长度使用，不能假设以零结尾。借用视图只在对应持有有效期间使用，保留载荷会继续占其完整读取或处理记录的预算。描述引用与事件数据引用分别管理。未知、跨实例或过期句柄必须视作失败。

订阅引用释放不关闭订阅，组成员引用释放不退出成员；使用明确关闭／退出入口或实例停止协议。组成员退出不关闭整个组。已领取的数据与尚未领取的交付分别管理，不因成员离开自动重放已经开始的处理。

处理记录的业务结果和实际退出分开。取得顺序资格、成功读取、成功报告业务结果都不能替代 `finish`；只有消费者确定本项处理不会继续执行后才能确认退出。处理控制引用不可复制，不能提前丢弃控制句柄来解除忙碌。等待、结果保留、通知和引用都有容量上限，资源耗尽应作为可报告的拒绝处理。

## 插件循环与定向收件

在启动阶段调用 `dfy.mailbox@1` 的 `attach` 取得收、发地址。宿主绑定当前实例与本体的发送权。`post` 从 1 开始使用连续序号，容量不足保留原编号和正文，待后续活动步骤重投。`poll` 每次转交队首和一项独立载荷引用。通过 `dfy.mailbox.text@1` 读取分类、版本和正文，复制需要留用的数据后释放引用。`ACCEPTED` 和 `REPEATED` 是本进程内的接纳回执，业务结果需通过关联请求编号另送。

在 `dfy.activity@1` 的 `step` 中推进收件、原生工作或子会话。`dfy.agent@1` 的 `submit` 复制有界输入并排队，宿主在离开原插件调用后派发相同执行器。每个子会话独立保存上下文，工具范围限本实例当前注册。插件通过 `status` 查询原回合，发出 `cancel` 或 `close` 后仍由宿主推进实际退出。忙碌实例拒绝直接卸载。[子会话契约](include/dafeiyu/agent_abi_v2.h) · [活动契约](include/dafeiyu/activity_abi_v2.h)

插件可另行查询 `dfy.agent.facts@1`。首次用 `DFY_V2_FACT_BASELINE` 并在 `input.prompt` 提交 `{"set":{"goal":"取材"},"closed":[]}`。后续沿同一作用域递增序号，重复提交原帧可取回原请求号。断序返回 `DFY_V2_NOT_FOUND` 和所需序号，可补齐缺口或提交新基线。工作用原代次、原工作号和连续修订分别登记接纳、实效和退出，后到的实收可在退出后追加。切回原提交入口须等当前回合退出。再次提交增量时，宿主使用当前镜像重建基线。[C ABI](include/dafeiyu/agent_abi_v2.h) · [回退回归](../tests/plugins/plugin_mailbox_dll.cpp)

协商 `dfy.agent.facts@2` 后，插件可在实际派发前调用 `settle_ordered` 登记原工作与本代次递增的首次接纳序号。容量不足时先结算已有工作，同一原件重试用原序号。采用有序接纳的代次，后续新工作继续使用有序入口。实效仍待查时可沿原号多次更新。领域方核实最后一份实效后，在该版实效上设置 `DFY_V2_WORK_FINAL_EFFECT`，实际退出单独登记。宿主至多保留 128 个活动工作，每个保留 16 版，满额时归档已退出且实效最终的工作。最近 128 个归档支持原号重传，窗口外旧序号会被拒绝。

`read_receipt` 按原会话、代次和阶段接纳号读回数据。调用方传入缓冲区，可先以零容量取得所需字节数。活动记录带原正文，归档记录带阶段和正文 SHA-256，超过保留窗口返回 `DFY_V2_NOT_FOUND`。读取沿用原实例权限，期间不触发模型、工具或游戏工作。[宿主索引](../apps/host/src/binding_facts.cpp) · [跨 DLL 用例](sdk/tests/fixtures/query_v2/mailbox_fixture.c)

关闭子会话后，可沿原实例和原会话号补报已登记工作的实效、退出或只读查证。宿主保留至多 16 份工作索引，活动和退役绑定共用该额度。新绑定首次登记工作时可回收已全部最终结算的退役索引。全为未决时会话问答仍可使用，新增工作返回容量不足，已有工作继续结算。插件实例卸载后撤销该实例的回执路由。[子会话生命周期](../apps/host/src/subagents.cpp)

## Hook 注册与改写规则

`describe` 在启动后枚举，最多 32 项。声明主会话、所属子会话或其他子会话范围。宿主加载时完成依赖与循环校验，按稳定拓扑顺序发布各点；依赖已满足的处理器遵循插件和处理器的注册顺序。实际调用持有顺序快照与原实例代码。运行诊断记录接纳时的公开登记版本，包含顺序、依赖、包版本和注册范围。[Hook 契约](include/dafeiyu/hook_abi_v2.h)

`dfy.hooks@1` 的 `decide` 同步返回 `ALLOW`、`DENY` 或工具启动专用的 `CONFIRM`。等待由当前运行的确认账本管理，批准后按原工具和精确参数再校验。工具结果阶段的决定控制后续调度，已发生的效果继续由原票据记录。

| 文本阶段 | `field` |
| --- | --- |
| `INPUT` | `prompt` |
| `MODEL_REQUEST` | `system` |
| `MODEL_RESPONSE` | `content` |
| `TOOL_ARGUMENTS` | 模型可见的工具名，`value` 为原 JSON 参数 |
| `REPLY` | `answer`，仅用于最终投递 |

`rewrite` 使用宿主提供的缓冲区写 UTF-8，`DFY_V2_TEXT_HOOK_UNCHANGED` 保留原字段，零长度表示显式清空。独立改写者同时提出不同值时，该字段失败。只有声明 `CHAIN` 并依赖当前全部改写者的处理器才能接续。宿主重新验证改写后的模型或工具输入，参数授权及批准从最终候选计算。字段转换回调同步返回，后台工作由活动接口调度。

## 包布局

包包含 `plugin.json`、入口 DLL、可选默认 JSON 配置和必要私有文件。清单采用版本 1，声明插件 ID、名称、版本、ABI、包内入口及可选默认配置。私有 DLL 依赖放在入口 DLL 同目录；宿主加载时使用该目录和 Windows 系统目录的受控搜索。

路径必须位于包内，不接受链接或重解析点。默认配置随包发布，用户配置保存在宿主独立的用户数据目录。实际生效配置是创建实例时的只读快照；目录刷新不替换运行实例，重新加载产生新实例身份。

发布 Release 包，使用与宿主兼容的 x64 工具链和运行库。Debug DLL 仅用于开发验收，需要本机相应开发运行库。源码中的中文注释和公开头文件使用 UTF-8；此 SDK 头和纯 C 示例带 BOM，以便独立 MSVC 消费者识别。

## 可选陪伴角色声明

活动的可信插件可额外发布 [dfy.companion 版本 1](include/dafeiyu/companion_abi_v2.h)。宿主在原实例的调用保护内复制局部状态工具名与状态主题，并向同一代工具目录验证只读范围。此声明只提供发现信息，开局和委托仍由宿主显式接纳。缺少此接口的 v2 工具包沿用原生命周期。

`dfy.companion.routes` 是独立的可选表。已配对实例可为 `player.inbox` 或 `visit.inspect` 返回本包当前登记的局部工具名，缺少用途返回 `NOT_FOUND`。宿主重新核对工具所属代次和只读／变更类别。
