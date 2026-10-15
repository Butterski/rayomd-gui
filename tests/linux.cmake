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

    # rayomd-gui --snapshot on headless compositors (tests/e2e/run_compositor.sh): weston has no
    # xdg-decoration (the app draws its header), sway answers server-side decorations. Each leaves
    # its PNG at e2e/<compositor>/out.png in the build directory.
    find_program(RGUI_PYTHON3 python3)
    foreach(compositor weston sway)
        string(TOUPPER ${compositor} upper)
        find_program(RGUI_${upper} ${compositor})
        if(NOT RGUI_${upper})
            message(STATUS "rayomd-gui-wayland-${compositor} skipped: ${compositor} not found")
        elseif(NOT RGUI_PYTHON3)
            message(STATUS "rayomd-gui-wayland-${compositor} skipped: python3 not found")
        else()
            add_test(NAME rayomd-gui-wayland-${compositor}
                     COMMAND ${CMAKE_SOURCE_DIR}/tests/e2e/run_compositor.sh ${compositor}
                             $<TARGET_FILE:rayomd-gui> ${CMAKE_BINARY_DIR}/e2e/${compositor})
            set_tests_properties(rayomd-gui-wayland-${compositor} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
endif()
