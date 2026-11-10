add_executable(dafeiyu main.cpp chat.cpp chat.hpp console.hpp)

target_link_libraries(dafeiyu PRIVATE dfy_agent dfy_build_options)
target_compile_definitions(dafeiyu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

set_target_properties(dafeiyu PROPERTIES
  FOLDER "App"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu>")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/chat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/chat.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/console.hpp")
