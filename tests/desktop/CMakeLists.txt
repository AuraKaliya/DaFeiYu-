dfy_test_executable(dfy_tray_headless_host_tests ui_tray_headless_host.cpp)
target_link_libraries(dfy_tray_headless_host_tests PRIVATE dfy_management dfy_build_options user32)
add_dependencies(dfy_tray_headless_host_tests dafeiyu)
add_test(NAME ui.tray_headless_host COMMAND dfy_tray_headless_host_tests "$<TARGET_FILE:dafeiyu>" "${PROJECT_BINARY_DIR}/tests/tray-headless")
set_tests_properties(ui.tray_headless_host PROPERTIES TIMEOUT 25 SKIP_RETURN_CODE 77)
if(BUILD_GUI)
  set(tray_panel_option -WithPanel)
endif()
add_test(NAME ui.tray_resident COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
  -File "${CMAKE_CURRENT_SOURCE_DIR}/ui_tray_resident.ps1" -App "$<TARGET_FILE:dafeiyu>"
  -Root "${PROJECT_BINARY_DIR}/tests/tray-resident" ${tray_panel_option})
set_tests_properties(ui.tray_resident PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

# 只在 GUI 构建中验证来源与 Frame 代次，不启动浏览器。
if(BUILD_GUI)
  # 只读桥接验收使用构建目录内可写的临时配置根。
  set(run_status_temp "${PROJECT_BINARY_DIR}/ui-run-temp")
  file(MAKE_DIRECTORY "${run_status_temp}")
  dfy_test_executable(dfy_ui_history_host ui_history_host.cpp)
  target_link_libraries(dfy_ui_history_host PRIVATE dfy_management dfy_build_options)
  add_dependencies(dfy_ui_history_host dfy_ui_probe)
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
