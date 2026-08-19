cmake_minimum_required(VERSION 3.21)
project(smolmux VERSION 0.1.2 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Need POSIX APIs (openpty, clock_gettime, etc.)
add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _XOPEN_SOURCE=600)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# ========================================================================
# Build options
# ========================================================================
option(SM_STATIC      "Link all dependencies statically (except glibc)" OFF)
option(SM_MUSL_STATIC "Fully static musl-libc build (zero runtime deps)" OFF)
option(SM_STRIP       "Strip binaries" OFF)

if(SM_MUSL_STATIC)
    add_link_options(-static)
endif()

# ========================================================================
# Feature flags via Kconfig
# ========================================================================
include(cmake/kconfig.cmake)

# ========================================================================
# Vendored cJSON
# ========================================================================
add_library(cjson STATIC deps/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC deps/cJSON)

# ========================================================================
# Source files — core (always compiled) + conditional features
# ========================================================================
set(SM_SOURCES
    src/util/str.c
    src/util/base64.c
    src/util/json_helpers.c
    src/util/sock_util.c
    src/util/shared_line.c
    src/util/timeutil.c
    src/util/keyspec.c
    src/util/profile_resolve.c
    src/logger.c
    src/protocol.c
    src/ring_buffer.c
    src/io_log.c
    src/text_log.c
    src/regex_engine.c
    src/expect.c
    src/anomaly.c
    src/boot_stage.c
    src/autoresponder.c
    src/device_profile.c
    src/mcp_broker_conn.c
    src/broker_info.c
    src/board_manifest.c
    src/client.c
    src/broker.c
    src/links/link_wq.c
    src/sinks/mcp_schemas.c
)

if(SM_ENABLE_UART)
    list(APPEND SM_SOURCES src/links/uart.c)
endif()

if(SM_ENABLE_GDB)
    list(APPEND SM_SOURCES src/links/gdb.c)
    # GDB-mcp foundations: MI parser + target profile (client-side intelligence)
    list(APPEND SM_SOURCES src/util/mi_parse.c)
    list(APPEND SM_SOURCES src/gdb_profile.c)
endif()

if(SM_ENABLE_LINK_SERIAL_TCP)
    list(APPEND SM_SOURCES src/links/serial_tcp.c)
endif()

if(SM_ENABLE_SINK_MCP)
    list(APPEND SM_SOURCES src/sinks/mcp.c src/sinks/mcp_tools.c)
endif()

if(SM_ENABLE_SINK_TCP)
    list(APPEND SM_SOURCES src/sinks/tcp.c)
endif()

if(SM_ENABLE_SINK_WS)
    list(APPEND SM_SOURCES src/sinks/ws.c src/util/sha1.c)
endif()

