# SPDX-License-Identifier: Apache-2.0
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RGUI_TEST_SOURCES
        test_linux_loop.cpp
        test_linux_wire.cpp
        test_linux_xkb.cpp)
    # The keymap reader against libxkbcommon's answers (tests/data/xkb, made by
    # scripts/make_xkb_fixtures.sh; no tool needed at test time).
    add_test(NAME rayomd-gui-xkb COMMAND rayomd-gui-tests linux.xkb)
endif()
