# 插件 C ABI v2 查询、引用与操作状态契约

R01a 固定独立查询与静态能力协商，R01b 增加句柄表示、宿主引用服务及借用期契约，均见 [SDK 头文件](../sdk/include/dafeiyu/plugin_abi_v2.h)。[纯 C11 查询夹具](../tests/fixtures/query_v2/query.c)和 [C++20 消费者](../tests/sdk_abi_v2.cpp)验收查询；[借用夹具](../tests/fixtures/query_v2/borrow.c)和 [隔离测试宿主](../tests/sdk_handles_v2.cpp)验收引用行为。生产句柄注册表、实例生命周期和异步操作尚未实现；生产 `PluginModule` 仍只接受 v1，v2 加载与兼容校验属于 R06。

R01c-1 新增独立的宿主操作状态服务，使用[纯 C 状态消费夹具](../tests/fixtures/query_v2/operation.c)和[隔离宿主](../tests/sdk_operations_v2.cpp)验证受理、快照读取、唯一终态和引用寿命。本步不提供生产操作执行、结果载荷读取、取消请求或完成通知。

## 引导入口与布局

- 支持 Windows x64，使用 `extern "C"` 和 `__cdecl`；固定导出为 `dfy_plugin_query_v2`，宏为 `DFY_V2_QUERY_EXPORT`。v1 的头文件、宏、导出与签名保持不变，两份头文件可以同时包含。
- `dfy_plugin_query_v2(requested_abi, &api, error)` 只接受主 ABI 2，返回 DLL 内只读静态 `dfy_v2_plugin_api`；查询不创建实例、不登记资源、不产生业务副作用，重复成功查询返回同一张表。
- 所有查询在同一宿主线程进行。调用方必须保持模块引用，表及其中函数地址借用至模块引用释放，不能自行释放或在释放后访问；错误和名称视图仅在调用期间借用。
- 结构使用 `pack(push, 8)` / `pack(pop)` 并恢复包含方原设置。只用固定宽度整数、字节视图及 C 函数指针，不跨 DLL 传递 STL、异常或 C++ 对象。

| 类型 | 大小／对齐（字节） | 关键位置 |
|---|---|---|
| `dfy_v2_string_view` | 16／8 | `data` 0，`length` 8 |
| `dfy_v2_error` | 260／4 | `length` 0，`text` 4，容量 256 |
| `dfy_v2_api_header` | 8／4 | `struct_size` 0，`interface_version` 4 |
| `dfy_v2_plugin_api` | 24／8 | `struct_size` 0，`abi_version` 4，`architecture` 8，`reserved` 12，`query_interface` 16 |

引导表大小必须精确为 24，主 ABI 为 2，架构为 `DFY_V2_ARCH_X64`（1），保留字段为零，`query_interface` 必须非空。该布局固定，新增能力不往引导表追加字段。

## 静态能力协商

`query_interface(name, interface_version, &table, error)` 按名称和精确版本查询 DLL 实际实现的能力。名称为非空 UTF-8 字节视图，调用方保证指定长度内的内存可读、文本有效；不要求零结尾，按字节及大小写精确匹配，不自动截断或规范化。能力版本为非零 uint32。

每张能力表以 `dfy_v2_api_header` 成员开头。`struct_size` 表示完整能力表大小，`interface_version` 表示该能力的精确版本，独立于主 ABI、插件和事件契约版本。调用方先核对查询状态、错误长度和非空输出，再核对该能力版本规定的精确表大小与版本，之后才访问其余字段；不能仅因表较大就推断兼容。返回表同样是只读静态借用，不能由调用方释放。

引导表仅负责查询插件提供的静态接口。R01b 固定下述宿主引用服务表，实例获取宿主服务的装配接口仍待 R06／R07 定稿；插件查询成功不表示宿主服务已接入。未知能力或版本应明确返回不支持，不能用未实现的占位成功表响应。

## 状态与输出责任

