# SPDX-License-Identifier: Apache-2.0

# Warnings and release flags for RayoMD GUI's own C, C++ and assembly. _GLIBCXX_ASSERTIONS
# bounds-checks the standard containers (operator[], front, back, ...) in every configuration,
# for the app's code only: in the engine it cost 7.5 % of BuildPdf on tester.md x70 (13.73 ->
# 14.76 ms), against 2 % of RenderPage here.
function(rgui_flags target)
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wshadow>"
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
        -ffunction-sections
        -fdata-sections
        "$<$<CONFIG:Release>:-O2>")
    target_compile_definitions(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_ASSERTIONS>")
endfunction()

# Every platform-independent source. Each module lists its own in <module>/sources.cmake,
# appending paths relative to this directory to RGUI_CORE_SOURCES.
set(RGUI_CORE_SOURCES)
foreach(module gfx text pdf ui app)
    include(${CMAKE_CURRENT_SOURCE_DIR}/${module}/sources.cmake)
endforeach()

add_library(rayomd_gui_core STATIC ${RGUI_CORE_SOURCES})
target_include_directories(rayomd_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rayomd_gui_core PUBLIC rayomd::engine)
target_compile_definitions(rayomd_gui_core PUBLIC RAYOMD_GUI_VERSION="${PROJECT_VERSION}")
rgui_flags(rayomd_gui_core)
# Win64 keeps the stack 16-byte aligned and GCC cannot align it to 32 for AVX2 code (GCC bug
# 54412): a ymm register spilled with an aligned move would fault. The assembler makes every
# such move unaligned (binutils 2.38 and newer).
if(MINGW)
    set_source_files_properties(gfx/kernels_simd.cpp PROPERTIES COMPILE_OPTIONS "-Wa,-muse-unaligned-vector-move")
endif()

# The Linux platform layer (platform/linux/sources.cmake sets RGUI_LINUX_SOURCES): a library the
# executable and the tests both link.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RGUI_LINUX_SOURCES)
    include(${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/sources.cmake)
    if(RGUI_LINUX_SOURCES)
        add_library(rayomd_gui_linux STATIC ${RGUI_LINUX_SOURCES})
        target_link_libraries(rayomd_gui_linux PUBLIC rayomd_gui_core)
        target_compile_definitions(rayomd_gui_linux PRIVATE _GNU_SOURCE)
        rgui_flags(rayomd_gui_linux)

        # rayomd-gui: glibc only, libstdc++ and libgcc linked in. NEEDED holds libc.so.6 and
        # ld-linux-x86-64.so.2, which libstdc++.a's TLS code references (see AGENTS.md).
        add_executable(rayomd-gui platform/linux/main_linux.cpp)
        target_link_libraries(rayomd-gui PRIVATE rayomd_gui_linux)
        target_compile_definitions(rayomd-gui PRIVATE _GNU_SOURCE)
        target_link_options(rayomd-gui PRIVATE
            "$<$<CONFIG:Release>:-s;-static-libstdc++;-static-libgcc;-Wl,--gc-sections;-Wl,--as-needed>")
        rgui_flags(rayomd-gui)
    endif()
endif()

# The Win32 platform layer (platform/win32/sources.cmake sets RGUI_WIN32_SOURCES): a library the
# executable links. comdlg32, shell32, dwmapi and imm32 are loaded on first use, not imported:
# the --wrap options travel to whatever links this library.
if(WIN32)
    set(RGUI_WIN32_SOURCES)
    include(${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/sources.cmake)
    add_library(rayomd_gui_win32 STATIC ${RGUI_WIN32_SOURCES})
    target_link_libraries(rayomd_gui_win32 PUBLIC rayomd_gui_core user32 gdi32 advapi32)
    target_compile_definitions(rayomd_gui_win32 PUBLIC UNICODE _UNICODE)
    foreach(function ${RGUI_WIN32_DELAY_LOADED})
        target_link_options(rayomd_gui_win32 INTERFACE "-Wl,--wrap=${function}")
    endforeach()
    rgui_flags(rayomd_gui_win32)

    # rayomd-gui.exe: a GUI-subsystem program (wWinMain), static, its icon and manifest from the
    # resource script.
    enable_language(RC)
    add_executable(rayomd-gui WIN32 platform/win32/main_win32.cpp platform/win32/rayomd-gui.rc)
    target_link_libraries(rayomd-gui PRIVATE rayomd_gui_win32)
    target_link_options(rayomd-gui PRIVATE -municode -s -static -Wl,--gc-sections)
    rgui_flags(rayomd-gui)
endif()
