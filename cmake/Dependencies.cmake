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

# ---- cpp-httplib (header-only), v0.15.3 ----
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        5c00bbf36ba8ff47b4fb97712fc38cb2884e5b98
)
FetchContent_MakeAvailable(httplib)

# ---- nlohmann/json (header-only), v3.11.3 ----
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

# ---- Lua 5.4.7 (upstream repo has no CMakeLists; compile the library sources
#      directly. Skips lua.c/luac.c which are standalone CLI drivers.) ----
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
