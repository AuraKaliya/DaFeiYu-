set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/test.public-settings")
set(outputs "${package_dir}/entry.dll" "${package_dir}/plugin.json" "${package_dir}/defaults.json" "${package_dir}/ui/manifest.json")
add_custom_command(OUTPUT ${outputs}
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${package_dir}/ui"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:dfy_query_v2_fixture>" "${package_dir}/entry.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${CMAKE_CURRENT_SOURCE_DIR}/defaults.json" "${package_dir}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/ui/manifest.json" "${package_dir}/ui/manifest.json"
  DEPENDS dfy_query_v2_fixture "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${CMAKE_CURRENT_SOURCE_DIR}/defaults.json" "${CMAKE_CURRENT_SOURCE_DIR}/ui/manifest.json"
  VERBATIM)
add_custom_target(dfy_public_settings_fixture DEPENDS ${outputs})
set_target_properties(dfy_public_settings_fixture PROPERTIES FOLDER "Tests/Fixtures" DFY_FIXTURE_PACKAGE "${package_dir}")
