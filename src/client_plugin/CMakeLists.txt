add_library(tsw_plugin SHARED
    dllmain.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/pipe.cpp
    plugin.def)
target_link_libraries(tsw_plugin PRIVATE tsw_core)
target_include_directories(tsw_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src/platform)
set_target_properties(tsw_plugin PROPERTIES OUTPUT_NAME "tsw_plugin")
# Embed Win32 version/metadata (publisher, product, version) - see the agent target.
set(TSW_FILEDESC "Task Switcher Remote Desktop plugin (mstsc DVC bridge)")
set(TSW_ORIGNAME "tsw_plugin.dll")
set(TSW_INTERNAL "tsw_plugin")
set(TSW_FILETYPE "VFT_DLL")
configure_file(${CMAKE_SOURCE_DIR}/cmake/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/tsw_plugin.rc @ONLY)
target_sources(tsw_plugin PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/tsw_plugin.rc)
