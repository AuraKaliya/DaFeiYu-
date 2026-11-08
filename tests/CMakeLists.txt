dfy_add_plugin(dfy_test_fail fixtures/fail_start.cpp fixtures/fail_start.json.in TEST_ONLY)
dfy_add_plugin(dfy_test_abi fixtures/bad_abi.cpp fixtures/bad_abi.json.in TEST_ONLY)
dfy_add_plugin(dfy_test_crash_channel fixtures/crash_channel.cpp fixtures/crash_channel.json.in TEST_ONLY)
dfy_add_plugin(dfy_test_crash_action fixtures/crash_action.cpp fixtures/crash_action.json.in TEST_ONLY)

add_executable(dfy_tests test_main.cpp)
target_link_libraries(dfy_tests PRIVATE dafeiyu::core dfy_warnings)
add_dependencies(dfy_tests dfy_model_scripted dfy_channel_simulator dfy_memory_minimal dfy_action_add dfy_test_fail dfy_test_abi)
set(demo_profile "${PROJECT_BINARY_DIR}/run/$<CONFIG>/profiles/demo.json")
foreach(case IN ITEMS loop checkpoint dedup denied_action invalid_arguments unknown_delivery not_sent foreign_actor cancellation observer_failure rollback abi dependency identity memory_owner immutable_events instance_lock policy_change budget)
  add_test(NAME core.${case} COMMAND dfy_tests ${case} "${demo_profile}" "${CMAKE_CURRENT_BINARY_DIR}/work")
  set_tests_properties(core.${case} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(dfy_crash_driver crash_driver.cpp)
target_link_libraries(dfy_crash_driver PRIVATE dafeiyu::core dfy_warnings)
add_dependencies(dfy_crash_driver dfy_test_crash_channel dfy_test_crash_action dfy_model_scripted dfy_memory_minimal dfy_action_add dfy_channel_simulator)
foreach(mode IN ITEMS action delivery)
  add_test(NAME recovery.crash_${mode} COMMAND ${CMAKE_COMMAND}
    "-DDRIVER=$<TARGET_FILE:dfy_crash_driver>" "-DPROFILE=${demo_profile}" "-DMODE=${mode}"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/work" -P ${CMAKE_CURRENT_SOURCE_DIR}/crash.cmake)
  set_tests_properties(recovery.crash_${mode} PROPERTIES TIMEOUT 30)
endforeach()

add_test(NAME distribution.install_and_sdk COMMAND ${CMAKE_COMMAND}
  "-DBUILD=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/work"
  "-DCONSUMER=${CMAKE_CURRENT_SOURCE_DIR}/sdk_consumer" "-DGENERATOR=${CMAKE_GENERATOR}"
  "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}" "-DHOST=$<TARGET_FILE:dafeiyu>"
  -P ${CMAKE_CURRENT_SOURCE_DIR}/install.cmake)
set_tests_properties(distribution.install_and_sdk PROPERTIES TIMEOUT 180)
