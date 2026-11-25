add_library(dfy_model STATIC
  src/async_model.cpp src/client.cpp src/config.cpp src/messages.cpp src/protocol.cpp
  include/dafeiyu/async_model.hpp include/dafeiyu/model_client.hpp include/dafeiyu/model_config.hpp
  include/dafeiyu/model_messages.hpp include/dafeiyu/model_protocol.hpp)
target_compile_features(dfy_model PUBLIC cxx_std_20)
target_link_libraries(dfy_model PUBLIC dfy_runtime)
target_include_directories(dfy_model PUBLIC include)
target_link_libraries(dfy_model PRIVATE dfy_build_options nlohmann_json::nlohmann_json dfy_http)
target_compile_definitions(dfy_model PRIVATE _CRT_SECURE_NO_WARNINGS)
dfy_group_target(dfy_model Model)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
