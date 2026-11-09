add_library(dfy_core STATIC common.cpp store.cpp plugin_manager.cpp calls.cpp policy.cpp operations.cpp context.cpp run.cpp inbox.cpp scheduler.cpp runtime.cpp)
add_library(dafeiyu::core ALIAS dfy_core)
target_include_directories(dfy_core PUBLIC include)
target_link_libraries(dfy_core PUBLIC dafeiyu::plugin_sdk dfy_support PRIVATE dfy_warnings Threads::Threads ${CMAKE_DL_LIBS})
