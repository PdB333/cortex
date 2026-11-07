cmake_minimum_required(VERSION 3.20)
project(cortex LANGUAGES C CXX)

include(CTest)

option(CORTEX_BUILD_LEGACY_COMPAT "Build the legacy cortex_host compatibility executable" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
# Once a dependency has been populated, never contact its remote during a
# normal reconfigure. A clean tree still downloads missing sources, while
# existing build directories remain fully offline and deterministic.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
option(CORTEX_OFFLINE "Require every FetchContent dependency to already exist locally" OFF)
if(CORTEX_OFFLINE)
    set(FETCHCONTENT_FULLY_DISCONNECTED ON)
endif()


# kiero (D3D9/10/11/12/OpenGL, engine auto-detected at runtime) is available
# to every bitness; D3D8 is an extra, 32-bit-only backend kept around
# specifically for Hitman: Contracts (2004). Both can be compiled into the
# same 32-bit build and coexist -- only the backend matching the target
# game's actual renderer ever installs a working hook (kiero itself no-ops
# cleanly if it can't detect a known render API already loaded).
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(CORTEX_ENABLE_D3D8 TRUE)
else()
    set(CORTEX_ENABLE_D3D8 FALSE)
endif()
set(CORTEX_ENABLE_KIERO TRUE)

# D3D12 support stays 64-bit only. The runtime capture path is primarily aimed
# at modern 64-bit D3D12 titles, while the 32-bit build keeps D3D8-D3D11 and
# OpenGL coverage without carrying an unused D3D12 hook surface.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CORTEX_ENABLE_D3D12 TRUE)
else()
    set(CORTEX_ENABLE_D3D12 FALSE)
endif()

# ---- MinHook ----
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        d94c64d32ea37bc4f5ee47d580709f70c6fb6080
)
FetchContent_MakeAvailable(minhook)

# ---- cpp-httplib (header-only) ----
# v0.15.3.
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        5c00bbf36ba8ff47b4fb97712fc38cb2884e5b98
)
FetchContent_MakeAvailable(httplib)

# ---- nlohmann/json (header-only) ----
# v3.11.3.
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

# ---- Zydis (disassembler) + its Zycore dependency ----
# Zydis's own CMakeLists expects Zycore as a git submodule under
# dependencies/zycore; FetchContent doesn't check out submodules, so we
# FetchContent Zycore ourselves first -- Zydis's "if (NOT TARGET Zycore)"
# guard then sees it already exists and skips the submodule requirement.
# Zycore v1.5.0.
FetchContent_Declare(
    zycore
    GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
    GIT_TAG        74620eefd233bec20daeb66e78e744ff06e273b7
)
# Zycore 1.5 creates its Doxygen target unconditionally whenever Doxygen is
# installed. Cortex does not ship dependency documentation, and making it an
# ALL target adds substantial noise and latency to every incremental build.
set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zycore)

# Zydis v4.1.0.
FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG        569320ad3c4856da13b9dbf1f0d9e20bda63870e
)
set(ZYDIS_BUILD_TOOLS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_TESTS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_DOXYGEN OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_MAN OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(zydis)

# ---- Lua 5.4 (upstream repo has no CMakeLists; compile the library sources
#      directly. Skips lua.c/luac.c which are the standalone interpreter and
#      compiler CLI drivers we do not need.) ----
# v5.4.7.
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3
)
FetchContent_MakeAvailable(lua)

