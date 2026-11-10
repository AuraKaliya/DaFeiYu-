# 构建原生插件并生成与各配置模块同目录的清单，可按需安装。
# 步骤：校验输出目录 → 创建插件目标并设置依赖 → 生成对应模块清单 → 按选项登记安装规则。
function(dfy_add_plugin target source manifest)
  cmake_parse_arguments(ARG "INSTALL" "OUTPUT_ROOT" "" ${ARGN})
  if(NOT ARG_OUTPUT_ROOT)
    message(FATAL_ERROR "${target} must declare its output root")
  endif()

  add_library(${target} MODULE ${source})
  target_link_libraries(${target} PRIVATE dafeiyu::plugin_sdk dfy_support dfy_warnings)
  set_target_properties(${target} PROPERTIES
    PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    LIBRARY_OUTPUT_DIRECTORY "${ARG_OUTPUT_ROOT}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${ARG_OUTPUT_ROOT}/plugins")

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${manifest}" manifest_content)
  string(REPLACE "@LIBRARY@" "$<TARGET_FILE_NAME:${target}>" manifest_content "${manifest_content}")
  file(GENERATE OUTPUT "${ARG_OUTPUT_ROOT}/plugins/${target}.json" CONTENT "${manifest_content}")

  if(ARG_INSTALL)
    install(TARGETS ${target} LIBRARY DESTINATION plugins RUNTIME DESTINATION plugins)
    install(FILES "${ARG_OUTPUT_ROOT}/plugins/${target}.json" DESTINATION plugins)
  endif()
endfunction()
