# SPDX-License-Identifier: Apache-2.0
list(APPEND RGUI_TEST_SOURCES
    test_ui_core.cpp
    test_ui_editor.cpp
    test_ui_editor_bench.cpp
    test_ui_theme.cpp)

# The cost of a keystroke in a 1 MiB document (prints p50 / p99 frame times).
add_test(NAME rayomd-gui-bench COMMAND rayomd-gui-tests bench.typing)
