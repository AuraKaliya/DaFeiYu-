add_library(dfy_terminal STATIC
  src/async_chat.cpp src/async_chat.hpp src/companion_host.cpp src/companion_host.hpp
  src/management/terminal_client.cpp src/management/terminal_client.hpp)
target_include_directories(dfy_terminal PUBLIC src)
target_link_libraries(dfy_terminal PUBLIC dfy_host_commands dfy_management dfy_input PRIVATE dfy_build_options)
dfy_group_target(dfy_terminal App)

if(BUILD_CLI)
  add_executable(dafeiyu-cli src/main.cpp src/console.hpp)
  target_link_libraries(dafeiyu-cli PRIVATE dfy_terminal dfy_resident_entry dfy_resident_tray dfy_build_options)
  target_compile_definitions(dafeiyu-cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(dafeiyu-cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu-cli>")
  add_dependencies(dafeiyu-cli dafeiyu-host)
  dfy_group_target(dafeiyu-cli App)
  install(TARGETS dafeiyu-cli RUNTIME DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
