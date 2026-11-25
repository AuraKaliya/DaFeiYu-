# 定义供终端和桌面入口复用的应用会话服务。
add_library(dfy_application STATIC application.cpp application.hpp conversation.cpp conversation.hpp tool_verification.cpp tool_verification.hpp
  diagnostic_files.cpp diagnostic_files.hpp)
target_include_directories(dfy_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(dfy_application PRIVATE companion.cpp companion.hpp companion_state.cpp companion_state.hpp)
target_sources(dfy_application PRIVATE subagents.cpp subagents.hpp)
target_sources(dfy_application PRIVATE persona.cpp persona.hpp)
target_sources(dfy_application PRIVATE dialogue_queue.cpp)
target_sources(dfy_application PRIVATE builtin_room.cpp builtin_room.hpp)
target_sources(dfy_application PRIVATE run_log.cpp run_log.hpp)
target_sources(dfy_application PRIVATE software_log.cpp software_log.hpp)
target_sources(dfy_application PRIVATE software_log_files.cpp software_log_files.hpp)
target_sources(dfy_application PRIVATE ask.cpp ask.hpp interaction_routes.cpp interaction_routes.hpp)
target_sources(dfy_application PRIVATE event_dispatch.cpp event_dispatch.hpp)
target_sources(dfy_application PRIVATE transition_notice.cpp transition_notice.hpp)
target_sources(dfy_application PRIVATE mail_reply_tool.cpp mail_reply_tool.hpp)
target_sources(dfy_application PRIVATE reply_delivery.cpp reply_delivery.hpp)
target_sources(dfy_application PRIVATE trace_read.cpp trace_read.hpp trace_replay.cpp trace_commands.hpp trace_model.hpp)
target_sources(dfy_application PRIVATE mailbox_intake.cpp mailbox_intake.hpp room_mailbox.cpp)
target_sources(dfy_application PRIVATE companion_exchange.cpp)
target_sources(dfy_application PRIVATE companion_messages.cpp companion_messages.hpp)
target_sources(dfy_application PRIVATE companion_partner.cpp)
target_sources(dfy_application PRIVATE companion_inbox.cpp companion_inbox.hpp)
target_sources(dfy_application PRIVATE companion_visit.cpp companion_visit.hpp)
target_sources(dfy_application PRIVATE companion_question.cpp companion_question.hpp)
target_sources(dfy_application PRIVATE companion_delivery.cpp companion_delivery.hpp)
target_sources(dfy_application PRIVATE companion_mining.cpp companion_mining.hpp)
target_sources(dfy_application PRIVATE companion_location.cpp companion_location.hpp)
target_sources(dfy_application PRIVATE companion_board.cpp companion_board.hpp)
target_sources(dfy_application PRIVATE companion_review.cpp)
target_link_libraries(dfy_application PUBLIC dfy_plugin_tools PRIVATE dfy_build_options dfy_digest nlohmann_json::nlohmann_json)
# 构建标签随二进制固定，源码提交仅在构建者明确提供时记录。
set(DFY_APPLICATION_COMMIT "" CACHE STRING "Verified source commit of this application build")
if(NOT DFY_APPLICATION_COMMIT STREQUAL "")
  string(LENGTH "${DFY_APPLICATION_COMMIT}" dfy_commit_length)
  if(NOT DFY_APPLICATION_COMMIT MATCHES "^[0-9A-Fa-f]+$" OR (NOT dfy_commit_length EQUAL 40 AND NOT dfy_commit_length EQUAL 64))
    message(FATAL_ERROR "DFY_APPLICATION_COMMIT requires a full source commit")
  endif()
endif()
target_compile_definitions(dfy_application PRIVATE
  "DFY_APPLICATION_COMMIT=\"${DFY_APPLICATION_COMMIT}\""
  "DFY_APPLICATION_BUILD=\"${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_GENERATOR_PLATFORM}-$<CONFIG>\"")
set_target_properties(dfy_application PROPERTIES FOLDER "App")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/application.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/application.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/conversation.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/conversation.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tool_verification.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tool_verification.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic_files.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic_files.hpp")

# 管理宿主独立持有业务线程，桌面和无窗口消费者共用同一套快照与命令处理。
add_library(dfy_management STATIC management/host.cpp management/host.hpp management/dialogue_channel.hpp management/room_location.hpp)
target_sources(dfy_management PRIVATE management/protocol.cpp management/protocol.hpp)
target_sources(dfy_management PRIVATE management/local_pipe.cpp management/local_pipe.hpp)
target_sources(dfy_management PRIVATE management/resident.cpp management/resident.hpp)
target_sources(dfy_management PRIVATE management/resident_watch.cpp management/resident_watch.hpp)
target_sources(dfy_management PRIVATE management/client.cpp management/client.hpp)
target_sources(dfy_management PRIVATE management/poll_log.cpp management/poll_log.hpp)
target_compile_definitions(dfy_management PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dfy_management PRIVATE advapi32)
target_link_libraries(dfy_management PUBLIC dfy_application nlohmann_json::nlohmann_json PRIVATE dfy_build_options dfy_digest)
set_target_properties(dfy_management PROPERTIES FOLDER App)

# 定义异步终端交互目标，便于独立验证输入和本地命令。
add_library(dfy_terminal STATIC application_commands.cpp application_commands.hpp chat.cpp chat.hpp async_chat.hpp)
target_sources(dfy_terminal PRIVATE diagnostics.cpp diagnostics.hpp)
target_sources(dfy_terminal PRIVATE companion_host.cpp companion_host.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.hpp")
target_sources(dfy_terminal PRIVATE plugin_commands.cpp plugin_commands.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_commands.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_commands.hpp")
target_include_directories(dfy_terminal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dfy_terminal PUBLIC dfy_application dfy_input PRIVATE dfy_build_options)
set_target_properties(dfy_terminal PROPERTIES FOLDER "App")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/application_commands.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/application_commands.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/chat.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/chat.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/async_chat.hpp")

# 定义终端应用目标，显式列出实现与内部头文件。
add_executable(dafeiyu main.cpp console.hpp paths.cpp paths.hpp)

# 配置依赖与编译环境：通过 Agent 使用模型能力，并限制 Windows 头文件的宏污染。
target_link_libraries(dafeiyu PRIVATE dfy_terminal dfy_management dfy_build_options shell32 ole32)
target_compile_definitions(dafeiyu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_custom_command(TARGET dafeiyu POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:dafeiyu>/config"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${PROJECT_SOURCE_DIR}/config/deepseek.example.json" "${PROJECT_SOURCE_DIR}/config/qwen.example.json"
    "$<TARGET_FILE_DIR:dafeiyu>/config"
  VERBATIM)

# 设置运行位置：按构建类型输出程序，VS 调试时从程序所在目录启动。
set_target_properties(dafeiyu PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu>")

# 组织 IDE 展示：保留 App 职责分组，源码按模块内实际目录排列。
set_target_properties(dafeiyu PROPERTIES FOLDER "App")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/console.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/paths.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/paths.hpp")
