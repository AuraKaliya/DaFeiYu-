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

include(cmake/BuildOptions.cmake)
option(BUILD_TESTING "Build project tests" OFF)
if(BUILD_TESTING)
  enable_testing()
  include(cmake/Testing.cmake)
endif()

# 装配工程：准备第三方依赖，再加入 SDK、已实现的运行模块和最小查询示例。
include(cmake/Dependencies.cmake)
add_subdirectory(sdk)
add_subdirectory(modules)
add_subdirectory(plugins/runtime)
add_subdirectory(plugins/tools)
add_subdirectory(plugins/examples/query)
add_subdirectory(plugins/examples/observer)
add_subdirectory(plugins/examples/status)
add_subdirectory(plugins/mc)
add_subdirectory(ui/native)
add_subdirectory(app)
option(BUILD_GUI "Build the native WebView2 validation window" OFF)
if(BUILD_GUI)
  include(cmake/WebView2.cmake)
  add_subdirectory(app/gui)
endif()
include(cmake/Distribution.cmake)

# 按需启用离线测试，普通构建不生成测试目标。
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# 指定 VS 默认启动的应用目标。
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT dafeiyu)
