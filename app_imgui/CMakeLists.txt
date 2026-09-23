cmake_minimum_required(VERSION 3.21)
project(cortex_imgui_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE CACHE BOOL "" FORCE)

# Dear ImGui docking branch, pinned for reproducible preview builds.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        3bae66c735670619baf51391eba7f3d90a25d125
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        5c00bbf36ba8ff47b4fb97712fc38cb2884e5b98
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    zycore
    GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
    GIT_TAG        74620eefd233bec20daeb66e78e744ff06e273b7
)
FetchContent_MakeAvailable(zycore)

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

add_library(cortex_imgui_vendor STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(cortex_imgui_vendor PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(cortex_imgui_vendor PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_source_files_properties(../injector/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexInjectMain")
set_source_files_properties(../mcp_bridge/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexMcpMain")
set_source_files_properties(../tools/diagnostics_host/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexDiagnoseMain")
set_source_files_properties(../tools/symbolize_main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=CortexSymbolizeMain")

add_executable(cortex_imgui WIN32
    main.cpp
    application/settings.cpp
    application/debugger_model.cpp
    application/project_model.cpp
    application/patches_model.cpp
    application/symbols_model.cpp
    application/structures_model.cpp
    application/pointer_maps_model.cpp
    application/snapshots_model.cpp
    application/re_model.cpp
    application/instrumentation_model.cpp
    application/watches_model.cpp
    application/actions_model.cpp
    application/network_model.cpp
    application/diagnostics_model.cpp
    application/scripts_model.cpp
    application/input_model.cpp
    application/screenshot_model.cpp
    application/runtime_events_model.cpp
    ui/memory_workspace.cpp
    ui/memory_browser_workspace.cpp
    ui/modules_workspace.cpp
    ui/overview_workspace.cpp
    ui/patches_workspace.cpp
    ui/project_workspace.cpp
    ui/symbols_workspace.cpp
    ui/structures_workspace.cpp
    ui/pointer_maps_workspace.cpp
    ui/snapshots_workspace.cpp
    ui/re_workspace.cpp
    ui/instrumentation_workspace.cpp
    ui/watches_workspace.cpp
    ui/actions_workspace.cpp
    ui/network_workspace.cpp
    ui/diagnostics_workspace.cpp
    ui/scripts_workspace.cpp
    ui/input_workspace.cpp
    ui/screenshot_workspace.cpp
    ui/events_workspace.cpp
    ui/disassembly_workspace.cpp
    ui/debugger_workspace.cpp
    ui/runtime_workspace.cpp
    ui/sessions_workspace.cpp
    ui/settings_workspace.cpp
    ui/trace_workspace.cpp
    ui/theme.cpp

    ../host/probe_cli.cpp
    ../injector/main.cpp
    ../mcp_bridge/main.cpp
    ../tools/diagnostics_host/main.cpp
    ../tools/symbolize_main.cpp
    ../host/diagnostics/external_host.cpp
    ../host/diagnostics/analyzer.cpp
    ../core/symbols/symbols.cpp
    ../core/symbols/external.cpp
    ../app/cortex.rc
    ../app/windows_debug_provider.cpp
    ../app/windows_debugger_backend.cpp
    ../app/veh_debug_provider.cpp

    ../core/target/local_backend.cpp
    ../core/target/session_manager.cpp
    ../core/target/module_provider.cpp
    ../core/target/thread_provider.cpp

    ../core/services/memory_service.cpp
    ../core/services/scan_service.cpp
    ../core/services/module_service.cpp
    ../core/services/disassembly_service.cpp
    ../core/services/debugger_service.cpp
    ../core/services/payload_client.cpp
)

target_include_directories(cortex_imgui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../app
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
    ${httplib_SOURCE_DIR}
)

target_compile_definitions(cortex_imgui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(cortex_imgui PRIVATE
    cortex_imgui_vendor
    nlohmann_json::nlohmann_json
    Zydis
    d3d11
    dxgi
    user32
    gdi32
    dwmapi
    d3dcompiler
    shell32
    advapi32
    psapi
    ws2_32
    bcrypt
    dbghelp
    windowscodecs
    ole32
    crypt32
)

if(MINGW)
    target_link_options(cortex_imgui PRIVATE -static-libgcc -static-libstdc++)
endif()

set_target_properties(cortex_imgui PROPERTIES
    OUTPUT_NAME "cortex"
)