| 状态 | 值 | 含义 |
|---|---|---|
| `DFY_V2_OK` | 0 | 调用成功，错误长度为零；查询返回非空表，`retain` 返回有效持有句柄 |
| `DFY_V2_INVALID_ARGUMENT` | 1 | 必需输出为空；能力名称指针为空、长度为零或能力版本为零 |
| `DFY_V2_ABI_MISMATCH` | 2 | 引导查询不支持请求的主 ABI |
| `DFY_V2_UNSUPPORTED` | 5 | 能力名称或精确版本不支持，或有效来源不允许共享 |
| `DFY_V2_INVALID_HANDLE` | 6 | 引用服务拒绝空来源、部分为零、未知域／编号、过期或已释放的引用 |
| `DFY_V2_RESOURCE_EXHAUSTED` | 7 | 引用服务不能再签发引用，包括容量或标识空间耗尽 |

输出指针必需，所有失败查询均将可写的表输出清空。先校验必需参数，再检查版本是否支持；能力版本不支持使用 `UNSUPPORTED`，不混同主 ABI 不兼容。未知非零状态均按失败处理，不读取失败结果中的表。

`error` 可为空。非空时由调用方分配；成功将 `length` 置零，失败可写入至多 256 字节的完整 UTF-8，也可用零长度只返回状态。文本不要求零结尾，超长诊断应省略或在完整字符边界截断；调用方先检查长度，不能读取未计入长度的字节。任何函数均不得让异常跨越 C ABI。

## R01b 句柄与借用

### 句柄身份与持有责任

`dfy_v2_handle` 是三个 uint64 组成的 24 字节、8 字节对齐标识：`domain`、`id`、`generation` 分别位于 0、8、16。三项全零是唯一空句柄；有效句柄三项均非零。它不含对象地址，不能解引用、持久化、跨进程使用或自行拼装。

- `domain` 标识宿主的签发域，不是插件 ID 或资源所有者。签发域在进程内不复用，宿主重建也使用新域；生产分配器在 R02 实现。
- `id` 标识一条持有引用记录，不是事件、类型或业务对象身份。`generation` 区分同一编号的复用；完整三元组不得再次签发，代次耗尽时停用该编号，不能回绕。消费者只原样传回并按完整值比较引用身份，不根据编号推导对象或权限。
- 原样赋值只复制标识，是同一引用的别名，不增加所有权或延长寿命。借用者不得释放借来的引用；跨回调、挂起或排队保存时，须在借用仍有效时取得独立持有引用，或复制数据到自身存储。
- 句柄不能替代资源归属、实例代次、使用凭证和 DLL 代码依赖登记。各能力还须验证对象种类及允许的操作；这些登记与访问控制由 R02、R04、R06 等对应单元实现。

### 宿主引用服务

`dfy_v2_reference_api` 的接口标识为 `dfy.references`，版本 1；表大小 24、对齐 8，`header`、`retain`、`release` 偏移为 0、8、16，两函数必须非空。这是宿主提供的服务，不能把插件的 `query_interface` 当作宿主服务发现入口。本步仅固定公开类型与契约；表在测试中由隔离宿主显式传给 C DLL，生产服务装配尚未实现。

| 操作 | 成功与失败责任 |
|---|---|
| `retain(source, &owned, error)` | 对仍有效且允许共享的来源签发一个不同的持有引用，指向同一对象；输出的完整句柄与来源不同，后续须单独释放。来源可为调用期间借来的引用。失败将输出全零，不消耗来源或产生残留引用 |
| `release(owned, error)` | 仅移交并释放该持有引用；成功后该三元组立即失效，调用方清空本地副本，所有旧别名均不可再用。其他持有引用独立有效。失败保持原引用及回收责任不变；无效引用失败不影响其他记录 |

`retain` 的输出指针必需，缺失返回 `INVALID_ARGUMENT`；调用方须使用不承载另一未释放引用的输出存储。空来源或失效来源返回 `INVALID_HANDLE`；有效但不允许共享的来源返回 `UNSUPPORTED`，不能假定订阅、构造器、使用凭证或插件实例均可复制。容量或标识耗尽返回 `RESOURCE_EXHAUSTED`。`release` 对全零空值成功，对部分为零及失效引用返回 `INVALID_HANDLE`；重复释放同一个非空三元组不是幂等成功。错误缓冲沿用上述调用方所有权规则。

