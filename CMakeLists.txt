# 确定语言基线：本体使用 C++20，SDK 查询示例使用 C11 验证 C 兼容性。
cmake_minimum_required(VERSION 3.28)
project(dafeiyu LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 启用 IDE 职责分组，将 CMake 自动生成的目标集中显示。
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakePredefinedTargets")

# 定义共享编译要求：统一警告级别，并让 MSVC 按 UTF-8 读取自有源码。
add_library(dfy_build_options INTERFACE)
target_compile_options(dfy_build_options INTERFACE
  "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
  "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>")

# 装配工程：准备第三方依赖，再加入 SDK、已实现的运行模块和最小查询示例。
include(cmake/Dependencies.cmake)
add_subdirectory(sdk)
add_subdirectory(platform)
add_subdirectory(events)
add_subdirectory(model)
add_subdirectory(agent)
add_subdirectory(plugins/runtime)
add_subdirectory(plugins/examples/query)
add_subdirectory(app)

# 按需启用离线测试，普通构建不生成测试目标。
option(BUILD_TESTING "Build project tests" OFF)
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

# 指定 VS 默认启动的应用目标。
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT dafeiyu)