set(LUA_LIB_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c
    ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
    lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c
    lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c
    lundump.c lutf8lib.c lvm.c lzio.c
)
list(TRANSFORM LUA_LIB_SOURCES PREPEND ${lua_SOURCE_DIR}/)
add_library(lua STATIC ${LUA_LIB_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
target_compile_definitions(lua PUBLIC LUA_COMPAT_5_3)

# ---- stb (single-header, only need stb_image_write.h) ----
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# =========================================================================
# core.dll  -- injected into the target game process
# =========================================================================
set(CORE_RENDER_SOURCES "")
if(CORTEX_ENABLE_KIERO)
    list(APPEND CORE_RENDER_SOURCES core/hook/kiero_hook.cpp third_party/kiero/kiero.cpp core/capture/capture.cpp)
endif()
if(CORTEX_ENABLE_D3D8)
    list(APPEND CORE_RENDER_SOURCES core/hook/d3d8_hook.cpp
        core/overlay/overlay_d3d8.cpp core/capture/capture_d3d8.cpp)
endif()

# Route translation units are compiled through api::RouteRegistrar. The
# registrar forwards every handler to cpp-httplib and mirrors it into the
# native in-process registry, so REST and MCP execute one shared handler.
set(CORTEX_API_ROUTE_SOURCES
    core/api/routes_status.cpp
    core/api/routes_modules.cpp
    core/api/routes_memory.cpp
    core/api/routes_scan.cpp
    core/api/routes_disasm.cpp
    core/api/routes_debug.cpp
    core/api/routes_symbols.cpp
    core/api/routes_project.cpp
    core/api/routes_screenshot.cpp
    core/api/routes_prompt.cpp
    core/api/routes_patch.cpp
    core/api/routes_input.cpp
    core/api/routes_freeze.cpp
    core/api/routes_struct.cpp
    core/api/routes_call.cpp
    core/api/routes_watch.cpp
    core/api/routes_analysis.cpp
    core/api/routes_dissect.cpp
    core/api/routes_batch.cpp
    core/api/routes_actions.cpp
    core/api/routes_events.cpp
    core/api/routes_context.cpp
    core/api/routes_pointermap.cpp
    core/api/routes_trace.cpp
    core/api/routes_ghidra.cpp
    core/api/routes_timeline.cpp
    core/api/routes_window.cpp
    core/api/routes_net.cpp
    core/api/routes_session.cpp
    core/api/routes_mcp.cpp
    core/api/routes_lua.cpp
    core/api/routes_ocr.cpp
    core/api/routes_re.cpp
)
set_source_files_properties(${CORTEX_API_ROUTE_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "CORTEX_ROUTE_REGISTRAR")

add_library(core SHARED
    core/dllmain.cpp
    core/config.cpp
    core/log.cpp
    ${CORE_RENDER_SOURCES}
    core/hook/input_hook.cpp
    core/hook/input_inject.cpp
    core/hook/dinput_hook.cpp
    core/hook/net_hook.cpp
    core/overlay/overlay.cpp
    core/overlay/overlay_common.cpp
    core/memory/memory.cpp
    core/memory/provenance.cpp
    core/memory/scan.cpp
    core/pointermap/pointermap.cpp
    core/disasm/disasm.cpp
    core/debugger/debugger.cpp
    core/symbols/symbols.cpp
    core/project/project.cpp
    core/process/modules.cpp
    core/process/address.cpp
    core/capture/capture_common.cpp
    core/prompt/prompt_queue.cpp
    core/patch/patch.cpp
    core/freeze/freeze.cpp
    core/struct/structs.cpp
    core/struct/infer.cpp
    core/ghidra/ghidra.cpp
    core/timeline/timeline.cpp
    core/call/call.cpp
    core/watch/watch.cpp
    core/re/re_tools.cpp
    core/watch/alloc_watch.cpp
    core/watch/page_watch.cpp
    core/analysis/analysis.cpp
    core/dissect/dissect.cpp
    core/action/action.cpp
    core/events/events.cpp
    core/context/investigation_context.cpp
    core/context/context_api.cpp
    core/api/native_routes.cpp
    core/api/mcp_tools.cpp
    core/api/mcp_pipe.cpp
    core/api/server.cpp
    ${CORTEX_API_ROUTE_SOURCES}
    core/scripting/lua_engine.cpp
    core/ocr/ocr.cpp
)

target_include_directories(core PRIVATE core)

set(CORE_RENDER_LIBS dxguid)
if(CORTEX_ENABLE_KIERO)
    list(APPEND CORE_RENDER_LIBS dxgi d3d9 d3d10 d3d11 d3d12 d3dcompiler opengl32)
endif()
if(CORTEX_ENABLE_D3D8)
    list(APPEND CORE_RENDER_LIBS d3d8)
endif()

target_link_libraries(core PRIVATE
    Zydis
    minhook
    nlohmann_json::nlohmann_json
    stb
    lua
    ${CORE_RENDER_LIBS}
    dinput8
    ws2_32
    winmm
    dwmapi
    dbghelp
    bcrypt
)

target_compile_definitions(core PRIVATE
    CPPHTTPLIB_THREAD_POOL_COUNT=12
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(core PRIVATE ${httplib_SOURCE_DIR})

if(CORTEX_ENABLE_KIERO)
    target_include_directories(core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/kiero)
    target_compile_definitions(core PRIVATE
        CORTEX_KIERO=1
        KIERO_INCLUDE_D3D9=1
        KIERO_INCLUDE_D3D10=1
        KIERO_INCLUDE_D3D11=1
        KIERO_INCLUDE_OPENGL=1
        KIERO_INCLUDE_VULKAN=0
        KIERO_USE_MINHOOK=1
    )
    if(CORTEX_ENABLE_D3D12)
        target_compile_definitions(core PRIVATE CORTEX_D3D12=1 KIERO_INCLUDE_D3D12=1)
    else()
        target_compile_definitions(core PRIVATE KIERO_INCLUDE_D3D12=0)
    endif()
endif()
if(CORTEX_ENABLE_D3D8)
    target_compile_definitions(core PRIVATE CORTEX_D3D8=1)
endif()

set_target_properties(core PROPERTIES
    OUTPUT_NAME "cortex_core"
    PREFIX ""
)

target_link_options(core PRIVATE -static -static-libgcc -static-libstdc++)

# =========================================================================
# cortex_host.exe -- all user-facing Cortex command-line tools in one binary
# =========================================================================
# Each tool exposes a named entry point (host/cli_entry_points.h) that
# unified_main.cpp dispatches to.
if(CORTEX_BUILD_LEGACY_COMPAT)

add_executable(cortex_host
    host/unified_main.cpp
    host/probe_cli.cpp
    host/main.cpp
    injector/main.cpp
    mcp_bridge/main.cpp
    mcp_bridge/host_mode.cpp
    core/artifacts/artifact.cpp
    core/artifacts/pe_provider.cpp
    core/artifacts/pe_extras.cpp
    core/artifacts/asar_provider.cpp
    core/artifacts/registry_default.cpp
    core/artifacts/minidump_provider.cpp
    core/artifacts/hive_provider.cpp
    core/artifacts/js_lexer.cpp
    core/artifacts/js_analysis.cpp
    core/artifacts/js_provider.cpp
    core/artifacts/source_map.cpp
    core/artifacts/inflate.cpp
    core/artifacts/zip_provider.cpp
    core/artifacts/dotnet_metadata.cpp
    core/artifacts/cil.cpp
    core/artifacts/dotnet_provider.cpp
    core/artifacts/native_code.cpp
    core/artifacts/annotations.cpp
    core/evidence/evidence.cpp
    core/investigation/investigation.cpp
    core/target/process_tree.cpp
    core/providers/process_runner.cpp
    core/providers/analysis_provider.cpp
    core/providers/ghidra_provider.cpp
    core/providers/ilspy_provider.cpp
    core/providers/vineflower_provider.cpp
    core/live/websocket.cpp
    core/live/protocol_frames.cpp
    core/live/cdp.cpp
    core/live/dap.cpp
    core/live/jdwp.cpp
    core/live/live_service.cpp
    tools/diagnostics_host/main.cpp
    tools/symbolize_main.cpp
    host/diagnostics/external_host.cpp
    host/diagnostics/analyzer.cpp
    core/symbols/symbols.cpp
    core/symbols/external.cpp
    host/debugger/windows_debug_provider.cpp
    host/debugger/windows_debugger_backend.cpp
    host/debugger/veh_debug_provider.cpp
    core/target/local_backend.cpp
    core/target/session_manager.cpp
    core/target/module_provider.cpp
    core/target/thread_provider.cpp
    core/services/debugger_service.cpp
    core/services/disassembly_service.cpp
    core/services/payload_client.cpp
    core/services/operation_manager.cpp
    core/services/crash_report_service.cpp
)
target_include_directories(cortex_host PRIVATE
    core
    sdk/include
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
target_link_options(cortex_host PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# =========================================================================
# GUI-subsystem fixture used to prove that the injected runtime stays silent:
# no AllocConsole/ConsoleWindowClass may appear with default configuration.
add_executable(cortex_silent_gui_target WIN32 tests/e2e/silent_gui_target.cpp)
target_compile_definitions(cortex_silent_gui_target PRIVATE
    _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(cortex_silent_gui_target PRIVATE user32)
target_link_options(cortex_silent_gui_target PRIVATE -static -static-libgcc -static-libstdc++)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(cortex_silent_gui_target PROPERTIES OUTPUT_NAME "cortex_silent_gui_target_x86")
else()
    set_target_properties(cortex_silent_gui_target PROPERTIES OUTPUT_NAME "cortex_silent_gui_target_x64")
endif()

# =========================================================================
# cortex_test_target -- fake game with known memory values / window / input
# for exercising Cortex end-to-end without needing a real game. Built in
# whichever bitness the tree is configured for; run both x86 and x64 tree
# builds to get both binaries.
# =========================================================================
add_executable(cortex_test_target WIN32 test_target/main.cpp)
target_compile_definitions(cortex_test_target PRIVATE
    _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(cortex_test_target PRIVATE -static -static-libgcc -static-libstdc++
    -Wl,--subsystem,console)  # keep stdout for the canary print
target_link_libraries(cortex_test_target PRIVATE user32 gdi32)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(cortex_test_target PROPERTIES OUTPUT_NAME "cortex_test_target_x86")
else()
    set_target_properties(cortex_test_target PROPERTIES OUTPUT_NAME "cortex_test_target_x64")
endif()

# =========================================================================
# Renderer-specific test targets: same canary layout, D3D9 and D3D11
# backends so the screenshot hook + overlay can be verified per renderer.
# =========================================================================
foreach(RENDERER 9 11)
    set(TGT cortex_test_target_d3d${RENDERER})
    add_executable(${TGT} WIN32 test_target/main_d3d.cpp)
    target_compile_definitions(${TGT} PRIVATE
        TEST_RENDERER=${RENDERER}
        _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_options(${TGT} PRIVATE -static -static-libgcc -static-libstdc++)
    if(RENDERER STREQUAL "9")
        target_link_libraries(${TGT} PRIVATE user32 gdi32 d3d9)
    else()
        target_link_libraries(${TGT} PRIVATE user32 gdi32 d3d11 dxgi)
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set_target_properties(${TGT} PROPERTIES OUTPUT_NAME "${TGT}_x86")
    else()
        set_target_properties(${TGT} PROPERTIES OUTPUT_NAME "${TGT}_x64")
    endif()
endforeach()

if(BUILD_TESTING)
    add_executable(cortex_config_defaults_tests tests/config_defaults_tests.cpp)
    target_include_directories(cortex_config_defaults_tests PRIVATE core)
    add_test(NAME cortex_config_defaults_tests COMMAND cortex_config_defaults_tests)

    add_executable(cortex_core_tests
        tests/core_tests.cpp
        tests/debugger_stub.cpp
        core/memory/memory.cpp
        core/memory/provenance.cpp
        core/memory/scan.cpp
        core/process/modules.cpp
        core/process/address.cpp
        core/patch/patch.cpp
        core/struct/infer.cpp
        core/timeline/timeline.cpp
        core/action/action.cpp
        core/events/events.cpp
    )
    target_include_directories(cortex_core_tests PRIVATE core)
    target_compile_definitions(cortex_core_tests PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_options(cortex_core_tests PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_libraries(cortex_core_tests PRIVATE Zydis nlohmann_json::nlohmann_json)
    add_test(NAME cortex_core_tests COMMAND cortex_core_tests)

    add_executable(cortex_pe_unwind_tests tests/pe_unwind_tests.cpp)
    target_link_libraries(cortex_pe_unwind_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_pe_unwind_tests COMMAND cortex_pe_unwind_tests)

    add_executable(cortex_test_code_context_tests tests/test_code_context_tests.cpp)
    target_link_libraries(cortex_test_code_context_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_test_code_context_tests COMMAND cortex_test_code_context_tests)

    add_executable(cortex_test_code_knowledge_tests tests/test_code_knowledge_tests.cpp)
    target_link_libraries(cortex_test_code_knowledge_tests PRIVATE nlohmann_json::nlohmann_json bcrypt)
    add_test(NAME cortex_test_code_knowledge_tests COMMAND cortex_test_code_knowledge_tests)

    add_executable(cortex_test_knowledge_tests tests/test_knowledge_tests.cpp)
    target_link_libraries(cortex_test_knowledge_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_test_knowledge_tests COMMAND cortex_test_knowledge_tests)

    add_executable(cortex_knowledge_graph_tests tests/knowledge_graph_tests.cpp)
    target_link_libraries(cortex_knowledge_graph_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_knowledge_graph_tests COMMAND cortex_knowledge_graph_tests)

    add_executable(cortex_test_code_candidates_tests tests/test_code_candidates_tests.cpp)
    target_link_libraries(cortex_test_code_candidates_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_test_code_candidates_tests COMMAND cortex_test_code_candidates_tests)

    add_executable(cortex_test_generate_tests tests/test_generate_tests.cpp)
    target_link_libraries(cortex_test_generate_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_test_generate_tests COMMAND cortex_test_generate_tests)

    add_executable(cortex_test_evidence_graph_tests tests/test_evidence_graph_tests.cpp)
    target_link_libraries(cortex_test_evidence_graph_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_test_evidence_graph_tests COMMAND cortex_test_evidence_graph_tests)

    add_executable(cortex_test_runner_tests tests/test_runner_tests.cpp)
    target_link_libraries(cortex_test_runner_tests PRIVATE nlohmann_json::nlohmann_json)
    if(MINGW)
        target_link_options(cortex_test_runner_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME cortex_test_runner_tests COMMAND cortex_test_runner_tests)

    add_executable(cortex_test_plan_tests tests/test_plan_tests.cpp)
    target_link_libraries(cortex_test_plan_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_test_plan_tests COMMAND cortex_test_plan_tests)

    add_executable(cortex_launch_config_tests tests/launch_config_tests.cpp)
    target_link_libraries(cortex_launch_config_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_launch_config_tests COMMAND cortex_launch_config_tests)

    add_executable(cortex_knowledge_query_tests tests/knowledge_query_tests.cpp)
    target_link_libraries(cortex_knowledge_query_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_knowledge_query_tests COMMAND cortex_knowledge_query_tests)

    add_executable(cortex_knowledge_model_tests tests/knowledge_model_tests.cpp)
    target_link_libraries(cortex_knowledge_model_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_knowledge_model_tests COMMAND cortex_knowledge_model_tests)

    add_executable(cortex_session_manager_tests
        tests/session_manager_tests.cpp
        core/target/session_manager.cpp
    )
    target_include_directories(cortex_session_manager_tests PRIVATE core)
    add_test(NAME cortex_session_manager_tests COMMAND cortex_session_manager_tests)

    # Project file recovery: one process per scenario because project::Init
    # runs once.
    add_executable(cortex_project_store_tests
        tests/project_store_tests.cpp
        core/project/project.cpp
        core/memory/memory.cpp
        core/memory/provenance.cpp
        core/process/modules.cpp
        core/events/events.cpp
    )
    target_include_directories(cortex_project_store_tests PRIVATE core)
    target_compile_definitions(cortex_project_store_tests PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(cortex_project_store_tests PRIVATE nlohmann_json::nlohmann_json)
    foreach(scenario backup damaged fresh locked)
        add_test(NAME cortex_project_store_${scenario} COMMAND cortex_project_store_tests ${scenario})
    endforeach()

    # Shared investigation context and its route semantics (portable).
    add_executable(cortex_investigation_context_tests
        tests/investigation_context_tests.cpp
        core/context/investigation_context.cpp
        core/context/context_api.cpp
    )
    target_include_directories(cortex_investigation_context_tests PRIVATE core)
    target_link_libraries(cortex_investigation_context_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_investigation_context_tests COMMAND cortex_investigation_context_tests)

    # Artifact providers: SHA-256, offline PE (on the test target), ASAR,
    # malformed and fuzzed inputs; and the MCP artifact tools' root limits.
    add_executable(cortex_artifacts_tests
        tests/artifacts_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
    )
    target_include_directories(cortex_artifacts_tests PRIVATE core)
    target_link_libraries(cortex_artifacts_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_artifacts_tests COMMAND cortex_artifacts_tests $<TARGET_FILE:cortex_test_target> ${CMAKE_SOURCE_DIR}/tests/fixtures)

    # Setup and updater: AI client registration on a fake home, engine
    # detection, update feed, checksums, archive unpacking.
    add_executable(cortex_setup_tests
        tests/setup_tests.cpp
        core/setup/setup.cpp
        core/setup/updater.cpp
        core/artifacts/artifact.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/inflate.cpp
    )
    target_include_directories(cortex_setup_tests PRIVATE core)
    target_link_libraries(cortex_setup_tests PRIVATE nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(cortex_setup_tests PRIVATE winhttp shell32)
    endif()
    add_test(NAME cortex_setup_tests COMMAND cortex_setup_tests)

    add_executable(cortex_js_tests
        tests/js_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
    )
    target_include_directories(cortex_js_tests PRIVATE core)
    target_link_libraries(cortex_js_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_js_tests COMMAND cortex_js_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

    add_executable(cortex_dotnet_tests
        tests/dotnet_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
    )
    target_include_directories(cortex_dotnet_tests PRIVATE core)
    target_link_libraries(cortex_dotnet_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_dotnet_tests COMMAND cortex_dotnet_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dotnet)

    add_executable(cortex_native_code_tests
        tests/native_code_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
        core/artifacts/native_code.cpp
        core/artifacts/annotations.cpp
    )
    target_include_directories(cortex_native_code_tests PRIVATE core)
    target_link_libraries(cortex_native_code_tests PRIVATE Zydis nlohmann_json::nlohmann_json)
    add_test(NAME cortex_native_code_tests COMMAND cortex_native_code_tests $<TARGET_FILE:cortex_test_target>)

    # Two builds of one program for evidence comparison across versions.
    add_executable(cortex_shift_fixture_a tests/fixtures/shift_fixture.c)
    add_executable(cortex_shift_fixture_b tests/fixtures/shift_fixture.c)
    target_compile_definitions(cortex_shift_fixture_b PRIVATE CORTEX_SHIFT)
    foreach(shift cortex_shift_fixture_a cortex_shift_fixture_b)
        target_compile_options(${shift} PRIVATE -O2)
        target_link_options(${shift} PRIVATE -static -static-libgcc)
    endforeach()

    add_executable(cortex_evidence_tests
        tests/evidence_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
        core/artifacts/native_code.cpp
        core/artifacts/annotations.cpp
        core/evidence/evidence.cpp
    )
    target_include_directories(cortex_evidence_tests PRIVATE core)
    target_link_libraries(cortex_evidence_tests PRIVATE Zydis nlohmann_json::nlohmann_json)
    add_test(NAME cortex_evidence_tests COMMAND cortex_evidence_tests $<TARGET_FILE:cortex_shift_fixture_a> $<TARGET_FILE:cortex_shift_fixture_b>)

    add_executable(cortex_investigation_tests
        tests/investigation_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
        core/artifacts/native_code.cpp
        core/artifacts/annotations.cpp
        core/evidence/evidence.cpp
        core/investigation/investigation.cpp
    )
    target_include_directories(cortex_investigation_tests PRIVATE core)
    target_link_libraries(cortex_investigation_tests PRIVATE Zydis nlohmann_json::nlohmann_json)
    add_test(NAME cortex_investigation_tests COMMAND cortex_investigation_tests $<TARGET_FILE:cortex_test_target> ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/electron-app.asar $<TARGET_FILE:cortex_shift_fixture_a>)

    add_executable(cortex_analysis_provider_tests
        tests/analysis_provider_tests.cpp
        core/providers/process_runner.cpp
        core/providers/analysis_provider.cpp
        core/providers/ghidra_provider.cpp
        core/providers/ilspy_provider.cpp
    core/providers/vineflower_provider.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
        core/artifacts/native_code.cpp
        core/artifacts/annotations.cpp)
    target_include_directories(cortex_analysis_provider_tests PRIVATE core)
    target_link_libraries(cortex_analysis_provider_tests PRIVATE Zydis nlohmann_json::nlohmann_json)
    # Real engines run when GHIDRA_INSTALL_DIR / CORTEX_ILSPYCMD are set (CI
    # sets them in the analysis-engines job); otherwise those parts skip.
    add_test(NAME cortex_analysis_provider_tests COMMAND cortex_analysis_provider_tests
             $<TARGET_FILE:cortex_test_target> ${CMAKE_SOURCE_DIR}/tests/fixtures/dotnet/Cortex.Fixture.dll
             ${CMAKE_SOURCE_DIR}/tests/fixtures/java/Cortex.Fixture.jar ${CMAKE_SOURCE_DIR}/tests/fixtures/elf/actor.elf)
    set_tests_properties(cortex_analysis_provider_tests PROPERTIES TIMEOUT 3600)

    add_executable(cortex_cdp_tests tests/cdp_tests.cpp core/live/websocket.cpp core/live/protocol_frames.cpp core/live/cdp.cpp core/live/dap.cpp core/live/jdwp.cpp core/live/live_service.cpp core/providers/process_runner.cpp)
    target_include_directories(cortex_cdp_tests PRIVATE core)
    target_link_libraries(cortex_cdp_tests PRIVATE nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(cortex_cdp_tests PRIVATE ws2_32)
    endif()
    # Node runs the fixture when it is on PATH (CI); otherwise that part skips.
    add_test(NAME cortex_cdp_tests COMMAND cortex_cdp_tests ${CMAKE_SOURCE_DIR}/tests/fixtures/node-live/app.js)
    set_tests_properties(cortex_cdp_tests PROPERTIES TIMEOUT 300)

    add_executable(cortex_dap_tests tests/dap_tests.cpp core/live/websocket.cpp core/live/protocol_frames.cpp core/live/cdp.cpp core/live/dap.cpp core/live/jdwp.cpp core/live/live_service.cpp core/providers/process_runner.cpp)
    target_include_directories(cortex_dap_tests PRIVATE core)
    target_link_libraries(cortex_dap_tests PRIVATE nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(cortex_dap_tests PRIVATE ws2_32)
    endif()
    # Python with debugpy runs the fixture when present (CI's analysis-engines
    # job also passes the .NET fixture and netcoredbg); otherwise it skips.
    add_test(NAME cortex_dap_tests COMMAND cortex_dap_tests ${CMAKE_SOURCE_DIR}/tests/fixtures/python-live/app.py)
    set_tests_properties(cortex_dap_tests PROPERTIES TIMEOUT 300)

    # Contract tests that contracts.yml used to compile by hand. They are
    # self-contained (headers only, or a few core sources) and now run with
    # every `ctest`.
    foreach(contract request_id pagination mcp_protocol mcp_pipe_protocol mcp_schema
                     mcp_query_schema mcp_bridge_policy)
        add_executable(cortex_${contract}_tests tests/${contract}_tests.cpp)
        target_include_directories(cortex_${contract}_tests PRIVATE core)
        target_link_libraries(cortex_${contract}_tests PRIVATE nlohmann_json::nlohmann_json)
        add_test(NAME cortex_${contract}_tests COMMAND cortex_${contract}_tests)
    endforeach()

    add_executable(cortex_mcp_authority_tests tests/mcp_authority_tests.cpp)
    target_include_directories(cortex_mcp_authority_tests PRIVATE core)
    target_link_libraries(cortex_mcp_authority_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_mcp_authority_tests
             COMMAND cortex_mcp_authority_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mcp_tool_risks.json)

    add_executable(cortex_semantic_tools_tests tests/semantic_tools_tests.cpp)
    target_include_directories(cortex_semantic_tools_tests PRIVATE core)
    target_link_libraries(cortex_semantic_tools_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_semantic_tools_tests
             COMMAND cortex_semantic_tools_tests ${CMAKE_CURRENT_SOURCE_DIR}/core/api/routes_status.cpp)

    add_executable(cortex_action_transaction_tests
        tests/action_transaction_tests.cpp core/action/action.cpp core/events/events.cpp)
    target_include_directories(cortex_action_transaction_tests PRIVATE core)
    target_link_libraries(cortex_action_transaction_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_action_transaction_tests COMMAND cortex_action_transaction_tests)

    if(WIN32)
        add_executable(cortex_call_scheduler_tests tests/call_scheduler_tests.cpp core/call/call.cpp)
        target_include_directories(cortex_call_scheduler_tests PRIVATE core)
        target_link_libraries(cortex_call_scheduler_tests PRIVATE nlohmann_json::nlohmann_json user32)
        if(MINGW)
            target_link_options(cortex_call_scheduler_tests PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME cortex_call_scheduler_tests COMMAND cortex_call_scheduler_tests)
    endif()

    add_executable(cortex_process_tree_tests tests/process_tree_tests.cpp core/target/process_tree.cpp)
    target_link_libraries(cortex_process_tree_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME cortex_process_tree_tests COMMAND cortex_process_tree_tests)

    add_executable(cortex_artifact_tools_tests
        tests/artifact_tools_tests.cpp
        core/artifacts/artifact.cpp
        core/artifacts/pe_provider.cpp
        core/artifacts/pe_extras.cpp
        core/artifacts/asar_provider.cpp
        core/artifacts/registry_default.cpp
        core/artifacts/minidump_provider.cpp
        core/artifacts/hive_provider.cpp
        core/artifacts/js_lexer.cpp
        core/artifacts/js_analysis.cpp
        core/artifacts/js_provider.cpp
        core/artifacts/source_map.cpp
        core/artifacts/inflate.cpp
        core/artifacts/zip_provider.cpp
        core/artifacts/dotnet_metadata.cpp
        core/artifacts/cil.cpp
        core/artifacts/dotnet_provider.cpp
        core/artifacts/native_code.cpp
        core/artifacts/annotations.cpp
    )
    target_include_directories(cortex_artifact_tools_tests PRIVATE core)
    target_link_libraries(cortex_artifact_tools_tests PRIVATE Zydis nlohmann_json::nlohmann_json)
    add_test(NAME cortex_artifact_tools_tests COMMAND cortex_artifact_tools_tests $<TARGET_FILE:cortex_test_target>)

    # Runtime event ring: bounded, contiguous ids, explicit gaps.
    add_executable(cortex_events_tests tests/events_tests.cpp core/events/events.cpp)
    target_include_directories(cortex_events_tests PRIVATE core)
    add_test(NAME cortex_events_tests COMMAND cortex_events_tests)
endif()

# Standalone test executables must not depend on compiler DLLs being on PATH.
if(BUILD_TESTING AND MINGW)
    foreach(test_target cortex_knowledge_graph_tests cortex_test_code_candidates_tests cortex_test_generate_tests cortex_test_evidence_graph_tests cortex_config_defaults_tests cortex_session_manager_tests cortex_project_store_tests cortex_investigation_context_tests cortex_events_tests cortex_artifacts_tests cortex_setup_tests cortex_artifact_tools_tests cortex_js_tests cortex_dotnet_tests cortex_native_code_tests cortex_evidence_tests cortex_investigation_tests cortex_process_tree_tests cortex_analysis_provider_tests cortex_cdp_tests cortex_dap_tests cortex_test_plan_tests cortex_test_knowledge_tests cortex_test_code_context_tests cortex_test_code_knowledge_tests cortex_pe_unwind_tests cortex_launch_config_tests cortex_knowledge_query_tests cortex_knowledge_model_tests
                        cortex_request_id_tests cortex_pagination_tests cortex_mcp_protocol_tests cortex_mcp_pipe_protocol_tests cortex_mcp_schema_tests cortex_mcp_query_schema_tests cortex_mcp_bridge_policy_tests cortex_mcp_authority_tests cortex_semantic_tools_tests cortex_action_transaction_tests)
        target_link_options(${test_target} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
endif()
