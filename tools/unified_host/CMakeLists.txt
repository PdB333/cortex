cmake_minimum_required(VERSION 3.20)
project(cortex_unified_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

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
    ${CORTEX_SERVE_SOURCE}
    ${CORTEX_INJECT_SOURCE}
    ${CORTEX_MCP_SOURCE}
    ${CORTEX_DIAG_SOURCE}
    ${CORTEX_SYMBOLIZE_SOURCE}
    ${CORTEX_ROOT}/host/diagnostics/external_host.cpp
    ${CORTEX_ROOT}/host/diagnostics/analyzer.cpp
    ${CORTEX_ROOT}/core/symbols/symbols.cpp
    ${CORTEX_ROOT}/core/symbols/external.cpp
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
    ws2_32
    bcrypt
    dbghelp
    user32
)

if(MINGW)
    target_link_options(cortex_host PRIVATE -static -static-libgcc -static-libstdc++)
endif()
