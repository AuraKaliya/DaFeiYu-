# 将 Windows 回环管道与原生插件生命周期分开验证。
add_library(dfy_mc_bridge STATIC pipe_exchange.cpp pipe_exchange.hpp world_protocol.cpp world_protocol.hpp game_work.cpp game_work.hpp)
target_sources(dfy_mc_bridge PRIVATE partner_feed.cpp partner_feed.hpp)
target_link_libraries(dfy_mc_bridge PRIVATE dfy_build_options nlohmann_json::nlohmann_json)
target_include_directories(dfy_mc_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(dfy_mc_bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_mc_bridge PROPERTIES FOLDER "Plugins")

# 生成独立可安装的可信原生包，保持宿主默认插件目录为空。
add_library(dfy_mc_companion SHARED native.cpp partner_loop.cpp partner_loop.hpp partner_confirm.cpp partner_mailbox.cpp)
target_sources(dfy_mc_companion PRIVATE partner_task.cpp partner_task.hpp)
target_sources(dfy_mc_companion PRIVATE partner_question.cpp)
target_link_libraries(dfy_mc_companion PRIVATE dfy_mc_bridge dfy_sdk dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_mc_companion PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_mc_companion PROPERTIES FOLDER "Plugins"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/mc.companion")
foreach(asset IN ITEMS plugin.json defaults.json)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/mc.companion/${asset}" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")
endforeach()
