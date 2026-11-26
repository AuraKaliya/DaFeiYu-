# 探针代码仅进入带测试的桌面目标。
target_sources(dafeiyu PRIVATE
  ui_run_status_probe.cpp ui_run_status_probe.hpp ui_room_preview.cpp
  ui_main_panel.cpp ui_main_panel_fixture.hpp ui_dialogue_queue.cpp ui_dialogue_recovery.cpp ui_tray_panel.cpp ui_history_panel.cpp
  ui_mailbox_panel.cpp ui_plugin_settings.cpp ui_plugin_windows.cpp ui_plugin_actions.cpp)
target_include_directories(dafeiyu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dafeiyu PRIVATE DFY_UI_RUN_STATUS_TEST)
add_dependencies(dafeiyu dfy_status_example)

dfy_test_executable(dfy_ui_scope_tests ui_frame_scope.cpp)
target_link_libraries(dfy_ui_scope_tests PRIVATE dfy_ui_scope dfy_build_options)
add_test(NAME ui.frame_scope COMMAND dfy_ui_scope_tests)

set(run_status_temp "${PROJECT_BINARY_DIR}/ui-run-temp")
file(MAKE_DIRECTORY "${run_status_temp}")
add_test(NAME ui.message_history_state COMMAND "${DFY_NPM_EXECUTABLE}" run test:history)
set_tests_properties(ui.message_history_state PROPERTIES WORKING_DIRECTORY "${DFY_UI_WEB}" TIMEOUT 15)
add_test(NAME ui.mailbox_observer COMMAND "${DFY_NPM_EXECUTABLE}" run test:mailbox)
set_tests_properties(ui.mailbox_observer PROPERTIES WORKING_DIRECTORY "${DFY_UI_WEB}" TIMEOUT 15)
add_test(NAME ui.plugin_directory COMMAND "${DFY_NPM_EXECUTABLE}" run test:plugins)
set_tests_properties(ui.plugin_directory PROPERTIES WORKING_DIRECTORY "${DFY_UI_WEB}" TIMEOUT 15)
foreach(mode IN ITEMS smoke offline interaction lifecycle close-early close-controller)
  add_test(NAME "ui.${mode}" COMMAND dafeiyu "--${mode}")
  set_tests_properties("ui.${mode}" PROPERTIES TIMEOUT 35)
endforeach()
foreach(sample IN ITEMS idle busy)
  add_test(NAME ui.room_preview_${sample} COMMAND dafeiyu --check-room-preview ${sample})
  set_tests_properties(ui.room_preview_${sample} PROPERTIES TIMEOUT 45 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
endforeach()
add_test(NAME ui.main_panel COMMAND dafeiyu --check-main-panel)
set_tests_properties(ui.main_panel PROPERTIES TIMEOUT 45 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
add_test(NAME ui.plugin_settings_samples COMMAND dafeiyu --check-plugin-settings)
set_tests_properties(ui.plugin_settings_samples PROPERTIES TIMEOUT 45 ENVIRONMENT "TEMP=${run_status_temp};TMP=${run_status_temp}")
