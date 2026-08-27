cmake_minimum_required(VERSION 3.20)
project(cortex LANGUAGES C CXX)

include(CTest)

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

# ---- Dear ImGui (no CMake support upstream, vendor sources directly) ----
# v1.90.9, pinned to the immutable commit resolved from the release tag.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        cb16be3a3fc1f9cd146ae24d52b615f8a05fa93d
)
FetchContent_MakeAvailable(imgui)

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

# D3D12 support is 64-bit only: imgui's own D3D12 backend packs a GPU
# descriptor handle (8 bytes) into ImTextureID, which is pointer-sized (4
# bytes on 32-bit) -- a static_assert in imgui_impl_dx12.cpp rejects 32-bit
# outright. Real-world D3D12 games are 64-bit anyway, so this costs nothing.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CORTEX_ENABLE_D3D12 TRUE)
else()
    set(CORTEX_ENABLE_D3D12 FALSE)
endif()

set(IMGUI_BACKEND_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
if(CORTEX_ENABLE_KIERO)
    list(APPEND IMGUI_BACKEND_SOURCES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx10.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    if(CORTEX_ENABLE_D3D12)
        list(APPEND IMGUI_BACKEND_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    endif()
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${IMGUI_BACKEND_SOURCES}
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

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
    list(APPEND CORE_RENDER_SOURCES core/hook/d3d8_hook.cpp core/overlay/imgui_impl_dx8.cpp
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
    core/watch/alloc_watch.cpp
    core/watch/page_watch.cpp
    core/analysis/analysis.cpp
    core/dissect/dissect.cpp
    core/action/action.cpp
    core/events/events.cpp
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
    imgui
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
# Preserve the existing, already-tested implementations by renaming each
# source file's main() at compile time and dispatching from unified_main.cpp.
set_source_files_properties(host/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexServeMain")
set_source_files_properties(injector/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexInjectMain")
set_source_files_properties(mcp_bridge/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexMcpMain")
set_source_files_properties(tools/diagnostics_host/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexDiagnoseMain")
set_source_files_properties(tools/symbolize_main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexSymbolizeMain")

add_executable(cortex_host
    host/unified_main.cpp
    host/main.cpp
    injector/main.cpp
    mcp_bridge/main.cpp
    tools/diagnostics_host/main.cpp
    tools/symbolize_main.cpp
    host/diagnostics/external_host.cpp
    host/diagnostics/analyzer.cpp
    core/symbols/symbols.cpp
    core/symbols/external.cpp
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
    ws2_32
    bcrypt
    dbghelp
    user32
)
target_link_options(cortex_host PRIVATE -static -static-libgcc -static-libstdc++)

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
endif()