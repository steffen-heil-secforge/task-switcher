set(TSW_TEST_SOURCES
    test_main.cpp
    test_smoke.cpp
    test_protocol.cpp
    test_window_filter.cpp
    test_merged_model.cpp
    test_activation.cpp
    activation_stub.cpp)
if(WIN32)
    list(APPEND TSW_TEST_SOURCES
        test_hub.cpp
        ${CMAKE_SOURCE_DIR}/src/client_agent/hub.cpp)
endif()
add_executable(tsw_tests ${TSW_TEST_SOURCES})
target_link_libraries(tsw_tests PRIVATE tsw_core)
target_include_directories(tsw_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}                     # doctest/doctest.h (vendored here, test-only)
    ${CMAKE_SOURCE_DIR}/src/client_agent)
add_test(NAME tsw_tests COMMAND tsw_tests)
