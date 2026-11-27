add_library(dfy_application STATIC
  src/application.cpp src/application.hpp src/ask.cpp src/ask.hpp src/binding_facts.cpp
  src/workspace_environment.cpp src/workspace_environment.hpp
  src/binding_facts.hpp src/builtin_room.cpp src/builtin_room.hpp src/room_motion.cpp src/room_perception.cpp src/room_decision_feed.cpp src/room_decision_feed.hpp
  src/companion.cpp src/companion.hpp
  src/companion_board.cpp src/companion_board.hpp src/companion_delivery.cpp src/companion_delivery.hpp
  src/companion_exchange.cpp src/companion_inbox.cpp src/companion_inbox.hpp src/companion_location.cpp
  src/companion_location.hpp src/companion_messages.cpp src/companion_messages.hpp
  src/companion_mining.cpp src/companion_mining.hpp src/companion_partner.cpp
  src/companion_question.cpp src/companion_question.hpp src/companion_review.cpp
  src/companion_state.cpp src/companion_state.hpp src/companion_visit.cpp src/companion_visit.hpp
  src/conversation.cpp src/conversation.hpp src/diagnostic_files.cpp src/diagnostic_files.hpp
  src/dialogue_queue.cpp src/event_dispatch.cpp src/event_dispatch.hpp src/interaction_routes.cpp
  src/interaction_routes.hpp src/mail_reply_tool.cpp src/mail_reply_tool.hpp src/mailbox_intake.cpp
  src/mailbox_intake.hpp src/mailbox_view.cpp src/mailbox_view.hpp src/main_messages.cpp src/main_messages.hpp src/main_output.cpp
  src/message_store.cpp src/message_store.hpp src/main_memory.cpp src/main_experience.cpp src/main_task_memory.cpp src/host_settings.cpp src/host_settings.hpp
  src/message_store_db.hpp src/task_store.cpp src/task_understanding.cpp src/task_reflection.cpp src/task_reflection.hpp
  src/host_settings_file.cpp src/host_settings_file.hpp src/persona.cpp src/persona.hpp src/reply_delivery.cpp
  src/reply_delivery.hpp src/room_mailbox.cpp src/run_log.cpp src/run_log.hpp src/software_log.cpp
  src/software_log.hpp src/software_log_files.cpp src/software_log_files.hpp src/subagents.cpp
  src/subagents.hpp src/tool_contract_trace.cpp src/tool_contract_trace.hpp src/tool_verification.cpp src/tool_verification.hpp src/trace_commands.hpp
  src/trace_model.hpp src/trace_read.cpp src/trace_read.hpp src/trace_replay.cpp
  src/transition_notice.cpp src/transition_notice.hpp)
target_include_directories(dfy_application PUBLIC src)
target_link_libraries(dfy_application PUBLIC dfy_plugin_tools
  PRIVATE dfy_build_options dfy_digest winsqlite3 nlohmann_json::nlohmann_json)
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
dfy_group_target(dfy_application App)

add_library(dfy_host_commands STATIC
  src/commands/application_commands.cpp src/commands/application_commands.hpp
  src/commands/chat.cpp src/commands/chat.hpp src/commands/diagnostics.cpp src/commands/diagnostics.hpp
  src/commands/plugin_commands.cpp src/commands/plugin_commands.hpp)
target_include_directories(dfy_host_commands PUBLIC src/commands)
target_link_libraries(dfy_host_commands PUBLIC dfy_application PRIVATE dfy_build_options nlohmann_json::nlohmann_json)
dfy_group_target(dfy_host_commands App)

add_library(dfy_management STATIC
  src/log_settings.cpp src/log_settings.hpp src/management/client.cpp src/management/client.hpp
  src/management/dialogue_channel.hpp src/management/host.cpp src/management/host.hpp
  src/management/local_pipe.cpp src/management/local_pipe.hpp src/management/message_protocol.cpp
  src/management/mailbox_protocol.cpp src/management/mailbox_protocol.hpp
  src/management/plugin_directory.cpp src/management/plugin_directory.hpp
  src/management/plugin_actions.cpp src/management/plugin_actions.hpp
  src/management/retired_plugin_requests.cpp src/management/retired_plugin_requests.hpp
  src/management/message_protocol.hpp src/management/poll_log.cpp src/management/poll_log.hpp
  src/management/protocol.cpp src/management/protocol.hpp src/management/resident.cpp
  src/management/settings_protocol.cpp
  src/management/resident.hpp src/management/resident_watch.cpp src/management/resident_watch.hpp
  src/management/room_location.hpp src/management/terminal_host.cpp src/management/terminal_host.hpp
  src/paths.cpp src/paths.hpp)
target_compile_definitions(dfy_management PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dfy_management PUBLIC dfy_application nlohmann_json::nlohmann_json
  PRIVATE dfy_host_commands dfy_build_options dfy_digest advapi32 shell32 ole32 winsqlite3)
dfy_group_target(dfy_management App)

add_library(dfy_resident_entry STATIC src/resident_entry.cpp src/resident_entry.hpp src/resident_tray.cpp src/resident_tray.hpp)
target_link_libraries(dfy_resident_entry PUBLIC dfy_management PRIVATE dfy_resident_tray dfy_build_options)
if(BUILD_GUI)
  target_compile_definitions(dfy_resident_entry PRIVATE DFY_DESKTOP_AVAILABLE)
endif()
dfy_group_target(dfy_resident_entry App)

add_executable(dafeiyu-host src/main.cpp)
target_link_libraries(dafeiyu-host PRIVATE dfy_resident_entry dfy_resident_tray dfy_build_options)
set_target_properties(dafeiyu-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>")
dfy_group_target(dafeiyu-host App)
install(TARGETS dafeiyu-host RUNTIME DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
