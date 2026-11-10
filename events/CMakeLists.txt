add_library(dfy_events STATIC bus.cpp include/dafeiyu/events.hpp)
target_compile_features(dfy_events PUBLIC cxx_std_20)
target_include_directories(dfy_events PUBLIC include)
target_link_libraries(dfy_events PRIVATE dfy_build_options)
set_target_properties(dfy_events PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/bus.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/events.hpp")
