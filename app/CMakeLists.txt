# 定义终端应用目标，显式列出实现与内部头文件。
add_executable(dafeiyu main.cpp chat.cpp chat.hpp console.hpp)

# 配置依赖与编译环境：通过 Agent 使用模型能力，并限制 Windows 头文件的宏污染。
target_link_libraries(dafeiyu PRIVATE dfy_agent dfy_build_options)
target_compile_definitions(dafeiyu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# 设置运行位置：按构建类型输出程序，VS 调试时从程序所在目录启动。
set_target_properties(dafeiyu PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu>")

# 组织 IDE 展示：保留 App 职责分组，源码按模块内实际目录排列。
set_target_properties(dafeiyu PROPERTIES FOLDER "App")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/chat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/chat.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/console.hpp")
