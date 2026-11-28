dfy_test_executable(dfy_software_log_tests software_log.cpp)
add_dependencies(dfy_software_log_tests dfy_query_v2_fixture)
target_compile_definitions(dfy_software_log_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dfy_software_log_tests PRIVATE dfy_application dfy_build_options nlohmann_json::nlohmann_json)
add_test(NAME app.software_log COMMAND dfy_software_log_tests "${PROJECT_BINARY_DIR}/tests/software-log" "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(app.software_log PROPERTIES TIMEOUT 12)

if(BUILD_CLI)
  dfy_test_executable(dfy_terminal_entry_fixture terminal_entry_fixture.cpp)
  target_link_libraries(dfy_terminal_entry_fixture PRIVATE dfy_management dfy_build_options)
  add_dependencies(dfy_terminal_entry_fixture dafeiyu-cli)
  add_test(NAME app.terminal_entry COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/terminal_entry.ps1" -App "$<TARGET_FILE:dafeiyu-cli>"
    -Fixture "$<TARGET_FILE:dfy_terminal_entry_fixture>" -Root "${PROJECT_BINARY_DIR}/tests/terminal-entry"
    -Program "$<TARGET_FILE:dfy_task_memory_tests>" -Panel "$<TARGET_FILE_DIR:dfy_status_example>"
    -Tools "$<TARGET_FILE_DIR:dfy_tools_fixture>" -Corpus "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/tasks.jsonl")
  set_tests_properties(app.terminal_entry PROPERTIES TIMEOUT 45)
endif()

# 验证一个进程内从空插件问答到安装、观察、卸载及移除的完整链路。
dfy_test_executable(dfy_application_lifecycle_tests application_lifecycle.cpp)
add_dependencies(dfy_application_lifecycle_tests dfy_observer)
target_link_libraries(dfy_application_lifecycle_tests PRIVATE dfy_application dfy_build_options nlohmann_json::nlohmann_json)
add_test(NAME app.lifecycle COMMAND dfy_application_lifecycle_tests "$<TARGET_FILE_DIR:dfy_observer>" "${PROJECT_BINARY_DIR}/tests/application-data/$<CONFIG>")

dfy_test_executable(dfy_application_tools_tests application_tools.cpp)
add_dependencies(dfy_application_tools_tests dfy_tools_fixture)
target_link_libraries(dfy_application_tools_tests PRIVATE dfy_terminal dfy_build_options)
add_test(NAME app.tools COMMAND dfy_application_tools_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${PROJECT_BINARY_DIR}/tests/application-tools/$<CONFIG>")
set_tests_properties(app.tools PROPERTIES TIMEOUT 20)

dfy_test_executable(dfy_status_example_tests status_example.cpp)
add_dependencies(dfy_status_example_tests dfy_status_example)
target_link_libraries(dfy_status_example_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_test(NAME plugins.status_example COMMAND dfy_status_example_tests "$<TARGET_FILE_DIR:dfy_status_example>" "${PROJECT_BINARY_DIR}/tests/status-example/$<CONFIG>")
set_tests_properties(plugins.status_example PROPERTIES TIMEOUT 15)

dfy_test_executable(dfy_tool_verification_tests tool_verification.cpp)
add_dependencies(dfy_tool_verification_tests dfy_status_example)
target_link_libraries(dfy_tool_verification_tests PRIVATE dfy_agent_test_support dfy_terminal dfy_build_options)
dfy_compiled_replay(dfy_tool_verification_tests tool_verification.cpp)
add_test(NAME app.tool_verification COMMAND dfy_tool_verification_tests "$<TARGET_FILE_DIR:dfy_status_example>" "${PROJECT_BINARY_DIR}/tests/tool-verification/$<CONFIG>")
add_test(NAME app.replay_task COMMAND dfy_tool_verification_tests "$<TARGET_FILE_DIR:dfy_status_example>"
  "${PROJECT_BINARY_DIR}/tests/tool-verification-replay/$<CONFIG>" --capture app_task_reconnect)
set_tests_properties(app.tool_verification app.replay_task PROPERTIES TIMEOUT 15)
dfy_test_executable(dfy_task_memory_tests task_memory.cpp)
target_sources(dfy_task_memory_tests PRIVATE task_execution_fixture.cpp task_resume.cpp tone_replay.cpp task_understanding_cases.cpp understanding_recall.cpp comparison_archive.cpp channel_observations.cpp task_memory_fixture.hpp)
add_dependencies(dfy_task_memory_tests dfy_status_example dfy_tools_fixture dfy_freight_fixture dfy_museum_fixture)
if(BUILD_CLI)
  add_dependencies(dfy_terminal_entry_fixture dfy_task_memory_tests)
endif()
target_link_libraries(dfy_task_memory_tests PRIVATE dfy_application dfy_build_options nlohmann_json::nlohmann_json winsqlite3)
add_test(NAME app.task_memory COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/tasks.jsonl" "${PROJECT_BINARY_DIR}/tests/task-memory/$<CONFIG>")
set_tests_properties(app.task_memory PROPERTIES TIMEOUT 90)
add_test(NAME app.task_resume COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:dfy_task_memory_tests>"
  "-DPANEL=$<TARGET_FILE_DIR:dfy_status_example>" "-DTOOLS=$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "-DCORPUS=${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/tasks.jsonl" "-DROOT=${PROJECT_BINARY_DIR}/tests/task-resume/$<CONFIG>"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/task_resume.cmake")
set_tests_properties(app.task_resume PROPERTIES TIMEOUT 90)
add_test(NAME app.tone_replay COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:dfy_task_memory_tests>"
  "-DROOT=${PROJECT_BINARY_DIR}/tests/tone-replay/$<CONFIG>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tone_replay.cmake")
add_test(NAME app.private_tone_replay COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:dfy_task_memory_tests>"
  "-DROOT=${PROJECT_BINARY_DIR}/tests/private-tone-replay/$<CONFIG>" -DPRIVATE_CHAT=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/tone_replay.cmake")
add_test(NAME app.semantic_tone_replay COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:dfy_task_memory_tests>"
  "-DROOT=${PROJECT_BINARY_DIR}/tests/semantic-tone-replay/$<CONFIG>" -DSEMANTIC_CHAT=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/tone_replay.cmake")
add_test(NAME app.cached_tone_replay COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:dfy_task_memory_tests>"
  "-DROOT=${PROJECT_BINARY_DIR}/tests/cached-tone-replay/$<CONFIG>" -DCACHE_CHAT=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/tone_replay.cmake")
add_test(NAME app.recent_dialogue_replay COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:dfy_task_memory_tests>"
  "-DROOT=${PROJECT_BINARY_DIR}/tests/recent-dialogue-replay/$<CONFIG>" -DRECENT_CHAT=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/tone_replay.cmake")
set_tests_properties(app.recent_dialogue_replay PROPERTIES TIMEOUT 90)
set_tests_properties(app.tone_replay app.private_tone_replay app.semantic_tone_replay app.cached_tone_replay PROPERTIES TIMEOUT 90)
add_test(NAME app.channel_observations COMMAND dfy_task_memory_tests "$<TARGET_FILE:dfy_freight_fixture>" "$<TARGET_FILE:dfy_museum_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/channel_observations.jsonl" "${PROJECT_BINARY_DIR}/tests/channel-observations/$<CONFIG>" --channels)
set_tests_properties(app.channel_observations PROPERTIES TIMEOUT 90)
add_test(NAME app.task_understanding COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/understandings.jsonl" "${PROJECT_BINARY_DIR}/tests/task-understanding/$<CONFIG>" --understandings)
set_tests_properties(app.task_understanding PROPERTIES TIMEOUT 90)
add_test(NAME app.understanding_recall COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/understanding_queries.jsonl" "${PROJECT_BINARY_DIR}/tests/understanding-recall/$<CONFIG>" --recall)
set_tests_properties(app.understanding_recall PROPERTIES TIMEOUT 90)
add_test(NAME app.comparison_recall COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/comparison_queries.jsonl" "${PROJECT_BINARY_DIR}/tests/comparison-recall/$<CONFIG>" --case-recall)
set_tests_properties(app.comparison_recall PROPERTIES TIMEOUT 120)
add_test(NAME app.comparison_paraphrases COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/comparison_paraphrases.jsonl" "${PROJECT_BINARY_DIR}/tests/comparison-paraphrases/$<CONFIG>" --case-recall)
set_tests_properties(app.comparison_paraphrases PROPERTIES TIMEOUT 120)
add_test(NAME app.comparison_term_changes COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/comparison_term_changes.jsonl" "${PROJECT_BINARY_DIR}/tests/comparison-term-changes/$<CONFIG>" --case-recall)
set_tests_properties(app.comparison_term_changes PROPERTIES TIMEOUT 120)
add_test(NAME app.comparison_history_budget COMMAND dfy_task_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/long_case_queries.jsonl" "${PROJECT_BINARY_DIR}/tests/comparison-history/$<CONFIG>" --long-case)
set_tests_properties(app.comparison_history_budget PROPERTIES TIMEOUT 90)
dfy_test_executable(dfy_child_memory_tests child_memory.cpp)
target_sources(dfy_child_memory_tests PRIVATE task_execution_fixture.cpp knowledge_probe.cpp knowledge_probe.hpp comparison_probe.cpp archive_fixture.cpp archive_fixture.hpp)
target_link_libraries(dfy_child_memory_tests PRIVATE dfy_host_commands dfy_build_options nlohmann_json::nlohmann_json)
add_dependencies(dfy_child_memory_tests dfy_status_example dfy_tools_fixture)
add_test(NAME app.child_memory COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/child_memory.jsonl" "${PROJECT_BINARY_DIR}/tests/child-memory/$<CONFIG>")
set_tests_properties(app.child_memory PROPERTIES TIMEOUT 60)
add_test(NAME app.child_letters COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/child_letters.jsonl" "${PROJECT_BINARY_DIR}/tests/child-letters/$<CONFIG>")
set_tests_properties(app.child_letters PROPERTIES TIMEOUT 60)
add_test(NAME app.knowledge_publication COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/knowledge.jsonl" "${PROJECT_BINARY_DIR}/tests/knowledge/$<CONFIG>")
set_tests_properties(app.knowledge_publication PROPERTIES TIMEOUT 60)
add_test(NAME app.cross_tool_memory COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/comparisons.jsonl" "${PROJECT_BINARY_DIR}/tests/comparisons/$<CONFIG>")
set_tests_properties(app.cross_tool_memory PROPERTIES TIMEOUT 90)
add_test(NAME app.letter_digest COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/letter_digests.jsonl" "${PROJECT_BINARY_DIR}/tests/letter-digests/$<CONFIG>")
set_tests_properties(app.letter_digest PROPERTIES TIMEOUT 90)
add_test(NAME app.memory_archive_recovery COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/archive_recovery.jsonl" "${PROJECT_BINARY_DIR}/tests/memory-archive/$<CONFIG>")
set_tests_properties(app.memory_archive_recovery PROPERTIES TIMEOUT 90)
add_test(NAME app.child_memory_context COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/child_context.jsonl" "${PROJECT_BINARY_DIR}/tests/child-context/$<CONFIG>")
set_tests_properties(app.child_memory_context PROPERTIES TIMEOUT 90)
add_test(NAME app.memory_idle_schedule COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/idle_schedule.jsonl" "${PROJECT_BINARY_DIR}/tests/idle-schedule/$<CONFIG>")
set_tests_properties(app.memory_idle_schedule PROPERTIES TIMEOUT 90)
add_test(NAME app.letter_authority COMMAND dfy_child_memory_tests "$<TARGET_FILE_DIR:dfy_status_example>" "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${PROJECT_SOURCE_DIR}/apps/host/tests/memory_recall/letter_authority.jsonl" "${PROJECT_BINARY_DIR}/tests/letter-authority/$<CONFIG>")
set_tests_properties(app.letter_authority PROPERTIES TIMEOUT 90)
set_tests_properties(app.lifecycle PROPERTIES TIMEOUT 15)

# 验证插件本地命令、中文路径解析及真实加载卸载。
dfy_test_executable(dfy_plugin_command_tests plugin_commands.cpp)
add_dependencies(dfy_plugin_command_tests dfy_observer)
target_link_libraries(dfy_plugin_command_tests PRIVATE dfy_terminal dfy_build_options)
add_test(NAME app.plugin_commands COMMAND dfy_plugin_command_tests "$<TARGET_FILE_DIR:dfy_observer>" "${PROJECT_BINARY_DIR}/tests/command-data/$<CONFIG>")
set_tests_properties(app.plugin_commands PROPERTIES TIMEOUT 15)
