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

# 给独立采集程序提供实际构建版本，保留未声明的源码版本为未知。
function(dfy_replay_labels target)
  target_compile_definitions(${target} PRIVATE
    "DFY_REPLAY_COMMIT=\"${DFY_APPLICATION_COMMIT}\""
    "DFY_REPLAY_BUILD=\"${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_GENERATOR_PLATFORM}-$<CONFIG>\"")
endfunction()

# 固定编译案例的完整源码和显式夹具作为前提，变化后重新计算材料摘要。
# 步骤：登记输入依赖 → 统一换行后逐文件摘要 → 绑定原案例材料及构建版本。
function(dfy_compiled_replay target)
  set(material "")
  foreach(input IN LISTS ARGN)
    cmake_path(ABSOLUTE_PATH input BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE path)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 digest "${contents}")
    string(APPEND material "${digest}\n")
  endforeach()
  string(SHA256 digest "${material}")
  target_link_libraries(${target} PRIVATE dfy_digest nlohmann_json::nlohmann_json)
  target_compile_definitions(${target} PRIVATE "DFY_REPLAY_MATERIAL=\"${digest}\"")
  dfy_replay_labels(${target})
endfunction()
