# 跨模块场景通过测试接口使用本模块的夹具头文件。
add_library(dfy_plugin_host_test_support INTERFACE)
target_include_directories(dfy_plugin_host_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# 清单测试使用真实目录和链接，只解析文件，不执行任何插件代码。
dfy_test_executable(dfy_plugin_manifest_tests plugin_manifest.cpp)
target_link_libraries(dfy_plugin_manifest_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
add_test(NAME plugins.manifest COMMAND dfy_plugin_manifest_tests "${CMAKE_CURRENT_BINARY_DIR}/manifest/$<CONFIG>")
set_tests_properties(plugins.manifest PROPERTIES TIMEOUT 15)

# 发现测试在隔离目录中增删包，验证刷新、冲突和快照的生命周期。
dfy_test_executable(dfy_plugin_registry_tests plugin_registry.cpp)
target_link_libraries(dfy_plugin_registry_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
add_test(NAME plugins.registry COMMAND dfy_plugin_registry_tests "${CMAKE_CURRENT_BINARY_DIR}/registry/$<CONFIG>")
set_tests_properties(plugins.registry PROPERTIES TIMEOUT 15)

# 配置测试验证默认值、独立用户覆盖和只读快照，不创建插件实例。
dfy_test_executable(dfy_plugin_config_tests plugin_config.cpp)
target_link_libraries(dfy_plugin_config_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
add_test(NAME plugins.config COMMAND dfy_plugin_config_tests "${CMAKE_CURRENT_BINARY_DIR}/plugin-config/$<CONFIG>")
set_tests_properties(plugins.config PROPERTIES TIMEOUT 15)

# 验证 C 订阅创建和关闭的独立接收方与失败回滚。
dfy_add_unit_test(plugins.subscriptions dfy_plugin_subscriptions_tests plugin_subscriptions.cpp dfy_plugins)

dfy_test_executable(dfy_plugin_mailboxes_tests plugin_mailboxes.cpp)
target_link_libraries(dfy_plugin_mailboxes_tests PRIVATE dfy_plugins dfy_build_options)
add_test(NAME plugins.mailboxes COMMAND dfy_plugin_mailboxes_tests)

dfy_test_executable(dfy_plugin_order_tests plugin_order.cpp)
target_link_libraries(dfy_plugin_order_tests PRIVATE dfy_plugins dfy_build_options)
add_test(NAME plugins.hook_order COMMAND dfy_plugin_order_tests)

# 验证字段约束独立持有及已实现内容种类的统一反射。
dfy_add_unit_test(plugins.constraints dfy_plugin_constraints_tests plugin_constraints.cpp dfy_plugins)

# 验证组能力的条件授权、加入事务、退出和跨宿主边界。
dfy_add_unit_test(plugins.groups dfy_plugin_groups_tests plugin_groups.cpp dfy_plugins)

# 验证本地包目录边界、重解析点拒绝及私有暂存清理。
dfy_test_executable(dfy_package_file_tests package_files.cpp)
target_link_libraries(dfy_package_file_tests PRIVATE dfy_plugins dfy_build_options)
add_test(NAME plugins.package_files COMMAND dfy_package_file_tests "${CMAKE_CURRENT_BINARY_DIR}/package-files/$<CONFIG>")
set_tests_properties(plugins.package_files PROPERTIES TIMEOUT 15)

# 验证宿主处理能力的顺序、结算及忙碌停止边界。
dfy_add_unit_test(plugins.processing dfy_plugin_processing_tests plugin_processing.cpp dfy_plugins)
