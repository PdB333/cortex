# Shared by the desktop, unified host, standalone client and native agent.
# Third-party TLS is pinned; Cortex defines its own application protocol.
if(TARGET cortex_remote)
    return()
endif()
include(FetchContent)
enable_language(C)
set(ENABLE_PROGRAMS OFF CACHE BOOL "Mbed TLS sample programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Mbed TLS own tests (Cortex tests are separate)" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cortex_mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG 068ff080b369adfac81509f9b57b2afabaf82dc5
    GIT_SUBMODULES framework)
FetchContent_MakeAvailable(cortex_mbedtls)
get_filename_component(CORTEX_REMOTE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(json)
endif()
add_library(cortex_remote STATIC
    "${CORTEX_REMOTE_ROOT}/core/remote/protocol.cpp"
    "${CORTEX_REMOTE_ROOT}/core/remote/queries.cpp"
    "${CORTEX_REMOTE_ROOT}/core/remote/validation.cpp"
    "${CORTEX_REMOTE_ROOT}/core/remote/tls.cpp"
    "${CORTEX_REMOTE_ROOT}/core/remote/client.cpp")
target_compile_features(cortex_remote PUBLIC cxx_std_17)
target_link_libraries(cortex_remote PUBLIC nlohmann_json::nlohmann_json mbedtls mbedx509 mbedcrypto)
if(WIN32)
    target_compile_definitions(cortex_remote PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(cortex_remote PUBLIC ws2_32 bcrypt)
endif()
