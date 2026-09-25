cmake_minimum_required(VERSION 3.20)
project(cortex_unified_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG 5c00bbf36ba8ff47b4fb97712fc38cb2884e5b98
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    zycore
    GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
    GIT_TAG 74620eefd233bec20daeb66e78e744ff06e273b7
)
FetchContent_MakeAvailable(zycore)

FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG 569320ad3c4856da13b9dbf1f0d9e20bda63870e
)
set(ZYDIS_BUILD_TOOLS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_TESTS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_DOXYGEN OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_MAN OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(zydis)

set(CORTEX_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(CORTEX_SERVE_SOURCE ${CORTEX_ROOT}/host/main.cpp)
set(CORTEX_INJECT_SOURCE ${CORTEX_ROOT}/injector/main.cpp)
set(CORTEX_MCP_SOURCE ${CORTEX_ROOT}/mcp_bridge/main.cpp)
set(CORTEX_DIAG_SOURCE ${CORTEX_ROOT}/tools/diagnostics_host/main.cpp)
set(CORTEX_SYMBOLIZE_SOURCE ${CORTEX_ROOT}/tools/symbolize_main.cpp)

set_source_files_properties(${CORTEX_SERVE_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "main=CortexServeMain")
set_source_files_properties(${CORTEX_INJECT_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "main=CortexInjectMain")
set_source_files_properties(${CORTEX_MCP_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "main=CortexMcpMain")
set_source_files_properties(${CORTEX_DIAG_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "main=CortexDiagnoseMain")
set_source_files_properties(${CORTEX_SYMBOLIZE_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "main=CortexSymbolizeMain")

add_executable(cortex_host
    ${CORTEX_ROOT}/host/unified_main.cpp
    ${CORTEX_ROOT}/host/probe_cli.cpp
    ${CORTEX_SERVE_SOURCE}
    ${CORTEX_INJECT_SOURCE}
    ${CORTEX_MCP_SOURCE}
    ${CORTEX_ROOT}/mcp_bridge/host_mode.cpp
    ${CORTEX_DIAG_SOURCE}
    ${CORTEX_SYMBOLIZE_SOURCE}
    ${CORTEX_ROOT}/host/diagnostics/external_host.cpp
    ${CORTEX_ROOT}/host/diagnostics/analyzer.cpp
    ${CORTEX_ROOT}/core/symbols/symbols.cpp
    ${CORTEX_ROOT}/core/symbols/external.cpp
    ${CORTEX_ROOT}/app/windows_debug_provider.cpp
    ${CORTEX_ROOT}/app/windows_debugger_backend.cpp
    ${CORTEX_ROOT}/app/veh_debug_provider.cpp
    ${CORTEX_ROOT}/core/target/local_backend.cpp
    ${CORTEX_ROOT}/core/target/session_manager.cpp
    ${CORTEX_ROOT}/core/target/module_provider.cpp
    ${CORTEX_ROOT}/core/target/thread_provider.cpp
    ${CORTEX_ROOT}/core/services/debugger_service.cpp
    ${CORTEX_ROOT}/core/services/disassembly_service.cpp
    ${CORTEX_ROOT}/core/services/payload_client.cpp
    ${CORTEX_ROOT}/core/services/operation_manager.cpp
    ${CORTEX_ROOT}/core/services/crash_report_service.cpp
)

target_include_directories(cortex_host PRIVATE
    ${CORTEX_ROOT}/core
    ${CORTEX_ROOT}/sdk/include
    ${httplib_SOURCE_DIR}
)

target_compile_definitions(cortex_host PRIVATE
    CPPHTTPLIB_THREAD_POOL_COUNT=12
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(cortex_host PRIVATE
    nlohmann_json::nlohmann_json
    Zydis
    ws2_32
    bcrypt
    dbghelp
    user32
)

if(MINGW)
    target_link_options(cortex_host PRIVATE -static -static-libgcc -static-libstdc++)
endif()
