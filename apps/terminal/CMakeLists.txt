add_library(dfy_terminal STATIC
  src/application_commands.cpp src/application_commands.hpp src/chat.cpp src/chat.hpp src/async_chat.hpp
  src/diagnostics.cpp src/diagnostics.hpp src/companion_host.cpp src/companion_host.hpp
  src/plugin_commands.cpp src/plugin_commands.hpp)
target_include_directories(dfy_terminal PUBLIC src)
target_link_libraries(dfy_terminal PUBLIC dfy_application dfy_input PRIVATE dfy_build_options)
dfy_group_target(dfy_terminal App)

add_executable(dafeiyu src/main.cpp src/console.hpp src/resident_tray.cpp src/resident_tray.hpp)
target_link_libraries(dafeiyu PRIVATE dfy_terminal dfy_management dfy_resident_tray dfy_build_options shell32 ole32)
target_compile_definitions(dafeiyu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dafeiyu PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu>")
dfy_group_target(dafeiyu App)
install(TARGETS dafeiyu RUNTIME DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
