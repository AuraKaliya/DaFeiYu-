# 构建真实依赖和引用它的 DLL，使用唯一依赖文件名避免与其他测试模块混淆。
add_library(dfy_loader_dependency SHARED dependency.c)
set_target_properties(dfy_loader_dependency PROPERTIES OUTPUT_NAME "dfy_loader_test_dependency_v1")
add_library(dfy_loader_payload SHARED payload.c)
target_link_libraries(dfy_loader_payload PRIVATE dfy_loader_dependency)

# 隔离构建 Win32 DLL，验证 x64 加载器面对真实架构不匹配时的诊断。
include(ExternalProject)
ExternalProject_Add(dfy_loader_x86
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/x86"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/x86"
  CMAKE_GENERATOR "Visual Studio 17 2022"
  CMAKE_GENERATOR_PLATFORM Win32
  CMAKE_ARGS "-DCMAKE_GENERATOR_INSTANCE:INTERNAL=${CMAKE_GENERATOR_INSTANCE}"
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG>
  BUILD_BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/x86/$<CONFIG>/dfy_loader_x86.dll"
  BUILD_ALWAYS TRUE
  INSTALL_COMMAND "")
set(DFY_LOADER_X86_DLL "${CMAKE_CURRENT_BINARY_DIR}/x86/$<CONFIG>/dfy_loader_x86.dll" PARENT_SCOPE)

# 将实际测试目标归入 Tests，按本目录展示夹具源码。
set_target_properties(dfy_loader_dependency dfy_loader_payload dfy_loader_x86 PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/dependency.c" "${CMAKE_CURRENT_SOURCE_DIR}/payload.c")
