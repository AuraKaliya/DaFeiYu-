add_executable(dfy_software_log_tests software_log.cpp)
add_dependencies(dfy_software_log_tests dfy_query_v2_fixture)
target_compile_definitions(dfy_software_log_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dfy_software_log_tests PRIVATE dfy_application dfy_build_options nlohmann_json::nlohmann_json)
set_target_properties(dfy_software_log_tests PROPERTIES FOLDER Tests)
add_test(NAME app.software_log COMMAND dfy_software_log_tests "${CMAKE_CURRENT_BINARY_DIR}/software-log" "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(app.software_log PROPERTIES TIMEOUT 12)

add_executable(dfy_terminal_entry_fixture terminal_entry_fixture.cpp)
target_link_libraries(dfy_terminal_entry_fixture PRIVATE dfy_management dfy_build_options)
add_dependencies(dfy_terminal_entry_fixture dafeiyu)
set_target_properties(dfy_terminal_entry_fixture PROPERTIES FOLDER Tests)
add_test(NAME app.terminal_entry COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/terminal_entry.ps1" -App "$<TARGET_FILE:dafeiyu>"
  -Fixture "$<TARGET_FILE:dfy_terminal_entry_fixture>" -Root "${CMAKE_CURRENT_BINARY_DIR}/terminal-entry")
set_tests_properties(app.terminal_entry PROPERTIES TIMEOUT 45)

add_executable(dfy_mc_work_receipt_tests mc_work_receipts.cpp ../plugins/mc/src/partner_facts.cpp)
target_link_libraries(dfy_mc_work_receipt_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
set_target_properties(dfy_mc_work_receipt_tests PROPERTIES FOLDER Tests)
add_test(NAME plugins.mc_work_receipts COMMAND dfy_mc_work_receipt_tests)

# Node 只用于游戏桥接组合检查，普通无插件构建保持独立。
add_executable(dfy_mc_playtest mc_playtest.cpp)
target_link_libraries(dfy_mc_playtest PRIVATE dfy_terminal nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_playtest dfy_mc_companion)
set_target_properties(dfy_mc_playtest PROPERTIES FOLDER "Tests")
add_executable(dfy_mc_game_visit_tests mc_game_visit.cpp)
target_link_libraries(dfy_mc_game_visit_tests PRIVATE dfy_terminal nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_game_visit_tests dfy_mc_companion)
set_target_properties(dfy_mc_game_visit_tests PROPERTIES FOLDER "Tests")
add_executable(dfy_mc_owner_inbox_tests mc_owner_inbox.cpp)
target_link_libraries(dfy_mc_owner_inbox_tests PRIVATE dfy_application dfy_build_options)
add_dependencies(dfy_mc_owner_inbox_tests dfy_mc_companion)
set_target_properties(dfy_mc_owner_inbox_tests PROPERTIES FOLDER "Tests")
add_executable(dfy_mc_native_visit_tests mc_native_visit.cpp)
target_link_libraries(dfy_mc_native_visit_tests PRIVATE dfy_plugin_tools nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_native_visit_tests dfy_mc_companion)
set_target_properties(dfy_mc_native_visit_tests PROPERTIES FOLDER "Tests")
add_executable(dfy_companion_session_tests companion_session.cpp)
target_link_libraries(dfy_companion_session_tests PRIVATE dfy_application dfy_build_options)
add_dependencies(dfy_companion_session_tests dfy_mc_companion dfy_status_example)
set_target_properties(dfy_companion_session_tests PROPERTIES FOLDER "Tests")
add_test(NAME app.companion_session COMMAND dfy_companion_session_tests "$<TARGET_FILE_DIR:dfy_mc_companion>"
  "$<TARGET_FILE_DIR:dfy_status_example>" "${CMAKE_CURRENT_BINARY_DIR}/companion-session")
set_tests_properties(app.companion_session PROPERTIES TIMEOUT 20)
find_program(DFY_MC_NODE NAMES node)
if(DFY_MC_NODE)
  add_executable(dfy_companion_board_tests companion_board.cpp)
  target_link_libraries(dfy_companion_board_tests PRIVATE dfy_terminal dfy_build_options)
  add_dependencies(dfy_companion_board_tests dfy_mc_companion)
  set_target_properties(dfy_companion_board_tests PROPERTIES FOLDER "Tests")
  add_test(NAME app.companion_board COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_companion_board_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-board" board-window)
  set_tests_properties(app.companion_board PROPERTIES TIMEOUT 20)
  add_executable(dfy_companion_review_tests companion_review.cpp)
  target_link_libraries(dfy_companion_review_tests PRIVATE dfy_terminal dfy_build_options)
  add_dependencies(dfy_companion_review_tests dfy_mc_companion)
  set_target_properties(dfy_companion_review_tests PROPERTIES FOLDER "Tests")
  add_test(NAME mc.player_feedback COMMAND "${DFY_MC_NODE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/player-feedback.test.cjs")
  foreach(mode IN ITEMS exploring blocked disconnect recheck gap)
    add_test(NAME app.review_${mode} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/exploration-host.cjs"
      "$<TARGET_FILE:dfy_companion_review_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-review" ${mode})
    set_tests_properties(app.review_${mode} PROPERTIES TIMEOUT 25)
  endforeach()
  add_executable(dfy_companion_exchange_tests companion_exchange.cpp)
  target_link_libraries(dfy_companion_exchange_tests PRIVATE dfy_application dfy_build_options)
  add_dependencies(dfy_companion_exchange_tests dfy_mc_companion)
  set_target_properties(dfy_companion_exchange_tests PROPERTIES FOLDER "Tests")
  add_test(NAME app.companion_exchange COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_companion_exchange_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-exchange")
  set_tests_properties(app.companion_exchange PROPERTIES TIMEOUT 20)
  add_executable(dfy_companion_state_tests companion_state.cpp)
  target_link_libraries(dfy_companion_state_tests PRIVATE dfy_terminal dfy_build_options)
  add_dependencies(dfy_companion_state_tests dfy_mc_companion)
  set_target_properties(dfy_companion_state_tests PROPERTIES FOLDER "Tests")
  add_test(NAME app.companion_state COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_companion_state_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-state")
  set_tests_properties(app.companion_state PROPERTIES TIMEOUT 20)
  add_executable(dfy_mc_plugin_tests mc_plugin.cpp)
  target_link_libraries(dfy_mc_plugin_tests PRIVATE dfy_plugin_tools dfy_build_options nlohmann_json::nlohmann_json)
  add_dependencies(dfy_mc_plugin_tests dfy_mc_companion)
  set_target_properties(dfy_mc_plugin_tests PROPERTIES FOLDER "Tests")
  add_test(NAME plugins.mc_world COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-world")
  set_tests_properties(plugins.mc_world PROPERTIES TIMEOUT 20)
  add_executable(dfy_mc_plugin_partner_tests mc_plugin_partner.cpp ../plugins/mc/src/partner_task.cpp)
  target_link_libraries(dfy_mc_plugin_partner_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_mc_plugin_partner_tests dfy_mc_companion)
  set_target_properties(dfy_mc_plugin_partner_tests PROPERTIES FOLDER "Tests")
  add_test(NAME plugins.mc_partner COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-loop)
  set_tests_properties(plugins.mc_partner PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_legacy_air COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-legacy-air)
  set_tests_properties(plugins.mc_partner_legacy_air PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_interrupt COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-interrupt)
  set_tests_properties(plugins.mc_partner_interrupt PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_controller_restart COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-controller-restart)
  set_tests_properties(plugins.mc_partner_controller_restart PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_motion COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-motion)
  set_tests_properties(plugins.mc_partner_motion PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_follow COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-follow)
  add_test(NAME plugins.mc_partner_follow_recovery COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-follow-recovery)
  set_tests_properties(plugins.mc_partner_follow_recovery PROPERTIES TIMEOUT 25)
  foreach(outcome collect collect-blocked)
    add_test(NAME plugins.mc_partner_follow_${outcome} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-follow-${outcome})
  endforeach()
  set_tests_properties(plugins.mc_partner_follow PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_confirmation COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-confirm)
  set_tests_properties(plugins.mc_partner_confirmation PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_refused_collection COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-refused-collection)
  set_tests_properties(plugins.mc_partner_refused_collection PROPERTIES TIMEOUT 25)
  foreach(entry queue plan)
    add_test(NAME plugins.mc_partner_refused_collection_${entry} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-refused-collection-${entry})
    set_tests_properties(plugins.mc_partner_refused_collection_${entry} PROPERTIES TIMEOUT 25)
  endforeach()
  add_test(NAME plugins.mc_partner_task COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-task)
  set_tests_properties(plugins.mc_partner_task PROPERTIES TIMEOUT 25)
  foreach(change notes goal scope approval direct-notes direct-recover-notes direct-scope direct-place direct-place-stale direct-approval direct-rejoin correction correction-lost correction-late navigation navigation-restart navigation-mixed repair-observe repair-plan repair-plan-observe observation-batch response-timing layered layered-blocked layered-stock attention question-scope question-batch question-pair question-pair-failed question-pair-body-failed opening-answer continuation-budget air-budget air-priority question-expiry pointed-support pointed-craft pointed-craft-no-pickup observe-dig-scope observe-dig-scope-plan observe-dig-scope-evidence observe-dig-scope-evidence-plan)
    add_test(NAME plugins.mc_partner_revision_${change} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-revision-${change})
    set_tests_properties(plugins.mc_partner_revision_${change} PROPERTIES TIMEOUT 25)
  endforeach()
  set_tests_properties(plugins.mc_partner_revision_question-expiry PROPERTIES TIMEOUT 60)
  add_test(NAME plugins.mc_partner_scene COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-scene)
  set_tests_properties(plugins.mc_partner_scene PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_departure COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-departure)
  set_tests_properties(plugins.mc_partner_departure PROPERTIES TIMEOUT 25)
  foreach(mode scenario released)
    add_test(NAME plugins.mc_partner_execution_${mode} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-execution-${mode})
    set_tests_properties(plugins.mc_partner_execution_${mode} PROPERTIES TIMEOUT 15)
  endforeach()
  foreach(delivery fallback sent unknown internal finish late retired capacity)
    add_test(NAME plugins.mc_partner_reply_${delivery} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-reply-${delivery})
    set_tests_properties(plugins.mc_partner_reply_${delivery} PROPERTIES TIMEOUT 15)
  endforeach()
  add_test(NAME plugins.mc_partner_work_lookup COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-partner" partner-reply-lookup)
  set_tests_properties(plugins.mc_partner_work_lookup PROPERTIES TIMEOUT 15)
  add_executable(dfy_mc_confirmation_route_tests mc_confirmation_route.cpp)
  target_link_libraries(dfy_mc_confirmation_route_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_mc_confirmation_route_tests dfy_mc_companion)
  set_target_properties(dfy_mc_confirmation_route_tests PROPERTIES FOLDER "Tests")
  foreach(route IN ITEMS panel game local manual first delayed lost-terminal retry-reply)
    add_test(NAME app.mc_confirmation_${route} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_confirmation_route_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-confirmation-route" partner-route-${route})
    set_tests_properties(app.mc_confirmation_${route} PROPERTIES TIMEOUT 25)
  endforeach()
  add_executable(dfy_mc_partner_exchange_tests mc_partner_exchange.cpp)
  target_link_libraries(dfy_mc_partner_exchange_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_mc_partner_exchange_tests dfy_mc_companion)
  set_target_properties(dfy_mc_partner_exchange_tests PROPERTIES FOLDER "Tests")
  add_test(NAME app.mc_partner_exchange COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/mc-companion/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_partner_exchange_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${CMAKE_CURRENT_BINARY_DIR}/mc-exchange" partner-exchange)
  set_tests_properties(app.mc_partner_exchange PROPERTIES TIMEOUT 25)
endif()

add_executable(dfy_tray_headless_host_tests ui_tray_headless_host.cpp)
target_link_libraries(dfy_tray_headless_host_tests PRIVATE dfy_management dfy_build_options user32)
set_target_properties(dfy_tray_headless_host_tests PROPERTIES FOLDER Tests)
add_test(NAME ui.tray_headless_host COMMAND dfy_tray_headless_host_tests "$<TARGET_FILE:dafeiyu>" "${CMAKE_CURRENT_BINARY_DIR}/tray-headless")
set_tests_properties(ui.tray_headless_host PROPERTIES TIMEOUT 25 SKIP_RETURN_CODE 77)
if(BUILD_GUI)
  set(tray_panel_option -WithPanel)
endif()
add_test(NAME ui.tray_resident COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/ui_tray_resident.ps1" -App "$<TARGET_FILE:dafeiyu>"
  -Root "${CMAKE_CURRENT_BINARY_DIR}/tray-resident" ${tray_panel_option})
set_tests_properties(ui.tray_resident PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

# 只在 GUI 构建中验证来源与 Frame 代次，不启动浏览器。
if(BUILD_GUI)
  # 只读桥接验收使用构建目录内可写的临时配置根。
  set(run_status_temp "${PROJECT_BINARY_DIR}/ui-run-temp")
  file(MAKE_DIRECTORY "${run_status_temp}")
  add_executable(dfy_ui_history_host ui_history_host.cpp)
  target_link_libraries(dfy_ui_history_host PRIVATE dfy_management dfy_build_options)
  set_target_properties(dfy_ui_history_host PROPERTIES FOLDER Tests)
  add_test(NAME ui.history_windows COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/ui_history_windows.ps1" -Panel "$<TARGET_FILE:dfy_ui_probe>"
    -Fixture "$<TARGET_FILE:dfy_ui_history_host>" -App "$<TARGET_FILE:dafeiyu>" -Root "${run_status_temp}")
  set_tests_properties(ui.history_windows PROPERTIES TIMEOUT 130 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
  add_test(NAME ui.run_status COMMAND dfy_ui_probe --run-status "$<TARGET_FILE_DIR:dfy_status_example>")
  set_tests_properties(ui.run_status PROPERTIES TIMEOUT 60 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
  add_test(NAME ui.resident_progress COMMAND dfy_ui_probe --check-panel-shutdown)
  set_tests_properties(ui.resident_progress PROPERTIES TIMEOUT 45 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
  add_test(NAME ui.resident_windows COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/ui_resident_windows.ps1" -Panel "$<TARGET_FILE:dfy_ui_probe>"
    -App "$<TARGET_FILE:dafeiyu>" -Root "${run_status_temp}")
  set_tests_properties(ui.resident_windows PROPERTIES TIMEOUT 75 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
  add_test(NAME ui.resident_exit COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/ui_resident_exit.ps1" -Panel "$<TARGET_FILE:dfy_ui_probe>"
    -App "$<TARGET_FILE:dafeiyu>" -Root "${run_status_temp}")
  set_tests_properties(ui.resident_exit PROPERTIES TIMEOUT 60 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
  add_test(NAME ui.resident_unexpected COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/ui_resident_exit.ps1" -Panel "$<TARGET_FILE:dfy_ui_probe>"
    -App "$<TARGET_FILE:dafeiyu>" -Root "${run_status_temp}" -Unexpected)
  set_tests_properties(ui.resident_unexpected PROPERTIES TIMEOUT 45 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
endif()

add_executable(dfy_session_tool_loop_tests session_tool_loop.cpp)
target_link_libraries(dfy_session_tool_loop_tests PRIVATE dfy_plugin_tools dfy_terminal dfy_build_options)
add_dependencies(dfy_session_tool_loop_tests dfy_tools_fixture)
set_target_properties(dfy_session_tool_loop_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/session_tool_loop.cpp")
add_test(NAME agent.session_tool_loop COMMAND dfy_session_tool_loop_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/session-tools-data/$<CONFIG>")
set_tests_properties(agent.session_tool_loop PROPERTIES TIMEOUT 15)
# 验证正式会话的可信确认入口及真实 DLL 等待、卸载与唯一效果。
add_executable(dfy_session_confirmation_tests session_confirmation.cpp)
target_link_libraries(dfy_session_confirmation_tests PRIVATE dfy_plugin_tools dfy_terminal dfy_build_options)
add_dependencies(dfy_session_confirmation_tests dfy_tools_fixture)
set_target_properties(dfy_session_confirmation_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/session_confirmation.cpp")
add_test(NAME agent.session_confirmation COMMAND dfy_session_confirmation_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/session-confirmation-data/$<CONFIG>")
set_tests_properties(agent.session_confirmation PROPERTIES TIMEOUT 15)
add_executable(dfy_observed_native_loop_tests observed_native_loop.cpp)
target_link_libraries(dfy_observed_native_loop_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_observed_native_loop_tests dfy_tools_fixture)
set_target_properties(dfy_observed_native_loop_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/observed_native_loop.cpp")
add_test(NAME agent.observed_native_loop COMMAND dfy_observed_native_loop_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/observed-native-data/$<CONFIG>")
set_tests_properties(agent.observed_native_loop PROPERTIES TIMEOUT 15)

add_executable(dfy_planned_native_loop_tests planned_native_loop.cpp)
target_link_libraries(dfy_planned_native_loop_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_planned_native_loop_tests dfy_tools_fixture)
set_target_properties(dfy_planned_native_loop_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/planned_native_loop.cpp")
add_test(NAME agent.planned_native_loop COMMAND dfy_planned_native_loop_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/planned-native-data/$<CONFIG>")
set_tests_properties(agent.planned_native_loop PROPERTIES TIMEOUT 15)
add_executable(dfy_native_action_feedback_tests native_action_feedback.cpp)
target_link_libraries(dfy_native_action_feedback_tests PRIVATE dfy_plugin_tools dfy_build_options)
dfy_compiled_replay(dfy_native_action_feedback_tests native_action_feedback.cpp "${PROJECT_SOURCE_DIR}/modules/agent/tests/support/scripted_model.hpp"
  fixtures/tools_v2/tools.c fixtures/tools_v2/plugin.json)
add_dependencies(dfy_native_action_feedback_tests dfy_tools_fixture)
set_target_properties(dfy_native_action_feedback_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/native_action_feedback.cpp")
add_test(NAME agent.native_action_feedback COMMAND dfy_native_action_feedback_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/native-feedback-data/$<CONFIG>")
add_test(NAME agent.replay_action_feedback COMMAND dfy_native_action_feedback_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/native-feedback-replay/$<CONFIG>" --capture native_action_satisfied)
add_test(NAME agent.replay_native_observation_action COMMAND dfy_native_action_feedback_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/native-observation-replay/$<CONFIG>" --capture native_observation_revoked)
set_tests_properties(agent.native_action_feedback agent.replay_action_feedback agent.replay_native_observation_action PROPERTIES TIMEOUT 15)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
"${CMAKE_CURRENT_SOURCE_DIR}/support/replay_material.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/support/replay_comparison.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/support/replay_evidence.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scenarios/confirmed-counter.json")

add_executable(dfy_native_observation_rebind_tests native_observation_rebind.cpp)
target_link_libraries(dfy_native_observation_rebind_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_native_observation_rebind_tests dfy_tools_fixture)
set_target_properties(dfy_native_observation_rebind_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/native_observation_rebind.cpp")
add_test(NAME agent.native_observation_rebind COMMAND dfy_native_observation_rebind_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/native-rebind-data/$<CONFIG>")
set_tests_properties(agent.native_observation_rebind PROPERTIES TIMEOUT 15)

add_executable(dfy_plugin_tools_tests plugin_tools.cpp)
target_link_libraries(dfy_plugin_tools_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_plugin_tools_tests dfy_tools_fixture dfy_query_v2_fixture)
set_target_properties(dfy_plugin_tools_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_tools.cpp")
add_test(NAME plugins.tools COMMAND dfy_plugin_tools_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/tools-data/$<CONFIG>" "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.tools PROPERTIES TIMEOUT 15)
add_executable(dfy_plugin_tool_work_tests plugin_tool_work.cpp)
target_link_libraries(dfy_plugin_tool_work_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_plugin_tool_work_tests dfy_tools_fixture)
set_target_properties(dfy_plugin_tool_work_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_tool_work.cpp")
add_test(NAME plugins.tool_work COMMAND dfy_plugin_tool_work_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/tool-work-data/$<CONFIG>")
set_tests_properties(plugins.tool_work PROPERTIES TIMEOUT 15)
add_subdirectory(fixtures/tools_v2)
add_executable(dfy_plugin_tool_abi_tests plugin_tool_abi.cpp)
target_link_libraries(dfy_plugin_tool_abi_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_tool_abi_tests dfy_tools_fixture)
set_target_properties(dfy_plugin_tool_abi_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_tool_abi.cpp")
add_test(NAME plugins.tool_abi COMMAND dfy_plugin_tool_abi_tests "$<TARGET_FILE:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/tool-abi-data/$<CONFIG>")
set_tests_properties(plugins.tool_abi PROPERTIES TIMEOUT 15)

# v2 查询消费者只依赖 SDK，通过真实 C DLL 验证独立入口与能力表协商。

# 使用独立真实 DLL 夹具测试平台加载器，不接入插件实例或 ABI 查询校验。

# 用可控真实 DLL 覆盖 ABI 查询结果及构造失败后的引用回收。
add_subdirectory(fixtures/query)
add_executable(dfy_plugin_module_tests plugin_module.cpp)
target_link_libraries(dfy_plugin_module_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_plugin_module_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_tests dfy_query_fixture dfy_query_example dfy_loader_dependency)

# 独立验证 v2 生产查询，不改变 v1 夹具或模块接口。
add_subdirectory(fixtures/query_v2_faults)
add_executable(dfy_plugin_module_v2_tests plugin_module_v2.cpp)
target_link_libraries(dfy_plugin_module_v2_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_plugin_module_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_v2_tests dfy_query_v2_faults dfy_query_example)
set_target_properties(dfy_plugin_module_v2_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_module_v2.cpp")
add_test(NAME plugins.module_v2 COMMAND dfy_plugin_module_v2_tests "$<TARGET_FILE:dfy_query_v2_faults>"
  "$<TARGET_FILE:dfy_query_example>" "${CMAKE_CURRENT_BINARY_DIR}/module-v2/$<CONFIG>")
set_tests_properties(plugins.module_v2 PROPERTIES TIMEOUT 15)

# 真实 C DLL 查询生产宿主服务，校验调用作用域与引用隔离。
add_executable(dfy_plugin_host_tests plugin_host.cpp)
target_link_libraries(dfy_plugin_host_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_host_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_host_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_host.cpp")
add_test(NAME plugins.host COMMAND dfy_plugin_host_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.host PROPERTIES TIMEOUT 15)

# 纯 C 消费生产读取服务，核对完整请求与载荷预算的寿命关系。
add_executable(dfy_plugin_reads_tests plugin_reads.cpp)
target_link_libraries(dfy_plugin_reads_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_reads_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_reads_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_reads.cpp")
add_test(NAME plugins.reads COMMAND dfy_plugin_reads_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.reads PROPERTIES TIMEOUT 15)

# 使用真实 C 回调串联生产读取、通知、资源登记和代码活动保护。
add_executable(dfy_plugin_notifications_tests plugin_notifications.cpp)
target_link_libraries(dfy_plugin_notifications_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_notifications_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_notifications_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_notifications.cpp")
add_test(NAME plugins.notifications COMMAND dfy_plugin_notifications_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.notifications PROPERTIES TIMEOUT 15)

add_executable(dfy_plugin_mailbox_dll_tests plugin_mailbox_dll.cpp)
target_link_libraries(dfy_plugin_mailbox_dll_tests PRIVATE dfy_management dfy_build_options)
add_dependencies(dfy_plugin_mailbox_dll_tests dfy_mailbox_fixture dfy_status_example)
set_target_properties(dfy_plugin_mailbox_dll_tests PROPERTIES FOLDER "Tests")
add_test(NAME plugins.mailbox_dll COMMAND dfy_plugin_mailbox_dll_tests "$<TARGET_FILE:dfy_mailbox_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/mailbox-dll" "$<TARGET_FILE_DIR:dfy_status_example>")

# 真实 C 实例验证停止受理与停止完成分离，保持 v1 生命周期契约冻结。
add_executable(dfy_sdk_lifecycle_v2_tests sdk_lifecycle_v2.cpp)
target_link_libraries(dfy_sdk_lifecycle_v2_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_sdk_lifecycle_v2_tests dfy_query_v2_fixture)
set_target_properties(dfy_sdk_lifecycle_v2_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/sdk_lifecycle_v2.cpp")
add_test(NAME sdk.lifecycle_v2 COMMAND dfy_sdk_lifecycle_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.lifecycle_v2 PROPERTIES TIMEOUT 15)

# 真实 C 实例验收宿主控制器和停止失败后的可查询保留状态。
add_executable(dfy_plugin_instance_tests plugin_instance.cpp)
target_link_libraries(dfy_plugin_instance_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_plugin_instance_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_instance_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_instance_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_instance.cpp")
add_test(NAME plugins.instance COMMAND dfy_plugin_instance_tests "$<TARGET_FILE:dfy_query_v2_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/instance/$<CONFIG>")
set_tests_properties(plugins.instance PROPERTIES TIMEOUT 15)

# 实际纯 C 观察插件只通过公开 ABI 消费生产事件，不链接到宿主测试。
add_executable(dfy_observer_tests observer.cpp)
target_link_libraries(dfy_observer_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_observer_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_observer_tests dfy_observer)
set_target_properties(dfy_observer_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/observer.cpp")
add_test(NAME plugins.observer COMMAND dfy_observer_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/observer-data/$<CONFIG>")
set_tests_properties(plugins.observer PROPERTIES TIMEOUT 15)

# 真实 C 消费者验证嵌套反射与递归别名的原始事件预算。
add_executable(dfy_plugin_nested_tests plugin_nested.cpp)
target_link_libraries(dfy_plugin_nested_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_nested_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_nested_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_nested.cpp")
add_test(NAME plugins.nested COMMAND dfy_plugin_nested_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.nested PROPERTIES TIMEOUT 15)

# 验证版本化 C 枚举服务的精确值、借用和独立描述持有。
add_executable(dfy_plugin_enums_tests plugin_enums.cpp)
target_link_libraries(dfy_plugin_enums_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_enums_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_enums_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_enums.cpp")
add_test(NAME plugins.enums COMMAND dfy_plugin_enums_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.enums PROPERTIES TIMEOUT 15)

# 验证联合分支反射、稳定标签、实际内容及独立引用寿命。
add_executable(dfy_plugin_unions_tests plugin_unions.cpp)
target_link_libraries(dfy_plugin_unions_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_unions_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_unions_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_unions.cpp")
add_test(NAME plugins.unions COMMAND dfy_plugin_unions_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.unions PROPERTIES TIMEOUT 15)

# 验证版本 2 组合数组的数据、匿名元素约束与叶引用寿命。
add_executable(dfy_plugin_arrays_tests plugin_arrays.cpp)
target_link_libraries(dfy_plugin_arrays_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_arrays_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_arrays_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_arrays.cpp")
add_test(NAME plugins.arrays COMMAND dfy_plugin_arrays_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.arrays PROPERTIES TIMEOUT 15)

# 验证版本 2 组合映射的精确键、完整值约束和数据寿命。
add_executable(dfy_plugin_maps_tests plugin_maps.cpp)
target_link_libraries(dfy_plugin_maps_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_maps_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_maps_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_maps.cpp")
add_test(NAME plugins.maps COMMAND dfy_plugin_maps_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.maps PROPERTIES TIMEOUT 15)

# 真实 C 消费者验证指定投递、独立宿主域与广播混合顺序。
add_executable(dfy_plugin_targets_tests plugin_targets.cpp)
target_link_libraries(dfy_plugin_targets_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_targets_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_targets_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_targets.cpp")
add_test(NAME plugins.targets COMMAND dfy_plugin_targets_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.targets PROPERTIES TIMEOUT 15)

# 通过真实 C 读取服务验证组成员控制器和跨宿主独立停止。
add_executable(dfy_plugin_group_reads_tests plugin_group_reads.cpp)
target_link_libraries(dfy_plugin_group_reads_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_group_reads_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_group_reads_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_group_reads.cpp")
add_test(NAME plugins.group_reads COMMAND dfy_plugin_group_reads_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.group_reads PROPERTIES TIMEOUT 15)

# 真实 C 回调验证组成员退出与代码活动保护、排队撤销的独立边界。
add_executable(dfy_plugin_group_notifications_tests plugin_group_notifications.cpp)
target_link_libraries(dfy_plugin_group_notifications_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_group_notifications_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_group_notifications_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_group_notifications.cpp")
add_test(NAME plugins.group_notifications COMMAND dfy_plugin_group_notifications_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.group_notifications PROPERTIES TIMEOUT 15)

# 验证一个进程内从空插件问答到安装、观察、卸载及移除的完整链路。
add_executable(dfy_application_lifecycle_tests application_lifecycle.cpp)
add_dependencies(dfy_application_lifecycle_tests dfy_observer)
target_link_libraries(dfy_application_lifecycle_tests PRIVATE dfy_application dfy_build_options nlohmann_json::nlohmann_json)
set_target_properties(dfy_application_lifecycle_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/application_lifecycle.cpp")
add_test(NAME app.lifecycle COMMAND dfy_application_lifecycle_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/application-data/$<CONFIG>")

add_executable(dfy_application_tools_tests application_tools.cpp)
add_dependencies(dfy_application_tools_tests dfy_tools_fixture)
target_link_libraries(dfy_application_tools_tests PRIVATE dfy_terminal dfy_build_options)
set_target_properties(dfy_application_tools_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/application_tools.cpp")
add_test(NAME app.tools COMMAND dfy_application_tools_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/application-tools/$<CONFIG>")
set_tests_properties(app.tools PROPERTIES TIMEOUT 20)

add_executable(dfy_status_example_tests status_example.cpp)
add_dependencies(dfy_status_example_tests dfy_status_example)
target_link_libraries(dfy_status_example_tests PRIVATE dfy_plugin_tools dfy_build_options)
set_target_properties(dfy_status_example_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/status_example.cpp")
add_test(NAME plugins.status_example COMMAND dfy_status_example_tests "$<TARGET_FILE_DIR:dfy_status_example>" "${CMAKE_CURRENT_BINARY_DIR}/status-example/$<CONFIG>")
set_tests_properties(plugins.status_example PROPERTIES TIMEOUT 15)

add_executable(dfy_tool_verification_tests tool_verification.cpp)
add_dependencies(dfy_tool_verification_tests dfy_status_example)
target_link_libraries(dfy_tool_verification_tests PRIVATE dfy_terminal dfy_build_options)
dfy_compiled_replay(dfy_tool_verification_tests tool_verification.cpp)
set_target_properties(dfy_tool_verification_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/tool_verification.cpp")
add_test(NAME app.tool_verification COMMAND dfy_tool_verification_tests "$<TARGET_FILE_DIR:dfy_status_example>" "${CMAKE_CURRENT_BINARY_DIR}/tool-verification/$<CONFIG>")
add_test(NAME app.replay_task COMMAND dfy_tool_verification_tests "$<TARGET_FILE_DIR:dfy_status_example>"
  "${CMAKE_CURRENT_BINARY_DIR}/tool-verification-replay/$<CONFIG>" --capture app_task_reconnect)
set_tests_properties(app.tool_verification app.replay_task PROPERTIES TIMEOUT 15)
set_tests_properties(app.lifecycle PROPERTIES TIMEOUT 15)

# 验证卸载后移除、用户配置保持及私有清理残留重试。
add_executable(dfy_package_remove_tests package_remove.cpp)
add_dependencies(dfy_package_remove_tests dfy_observer)
target_link_libraries(dfy_package_remove_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_package_remove_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_package_remove_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/package_remove.cpp")
add_test(NAME plugins.remove COMMAND dfy_package_remove_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/remove-data/$<CONFIG>")
set_tests_properties(plugins.remove PROPERTIES TIMEOUT 15)

# 验证本地包暂存、默认配置复核、同名拒绝和发布回滚。
add_executable(dfy_package_install_tests package_install.cpp)
add_dependencies(dfy_package_install_tests dfy_observer)
target_link_libraries(dfy_package_install_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_package_install_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_package_install_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/package_install.cpp")
add_test(NAME plugins.install COMMAND dfy_package_install_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/install-data/$<CONFIG>")
set_tests_properties(plugins.install PROPERTIES TIMEOUT 15)

# 验证插件本地命令、中文路径解析及真实加载卸载。
add_executable(dfy_plugin_command_tests plugin_commands.cpp)
add_dependencies(dfy_plugin_command_tests dfy_observer)
target_link_libraries(dfy_plugin_command_tests PRIVATE dfy_terminal dfy_build_options)
set_target_properties(dfy_plugin_command_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_commands.cpp")
add_test(NAME app.plugin_commands COMMAND dfy_plugin_command_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/command-data/$<CONFIG>")
set_tests_properties(app.plugin_commands PROPERTIES TIMEOUT 15)

# 验证管理器发现、加载、消费、卸载及失败状态整体保留。
add_executable(dfy_plugin_manager_tests plugin_manager.cpp)
add_dependencies(dfy_plugin_manager_tests dfy_observer dfy_query_v2_fixture)
target_link_libraries(dfy_plugin_manager_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_manager_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_manager.cpp")
add_test(NAME plugins.manager COMMAND dfy_plugin_manager_tests "$<TARGET_FILE_DIR:dfy_observer>" "$<TARGET_FILE:dfy_query_v2_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/manager-data/$<CONFIG>")
set_tests_properties(plugins.manager PROPERTIES TIMEOUT 15)

# 验证真实插件实例依赖、环检查及旧绑定代码寿命。
add_executable(dfy_plugin_dependency_tests plugin_dependencies.cpp)
add_dependencies(dfy_plugin_dependency_tests dfy_observer)
target_link_libraries(dfy_plugin_dependency_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_dependency_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_dependencies.cpp")
add_test(NAME plugins.dependencies COMMAND dfy_plugin_dependency_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/dependency-data/$<CONFIG>")
set_tests_properties(plugins.dependencies PROPERTIES TIMEOUT 15)

# 验证真实纯 C DLL 的分步处理与跨配置数据、代码寿命。
add_executable(dfy_plugin_processing_dll_tests plugin_processing_dll.cpp)
target_link_libraries(dfy_plugin_processing_dll_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_processing_dll_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_processing_dll.cpp")
add_test(NAME plugins.processing_dll COMMAND dfy_plugin_processing_dll_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.processing_dll PROPERTIES TIMEOUT 15)

# 组织 IDE 展示：测试自身源码按本目录排列，借用的业务源码保留来源模块路径。
set_target_properties(dfy_plugin_module_tests PROPERTIES FOLDER "Tests")

# 注册 CTest：配置测试使用隔离工作目录，全部测试统一设置执行时限。
add_test(NAME plugins.module COMMAND dfy_plugin_module_tests "$<TARGET_FILE:dfy_query_fixture>"
  "$<TARGET_FILE:dfy_query_example>" "$<TARGET_FILE:dfy_loader_dependency>" "${CMAKE_CURRENT_BINARY_DIR}/module/$<CONFIG>")
set_tests_properties(plugins.module PROPERTIES TIMEOUT 15)
