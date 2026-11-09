if(DFY_BUILD_EXAMPLES)
  set(minimal_root "${PROJECT_BINARY_DIR}/run/$<CONFIG>")
  set(install_example INSTALL)
else()
  set(minimal_root "${PROJECT_BINARY_DIR}/tests/runtime/$<CONFIG>")
  set(install_example)
endif()

foreach(role IN ITEMS model_scripted strategy_default channel_simulator memory_minimal action_add)
  dfy_add_plugin(dfy_${role} ${role}/plugin.cpp ${role}/manifest.json.in OUTPUT_ROOT "${minimal_root}" ${install_example})
endforeach()

file(READ "${PROJECT_SOURCE_DIR}/profiles/minimal.json" minimal_profile)
file(READ "${PROJECT_SOURCE_DIR}/resources/foundation.txt" foundation)
file(GENERATE OUTPUT "${minimal_root}/profiles/minimal.json" CONTENT "${minimal_profile}")
file(GENERATE OUTPUT "${minimal_root}/resources/foundation.txt" CONTENT "${foundation}")
if(DFY_BUILD_EXAMPLES)
  install(FILES ${PROJECT_SOURCE_DIR}/profiles/minimal.json DESTINATION profiles)
  install(FILES ${PROJECT_SOURCE_DIR}/resources/foundation.txt DESTINATION resources)
endif()

# Tests get their own runtime layout; it never augments the example's plugin directory.
if(BUILD_TESTING)
  set(test_root "${PROJECT_BINARY_DIR}/tests/runtime/$<CONFIG>")
  if(DFY_BUILD_EXAMPLES)
    file(GENERATE OUTPUT "${test_root}/profiles/minimal.json" CONTENT "${minimal_profile}")
    file(GENERATE OUTPUT "${test_root}/resources/foundation.txt" CONTENT "${foundation}")
  endif()
  add_custom_target(dfy_test_minimal_modules)
  foreach(role IN ITEMS model_scripted strategy_default channel_simulator memory_minimal action_add)
    add_dependencies(dfy_test_minimal_modules dfy_${role})
    if(DFY_BUILD_EXAMPLES)
      add_custom_command(TARGET dfy_test_minimal_modules POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${test_root}/plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dfy_${role}>" "${test_root}/plugins/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${minimal_root}/plugins/dfy_${role}.json" "${test_root}/plugins/"
        VERBATIM)
    endif()
  endforeach()
endif()
