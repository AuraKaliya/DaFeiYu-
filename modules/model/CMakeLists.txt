add_library(dfy_model STATIC
  src/async_model.cpp src/client.cpp src/config.cpp src/messages.cpp src/protocol.cpp
  src/embedding_tokens.cpp src/embedding_tokens.hpp src/local_embedding.cpp src/embedding_session.hpp
  include/dafeiyu/async_model.hpp include/dafeiyu/model_client.hpp include/dafeiyu/model_config.hpp
  include/dafeiyu/model_messages.hpp include/dafeiyu/model_protocol.hpp include/dafeiyu/local_embedding.hpp)
target_compile_features(dfy_model PUBLIC cxx_std_20)
target_link_libraries(dfy_model PUBLIC dfy_runtime)
target_include_directories(dfy_model PUBLIC include)
target_link_libraries(dfy_model PRIVATE dfy_build_options nlohmann_json::nlohmann_json dfy_http icu)
target_compile_definitions(dfy_model PRIVATE _CRT_SECURE_NO_WARNINGS)
dfy_group_target(dfy_model Model)

# 只有显式提供 CPU 运行时包才编入本机编码后端。未部署 DLL 时仍可启动本体。
set(DFY_ONNXRUNTIME_ROOT "" CACHE PATH "Extracted Microsoft.ML.OnnxRuntime CPU NuGet package")
if(DFY_ONNXRUNTIME_ROOT)
  set(dfy_ort_include "${DFY_ONNXRUNTIME_ROOT}/build/native/include")
  set(dfy_ort_dll "${DFY_ONNXRUNTIME_ROOT}/runtimes/win-x64/native/onnxruntime.dll")
  if(NOT EXISTS "${dfy_ort_include}/onnxruntime_cxx_api.h" OR NOT EXISTS "${dfy_ort_dll}")
    message(FATAL_ERROR "DFY_ONNXRUNTIME_ROOT requires the Windows x64 CPU NuGet package")
  endif()
  target_include_directories(dfy_model SYSTEM PRIVATE "${dfy_ort_include}")
  target_compile_definitions(dfy_model PRIVATE DFY_WITH_ONNX_RUNTIME)
  target_link_libraries(dfy_model PRIVATE dfy_library dfy_digest)
  set(dfy_ort_staged "${PROJECT_BINARY_DIR}/bin/$<CONFIG>/onnxruntime.dll")
  add_custom_command(OUTPUT "${dfy_ort_staged}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/bin/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dfy_ort_dll}" "${dfy_ort_staged}"
    DEPENDS "${dfy_ort_dll}" VERBATIM)
  add_custom_target(dfy_onnx_runtime DEPENDS "${dfy_ort_staged}")
  dfy_group_target(dfy_onnx_runtime Model)
  install(FILES "${dfy_ort_dll}" DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
  install(FILES "${DFY_ONNXRUNTIME_ROOT}/LICENSE" DESTINATION licenses/onnxruntime CONFIGURATIONS Release COMPONENT Runtime)
  install(FILES "${DFY_ONNXRUNTIME_ROOT}/ThirdPartyNotices.txt" DESTINATION licenses/onnxruntime CONFIGURATIONS Release COMPONENT Runtime)
endif()

# 配置模板也是 CMake 的输入，单独修改模板即可更新各配置的运行资源。
foreach(asset IN ITEMS deepseek.example.json qwen.example.json)
  file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/bin/$<CONFIG>/config/${asset}"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/resources/config/${asset}")
  install(FILES "resources/config/${asset}" DESTINATION config CONFIGURATIONS Release COMPONENT Runtime)
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
