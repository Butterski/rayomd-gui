# SPDX-License-Identifier: Apache-2.0
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RGUI_TEST_SOURCES
        test_linux_dbus.cpp
        test_linux_omarchy.cpp)

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
