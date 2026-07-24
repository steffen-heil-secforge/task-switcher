add_library(tsw_core STATIC
    version.hpp
    protocol.cpp
    window_filter.cpp
    merged_model.cpp
    activation_logic.cpp)
target_include_directories(tsw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party)
target_compile_features(tsw_core PUBLIC cxx_std_17)
