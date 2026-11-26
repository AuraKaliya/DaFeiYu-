# 安装独立 Release 运行目录，配置只携带环境变量名称，不打包用户设置或密钥。
install(DIRECTORY DESTINATION plugins CONFIGURATIONS Release COMPONENT Runtime)
# 三份运行手册放在同一目录，终端配置也能打开构建说明中的桌面和许可章节。
install(FILES
  "${DFY_USER_MANUAL_DIR}/构建与运行说明.md"
  "${DFY_USER_MANUAL_DIR}/桌面管理中心说明.md"
  "${DFY_USER_MANUAL_DIR}/第三方软件与素材许可.md"
  DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${DFY_USER_MANUAL_DIR}/构建与运行说明.md" DESTINATION . RENAME "运行说明.md" CONFIGURATIONS Release COMPONENT Runtime)

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
install(FILES "${DFY_USER_MANUAL_DIR}/第三方软件与素材许可.md" DESTINATION . RENAME THIRD_PARTY.md CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses RENAME nlohmann-json-LICENSE.MIT CONFIGURATIONS Release COMPONENT Runtime)
