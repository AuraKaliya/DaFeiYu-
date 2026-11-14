# 大肥鱼原生插件 SDK

此目录提供 Windows x64 的 C ABI。插件为同进程可信原生 DLL，推荐使用 VS2022、C11 和 CMake 3.28 或更新版本。C++ 插件同样可以使用这些头，但不能跨边界传递 C++ 对象、异常或分配器所有权。

## 独立构建观察示例

本目录的 `examples/observer` 自带独立构建文件，只依赖相对路径下的 `include`：

```powershell
cmake -S .\examples\observer -B .\build-observer -G "Visual Studio 17 2022" -A x64
cmake --build .\build-observer --config Release
```

生成包位于 `build-observer/Release/example.observer`。在运行中的宿主输入 `/plugins install "生成包的绝对路径"`，再明确 `/plugins load example.observer`。示例名称和配置都在包中，宿主没有固定必需插件清单。

## 入口与生命周期

公开头文件位于 [include/dafeiyu](include/dafeiyu)。[plugin_abi.h](include/dafeiyu/plugin_abi.h) 的 v1 查询接口保持冻结；正式实例使用 [plugin_abi_v2.h](include/dafeiyu/plugin_abi_v2.h) 的独立 v2 导出及精确版本查询。定义 `DFY_PLUGIN_BUILD` 以导出入口。

每次查询均核对名称长度、精确版本、公共头大小和全部必需函数。查询表只在相应模块持有期间有效。未知能力返回不支持并清空输出，不通过强制转换读取未知布局；主 ABI 与各能力自己的版本分别协商。

[lifecycle_abi_v2.h](include/dafeiyu/lifecycle_abi_v2.h) 规定创建、启动、停止请求、停止轮询和单次销毁。停止请求成功只表示接受请求，`STOPPED` 才表示插件自己的任务已退出；停止失败不能继续释放仍被使用的代码。实例由创建方销毁，不能跨 CRT 释放对方分配的内存。

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
| `dfy.groups` | 1：宿主明确授予组时加入、退出和查看本成员状态 |
| `dfy.processing` | 1：宿主明确提供处理记录时的开始、业务结算、实际退出、取消和载荷 |

各能力以对应头文件的结构和函数注释为准：[数据](include/dafeiyu/data_abi_v2.h)、[事件](include/dafeiyu/event_abi_v2.h)、[组](include/dafeiyu/group_abi_v2.h)、[处理](include/dafeiyu/processing_abi_v2.h)。普通观察实例默认获得订阅和读取能力，不自动承担必要业务参与者职责，也不会自动获得组或处理授权。

所有宿主服务只在创建它的宿主线程、有效调用绑定内使用。后台线程不得直接调用宿主表。完成通知延后交付，发布位置不会内联调用观察者；释放客户端引用不等于取消业务或结束宿主责任。

字符串和字节视图按明确长度使用，不能假设以零结尾。借用视图只在对应持有有效期间使用，保留载荷会继续占其完整读取或处理记录的预算。描述引用与事件数据引用分别管理。未知、跨实例或过期句柄必须视作失败。

订阅引用释放不关闭订阅，组成员引用释放不退出成员；使用明确关闭／退出入口或实例停止协议。组成员退出不关闭整个组。已领取的数据与尚未领取的交付分别管理，不因成员离开自动重放已经开始的处理。

处理记录的业务结果和实际退出分开。取得顺序资格、成功读取、成功报告业务结果都不能替代 `finish`；只有消费者确定本项处理不会继续执行后才能确认退出。处理控制引用不可复制，不能提前丢弃控制句柄来解除忙碌。等待、结果保留、通知和引用都有容量上限，资源耗尽应作为可报告的拒绝处理。

## 包布局

包包含 `plugin.json`、入口 DLL、可选默认 JSON 配置和必要私有文件。清单采用版本 1，声明插件 ID、名称、版本、ABI、包内入口及可选默认配置。私有 DLL 依赖放在入口 DLL 同目录；宿主加载时使用该目录和 Windows 系统目录的受控搜索。

路径必须位于包内，不接受链接或重解析点。默认配置随包发布，用户配置保存在宿主独立的用户数据目录。实际生效配置是创建实例时的只读快照；目录刷新不替换运行实例，重新加载产生新实例身份。

发布 Release 包，使用与宿主兼容的 x64 工具链和运行库。Debug DLL 仅用于开发验收，需要本机相应开发运行库。源码中的中文注释和公开头文件使用 UTF-8；此 SDK 头和纯 C 示例带 BOM，以便独立 MSVC 消费者识别。
