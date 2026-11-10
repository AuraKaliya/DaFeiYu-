add_library(dfy_model STATIC config.cpp include/dafeiyu/model_config.hpp)
target_compile_features(dfy_model PUBLIC cxx_std_20)
target_include_directories(dfy_model PUBLIC include)
target_link_libraries(dfy_model PRIVATE dfy_build_options nlohmann_json::nlohmann_json)

set_target_properties(dfy_model PROPERTIES FOLDER "Model")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/config.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/model_config.hpp")
