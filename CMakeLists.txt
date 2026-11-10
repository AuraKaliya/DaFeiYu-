cmake_minimum_required(VERSION 3.28)
project(dafeiyu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakePredefinedTargets")

add_library(dfy_build_options INTERFACE)
target_compile_options(dfy_build_options INTERFACE
  "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
  "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>")

include(cmake/Dependencies.cmake)
add_subdirectory(model)
add_subdirectory(app)

option(BUILD_TESTING "Build project tests" OFF)
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT dafeiyu)
