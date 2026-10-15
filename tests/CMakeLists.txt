# SPDX-License-Identifier: Apache-2.0

# The unit tests of every module; each phase adds its files to its own list.
set(RGUI_TEST_SOURCES
    main.cpp
    runner.cpp
    test_build.cpp)

add_executable(rayomd-gui-tests ${RGUI_TEST_SOURCES})
target_link_libraries(rayomd-gui-tests PRIVATE rayomd_gui_core)
target_include_directories(rayomd-gui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rayomd-gui-tests PRIVATE
    RGUI_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RGUI_BUILD_DIR="${CMAKE_BINARY_DIR}")
rgui_flags(rayomd-gui-tests)

add_test(NAME rayomd-gui-unit COMMAND rayomd-gui-tests)
