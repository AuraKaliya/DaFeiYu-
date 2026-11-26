dfy_test_executable(dfy_mc_work_receipt_tests mc_work_receipts.cpp ../../plugins/mc/src/partner_facts.cpp)
target_link_libraries(dfy_mc_work_receipt_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
add_test(NAME plugins.mc_work_receipts COMMAND dfy_mc_work_receipt_tests)

dfy_test_executable(dfy_mc_status_sample_tests mc_status_sample.cpp ../../plugins/mc/src/partner_facts.cpp ../../plugins/mc/src/partner_task.cpp)
target_link_libraries(dfy_mc_status_sample_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_status_sample_tests dfy_mc_companion)
add_test(NAME plugins.mc_status_sample COMMAND dfy_mc_status_sample_tests "$<TARGET_FILE_DIR:dfy_mc_companion>")

# Node 只用于游戏桥接组合检查，普通无插件构建保持独立。
dfy_test_executable(dfy_mc_playtest mc_playtest.cpp)
target_link_libraries(dfy_mc_playtest PRIVATE dfy_terminal nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_playtest dfy_mc_companion)
dfy_test_executable(dfy_mc_game_visit_tests mc_game_visit.cpp)
target_link_libraries(dfy_mc_game_visit_tests PRIVATE dfy_terminal nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_game_visit_tests dfy_mc_companion)
dfy_test_executable(dfy_mc_owner_inbox_tests mc_owner_inbox.cpp)
target_link_libraries(dfy_mc_owner_inbox_tests PRIVATE dfy_application dfy_build_options)
add_dependencies(dfy_mc_owner_inbox_tests dfy_mc_companion)
dfy_test_executable(dfy_mc_native_visit_tests mc_native_visit.cpp)
target_link_libraries(dfy_mc_native_visit_tests PRIVATE dfy_plugin_tools nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_mc_native_visit_tests dfy_mc_companion)
dfy_test_executable(dfy_companion_session_tests companion_session.cpp)
target_link_libraries(dfy_companion_session_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
add_dependencies(dfy_companion_session_tests dfy_mc_companion dfy_status_example)
add_test(NAME app.companion_session COMMAND dfy_companion_session_tests "$<TARGET_FILE_DIR:dfy_mc_companion>"
  "$<TARGET_FILE_DIR:dfy_status_example>" "${PROJECT_BINARY_DIR}/tests/companion-session")
set_tests_properties(app.companion_session PROPERTIES TIMEOUT 20)
find_program(DFY_MC_NODE NAMES node)
if(DFY_MC_NODE)
  dfy_test_executable(dfy_companion_board_tests companion_board.cpp)
  target_link_libraries(dfy_companion_board_tests PRIVATE dfy_terminal dfy_build_options)
  add_dependencies(dfy_companion_board_tests dfy_mc_companion)
  add_test(NAME app.companion_board COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_companion_board_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-board" board-window)
  set_tests_properties(app.companion_board PROPERTIES TIMEOUT 20)
  dfy_test_executable(dfy_companion_review_tests companion_review.cpp)
  target_link_libraries(dfy_companion_review_tests PRIVATE dfy_terminal nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_companion_review_tests dfy_mc_companion)
  add_test(NAME mc.player_feedback COMMAND "${DFY_MC_NODE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/player-feedback.test.cjs")
  add_test(NAME mc.status_replay COMMAND "${DFY_MC_NODE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/status-request.test.cjs")
  foreach(mode IN ITEMS exploring blocked disconnect recheck gap)
    add_test(NAME app.review_${mode} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/exploration-host.cjs"
      "$<TARGET_FILE:dfy_companion_review_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-review" ${mode})
    set_tests_properties(app.review_${mode} PROPERTIES TIMEOUT 25)
  endforeach()
  dfy_test_executable(dfy_companion_exchange_tests companion_exchange.cpp)
  target_link_libraries(dfy_companion_exchange_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_companion_exchange_tests dfy_mc_companion)
  add_test(NAME app.companion_exchange COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_companion_exchange_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-exchange")
  set_tests_properties(app.companion_exchange PROPERTIES TIMEOUT 20)
  dfy_test_executable(dfy_companion_state_tests companion_state.cpp)
  target_link_libraries(dfy_companion_state_tests PRIVATE dfy_terminal dfy_build_options)
  add_dependencies(dfy_companion_state_tests dfy_mc_companion)
  add_test(NAME app.companion_state COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_companion_state_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-state")
  set_tests_properties(app.companion_state PROPERTIES TIMEOUT 20)
  dfy_test_executable(dfy_mc_plugin_tests mc_plugin.cpp)
  target_link_libraries(dfy_mc_plugin_tests PRIVATE dfy_plugin_tools dfy_build_options nlohmann_json::nlohmann_json)
  add_dependencies(dfy_mc_plugin_tests dfy_mc_companion)
  add_test(NAME plugins.mc_world COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-world")
  set_tests_properties(plugins.mc_world PROPERTIES TIMEOUT 20)
  dfy_test_executable(dfy_mc_plugin_partner_tests mc_plugin_partner.cpp ../../plugins/mc/src/partner_task.cpp)
  target_link_libraries(dfy_mc_plugin_partner_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_mc_plugin_partner_tests dfy_mc_companion)
  add_test(NAME plugins.mc_status_request COMMAND dfy_mc_plugin_partner_tests --status-request)
  add_test(NAME plugins.mc_partner COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-loop)
  set_tests_properties(plugins.mc_partner PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_legacy_air COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-legacy-air)
  set_tests_properties(plugins.mc_partner_legacy_air PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_interrupt COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-interrupt)
  set_tests_properties(plugins.mc_partner_interrupt PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_controller_restart COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-controller-restart)
  set_tests_properties(plugins.mc_partner_controller_restart PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_motion COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-motion)
  set_tests_properties(plugins.mc_partner_motion PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_follow COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-follow)
  add_test(NAME plugins.mc_partner_follow_recovery COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-follow-recovery)
  set_tests_properties(plugins.mc_partner_follow_recovery PROPERTIES TIMEOUT 25)
  foreach(outcome collect collect-blocked)
    add_test(NAME plugins.mc_partner_follow_${outcome} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-follow-${outcome})
  endforeach()
  set_tests_properties(plugins.mc_partner_follow PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_confirmation COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-confirm)
  set_tests_properties(plugins.mc_partner_confirmation PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_refused_collection COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-refused-collection)
  set_tests_properties(plugins.mc_partner_refused_collection PROPERTIES TIMEOUT 25)
  foreach(entry queue plan)
    add_test(NAME plugins.mc_partner_refused_collection_${entry} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-refused-collection-${entry})
    set_tests_properties(plugins.mc_partner_refused_collection_${entry} PROPERTIES TIMEOUT 25)
  endforeach()
  add_test(NAME plugins.mc_partner_task COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-task)
  set_tests_properties(plugins.mc_partner_task PROPERTIES TIMEOUT 25)
  foreach(change notes goal scope approval direct-notes direct-recover-notes direct-scope direct-place direct-place-stale direct-approval direct-rejoin correction correction-lost correction-late navigation navigation-restart navigation-mixed repair-observe repair-plan repair-plan-observe observation-batch response-timing layered layered-blocked layered-stock attention question-scope question-batch question-pair question-pair-failed question-pair-body-failed opening-answer continuation-budget air-budget air-priority question-expiry pointed-support pointed-craft pointed-craft-no-pickup observe-dig-scope observe-dig-scope-plan observe-dig-scope-evidence observe-dig-scope-evidence-plan)
    add_test(NAME plugins.mc_partner_revision_${change} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-revision-${change})
    set_tests_properties(plugins.mc_partner_revision_${change} PROPERTIES TIMEOUT 25)
  endforeach()
  set_tests_properties(plugins.mc_partner_revision_question-expiry PROPERTIES TIMEOUT 60)
  add_test(NAME plugins.mc_partner_scene COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-scene)
  set_tests_properties(plugins.mc_partner_scene PROPERTIES TIMEOUT 25)
  add_test(NAME plugins.mc_partner_departure COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-departure)
  set_tests_properties(plugins.mc_partner_departure PROPERTIES TIMEOUT 25)
  foreach(mode scenario released)
    add_test(NAME plugins.mc_partner_execution_${mode} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-execution-${mode})
    set_tests_properties(plugins.mc_partner_execution_${mode} PROPERTIES TIMEOUT 15)
  endforeach()
  foreach(delivery fallback sent unknown internal finish late retired capacity)
    add_test(NAME plugins.mc_partner_reply_${delivery} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-reply-${delivery})
    set_tests_properties(plugins.mc_partner_reply_${delivery} PROPERTIES TIMEOUT 15)
  endforeach()
  add_test(NAME plugins.mc_partner_work_lookup COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_plugin_partner_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-partner" partner-reply-lookup)
  set_tests_properties(plugins.mc_partner_work_lookup PROPERTIES TIMEOUT 15)
  dfy_test_executable(dfy_mc_confirmation_route_tests mc_confirmation_route.cpp)
  target_link_libraries(dfy_mc_confirmation_route_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_mc_confirmation_route_tests dfy_mc_companion)
  foreach(route IN ITEMS panel game local manual first delayed lost-terminal retry-reply)
    add_test(NAME app.mc_confirmation_${route} COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
      "$<TARGET_FILE:dfy_mc_confirmation_route_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-confirmation-route" partner-route-${route})
    set_tests_properties(app.mc_confirmation_${route} PROPERTIES TIMEOUT 25)
  endforeach()
  dfy_test_executable(dfy_mc_partner_exchange_tests mc_partner_exchange.cpp)
  target_link_libraries(dfy_mc_partner_exchange_tests PRIVATE dfy_application nlohmann_json::nlohmann_json dfy_build_options)
  add_dependencies(dfy_mc_partner_exchange_tests dfy_mc_companion)
  add_test(NAME app.mc_partner_exchange COMMAND "${DFY_MC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/native-world.cjs"
    "$<TARGET_FILE:dfy_mc_partner_exchange_tests>" "$<TARGET_FILE_DIR:dfy_mc_companion>" "${PROJECT_BINARY_DIR}/tests/mc-exchange" partner-exchange)
  set_tests_properties(app.mc_partner_exchange PROPERTIES TIMEOUT 25)
endif()
