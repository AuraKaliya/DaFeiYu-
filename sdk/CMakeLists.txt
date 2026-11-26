# 公开 C ABI 头文件仅向消费者传递 include 路径。
add_library(dfy_sdk INTERFACE
  include/dafeiyu/activity_abi_v2.h
  include/dafeiyu/agent_abi_v2.h
  include/dafeiyu/companion_abi_v2.h
  include/dafeiyu/data_abi_v2.h
  include/dafeiyu/event_abi_v2.h
  include/dafeiyu/group_abi_v2.h
  include/dafeiyu/hook_abi_v2.h
  include/dafeiyu/lifecycle_abi_v2.h
  include/dafeiyu/mailbox_abi_v2.h
  include/dafeiyu/plugin_abi.h
  include/dafeiyu/plugin_abi_v2.h
  include/dafeiyu/processing_abi_v2.h
  include/dafeiyu/routed_event_abi_v2.h
  include/dafeiyu/tool_abi_v2.h)
target_include_directories(dfy_sdk INTERFACE include)
dfy_group_target(dfy_sdk SDK)

install(DIRECTORY include/ DESTINATION sdk/include CONFIGURATIONS Release COMPONENT SDK)
# SDK 可独立安装，开发手册和公开接口契约相邻，原有 README 入口共用同一份正文。
install(FILES "${DFY_USER_MANUAL_DIR}/插件开发说明.md" "${DFY_USER_MANUAL_DIR}/插件接口说明.md"
  DESTINATION sdk CONFIGURATIONS Release COMPONENT SDK)
install(FILES "${DFY_USER_MANUAL_DIR}/插件开发说明.md" DESTINATION sdk RENAME README.md CONFIGURATIONS Release COMPONENT SDK)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
