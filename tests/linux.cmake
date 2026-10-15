# SPDX-License-Identifier: Apache-2.0
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RGUI_TEST_SOURCES
        test_linux_dbus.cpp
        test_linux_omarchy.cpp
        test_linux_loop.cpp
        test_linux_window.cpp
        test_linux_wire.cpp
        test_linux_xkb.cpp)

    # The keymap reader against libxkbcommon's answers (tests/data/xkb, made by
    # scripts/make_xkb_fixtures.sh; no tool needed at test time).
    add_test(NAME rayomd-gui-xkb COMMAND rayomd-gui-tests linux.xkb)

    # linux.dbus_session talks to a real bus: a private one from dbus-run-session.
    find_program(RGUI_DBUS_RUN_SESSION dbus-run-session)
    if(RGUI_DBUS_RUN_SESSION)
        add_test(NAME rayomd-gui-dbus
                 COMMAND ${RGUI_DBUS_RUN_SESSION} -- $<TARGET_FILE:rayomd-gui-tests> linux.dbus)
        set_tests_properties(rayomd-gui-dbus PROPERTIES ENVIRONMENT "RGUI_DBUS_TEST=1" TIMEOUT 60)
    else()
        message(STATUS "rayomd-gui-dbus skipped: dbus-run-session not found")
    endif()
endif()
