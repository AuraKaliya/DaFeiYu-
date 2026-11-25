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
install(FILES README.md DESTINATION sdk CONFIGURATIONS Release COMPONENT SDK)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
