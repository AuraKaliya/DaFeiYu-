# 将定向测试和回放工具输出到同一配置目录，供 CTest 和独立验收脚本定位。
function(dfy_test_executable target)
  add_executable(${target} ${ARGN})
  set_target_properties(${target} PROPERTIES FOLDER Tests
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests/$<CONFIG>")
endfunction()

# 注册使用项目默认编译选项与时限的单模块测试。
# 步骤：建立测试程序 → 绑定被测依赖 → 注册 CTest 名称和执行时限。
function(dfy_add_unit_test name target source)
  dfy_test_executable(${target} "${source}")
  target_link_libraries(${target} PRIVATE dfy_build_options ${ARGN})
  add_test(NAME "${name}" COMMAND ${target})
  set_tests_properties("${name}" PROPERTIES TIMEOUT 15)
endfunction()
