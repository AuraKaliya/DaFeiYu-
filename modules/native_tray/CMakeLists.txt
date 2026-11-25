add_library(dfy_resident_tray STATIC
  src/tray_icon.cpp src/panel_activation.cpp src/exit_confirmation.cpp
  include/tray_icon.hpp include/panel_activation.hpp include/exit_confirmation.hpp)
target_include_directories(dfy_resident_tray PUBLIC include resources)
target_sources(dfy_resident_tray INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/resources/tray.rc")
target_link_libraries(dfy_resident_tray PRIVATE dfy_build_options shell32 user32)
target_compile_definitions(dfy_resident_tray PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
dfy_group_target(dfy_resident_tray App)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
