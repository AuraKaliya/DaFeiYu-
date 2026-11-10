add_executable(dafeiyu main.cpp)

target_compile_options(dafeiyu PRIVATE
  "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
  "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>")

set_target_properties(dafeiyu PROPERTIES
  FOLDER "App"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu>")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