引用服务限同一宿主线程，且不得同步调用业务处理者或抛出跨 ABI 异常。服务表和签发宿主须覆盖引用的全部使用及释放；借用服务表不能使宿主自身延寿。生产 C++ 包装层应移动转交，需共享时显式保留；本步未提供 RAII 包装器。

释放引用不等于业务成功、取消、停止成功或异步资源回收完成。最后一个只读普通数据引用可以触发宿主数据回收；需要等待的任务或外部资源继续由宿主跟踪，不能因为局部引用消失而丢失清理责任。操作终态由 R01c 及后续执行单元另行表达。

### 借用期与数据视图

`dfy_v2_bytes_view` 大小 16、对齐 8，`data` 与 `length` 偏移为 0、8。它是不拥有数据的只读二进制视图，允许包含零字节；长度非零时指针必须指向至少该长度的可读区域，长度为零时不解引用指针。视图不提供释放操作，不能跨分配方调用 `free` 或 `delete`。

| 借用类别 | 有效期与延长方式 |
|---|---|
| 调用借用 | 输入名称、参数视图、服务参数及借来的句柄只到该调用或回调返回；接收方不能直接保存地址供稍后使用 |
| 持有引用借用 | 返回接口须指定依赖哪个持有引用；只读普通数据及其地址在该引用有效期间稳定。释放该引用后原视图失效，即使其他独立引用仍存活，也必须从新引用重新取得视图 |
| 模块借用 | R01a 查询表和 DLL 函数地址只在相应模块引用有效时可用，保留数据引用不能延长这些地址的寿命 |

`retain` 只延长其对象的持有寿命，不自动延长来源视图或调用参数。延长数据使用时先保留引用，再通过该新引用读取视图；需要任意修改或独立于宿主存活时复制数据。宿主完全拥有的普通数据不因来源或消费插件的 DLL 卸载而失效，包含插件析构或恢复代码的资源则仍需单独登记模块依赖。

`test.borrow@1` 只用于验收：C DLL 在调用内读取宿主的 `A\0B`，调用宿主 `retain`，成功后把新引用转交调用方；它不缓存服务表、不释放借入引用、不提供生产值访问能力。隔离宿主只有两个引用槽位，证明错误拒绝及寿命责任可由纯 C 与 C++20 共同表达，不替代 R02 的正式注册表。

## R01c-1 操作状态与读取

### 受理、操作引用与终态

提交操作的具体入口由各能力定义，不提供通用的 `submit(kind, void*)` 或消费者结算入口。受理成功须返回有效的操作持有引用；拒绝受理时清空输出，不留下调用方的引用或未登记工作。受理仅表示宿主已经承担推进和收尾责任，不能解释为结果已成功。操作可以在首次读取前已经完成，消费者始终通过状态与对应能力的结果接口判断。

操作引用沿用 R01b：`retain` 取得指向同一次操作的独立引用，各引用观察同一个操作记录；句柄数值不充当操作身份。释放任意一份乃至全部客户端引用都不取消操作，也不丢弃宿主尚未完成的工作或清理责任。操作记录须覆盖有效持有引用的读取；宿主工作责任与客户端引用分别结束后才能回收相应记录。

状态用固定宽度 uint32 表达，枚举值如下；零值是失败输出使用的无效状态。

| 状态宏 | 值 | 含义 |
|---|---|---|
| `DFY_V2_OPERATION_PENDING` | 1 | 尚未确定正式终态，不区分排队、执行、等待或收尾阶段 |
| `DFY_V2_OPERATION_SUCCEEDED` | 2 | 按该能力约定确定成功结果 |
| `DFY_V2_OPERATION_FAILED` | 3 | 按该能力约定确定失败结果 |
| `DFY_V2_OPERATION_CANCELLED` | 4 | 已确定取消结果，不等同于仅收到取消请求 |

