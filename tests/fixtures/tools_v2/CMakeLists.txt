# 独立 C11 工具生产方，仅使用公开 SDK，不依赖宿主 C++ 类型。
add_library(dfy_tools_fixture SHARED tools.c)
target_link_libraries(dfy_tools_fixture PRIVATE dfy_sdk)
target_compile_features(dfy_tools_fixture PRIVATE c_std_11)
target_compile_definitions(dfy_tools_fixture PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_tools_fixture PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/tools.c")
add_custom_command(TARGET dfy_tools_fixture POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "$<TARGET_FILE_DIR:dfy_tools_fixture>/plugin.json")

# 两个独立 DLL 各自接纳一类原始业务记录，通过同一公开工具 ABI 向本体返回采样。
foreach(domain IN ITEMS freight museum)
  set(channel "dfy_${domain}_fixture")
  add_library(${channel} SHARED tools.c channel_reading.cpp)
  target_link_libraries(${channel} PRIVATE dfy_sdk nlohmann_json::nlohmann_json dfy_build_options)
  target_compile_definitions(${channel} PRIVATE DFY_PLUGIN_BUILD DFY_CHANNEL_FIXTURE)
  set_target_properties(${channel} PROPERTIES FOLDER "Tests" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${domain}")
endforeach()
target_compile_definitions(dfy_freight_fixture PRIVATE DFY_FREIGHT_FIXTURE)
