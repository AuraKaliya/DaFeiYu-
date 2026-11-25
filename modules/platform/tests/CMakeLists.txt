add_subdirectory(loader)

dfy_test_executable(dfy_library_tests dynamic_library.cpp)
target_link_libraries(dfy_library_tests PRIVATE dfy_library dfy_build_options)
target_compile_definitions(dfy_library_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_library_tests dfy_loader_payload dfy_loader_x86)
add_test(NAME platform.library COMMAND dfy_library_tests "$<TARGET_FILE:dfy_loader_dependency>"
  "$<TARGET_FILE:dfy_loader_payload>" "${DFY_LOADER_X86_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/library/$<CONFIG>")
set_tests_properties(platform.library PROPERTIES TIMEOUT 15)

# 使用真实 DLL 验证独立代码凭证与 FIFO 排队工作中的模块寿命。
dfy_test_executable(dfy_code_module_tests code_module.cpp)
target_link_libraries(dfy_code_module_tests PRIVATE dfy_library dfy_runtime dfy_build_options)
target_compile_definitions(dfy_code_module_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_code_module_tests dfy_loader_payload)
add_test(NAME platform.code_module COMMAND dfy_code_module_tests "$<TARGET_FILE:dfy_loader_payload>")
set_tests_properties(platform.code_module PROPERTIES TIMEOUT 15)

# 验证真实 Windows 管道的有界行读取、背压和取消竞争。
dfy_test_executable(dfy_line_input_tests line_input.cpp)
target_link_libraries(dfy_line_input_tests PRIVATE dfy_input dfy_build_options)
target_compile_definitions(dfy_line_input_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME platform.line_input COMMAND dfy_line_input_tests)
set_tests_properties(platform.line_input PROPERTIES TIMEOUT 15)
