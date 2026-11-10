add_executable(dfy_contract_tests contracts.cpp ../../core/run_view.cpp)
target_include_directories(dfy_contract_tests PRIVATE ../../core ../../core/include)
target_link_libraries(dfy_contract_tests PRIVATE dafeiyu::contracts dfy_warnings)
add_test(NAME contracts.public_boundary COMMAND dfy_contract_tests)
