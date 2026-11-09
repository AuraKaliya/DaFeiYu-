add_library(dfy_core STATIC common.cpp store.cpp plugin_manager.cpp calls.cpp policy.cpp operations.cpp context.cpp run.cpp run_view.cpp conversations.cpp inbox.cpp scheduler.cpp runtime.cpp)
add_library(dafeiyu::core ALIAS dfy_core)
target_include_directories(dfy_core PUBLIC include)
target_link_libraries(dfy_core PUBLIC dafeiyu::contracts PRIVATE dafeiyu::plugin_sdk dfy_support dfy_warnings Threads::Threads ${CMAKE_DL_LIBS})
