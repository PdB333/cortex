cmake_minimum_required(VERSION 3.20)
project(cortex_diag_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(cortex_diag_host
    main.cpp
    ../../host/diagnostics/external_host.cpp
    ../../host/diagnostics/analyzer.cpp
)

target_include_directories(cortex_diag_host PRIVATE
    ../..
    ../../sdk/include
)

target_compile_definitions(cortex_diag_host PRIVATE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(cortex_diag_host PRIVATE dbghelp)
