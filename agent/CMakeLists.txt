# 定义会话静态库，显式列出实现与公开接口。
add_library(dfy_agent STATIC session.cpp include/dafeiyu/session.hpp include/dafeiyu/run_identity.hpp)

# 配置接口依赖：向使用者公开会话头文件及其依赖的模型、事件接口。
target_include_directories(dfy_agent PUBLIC include)
target_sources(dfy_agent PRIVATE run_journal.cpp include/dafeiyu/run_journal.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/run_journal.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/run_journal.hpp")
target_sources(dfy_agent PRIVATE confirmation.cpp include/dafeiyu/confirmation.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/confirmation.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/confirmation.hpp")
target_sources(dfy_agent PRIVATE run_loop.cpp include/dafeiyu/run_loop.hpp include/dafeiyu/run_options.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/run_loop.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/run_loop.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/run_options.hpp")
target_sources(dfy_agent PRIVATE tool_batch.cpp include/dafeiyu/tool_batch.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tool_batch.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/tool_batch.hpp")
target_sources(dfy_agent PRIVATE run_trace.cpp include/dafeiyu/run_trace.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/run_trace.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/run_trace.hpp")
target_sources(dfy_agent PRIVATE trace_identity.cpp include/dafeiyu/trace_identity.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/trace_identity.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/trace_identity.hpp")
target_sources(dfy_agent PRIVATE execution_policy.cpp include/dafeiyu/execution_policy.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/execution_policy.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/execution_policy.hpp")
target_sources(dfy_agent PRIVATE run_context.cpp include/dafeiyu/run_context.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/run_context.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/run_context.hpp")
target_sources(dfy_agent PRIVATE tool_executor.cpp include/dafeiyu/tool_executor.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tool_executor.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/tool_executor.hpp")
target_sources(dfy_agent PRIVATE tool_work.cpp include/dafeiyu/tool_work.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tool_work.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/tool_work.hpp")
target_sources(dfy_agent PRIVATE tool_catalog.cpp include/dafeiyu/tool_catalog.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tool_catalog.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/tool_catalog.hpp")
target_sources(dfy_agent PRIVATE tool_schema.cpp include/dafeiyu/tool_schema.hpp)
target_link_libraries(dfy_agent PRIVATE nlohmann_json::nlohmann_json)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tool_schema.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/tool_schema.hpp")
target_sources(dfy_agent PRIVATE include/dafeiyu/async_session.hpp)
target_link_libraries(dfy_agent PUBLIC dfy_event_streams)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/async_session.hpp")
target_link_libraries(dfy_agent PUBLIC dfy_model PRIVATE dfy_build_options)

# 组织 IDE 展示：保留 Agent 职责分组，源码按模块内实际目录排列。
set_target_properties(dfy_agent PROPERTIES FOLDER "Agent")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/session.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/run_identity.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/session.hpp")