唯一状态迁移为 `Pending → Succeeded / Failed / Cancelled`。终态一经确定不可改写，包括重复结算为同一状态；消费者只有读取权限。具体的成功提交边界、取消请求和竞争裁定在 R01c-2 及对应能力中定稿，本步的取消终态由测试宿主手动设置，不代表取消接口已实现。

操作终态只证明该操作声明的完成条件。业务结果确定不代表相关清理结束，读取到流结束不代表消费任务退出；关闭、回收与 DLL 释放条件由各生命周期能力分别声明，不能仅凭任意操作进入终态就释放模块。

### 宿主状态服务与快照

接口标识为 `dfy.operation.state`，精确版本为 1；它是宿主服务，不是插件静态查询应提供的能力。服务表和宿主必须覆盖调用期，操作引用在调用期间有效，全部调用限同一宿主线程。生产服务装配仍属于 R06／R07。

| 类型 | 大小／对齐（字节） | 关键位置 |
|---|---|---|
| `dfy_v2_operation_snapshot` | 4／4 | `state` 0，快照无指针且属于调用方 |
| `dfy_v2_operation_state_api` | 16／8 | `header` 0，必需函数 `read` 8 |

`read(operation, &snapshot, error)` 只复制当前状态，不等待、不推进工作、不派发回调，也不增加或消耗持有引用。快照是独立副本：后续结算不改变旧快照；释放操作引用或消费 DLL 后仍可读取该副本。它不含结果载荷或借用地址，业务结果和失败原因由对应能力提供独立的类型化读取接口。

- `snapshot` 必需，缺失返回 `INVALID_ARGUMENT`；其余失败也将非空输出置为全零。输出与可选错误缓冲必须是独立、可写的调用方存储。
- 空、部分为零、跨域、未知编号、过期或已释放的引用返回 `INVALID_HANDLE`；有效但不是操作对象的引用返回 `INVALID_ARGUMENT`。拒绝不改变原操作或其他引用。
- 查询成功统一返回 `OK`，`error.length` 为零，包括读取到 `Failed` 或 `Cancelled`。`error` 仅描述本次查询失败，不承载原操作失败原因。
- 服务表使用精确大小及版本，调用前须检查 `header` 和非空 `read`；成功快照只允许上述四个状态，零或未知状态应视为契约错误，不能解释成成功或等待中。
- 取消和通知后续使用独立版本化能力，不向本表追加字段或改变 `read` 的副作用和返回含义。已有 v1、v2 引导表及引用服务布局保持不变。

`test.operation@1` 仅是纯 C 消费夹具：在调用期间借用显式传入的宿主状态表和操作引用，校验服务布局后读取快照；不保存地址，不发布 `dfy.operation.state`。隔离宿主只有一份受管理工作和两个引用槽，以手动结算验证契约，不提供生产注册表、线程调度或取消／完成竞争实现。

## R01c-2 取消请求与原因读取

独立宿主能力 `dfy.operation.cancel@1` 提供 `request` 与 `read`，不改写状态表布局。表 `dfy_v2_operation_cancel_api` 为 24 字节、8 字节对齐，`header/request/read` 偏移为 0/8/16；两个函数均必需。`dfy_v2_cancel_snapshot` 为 4 字节、4 字节对齐，唯一字段 `reason` 位于 0，是调用方持有的副本。

所有调用限同一宿主线程；服务和操作引用须覆盖调用期。不阻塞、不推进消费者或清理、不隐式取消其他等待者。操作是否支持取消在创建时确定，`read` 和 `request` 对不支持的操作均返回 `UNSUPPORTED`；取消某个等待者的通知属于后续通知撤销能力。

`request(operation, reason, &disposition, error)` 按必需输出、原因、有效操作引用、取消支持性顺序检查。输出缺失或原因非法返回 `INVALID_ARGUMENT`；无效引用返回 `INVALID_HANDLE`，有效但对象种类错误返回 `INVALID_ARGUMENT`。失败清空非空输出，保持操作及首个原因不变；`error` 可空，成功长度为零。所有输出存储与错误缓冲相互独立。

