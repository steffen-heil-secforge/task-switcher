add_library(tsw_plugin SHARED
    dllmain.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/pipe.cpp
    plugin.def)
target_link_libraries(tsw_plugin PRIVATE tsw_core)
target_include_directories(tsw_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src/platform)
set_target_properties(tsw_plugin PROPERTIES OUTPUT_NAME "tsw_plugin")
