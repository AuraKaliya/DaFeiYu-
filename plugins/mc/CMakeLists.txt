# 将 Windows 回环管道与原生插件生命周期分开验证。
add_library(dfy_mc_bridge STATIC src/pipe_exchange.cpp src/pipe_exchange.hpp src/world_protocol.cpp src/world_protocol.hpp src/game_work.cpp src/game_work.hpp)
target_sources(dfy_mc_bridge PRIVATE src/partner_feed.cpp src/partner_feed.hpp)
target_link_libraries(dfy_mc_bridge PRIVATE dfy_build_options nlohmann_json::nlohmann_json)
target_include_directories(dfy_mc_bridge PUBLIC src)
target_compile_definitions(dfy_mc_bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_mc_bridge PROPERTIES FOLDER "Plugins")

# 生成独立可安装的可信原生包，保持宿主默认插件目录为空。
add_library(dfy_mc_companion SHARED src/native.cpp src/partner_loop.cpp src/partner_loop.hpp src/partner_confirm.cpp src/partner_mailbox.cpp)
target_sources(dfy_mc_companion PRIVATE src/partner_task.cpp src/partner_task.hpp src/partner_queue.cpp src/partner_plan.cpp)
target_sources(dfy_mc_companion PRIVATE src/partner_question.cpp src/follow_attention.hpp)
target_sources(dfy_mc_companion PRIVATE src/partner_model_view.cpp src/partner_model_view.hpp)
target_sources(dfy_mc_companion PRIVATE src/partner_facts.cpp src/partner_facts.hpp)
target_link_libraries(dfy_mc_companion PRIVATE dfy_mc_bridge dfy_sdk dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_mc_companion PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_mc_companion PROPERTIES FOLDER "Plugins"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/mc.companion")
foreach(asset IN ITEMS plugin.json defaults.json agent/status.json)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/mc.companion/${asset}" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/resources/${asset}")
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
