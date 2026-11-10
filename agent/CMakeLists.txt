add_library(dfy_agent STATIC session.cpp include/dafeiyu/session.hpp)
target_include_directories(dfy_agent PUBLIC include)
target_link_libraries(dfy_agent PUBLIC dfy_model PRIVATE dfy_build_options)
set_target_properties(dfy_agent PROPERTIES FOLDER "Agent")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/session.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/session.hpp")
