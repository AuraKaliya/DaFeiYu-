# 验证生产域签发器与同一内部算法的整数边界，不连接任何插件或模型服务。
dfy_add_unit_test(runtime.identifiers dfy_runtime_ids_tests runtime_ids.cpp dfy_runtime)

# 直接验收生产引用表，不用 SDK 隔离夹具替代槽位与代次实现。
dfy_add_unit_test(runtime.handles dfy_runtime_handles_tests runtime_handles.cpp dfy_runtime)

# 验证宿主基础值的类型、精度及生产引用表中的寿命。
dfy_add_unit_test(runtime.values dfy_runtime_values_tests runtime_values.cpp dfy_runtime)

# 验证最小结构描述的输入隔离、字段约束及引用寿命，不接入类型目录。
dfy_add_unit_test(runtime.struct_descriptors dfy_runtime_struct_descriptors_tests runtime_struct_descriptors.cpp dfy_runtime)

# 验证有界类型目录的精确查询、冲突拒绝及独立快照寿命。
dfy_add_unit_test(runtime.types dfy_runtime_types_tests runtime_types.cpp dfy_runtime)

# 验证结构载荷的严格构造、只读反射和描述寿命。
dfy_add_unit_test(runtime.struct_values dfy_runtime_struct_values_tests runtime_struct_values.cpp dfy_runtime)

# 验证有界执行器的延后 FIFO、重入拒绝和捕获寿命。
dfy_add_unit_test(runtime.executor dfy_runtime_executor_tests runtime_executor.cpp dfy_runtime)

# 验证宿主操作终态与客户端引用分离的工作寿命。
dfy_add_unit_test(runtime.operations dfy_runtime_operations_tests runtime_operations.cpp dfy_runtime)

# 验证通知登记、不可共享引用与独立撤销，调度另行分步加入。
dfy_add_unit_test(runtime.notifications dfy_runtime_notifications_tests runtime_notifications.cpp dfy_runtime)

# 验证资源所有者的登记、清理阶段及实例代次隔离。
dfy_add_unit_test(runtime.resources dfy_runtime_resources_tests runtime_resources.cpp dfy_runtime)

# 验证有界嵌套结构与定义依赖，独立于 C ABI 的版本化扩展。
dfy_add_unit_test(runtime.nested dfy_runtime_nested_tests runtime_nested.cpp dfy_runtime)

# 验证可选、可空与实际内容的独立约束。
dfy_add_unit_test(runtime.presence dfy_runtime_presence_tests runtime_presence.cpp dfy_runtime)

# 验证标量数组的精确类型、不可变持有和两层预算。
dfy_add_unit_test(runtime.arrays dfy_runtime_arrays_tests runtime_arrays.cpp dfy_runtime)

# 验证 UTF-8 字符串键映射的唯一性、规范顺序及数据预算。
dfy_add_unit_test(runtime.maps dfy_runtime_maps_tests runtime_maps.cpp dfy_runtime)

# 验证闭合枚举的精确整数、不可变定义与有界成员集。
dfy_add_unit_test(runtime.enums dfy_runtime_enums_tests runtime_enums.cpp dfy_runtime)

# 验证闭合联合的显式标签、分支约束及完整描述预算。
dfy_add_unit_test(runtime.unions dfy_runtime_unions_tests runtime_unions.cpp dfy_runtime)

# 验证完整匿名元素约束及容器层的展开计量。
dfy_add_unit_test(runtime.elements dfy_runtime_elements_tests runtime_elements.cpp dfy_runtime)

# 验证有界重试的安全条件、退避、调度及实际退出。
dfy_add_unit_test(runtime.retry dfy_retry_controller_tests retry_controller.cpp dfy_runtime)

# 验证显式业务参与者的结果策略、取消和结果预算寿命。
dfy_add_unit_test(runtime.result_group dfy_result_group_tests result_group.cpp dfy_runtime)

# 验证完成通知接入公平调度后的背压、异常及撤销。
dfy_add_unit_test(runtime.owner_notifications dfy_owner_notification_tests owner_notifications.cpp dfy_runtime)

# 验证所有者权重、前置执行资格及活动任务容量。
dfy_add_unit_test(runtime.owner_scheduler dfy_owner_scheduler_tests owner_scheduler.cpp dfy_runtime)

# 验证分层条目与字节预算的原子计费、移动归还及极限。
dfy_add_unit_test(runtime.budget dfy_budget_tests budget.cpp dfy_runtime)

# 验证顺序域的 FIFO 授予、跨等待占用及独立取消关闭。
dfy_add_unit_test(runtime.serial_domain dfy_serial_domain_tests serial_domain.cpp dfy_runtime)

# 验证顺序域显式等待依赖的同域拒绝、跨域环和登记预算。
dfy_add_unit_test(runtime.serial_wait_graph dfy_serial_wait_graph_tests serial_wait_graph.cpp dfy_runtime)
