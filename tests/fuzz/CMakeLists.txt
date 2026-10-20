# SPDX-License-Identifier: Apache-2.0
# The libFuzzer executables rgui-fuzz-<harness> and the seed generator rgui-fuzz-seeds (RGUI_FUZZ=ON,
# preset linux-fuzz; the top-level CMakeLists.txt instruments every object for coverage, ASan and
# UBSan). run_fuzzers.sh runs them. The same harness sources are in rayomd-gui-tests (tests/fuzz.cmake).
add_library(rgui_fuzz_harnesses STATIC
    harness_common.cpp
    fuzz_pdf.cpp
    fuzz_jpeg.cpp
    fuzz_font.cpp
    fuzz_theme.cpp
    fuzz_fontconf.cpp
    fuzz_settings.cpp
    fuzz_markdown.cpp
    fuzz_highlight.cpp
    fuzz_xkb.cpp
    fuzz_compose.cpp
    fuzz_dbus.cpp
    fuzz_wl_wire.cpp)
target_link_libraries(rgui_fuzz_harnesses PUBLIC rayomd_gui_linux)
rgui_flags(rgui_fuzz_harnesses)

foreach(entry pdf:FuzzPdf jpeg:FuzzJpeg font:FuzzFont theme:FuzzTheme fontconf:FuzzFontconf settings:FuzzSettings
              markdown:FuzzMarkdown highlight:FuzzHighlight xkb:FuzzXkb compose:FuzzCompose dbus:FuzzDbus
              wl_wire:FuzzWlWire)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 name)
    list(GET parts 1 function)
    add_executable(rgui-fuzz-${name} fuzz_main.cpp)
    target_compile_definitions(rgui-fuzz-${name} PRIVATE RGUI_FUZZ_HARNESS=${function})
    target_link_libraries(rgui-fuzz-${name} PRIVATE rgui_fuzz_harnesses)
    target_link_options(rgui-fuzz-${name} PRIVATE -fsanitize=fuzzer)
    rgui_flags(rgui-fuzz-${name})
endforeach()

add_executable(rgui-fuzz-seeds make_seeds.cpp)
target_link_libraries(rgui-fuzz-seeds PRIVATE rgui_fuzz_harnesses)
rgui_flags(rgui-fuzz-seeds)
