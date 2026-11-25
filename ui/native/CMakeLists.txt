# 常驻图标只依赖 Windows Shell，可在无 WebView2 的构建中独立验证。
add_library(dfy_resident_tray STATIC tray_icon.cpp tray_icon.hpp)
target_sources(dfy_resident_tray PRIVATE panel_activation.cpp panel_activation.hpp)
target_sources(dfy_resident_tray PRIVATE exit_confirmation.cpp exit_confirmation.hpp)
target_include_directories(dfy_resident_tray PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(dfy_resident_tray INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tray.rc")
target_link_libraries(dfy_resident_tray PRIVATE dfy_build_options shell32 user32)
target_compile_definitions(dfy_resident_tray PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_resident_tray PROPERTIES FOLDER App)
