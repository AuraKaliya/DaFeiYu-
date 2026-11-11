# 插件 C ABI v2 查询、句柄与借用契约

R01a 固定独立查询与静态能力协商，R01b 增加句柄表示、宿主引用服务及借用期契约，均见 [SDK 头文件](../sdk/include/dafeiyu/plugin_abi_v2.h)。[纯 C11 查询夹具](../tests/fixtures/query_v2/query.c)和 [C++20 消费者](../tests/sdk_abi_v2.cpp)验收查询；[借用夹具](../tests/fixtures/query_v2/borrow.c)和 [隔离测试宿主](../tests/sdk_handles_v2.cpp)验收引用行为。生产句柄注册表、实例生命周期和异步操作尚未实现；生产 `PluginModule` 仍只接受 v1，v2 加载与兼容校验属于 R06。

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

本步不实现生产 v2 加载器、句柄注册表、事件反射、异步执行或插件实例。下一单元 R01c 细化操作受理、完成与取消；每个单元分别验证、提交并停止审查。
