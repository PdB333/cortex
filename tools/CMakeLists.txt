cmake_minimum_required(VERSION 3.20)
project(cortex_symbolize LANGUAGES CXX)

add_executable(cortex_symbolize
    symbolize_main.cpp
    ../core/symbols/symbols.cpp
    ../core/symbols/external.cpp
)

target_compile_features(cortex_symbolize PRIVATE cxx_std_17)
target_include_directories(cortex_symbolize PRIVATE ../core)
target_compile_definitions(cortex_symbolize PRIVATE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(cortex_symbolize PRIVATE dbghelp)

if(MINGW)
    target_link_options(cortex_symbolize PRIVATE -static -static-libgcc -static-libstdc++)
endif()
