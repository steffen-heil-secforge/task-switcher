# Single agent binary: client-agent + session-agent code compiled together; the role is chosen
# at runtime (SM_REMOTESESSION) in agent/main.cpp.
add_executable(tsw_agent
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/client_agent/client_agent.cpp
    ${CMAKE_SOURCE_DIR}/src/client_agent/hub.cpp
    ${CMAKE_SOURCE_DIR}/src/client_agent/picker.cpp
    ${CMAKE_SOURCE_DIR}/src/session_agent/session_agent.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/pipe.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/win_enumerator.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/activation_win.cpp)
target_link_libraries(tsw_agent PRIVATE tsw_core)
target_include_directories(tsw_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/client_agent)
# Embed Win32 version/metadata (publisher, product, version) so the binary isn't an
# anonymous unsigned blob to AV heuristics / reputation systems.
set(TSW_FILEDESC "Task Switcher agent (client + RDP session)")
set(TSW_ORIGNAME "tsw_agent.exe")
set(TSW_INTERNAL "tsw_agent")
set(TSW_FILETYPE "VFT_APP")
configure_file(${CMAKE_SOURCE_DIR}/cmake/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/tsw_agent.rc @ONLY)
target_sources(tsw_agent PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/tsw_agent.rc)
# GUI subsystem: no console window; keep main() as the entry via mainCRTStartup.
set_target_properties(tsw_agent PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(tsw_agent PRIVATE /ENTRY:mainCRTStartup)
