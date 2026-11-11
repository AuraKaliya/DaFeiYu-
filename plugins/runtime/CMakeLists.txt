# 定义插件运行时静态库，包含包描述、配置及持有模块引用的 ABI 查询。
add_library(dfy_plugins STATIC manifest.cpp registry.cpp config.cpp module.cpp
  include/dafeiyu/plugin_manifest.hpp include/dafeiyu/plugin_registry.hpp include/dafeiyu/plugin_config.hpp
  include/dafeiyu/plugin_module.hpp)

# 配置公开 SDK 与平台接口，JSON 解析保留在实现层，不依赖 Agent。
target_compile_features(dfy_plugins PUBLIC cxx_std_20)
target_include_directories(dfy_plugins PUBLIC include)
target_link_libraries(dfy_plugins PUBLIC dfy_library dfy_sdk PRIVATE dfy_build_options nlohmann_json::nlohmann_json)

# 组织 IDE 展示：按 Plugins 职责分组，源码保持模块内实际目录。
set_target_properties(dfy_plugins PROPERTIES FOLDER "Plugins")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/manifest.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/registry.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/config.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_manifest.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_config.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_module.hpp")
