# Handler tests of the MCP bridge (Plan 3 P3.3 S10): drives real bridge handlers through the command table on a
# headless PluginProcessor (no editor). Included from the top-level CMakeLists.txt; does nothing unless
# -DMCP_BRIDGE_TESTS=ON. Linux only for now (same link line as plugdata_standalone).
#
#   cmake -S . -B build -DMCP_BRIDGE_TESTS=ON && cmake --build build --target mcp_bridge_tests
#   scripts/run-mcp-bridge-tests.sh
# Realtime guard (Plan 3 P3.4 S6): counts allocations and blocking locks inside the MCP audio hooks
# (Source/Mcp/McpRtGuard.h); /pd/perf then reports rtAllocs / rtLocks. Test builds only.
option(MCP_RT_GUARD "Link the MCP realtime guard into plugdata_standalone" OFF)
if(MCP_RT_GUARD)
    target_sources(plugdata_standalone PRIVATE ${CMAKE_CURRENT_LIST_DIR}/rt_guard_new.cpp)
endif()

option(MCP_BRIDGE_TESTS "Build mcp_bridge_tests, the handler tests of the MCP bridge" OFF)
if(NOT MCP_BRIDGE_TESTS)
    return()
endif()
if(NOT LINUX)
    message(FATAL_ERROR "MCP_BRIDGE_TESTS: only the Linux link line is set up")
endif()

add_executable(mcp_bridge_tests
    ${CMAKE_CURRENT_LIST_DIR}/bridge_handler_tests.cpp
    ${SOURCES_DIRECTORY}/Utility/Config.cpp
)
target_compile_definitions(mcp_bridge_tests PRIVATE ${PLUGDATA_COMPILE_DEFINITIONS})
target_include_directories(mcp_bridge_tests PRIVATE ${PLUGDATA_INCLUDE_DIRECTORY})
target_link_libraries(mcp_bridge_tests PRIVATE plugdata_core pd-src externals "-Wl,-export-dynamic")
copy_binarydata(mcp_bridge_tests)
set_target_properties(mcp_bridge_tests PROPERTIES FOLDER "Tests")
