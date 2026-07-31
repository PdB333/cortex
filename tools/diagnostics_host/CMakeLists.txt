cmake_minimum_required(VERSION 3.20)
project(cortex_diag_host_compat LANGUAGES CXX)

message(STATUS "cortex_diag_host was merged into cortex_host; building the unified executable")
add_subdirectory(../unified_host ${CMAKE_BINARY_DIR}/cortex_unified_host)
