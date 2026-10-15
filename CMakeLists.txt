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

# Exploit mitigations for every object of the app, the engine compiled into it included: set
# here, before add_subdirectory(external/rayomd), so the directory options reach its targets too.
# CI checks the result (readelf on Linux, objdump on Windows; .github/workflows/ci.yml).
if(WIN32)
    # MinGW-w64: ASLR with 64-bit entropy and DEP (ld sets these by default since binutils 2.36;
    # spelled out so an older linker cannot drop them), stack canaries, checked libc calls.
    add_compile_options(
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fstack-protector-strong;-D_FORTIFY_SOURCE=2>")
    add_link_options(
        "$<$<CONFIG:Release>:-fstack-protector-strong;-Wl,--dynamicbase,--nxcompat,--high-entropy-va>")
else()
    # A position-independent executable. Every library here is static and linked only into
    # executables, so the libraries get PIE code too (-fPIE, not -fPIC: no interposable calls).
    include(CheckPIESupported)
    check_pie_supported(LANGUAGES C CXX)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    foreach(lang C CXX ASM)
        set(CMAKE_${lang}_COMPILE_OPTIONS_PIC ${CMAKE_${lang}_COMPILE_OPTIONS_PIE})
    endforeach()
    # Checked libc calls (level 3: sizes known at run time too), stack canaries, stack-clash
    # probes, Intel CET (IBT and SHSTK; the .S kernels emit their CET note when __CET__ is
    # defined, so -fcf-protection reaches ASM as well); read-only relocations bound at load time
    # and a non-executable stack.
    add_compile_options(
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>"
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fstack-protector-strong;-fstack-clash-protection>"
        "$<$<CONFIG:Release>:-fcf-protection>")
    add_link_options("$<$<CONFIG:Release>:-Wl,-z,relro,-z,now;-Wl,-z,noexecstack>")
endif()

# libFuzzer harnesses of the app's readers of outside data (tests/fuzz, preset linux-fuzz): every
# object, the engine's included, gets coverage instrumentation, ASan and UBSan (any finding aborts).
option(RGUI_FUZZ "Build the libFuzzer harnesses of tests/fuzz (Clang on Linux)" OFF)
if(RGUI_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "RGUI_FUZZ needs Clang on Linux (libFuzzer)")
    endif()
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=fuzzer-no-link,address,undefined;-fno-sanitize-recover=all>"
        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(external/rayomd EXCLUDE_FROM_ALL)   # RAYOMD_USE_CURL stays OFF: libc only on Linux
add_subdirectory(src)
if(RGUI_FUZZ)
    add_subdirectory(tests/fuzz)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
