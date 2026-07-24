cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)   # honor CMAKE_MSVC_RUNTIME_LIBRARY
project(task_switcher CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the binaries run on fresh Windows with no VC++ runtime installed
# (a fresh Win11 has neither the debug nor release CRT DLLs). Self-contained exes/DLL.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(src/core)

# ---- Production components (Windows-only) --------------------------------------------------
if(WIN32)
    add_subdirectory(src/client_plugin)
    add_subdirectory(src/agent)          # unified client+session agent (role chosen at runtime)

    # Assemble the release bundle into <source>/dist: the two binaries + the installer scripts
    # (+ README) - the absolute minimum to install in production. `--target package` builds the
    # binaries first, then copies. (dist/ is git-ignored; produced by build.bat / wsl-build.bat.)
    add_custom_target(package
        COMMAND ${CMAKE_COMMAND} -E make_directory   ${CMAKE_SOURCE_DIR}/dist
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tsw_agent>  ${CMAKE_SOURCE_DIR}/dist/tsw_agent.exe
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tsw_plugin> ${CMAKE_SOURCE_DIR}/dist/tsw_plugin.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/install.cmd   ${CMAKE_SOURCE_DIR}/dist/install.cmd
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/uninstall.cmd ${CMAKE_SOURCE_DIR}/dist/uninstall.cmd
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/README.md     ${CMAKE_SOURCE_DIR}/dist/README.md
        COMMENT "Assembling release bundle into dist/")
    add_dependencies(package tsw_agent tsw_plugin)
endif()

# ---- Tests + diagnostic harnesses (everything test-related lives under .test/) --------------
option(TSW_BUILD_TESTS "Build the unit tests" ON)
if(TSW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(.test/unit)
endif()
