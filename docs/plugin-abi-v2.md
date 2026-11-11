# 插件 C ABI v2 查询契约

R01a 固定本契约及 [SDK 头文件](../sdk/include/dafeiyu/plugin_abi_v2.h)，并以 [纯 C11 查询夹具](../tests/fixtures/query_v2/query.c)和 [C++20 消费者](../tests/sdk_abi_v2.cpp)验收。当前只完成独立查询与静态能力协商，不提供实例句柄、宿主服务、事件流或异步停止。生产 `PluginModule` 仍只接受 v1，v2 生产加载与兼容校验属于 R06。

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

引导表仅负责查询插件提供的静态接口。宿主传给实例的服务、实例创建及操作句柄另行定稿；查询成功不表示这些能力已实现。未知能力或版本应明确返回不支持，不能用未实现的占位成功表响应。

## 状态与输出责任

| 状态 | 值 | 含义 |
|---|---|---|
| `DFY_V2_OK` | 0 | 本次查询成功，输出非空且错误长度为零 |
| `DFY_V2_INVALID_ARGUMENT` | 1 | 必需输出为空；能力名称指针为空、长度为零或能力版本为零 |
| `DFY_V2_ABI_MISMATCH` | 2 | 引导查询不支持请求的主 ABI |
| `DFY_V2_UNSUPPORTED` | 5 | 能力名称或精确能力版本不支持 |

输出指针必需，所有失败查询均将可写的表输出清空。先校验必需参数，再检查版本是否支持；能力版本不支持使用 `UNSUPPORTED`，不混同主 ABI 不兼容。未知非零状态均按失败处理，不读取失败结果中的表。

`error` 可为空。非空时由调用方分配；成功将 `length` 置零，失败可写入至多 256 字节的完整 UTF-8，也可用零长度只返回状态。文本不要求零结尾，超长诊断应省略或在完整字符边界截断；调用方先检查长度，不能读取未计入长度的字节。任何函数均不得让异常跨越 C ABI。

## 验收与下一单元

`sdk.query_v2` 使用真实 C DLL 验证独立导出、主 ABI 拒绝、名称按长度匹配、大小写与版本精确匹配、缺失参数、失败清空输出、可空错误缓冲、重复查询及能力函数调用。C/C++ 两侧检查布局和外层 `pack(1)` 恢复，C++ 消费者同时包含 v1/v2 头文件；模块引用释放后只检查调用方诊断缓冲，不再访问借用表。

夹具的 `test.query@1` 只有返回固定测试值的函数，定义位于 [夹具头文件](../tests/fixtures/query_v2/api.h)，不属于生产能力或公开 SDK 业务表。测试源码及公开头文件使用 UTF-8 BOM，夹具与消费者不继承本体字符集选项。

本次验证命令及结果：

| 命令（项目根目录） | 结果 |
|---|---|
| `cmake --preset windows`；`cmake --preset windows-tests` | 配置成功；测试配置有既有 `dfy_loader_x86` 的 CMP0225 开发者警告 |
| `cmake --build --preset windows-debug`；`cmake --build --preset windows-release`；`cmake --build --preset windows-tests` | 均成功 |
| `ctest --preset windows-tests` | 8／11 通过，包含 `sdk.query_v2`；三项既有测试在创建符号链接时因权限不足失败 |
| `cmake --build build/rebuild/windows-tests --config Release --target dfy_query_v2_fixture` | 成功 |
| `build/rebuild/windows-tests/tests/Debug/dfy_sdk_v2_tests.exe build/rebuild/windows-tests/tests/fixtures/query_v2/Release/dfy_query_v2_fixture.dll` | Debug 消费者调用 Release C DLL 成功 |

未通过的 `plugins.manifest`、`plugins.registry`、`plugins.config` 均报告 `A required privilege is not held by the client`；当前进程特权列表没有 `SeCreateSymbolicLinkPrivilege`。这些测试和对应业务源码未修改，仍保留真实链接校验；需在具备创建链接权限的环境补跑完整 CTest，不能将本次记录视为全套通过。

R01a 只验收这一查询协议，不实现生产 v2 加载器、事件反射、异步执行或插件实例。下一单元 R01b 细化句柄与借用契约，R01c 再细化操作受理、完成与取消；每个单元分别验证、提交并停止审查。
