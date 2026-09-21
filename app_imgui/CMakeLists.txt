cmake_minimum_required(VERSION 3.21)
project(cortex_imgui_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Dear ImGui docking branch, pinned to a known commit so the prototype is reproducible.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        3bae66c735670619baf51391eba7f3d90a25d125
)
FetchContent_MakeAvailable(imgui)

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

add_executable(cortex_imgui WIN32
    main.cpp
    ui/memory_workspace.cpp
    ui/theme.cpp
    ../core/target/local_backend.cpp
    ../core/target/session_manager.cpp
    ../core/services/memory_service.cpp
    ../core/services/scan_service.cpp
)

target_include_directories(cortex_imgui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
)

target_compile_definitions(cortex_imgui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(cortex_imgui PRIVATE
    cortex_imgui_vendor
    d3d11
    dxgi
    user32
    gdi32
    dwmapi
    d3dcompiler
)

if(MINGW)
    target_link_options(cortex_imgui PRIVATE -static-libgcc -static-libstdc++)
endif()

set_target_properties(cortex_imgui PROPERTIES
    OUTPUT_NAME "cortex-imgui-prototype"
)
