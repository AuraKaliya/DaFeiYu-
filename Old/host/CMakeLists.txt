add_executable(dafeiyu main.cpp control.cpp)
target_link_libraries(dafeiyu PRIVATE dafeiyu::core dfy_warnings)
set_target_properties(dafeiyu PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/run/$<CONFIG>/bin")
install(TARGETS dafeiyu RUNTIME DESTINATION bin)
