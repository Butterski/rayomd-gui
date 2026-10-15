# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAYOMD_GUI_VERSION LIMIT_COUNT 1)
project(rayomd-gui VERSION ${RAYOMD_GUI_VERSION} LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "rayomd-gui builds with GCC or Clang (MinGW-w64 on Windows)")
endif()
# The target, not the host: a cross-build names no CMAKE_SYSTEM_PROCESSOR.
include(CheckCSourceCompiles)
check_c_source_compiles("#ifndef __x86_64__\n#error not x86-64\n#endif\nint main(void) { return 0; }"
    RGUI_TARGET_X86_64)
if(NOT RGUI_TARGET_X86_64)
    message(FATAL_ERROR "rayomd-gui targets x86-64")
endif()

add_subdirectory(external/rayomd EXCLUDE_FROM_ALL)   # RAYOMD_USE_CURL stays OFF: libc only on Linux
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
