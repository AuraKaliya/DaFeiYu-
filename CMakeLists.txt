cmake_minimum_required(VERSION 3.28)
project(dafeiyu VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CTest)
include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

add_library(dfy_warnings INTERFACE)
target_compile_options(dfy_warnings INTERFACE
  "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
  "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>")

add_subdirectory(sdk)
add_subdirectory(support)
add_subdirectory(core)
include(cmake/Plugins.cmake)
add_subdirectory(plugins)
add_subdirectory(host)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(FILES profiles/demo.json DESTINATION profiles)
install(FILES resources/foundation.txt DESTINATION resources)
install(FILES README.md THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs FILES_MATCHING PATTERN "*.md")
