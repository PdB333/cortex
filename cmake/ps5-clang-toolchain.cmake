# Cross-build Cortex's own agent using an externally installed open PS5 SDK.
# No SDK source or proprietary platform files are vendored in Cortex.
# Example: cmake -S tools/remote -B build/ps5 -G Ninja
#   -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps5-clang-toolchain.cmake
#   -DCORTEX_PS5_SDK=<ps5-payload-sdk> -DCORTEX_LLVM_BIN=<llvm>/bin
#   -DCORTEX_REMOTE_PS5=ON -DBUILD_TESTING=OFF
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 9)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CORTEX_PS5_SDK CORTEX_LLVM_BIN)
if(NOT CORTEX_PS5_SDK OR NOT CORTEX_LLVM_BIN)
    message(FATAL_ERROR "Set CORTEX_PS5_SDK and CORTEX_LLVM_BIN explicitly")
endif()
file(TO_CMAKE_PATH "${CORTEX_PS5_SDK}" SDK)
file(TO_CMAKE_PATH "${CORTEX_LLVM_BIN}" LLVM)
if(CMAKE_HOST_WIN32)
    set(EXE ".exe")
else()
    set(EXE "")
endif()
set(CMAKE_C_COMPILER "${LLVM}/clang${EXE}")
set(CMAKE_CXX_COMPILER "${LLVM}/clang++${EXE}")
set(CMAKE_AR "${LLVM}/llvm-ar${EXE}")
set(CMAKE_RANLIB "${LLVM}/llvm-ranlib${EXE}")
set(CMAKE_STRIP "${LLVM}/llvm-strip${EXE}")
set(CMAKE_C_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_SYSROOT "${SDK}")
set(CMAKE_FIND_ROOT_PATH "${SDK}/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "PS5 agent uses static project libraries" FORCE)
set(COMMON "-fno-stack-protector -fno-plt -femulated-tls -fvisibility-nodllstorageclass=default")
set(CMAKE_C_FLAGS_INIT "${COMMON} -isystem \"${SDK}/target/include\"")
# C++ headers must precede the SDK C headers (libc++ uses include_next).
set(CMAKE_CXX_FLAGS_INIT "${COMMON} -frtti -fexceptions -nostdinc++ -isystem \"${SDK}/target/include/c++/v1\" -isystem \"${SDK}/target/include\"")
# The PS5 Clang driver selects its own linker wrapper and ignores --ld-path.
# Invoke the actual ELF linker directly, using the SDK's external link script.
set(CMAKE_C_LINK_EXECUTABLE "\"${LLVM}/ld.lld${EXE}\" <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "\"${LLVM}/ld.lld${EXE}\" <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L\"${SDK}/target/lib\" \"${SDK}/target/lib/crt1.o\" -m elf_x86_64 -pie -T \"${SDK}/ldscripts/elf_x86_64.x\" --eh-frame-hdr -z max-page-size=0x4000 --hash-style=gnu")
set(CMAKE_C_STANDARD_LIBRARIES "--start-group -lc -lkernel_web -lSceLibcInternal -lSceNet --end-group")
set(CMAKE_CXX_STANDARD_LIBRARIES "--start-group -lc++ -lc++abi -lunwind -lc -lpthread -lkernel_web -lSceLibcInternal -lSceNet --end-group")
