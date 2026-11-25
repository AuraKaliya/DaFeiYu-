add_library(dfy_application STATIC
  src/application.cpp src/application.hpp src/ask.cpp src/ask.hpp src/binding_facts.cpp
  src/binding_facts.hpp src/builtin_room.cpp src/builtin_room.hpp src/companion.cpp src/companion.hpp
  src/companion_board.cpp src/companion_board.hpp src/companion_delivery.cpp src/companion_delivery.hpp
  src/companion_exchange.cpp src/companion_inbox.cpp src/companion_inbox.hpp src/companion_location.cpp
  src/companion_location.hpp src/companion_messages.cpp src/companion_messages.hpp
  src/companion_mining.cpp src/companion_mining.hpp src/companion_partner.cpp
  src/companion_question.cpp src/companion_question.hpp src/companion_review.cpp
  src/companion_state.cpp src/companion_state.hpp src/companion_visit.cpp src/companion_visit.hpp
  src/conversation.cpp src/conversation.hpp src/diagnostic_files.cpp src/diagnostic_files.hpp
  src/dialogue_queue.cpp src/event_dispatch.cpp src/event_dispatch.hpp src/interaction_routes.cpp
  src/interaction_routes.hpp src/mail_reply_tool.cpp src/mail_reply_tool.hpp src/mailbox_intake.cpp
  src/mailbox_intake.hpp src/main_messages.cpp src/main_messages.hpp src/main_output.cpp
  src/message_store.cpp src/message_store.hpp src/persona.cpp src/persona.hpp src/reply_delivery.cpp
  src/reply_delivery.hpp src/room_mailbox.cpp src/run_log.cpp src/run_log.hpp src/software_log.cpp
  src/software_log.hpp src/software_log_files.cpp src/software_log_files.hpp src/subagents.cpp
  src/subagents.hpp src/tool_verification.cpp src/tool_verification.hpp src/trace_commands.hpp
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

add_library(dfy_management STATIC
  src/log_settings.cpp src/log_settings.hpp src/management/client.cpp src/management/client.hpp
  src/management/dialogue_channel.hpp src/management/host.cpp src/management/host.hpp
  src/management/local_pipe.cpp src/management/local_pipe.hpp src/management/message_protocol.cpp
  src/management/message_protocol.hpp src/management/poll_log.cpp src/management/poll_log.hpp
  src/management/protocol.cpp src/management/protocol.hpp src/management/resident.cpp
  src/management/resident.hpp src/management/resident_watch.cpp src/management/resident_watch.hpp
  src/management/room_location.hpp src/management/terminal_client.cpp
  src/management/terminal_client.hpp src/management/terminal_host.cpp src/management/terminal_host.hpp
  src/paths.cpp src/paths.hpp)
target_compile_definitions(dfy_management PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dfy_management PUBLIC dfy_application nlohmann_json::nlohmann_json
  PRIVATE dfy_terminal dfy_build_options dfy_digest advapi32 shell32 ole32)
dfy_group_target(dfy_management App)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
