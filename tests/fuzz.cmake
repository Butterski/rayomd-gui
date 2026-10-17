# SPDX-License-Identifier: Apache-2.0
# The fuzz harnesses, compiled into rayomd-gui-tests for fuzz.regressions, which replays the
# inputs under fuzz/regressions/. The libFuzzer executables are tests/fuzz/CMakeLists.txt.
list(APPEND RGUI_TEST_SOURCES
    test_fuzz_regressions.cpp
    fuzz/harness_common.cpp
    fuzz/fuzz_pdf.cpp
    fuzz/fuzz_jpeg.cpp
    fuzz/fuzz_font.cpp
    fuzz/fuzz_theme.cpp
    fuzz/fuzz_fontconf.cpp
    fuzz/fuzz_settings.cpp
    fuzz/fuzz_markdown.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RGUI_TEST_SOURCES
        fuzz/fuzz_xkb.cpp
        fuzz/fuzz_compose.cpp
        fuzz/fuzz_dbus.cpp
        fuzz/fuzz_wl_wire.cpp)
endif()
