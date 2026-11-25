# 同步总线和投递流分别提供链接目标，共用事件模块的公开头文件。
add_library(dfy_events STATIC src/bus.cpp include/dafeiyu/events.hpp)
target_compile_features(dfy_events PUBLIC cxx_std_20)
target_include_directories(dfy_events PUBLIC include)
target_link_libraries(dfy_events PRIVATE dfy_build_options)
dfy_group_target(dfy_events Platform)

add_library(dfy_event_streams STATIC
  src/event_store.cpp src/subscription_queue.cpp src/broadcast_channel.cpp src/subscription_reader.cpp
  src/mailboxes.cpp src/hooks.cpp src/processing_queue.cpp src/processing.cpp src/group_reader.cpp
  src/consumer_group.cpp src/sequence.hpp
  include/dafeiyu/event_store.hpp include/dafeiyu/subscription_queue.hpp include/dafeiyu/broadcast_channel.hpp
  include/dafeiyu/subscription_reader.hpp include/dafeiyu/mailboxes.hpp include/dafeiyu/hooks.hpp
  include/dafeiyu/processing_queue.hpp include/dafeiyu/processing.hpp include/dafeiyu/group_reader.hpp
  include/dafeiyu/consumer_group.hpp)
target_compile_features(dfy_event_streams PUBLIC cxx_std_20)
target_include_directories(dfy_event_streams PUBLIC include)
target_link_libraries(dfy_event_streams PUBLIC dfy_runtime PRIVATE dfy_build_options)
dfy_group_target(dfy_event_streams Platform)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
