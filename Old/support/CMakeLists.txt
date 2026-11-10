add_library(dfy_support STATIC sqlite.cpp)
target_compile_features(dfy_support PUBLIC cxx_std_20)
target_include_directories(dfy_support PUBLIC include)
target_link_libraries(dfy_support PUBLIC dfy_sqlite PRIVATE dfy_warnings)
