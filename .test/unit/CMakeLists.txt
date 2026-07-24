add_executable(tsw_tests
    test_main.cpp
    test_smoke.cpp
    test_protocol.cpp
    test_window_filter.cpp
    test_merged_model.cpp
    test_activation.cpp
    test_hub.cpp
    activation_stub.cpp
    ${CMAKE_SOURCE_DIR}/src/client_agent/hub.cpp)
target_link_libraries(tsw_tests PRIVATE tsw_core)
target_include_directories(tsw_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}                     # doctest/doctest.h (vendored here, test-only)
    ${CMAKE_SOURCE_DIR}/src/client_agent)
add_test(NAME tsw_tests COMMAND tsw_tests)
