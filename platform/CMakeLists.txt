if(NOT WIN32)
  message(FATAL_ERROR "The current HTTPS transport requires Windows WinHTTP")
endif()

add_library(dfy_http STATIC winhttp.cpp include/dafeiyu/http.hpp)
target_include_directories(dfy_http PUBLIC include)
target_link_libraries(dfy_http PRIVATE dfy_build_options winhttp)
target_compile_definitions(dfy_http PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_http PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/winhttp.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/http.hpp")
