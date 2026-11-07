# libFuzzer harnesses for Cortex's file parsers. A separate project, built on
# Linux with clang (the parsers are portable C++17):
#   cmake -S tests/fuzz -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
#   cmake --build build/fuzz
#   build/fuzz/fuzz_artifact -max_total_time=60 build/fuzz/corpus/artifact
# CORTEX_FUZZ_REPLAY=ON builds the same harnesses as plain executables that
# replay a corpus directory (no libFuzzer), for ctest.
cmake_minimum_required(VERSION 3.21)
project(cortex_fuzz CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(CORTEX_FUZZ_REPLAY "Build replay executables instead of libFuzzer targets" OFF)

include(FetchContent)
# Same revisions as the main build (CMakeLists.txt at the repository root).
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git
                          GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)
FetchContent_Declare(zycore GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
                            GIT_TAG 74620eefd233bec20daeb66e78e744ff06e273b7)
set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zycore)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git
                           GIT_TAG 569320ad3c4856da13b9dbf1f0d9e20bda63870e)
set(ZYDIS_BUILD_TOOLS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_TESTS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_DOXYGEN OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_MAN OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(zydis)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ARTIFACT_SOURCES
    ${ROOT}/core/artifacts/artifact.cpp ${ROOT}/core/artifacts/pe_provider.cpp ${ROOT}/core/artifacts/pe_extras.cpp
    ${ROOT}/core/artifacts/asar_provider.cpp ${ROOT}/core/artifacts/registry_default.cpp
    ${ROOT}/core/artifacts/js_lexer.cpp ${ROOT}/core/artifacts/js_analysis.cpp
    ${ROOT}/core/artifacts/js_provider.cpp ${ROOT}/core/artifacts/source_map.cpp
    ${ROOT}/core/artifacts/inflate.cpp ${ROOT}/core/artifacts/zip_provider.cpp
    ${ROOT}/core/artifacts/dotnet_metadata.cpp ${ROOT}/core/artifacts/cil.cpp
    ${ROOT}/core/artifacts/dotnet_provider.cpp ${ROOT}/core/artifacts/native_code.cpp
    ${ROOT}/core/artifacts/annotations.cpp)
add_library(cortex_fuzz_parsers STATIC ${ARTIFACT_SOURCES} ${ROOT}/core/live/protocol_frames.cpp
    ${ROOT}/core/evidence/evidence.cpp ${ROOT}/core/investigation/investigation.cpp)
target_include_directories(cortex_fuzz_parsers PUBLIC ${ROOT}/core)
target_link_libraries(cortex_fuzz_parsers PUBLIC nlohmann_json::nlohmann_json Zydis)

if(CORTEX_FUZZ_REPLAY)
    set(SAN -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
else()
    set(SAN -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
endif()
target_compile_options(cortex_fuzz_parsers PRIVATE ${SAN} -g -O1)

foreach(harness fuzz_artifact fuzz_pe_code fuzz_text fuzz_protocols fuzz_stores)
    if(CORTEX_FUZZ_REPLAY)
        add_executable(${harness} ${harness}.cpp replay_main.cpp)
        target_compile_options(${harness} PRIVATE ${SAN} -g -O1)
        target_link_options(${harness} PRIVATE -fsanitize=address,undefined)
    else()
        add_executable(${harness} ${harness}.cpp)
        target_compile_options(${harness} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1)
        target_link_options(${harness} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
    target_link_libraries(${harness} PRIVATE cortex_fuzz_parsers)
endforeach()
target_link_options(cortex_fuzz_parsers INTERFACE $<IF:$<BOOL:${CORTEX_FUZZ_REPLAY}>,-fsanitize=address$<COMMA>undefined,-fsanitize=fuzzer-no-link$<COMMA>address$<COMMA>undefined>)

if(CORTEX_FUZZ_REPLAY)
    enable_testing()
    foreach(harness fuzz_artifact fuzz_pe_code fuzz_text fuzz_protocols fuzz_stores)
        add_test(NAME ${harness}_regressions COMMAND ${harness} ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${harness})
    endforeach()
endif()
