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
option(BUILD_GUI "Build the DaFeiYu desktop application" ON)
option(BUILD_CLI "Build the optional command-line client" OFF)
option(BUILD_TESTING "Build project tests" OFF)
if(BUILD_TESTING)
  enable_testing()
  include(cmake/Testing.cmake)
endif()

# 装配工程：准备第三方依赖，再加入 SDK、已实现的运行模块和最小查询示例。
include(cmake/Dependencies.cmake)
set(DFY_USER_MANUAL_DIR "${PROJECT_SOURCE_DIR}/文档/使用说明")
add_subdirectory(sdk)
add_subdirectory(modules)
add_subdirectory(plugins)
add_subdirectory(apps)
include(cmake/Distribution.cmake)

# 按需启用离线测试，普通构建不生成测试目标。
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# VS 随当前配置启动桌面、可选终端或独立后台。
if(BUILD_GUI)
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT dafeiyu)
elseif(BUILD_CLI)
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT dafeiyu-cli)
else()
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT dafeiyu-host)
endif()
