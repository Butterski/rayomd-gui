# SPDX-License-Identifier: Apache-2.0
list(APPEND RGUI_TEST_SOURCES
    headless_platform.cpp
    test_app_latency.cpp
    test_app_safefile.cpp
    test_app_model.cpp
    test_app_window.cpp)

# Keystroke -> preview latency in documents of 1 to 700 copies of tester.md (prints p50 / p99
# before and after, and the UI thread's time per keystroke).
add_test(NAME rayomd-gui-bench-latency COMMAND rayomd-gui-tests bench.preview_latency)
