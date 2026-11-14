# 定义独立事件静态库，显式列出分发实现与公开接口。
add_library(dfy_events STATIC bus.cpp include/dafeiyu/events.hpp)

# 配置接口与编译要求：公开头文件和 C++20 要求，仅引入自有代码编译选项。
target_compile_features(dfy_events PUBLIC cxx_std_20)
target_include_directories(dfy_events PUBLIC include)
target_link_libraries(dfy_events PRIVATE dfy_build_options)

# 组织 IDE 展示：保留 Platform 职责分组，源码按模块内实际目录排列。
set_target_properties(dfy_events PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/bus.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/events.hpp")

# 为新事件链路建立实际数据目标，保留现有同步总线作为 App 基线。
add_library(dfy_event_streams STATIC event_store.cpp subscription_queue.cpp broadcast_channel.cpp subscription_reader.cpp sequence.hpp
  include/dafeiyu/event_store.hpp include/dafeiyu/subscription_queue.hpp include/dafeiyu/broadcast_channel.hpp
  include/dafeiyu/subscription_reader.hpp)
target_compile_features(dfy_event_streams PUBLIC cxx_std_20)
target_sources(dfy_event_streams PRIVATE consumer_group.cpp include/dafeiyu/consumer_group.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/consumer_group.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/consumer_group.hpp")
target_include_directories(dfy_event_streams PUBLIC include)
target_link_libraries(dfy_event_streams PUBLIC dfy_runtime PRIVATE dfy_build_options)
set_target_properties(dfy_event_streams PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/event_store.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sequence.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/subscription_queue.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/broadcast_channel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/subscription_reader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/event_store.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/subscription_queue.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/broadcast_channel.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/subscription_reader.hpp")
