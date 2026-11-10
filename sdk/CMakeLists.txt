# 定义只含公开 C 头文件的 SDK，不传播本体编译选项或 C++ 依赖。
add_library(dfy_sdk INTERFACE include/dafeiyu/plugin_abi.h)
target_include_directories(dfy_sdk INTERFACE include)

# 组织 SDK 职责分组及实际头文件目录。
set_target_properties(dfy_sdk PROPERTIES FOLDER "SDK")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_abi.h")
