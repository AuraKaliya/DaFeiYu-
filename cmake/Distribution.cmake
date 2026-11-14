# 安装独立 Release 运行目录，配置只携带环境变量名称，不打包用户设置或密钥。
install(TARGETS dafeiyu RUNTIME DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/config/deepseek.example.json" "${PROJECT_SOURCE_DIR}/config/qwen.example.json"
  DESTINATION config CONFIGURATIONS Release COMPONENT Runtime)
install(DIRECTORY DESTINATION plugins CONFIGURATIONS Release COMPONENT Runtime)
install(TARGETS dfy_observer RUNTIME DESTINATION examples/plugins/example.observer CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/plugins/examples/observer/plugin.json" "${PROJECT_SOURCE_DIR}/plugins/examples/observer/defaults.json"
  DESTINATION examples/plugins/example.observer CONFIGURATIONS Release COMPONENT Runtime)
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

# SDK 仅发布 C ABI 头和独立可构建示例，不依赖宿主源码树、Old 或测试目录。
install(DIRECTORY "${PROJECT_SOURCE_DIR}/sdk/include/" DESTINATION sdk/include CONFIGURATIONS Release COMPONENT SDK)
install(FILES "${PROJECT_SOURCE_DIR}/sdk/README.md" DESTINATION sdk CONFIGURATIONS Release COMPONENT SDK)
install(FILES "${PROJECT_SOURCE_DIR}/plugins/examples/observer/observer.c" "${PROJECT_SOURCE_DIR}/plugins/examples/observer/observer_api.h"
  "${PROJECT_SOURCE_DIR}/plugins/examples/observer/plugin.json" "${PROJECT_SOURCE_DIR}/plugins/examples/observer/defaults.json"
  DESTINATION sdk/examples/observer CONFIGURATIONS Release COMPONENT SDK)
install(FILES "${PROJECT_SOURCE_DIR}/cmake/ObserverSdkExample.cmake" DESTINATION sdk/examples/observer RENAME CMakeLists.txt CONFIGURATIONS Release COMPONENT SDK)

# 保留实际引入的第三方说明及原始 MIT 许可证。
install(FILES "${PROJECT_SOURCE_DIR}/THIRD_PARTY.md" DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses RENAME nlohmann-json-LICENSE.MIT CONFIGURATIONS Release COMPONENT Runtime)