| 原因宏 | 值 | 含义 |
|---|---|---|
| `DFY_V2_CANCEL_USER` | 1 | 调用方主动请求取消 |
| `DFY_V2_CANCEL_OWNER_STOP` | 2 | 所有者停止流程请求取消 |
| `DFY_V2_CANCEL_DEADLINE` | 3 | 期限策略请求取消 |

原因 0 和未知值不接受；这些值只记录原因，不创建计时器、不自动传播所有权，也不扩大调用权限。能力装配时须另行校验实例及访问关系。第一个有效请求的原因在该操作内保持不变，独立引用观察同一个原因。

| 成功回执宏 | 值 | 本次效果 |
|---|---|---|
| `DFY_V2_CANCEL_RECORDED` | 1 | Pending 且未请求过，记录首个原因 |
| `DFY_V2_CANCEL_ALREADY_REQUESTED` | 2 | Pending 且已有请求，保留原原因 |
| `DFY_V2_CANCEL_ALREADY_SETTLED` | 3 | 已有正式终态，不写入新原因也不修改结果 |

回执零值无效。已终态优先于已有请求，终态后的重复调用均返回 `ALREADY_SETTLED`。回执成功只说明请求已处理，不承诺结果是 `Cancelled`。`read(operation, &snapshot, error)` 只读首个原因，尚未请求时成功返回零；失败清空输出，空输出返回 `INVALID_ARGUMENT`，引用及支持性规则同上。

取消请求与结果结算分开：业务在自己的提交边界检查请求并决定能否取消；已经发生的业务提交不能被取消回执回滚。取消请求之后仍可能成功或失败，先确定的正式终态永不改写。取消原因在终态后仍可查询，直到相应引用释放；任何终态都不替代实际退出与资源回收判断。生产调度与跨线程结果汇入在后续执行单元实现，本步只验证单宿主线程下的确定先后顺序。

隔离宿主与纯 C `test.cancel@1` 验证三种原因、重复请求、完成先到及取消先到、不支持拒绝、错误布局和参数、过期句柄、独立引用以及请求后仍可成功／失败；不提供通知撤销或生产取消传播。

R01c-2 验证：从 `59aa491` 开始；Debug／Release 主工程、Debug 测试构建通过，完整 CTest 13／13 通过，Debug 操作消费者调用 Release C DLL 通过。沿用下节同一组构建和跨配置命令；下一单元为 R01c-3 完成通知，仍按独立提交推进。

## 验收与下一单元

`sdk.query_v2` 使用真实 C DLL 验证独立导出、主 ABI 拒绝、名称按长度匹配、大小写与版本精确匹配、缺失参数、失败清空输出、可空错误缓冲、重复查询及能力函数调用。C/C++ 两侧检查布局和外层 `pack(1)` 恢复，C++ 消费者同时包含 v1/v2 头文件；模块引用释放后只检查调用方诊断缓冲，不再访问借用表。

夹具的 `test.query@1` 只有返回固定测试值的函数，定义位于 [夹具头文件](../tests/fixtures/query_v2/api.h)，不属于生产能力或公开 SDK 业务表。测试源码及公开头文件使用 UTF-8 BOM，夹具与消费者不继承本体字符集选项。

R01a 当次验证命令及结果（保留历史记录）：

| 命令（项目根目录） | 结果 |
|---|---|
| `cmake --preset windows`；`cmake --preset windows-tests` | 配置成功；测试配置有既有 `dfy_loader_x86` 的 CMP0225 开发者警告 |
| `cmake --build --preset windows-debug`；`cmake --build --preset windows-release`；`cmake --build --preset windows-tests` | 均成功 |
| `ctest --preset windows-tests` | 8／11 通过，包含 `sdk.query_v2`；三项既有测试在创建符号链接时因权限不足失败 |
| `cmake --build build/rebuild/windows-tests --config Release --target dfy_query_v2_fixture` | 成功 |
| `build/rebuild/windows-tests/tests/Debug/dfy_sdk_v2_tests.exe build/rebuild/windows-tests/tests/fixtures/query_v2/Release/dfy_query_v2_fixture.dll` | Debug 消费者调用 Release C DLL 成功 |

