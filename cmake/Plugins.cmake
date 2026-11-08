# The manifest is generated beside the actual module, including multi-config builds.
function(dfy_add_plugin target source manifest)
  cmake_parse_arguments(ARG "TEST_ONLY" "" "" ${ARGN})
  add_library(${target} MODULE ${source})
  target_link_libraries(${target} PRIVATE dafeiyu::plugin_sdk dfy_support dfy_warnings)
  set_target_properties(${target} PROPERTIES
    PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/run/$<CONFIG>/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/run/$<CONFIG>/plugins")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${manifest}" manifest_content)
  string(REPLACE "@LIBRARY@" "$<TARGET_FILE_NAME:${target}>" manifest_content "${manifest_content}")
  file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/run/$<CONFIG>/plugins/${target}.json" CONTENT "${manifest_content}")
  if(NOT ARG_TEST_ONLY)
    install(TARGETS ${target} LIBRARY DESTINATION plugins RUNTIME DESTINATION plugins)
    install(FILES "${PROJECT_BINARY_DIR}/run/$<CONFIG>/plugins/${target}.json" DESTINATION plugins)
  endif()
endfunction()
