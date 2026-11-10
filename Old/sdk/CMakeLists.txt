add_library(dfy_contracts INTERFACE)
add_library(dafeiyu::contracts ALIAS dfy_contracts)
set_target_properties(dfy_contracts PROPERTIES EXPORT_NAME contracts)
target_compile_features(dfy_contracts INTERFACE cxx_std_20)
target_include_directories(dfy_contracts INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dfy_contracts INTERFACE nlohmann_json::nlohmann_json)
add_library(dfy_plugin_sdk INTERFACE)
add_library(dafeiyu::plugin_sdk ALIAS dfy_plugin_sdk)
set_target_properties(dfy_plugin_sdk PROPERTIES EXPORT_NAME plugin_sdk)
target_link_libraries(dfy_plugin_sdk INTERFACE dafeiyu::contracts)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS dfy_contracts dfy_plugin_sdk EXPORT dafeiyuTargets)
install(EXPORT dafeiyuTargets NAMESPACE dafeiyu:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dafeiyu)
include(CMakePackageConfigHelpers)
configure_package_config_file(dafeiyuConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/dafeiyuConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dafeiyu)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/dafeiyuConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dafeiyuConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/dafeiyuConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dafeiyu)
