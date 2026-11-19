# v2 ABI 早期验证记录

保留 R01 阶段的验证命令、已知限制与当时的接续安排。当前开发请查[v2 契约](../../plugin-abi-v2.md)。

## R01c-3b 通知行为隔离验收

从 `d3a2481` 开始，新增 [sdk.notifications_v2](../../../tests/sdk_notifications_v2.cpp) 与[纯 C 回调夹具](../../../tests/fixtures/query_v2/notification.c)。夹具静态能力 `test.notification@1` 仅返回实际 C 回调，不伪装成生产宿主通知服务。隔离宿主使用两个通知槽和十二个引用槽，由测试显式完成操作和推进通知；不提供正式执行器、资源登记或自动 DLL 保持引用。

验收覆盖：Pending 时不通知；先完成后登记仍不重入；每个登记最多调用一次；独立撤销及重复撤销；活动 C 回调自撤销返回 Busy；操作借用在 C 回调内可保留；原操作引用释放后仍能通知；回调中新登记留到下一轮；复用空槽不改变登记顺序；登记引用不可共享；释放客户端登记引用不隐式撤销；通知及引用容量不足时没有半登记；过期登记拒绝。

真实 DLL 引用由测试宿主显式保持，先确认待调度集合和引用表均为空，再释放模块引用并再次推进以检查没有残留入口。这证明本隔离实现的责任与调用链可行，不替代 R04 的模块依赖管理或 R08 的生产插件完整链路。R01 的查询、引用、操作状态、取消与通知契约及隔离验收已齐备，下一单元进入 R02a 标识与代次的生产基础。

验证：Debug／Release 主工程、Debug 测试构建成功，完整 CTest 14／14 通过；Release C DLL 构建及 Debug 的 `dfy_sdk_notifications_v2_tests.exe` 调用该 DLL 通过。新边界源码保留 UTF-8 BOM，不继承本体字符集选项。

## 验收与下一单元

`sdk.query_v2` 使用真实 C DLL 验证独立导出、主 ABI 拒绝、名称按长度匹配、大小写与版本精确匹配、缺失参数、失败清空输出、可空错误缓冲、重复查询及能力函数调用。C/C++ 两侧检查布局和外层 `pack(1)` 恢复，C++ 消费者同时包含 v1/v2 头文件；模块引用释放后只检查调用方诊断缓冲，不再访问借用表。

夹具的 `test.query@1` 只有返回固定测试值的函数，定义位于 [夹具头文件](../../../tests/fixtures/query_v2/api.h)，不属于生产能力或公开 SDK 业务表。测试源码及公开头文件使用 UTF-8 BOM，夹具与消费者不继承本体字符集选项。

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
