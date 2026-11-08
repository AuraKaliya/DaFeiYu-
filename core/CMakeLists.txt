add_library(dfy_core STATIC plugin_manager.cpp store.cpp runtime.cpp)
add_library(dafeiyu::core ALIAS dfy_core)
target_include_directories(dfy_core PUBLIC include)
target_link_libraries(dfy_core PUBLIC dafeiyu::plugin_sdk dfy_support PRIVATE dfy_warnings ${CMAKE_DL_LIBS})
