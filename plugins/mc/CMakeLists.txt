# 将 Windows 回环管道与原生插件生命周期分开验证。
add_library(dfy_mc_bridge STATIC pipe_exchange.cpp pipe_exchange.hpp)
target_link_libraries(dfy_mc_bridge PRIVATE dfy_build_options)
target_include_directories(dfy_mc_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dfy_mc_bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_mc_bridge PROPERTIES FOLDER "Plugins")
