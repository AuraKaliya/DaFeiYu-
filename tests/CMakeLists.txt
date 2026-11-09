dfy_add_plugin(dfy_test_probe fixtures/probe.cpp fixtures/probe.json.in TEST_ONLY)
add_executable(dfy_tests test_main.cpp)
target_link_libraries(dfy_tests PRIVATE dafeiyu::core dfy_warnings)
add_dependencies(dfy_tests dfy_test_probe dfy_model_scripted dfy_strategy_default dfy_channel_simulator dfy_memory_minimal dfy_action_add dfy_action_search dfy_action_remember dfy_action_background dfy_action_write_demo dfy_search_primary dfy_search_secondary dfy_context_clock dfy_activity_reflection dfy_action_behavior)
set(demo_profile "${PROJECT_BINARY_DIR}/run/$<CONFIG>/profiles/demo.json")
foreach(case IN ITEMS loop dedup restart_inbox mode_expiry review provider_swap nested_memory unknown_effect revocation cancel steer responsive deadline budget job job_early activity duplicate_terminal nested_denied forged_scope startup_rollback degraded schema contract_conflict abi_reject dependency_cycle instance_lock storage_version command_receipt binding_change behavior_plugin steer_delivery rejected_steer channel_input config_change unrelated_config)
  add_test(NAME runtime.${case} COMMAND dfy_tests ${case} "${demo_profile}" "${CMAKE_CURRENT_BINARY_DIR}/work")
  set_tests_properties(runtime.${case} PROPERTIES TIMEOUT 120)
endforeach()

add_executable(dfy_host_tests host_integration.cpp ../host/control.cpp)
target_link_libraries(dfy_host_tests PRIVATE dafeiyu::core dfy_warnings)
add_dependencies(dfy_host_tests dafeiyu)
foreach(mode IN ITEMS resident delivery action job)
  add_test(NAME host.${mode} COMMAND dfy_host_tests ${mode} "$<TARGET_FILE:dafeiyu>" "${demo_profile}" "${CMAKE_CURRENT_BINARY_DIR}/work")
  set_tests_properties(host.${mode} PROPERTIES TIMEOUT 120)
endforeach()
add_test(NAME distribution.install_and_sdk COMMAND ${CMAKE_COMMAND}
  "-DBUILD=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/work"
  "-DCONSUMER=${CMAKE_CURRENT_SOURCE_DIR}/sdk_consumer" "-DGENERATOR=${CMAKE_GENERATOR}"
  "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}" -P ${CMAKE_CURRENT_SOURCE_DIR}/install.cmake)
set_tests_properties(distribution.install_and_sdk PROPERTIES TIMEOUT 180)