# ========================================================================
# Library target (all modules except main)
# ========================================================================
add_library(smolmux_lib STATIC ${SM_SOURCES})
target_include_directories(smolmux_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/cJSON
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(smolmux_lib PUBLIC cjson m)

if(SM_ENABLE_SINK_WS)
    target_link_libraries(smolmux_lib PUBLIC pthread)
endif()

# ========================================================================
# Optional: PCRE2
# ========================================================================
if(SM_ENABLE_PCRE2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PCRE2 REQUIRED libpcre2-8)
    target_include_directories(smolmux_lib PUBLIC ${PCRE2_INCLUDE_DIRS})
    target_link_libraries(smolmux_lib PUBLIC ${PCRE2_LIBRARIES})
endif()

# ========================================================================
# Main executable
# ========================================================================
add_executable(smolmux src/main.c)
target_link_libraries(smolmux PRIVATE smolmux_lib)

add_executable(smolmux-monitor src/monitor.c)
target_link_libraries(smolmux-monitor PRIVATE smolmux_lib)

add_executable(smolmux-cli src/cli.c)
target_link_libraries(smolmux-cli PRIVATE smolmux_lib)

add_executable(smolmux-mcp src/mcp_client.c)
target_link_libraries(smolmux-mcp PRIVATE smolmux_lib)

if(SM_ENABLE_GDB)
    add_executable(smolmux-gdb-mcp src/gdb_mcp.c)
    target_link_libraries(smolmux-gdb-mcp PRIVATE smolmux_lib)
endif()

if(SM_ENABLE_WATCHER)
    add_executable(smolmux-watcher src/watcher.c)
    target_link_libraries(smolmux-watcher PRIVATE smolmux_lib)
endif()

# Developer benchmarks (opt-in: -DSM_BUILD_BENCH=ON), not built by default.
option(SM_BUILD_BENCH "Build developer benchmark harnesses" OFF)
if(SM_BUILD_BENCH)
    add_executable(bench_coalesce bench/bench_coalesce.c)
    target_link_libraries(bench_coalesce PRIVATE smolmux_lib)
endif()

# ========================================================================
# Strip binaries
# ========================================================================
if(SM_STRIP AND CMAKE_STRIP)
    add_custom_command(TARGET smolmux POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:smolmux>)
    add_custom_command(TARGET smolmux-monitor POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:smolmux-monitor>)
    add_custom_command(TARGET smolmux-cli POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:smolmux-cli>)
    add_custom_command(TARGET smolmux-mcp POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:smolmux-mcp>)
    if(SM_ENABLE_WATCHER)
        add_custom_command(TARGET smolmux-watcher POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:smolmux-watcher>)
    endif()
    if(SM_ENABLE_GDB)
        add_custom_command(TARGET smolmux-gdb-mcp POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:smolmux-gdb-mcp>)
    endif()
endif()

# ========================================================================
# Tests
# ========================================================================
include(CTest)

if(BUILD_TESTING)
set(TEST_SOURCES
    tests/test_str.c
    tests/test_base64.c
    tests/test_protocol.c
    tests/test_ring_buffer.c
    tests/test_io_log.c
    tests/test_expect.c
    tests/test_anomaly.c
    tests/test_boot_stage.c
    tests/test_autoresponder.c
    tests/test_regex_engine.c
    tests/test_device_profile.c
    tests/test_text_log.c
    tests/test_sock_util.c
    tests/test_board_manifest.c
    tests/test_keyspec.c
)

if(SM_ENABLE_UART)
    list(APPEND TEST_SOURCES tests/test_uart.c)
    list(APPEND TEST_SOURCES tests/test_broker.c)
    list(APPEND TEST_SOURCES tests/test_monitor.c)
endif()

if(SM_ENABLE_GDB)
    list(APPEND TEST_SOURCES tests/test_gdb.c)
    list(APPEND TEST_SOURCES tests/test_gdb_broker.c)
    list(APPEND TEST_SOURCES tests/test_mi_parse.c)
    list(APPEND TEST_SOURCES tests/test_gdb_profile.c)
endif()

if(SM_ENABLE_LINK_SERIAL_TCP)
    list(APPEND TEST_SOURCES tests/test_serial_tcp.c)
endif()

if(SM_ENABLE_SINK_MCP AND SM_ENABLE_UART)
    list(APPEND TEST_SOURCES tests/test_mcp.c)
endif()

if(SM_ENABLE_SINK_MCP)
    list(APPEND TEST_SOURCES tests/test_mcp_report.c)
endif()

if(SM_ENABLE_SINK_TCP AND SM_ENABLE_UART)
    list(APPEND TEST_SOURCES tests/test_tcp.c)
endif()

if(SM_ENABLE_SINK_WS AND SM_ENABLE_UART)
    list(APPEND TEST_SOURCES tests/test_ws.c)
endif()

if(SM_ENABLE_WATCHER)
    list(APPEND TEST_SOURCES tests/test_watcher.c)
endif()

# smolmux-cli is always built, so its reader test is unconditional.
list(APPEND TEST_SOURCES tests/test_cli.c)

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(${test_name} PRIVATE smolmux_lib)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Watcher test needs watcher.c compiled with main renamed
if(SM_ENABLE_WATCHER)
    add_library(watcher_testlib OBJECT src/watcher.c)
    target_include_directories(watcher_testlib PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/deps/cJSON
        ${CMAKE_BINARY_DIR}
    )
    target_compile_definitions(watcher_testlib PRIVATE main=watcher_main)
    target_link_libraries(test_watcher PRIVATE smolmux_lib)
    target_sources(test_watcher PRIVATE $<TARGET_OBJECTS:watcher_testlib>)
endif()

# CLI test needs cli.c compiled with main renamed (for cli_test_* hooks)
add_library(cli_testlib OBJECT src/cli.c)
target_include_directories(cli_testlib PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/cJSON
    ${CMAKE_BINARY_DIR}
)
target_compile_definitions(cli_testlib PRIVATE main=cli_main)
target_sources(test_cli PRIVATE $<TARGET_OBJECTS:cli_testlib>)
# with-port test drives a broker-in-thread over a PTY (openpty needs libutil)
target_link_libraries(test_cli PRIVATE pthread util)

# Broker/monitor tests need pthreads and util (openpty)
if(SM_ENABLE_UART)
    target_link_libraries(test_broker PRIVATE pthread util)
    target_link_libraries(test_monitor PRIVATE pthread util)
    target_link_libraries(test_uart PRIVATE util)
endif()

# GDB broker integration test runs the broker on a thread
if(SM_ENABLE_GDB)
    target_link_libraries(test_gdb_broker PRIVATE pthread)

    # smolmux-gdb-mcp e2e: spawns the real binary against a broker whose GDB
    # link runs the fake_gdb helper. Registered separately to pass both paths.
    add_executable(fake_gdb tests/fake_gdb.c)
    add_executable(test_gdb_mcp tests/test_gdb_mcp.c)
    target_include_directories(test_gdb_mcp PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(test_gdb_mcp PRIVATE smolmux_lib pthread util)
    add_test(NAME test_gdb_mcp
             COMMAND test_gdb_mcp $<TARGET_FILE:smolmux-gdb-mcp> $<TARGET_FILE:fake_gdb>)
endif()

# smolmux-mcp e2e smoke test spawns the real binary, so it is registered
# separately to pass the binary path as argv[1]
if(SM_ENABLE_UART)
    add_executable(test_mcp_client tests/test_mcp_client.c)
    target_include_directories(test_mcp_client PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(test_mcp_client PRIVATE smolmux_lib pthread util)
    add_test(NAME test_mcp_client
             COMMAND test_mcp_client $<TARGET_FILE:smolmux-mcp>)
endif()

# MCP test needs pthreads and util
if(SM_ENABLE_SINK_MCP AND SM_ENABLE_UART)
    target_link_libraries(test_mcp PRIVATE pthread util)
endif()

# TCP test needs pthreads and util
if(SM_ENABLE_SINK_TCP AND SM_ENABLE_UART)
    target_link_libraries(test_tcp PRIVATE pthread util)
endif()

# WS test needs pthreads and util
if(SM_ENABLE_SINK_WS AND SM_ENABLE_UART)
    target_link_libraries(test_ws PRIVATE pthread util)
endif()
endif() # BUILD_TESTING

# ========================================================================
# Install
# ========================================================================
set(_INSTALL_TARGETS smolmux smolmux-monitor smolmux-cli smolmux-mcp)
if(SM_ENABLE_WATCHER)
    list(APPEND _INSTALL_TARGETS smolmux-watcher)
endif()
install(TARGETS ${_INSTALL_TARGETS} DESTINATION bin)
