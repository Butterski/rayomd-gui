# SPDX-License-Identifier: Apache-2.0
# The Win32 platform layer's own tests, on Windows (rayomd-gui-tests links rayomd_gui_win32 there).
if(WIN32)
    list(APPEND RGUI_TEST_SOURCES
        test_win32_pacing.cpp)
endif()