未通过的 `plugins.manifest`、`plugins.registry`、`plugins.config` 均报告 `A required privilege is not held by the client`；当前进程特权列表没有 `SeCreateSymbolicLinkPrivilege`。这些测试和对应业务源码未修改，仍保留真实链接校验；需在具备创建链接权限的环境补跑完整 CTest，不能将本次记录视为全套通过。

R01b 本次复用上述配置和三项构建命令，均成功；完整 `ctest --preset windows-tests` 为 9／12 通过，新增 `sdk.handles_v2` 通过，仍是上述三项真实符号链接测试因权限不足失败。测试配置仍有既有 CMP0225 开发者警告。

`sdk.handles_v2` 在 C/C++ 两侧检查布局和打包恢复，并验证实际宿主回调、含零字节视图、独立保留、不可共享拒绝、容量耗尽、跨域／错误编号／过期代次拒绝、失败清空输出、重复释放、空句柄与代次耗尽。DLL 引用释放后从宿主持有引用重新读取数据，最终释放后确认宿主数据已回收；不会读取失效视图。

另执行上述 Release 夹具构建命令，并分别使用 Debug 的 `dfy_sdk_v2_tests.exe` 和 `dfy_sdk_handles_v2_tests.exe` 调用该 Release DLL，均通过。后者命令为 `build/rebuild/windows-tests/tests/Debug/dfy_sdk_handles_v2_tests.exe build/rebuild/windows-tests/tests/fixtures/query_v2/Release/dfy_query_v2_fixture.dll`。新增源码和公开头文件保持 UTF-8 BOM，夹具与独立消费者不继承本体字符集选项。

以上 R01b 记录保留历史状态；R01c 分为状态读取、取消请求和完成通知三个独立审查单元。本次 R01c-1 只固定状态读取契约及隔离验收，下一单元为 R01c-2 取消请求。每个单元分别验证、提交并停止审查。

R01c-1 从 `7275306` 的干净工作区开始，先重新配置并构建测试，改动前完整 CTest 为 12／12 通过。此前三项符号链接测试本次已通过，未修改测试或跳过任何校验。以下命令中的 CMake／CTest 使用本机已安装工具的完整路径调用，预设仍选择 VS2022：

| 命令（项目根目录） | R01c-1 结果 |
|---|---|
| `cmake --preset windows`；`cmake --preset windows-tests` | 配置成功 |
| `cmake --build --preset windows-debug`；`cmake --build --preset windows-release`；`cmake --build --preset windows-tests` | 均成功 |
| `ctest --preset windows-tests` | 13／13 通过，包含新增 `sdk.operations_v2` 及全部真实链接测试 |
| `cmake --build build/rebuild/windows-tests --config Release --target dfy_query_v2_fixture` | 成功 |
| Debug 的 `dfy_sdk_v2_tests.exe`、`dfy_sdk_handles_v2_tests.exe`、`dfy_sdk_operations_v2_tests.exe` 分别调用 Release 夹具 DLL | 三项均通过 |

跨配置操作验收的完整命令为 `build/rebuild/windows-tests/tests/Debug/dfy_sdk_operations_v2_tests.exe build/rebuild/windows-tests/tests/fixtures/query_v2/Release/dfy_query_v2_fixture.dll`。

新增验收覆盖 C11／C++20 布局及外层打包恢复、错误服务版本／大小／入口、受理拒绝与输出清空、只读 Pending 查询、对象种类和过期引用拒绝、三种终态及重复结算拒绝、独立引用、旧快照保持、最后一份客户端引用释放后宿主继续承担工作，以及消费 DLL 释放后宿主仍能结算。状态来自隔离宿主手动推进，不把这些断言当作生产执行器、取消竞争或通知寿命的验收。
