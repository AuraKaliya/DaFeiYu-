# 安装独立 Release 运行目录，配置只携带环境变量名称，不打包用户设置或密钥。
install(DIRECTORY DESTINATION plugins CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/docs/run-guide.md" DESTINATION . RENAME "运行说明.md" CONFIGURATIONS Release COMPONENT Runtime)

# 只复制编译器提供的 Release 运行库，系统 UCRT 由 Windows 10／11 提供。
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)
if(MSVC AND NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  message(FATAL_ERROR "MSVC Release runtime libraries are required for the runnable package")
endif()
foreach(runtime_file IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  if(NOT EXISTS "${runtime_file}")
    message(FATAL_ERROR "Required redistributable runtime is missing: ${runtime_file}")
  endif()
endforeach()
install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)

# 保留实际引入的第三方说明及原始 MIT 许可证。
install(FILES "${PROJECT_SOURCE_DIR}/THIRD_PARTY.md" DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses RENAME nlohmann-json-LICENSE.MIT CONFIGURATIONS Release COMPONENT Runtime)
