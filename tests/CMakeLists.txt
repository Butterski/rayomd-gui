# SPDX-License-Identifier: Apache-2.0

# The unit tests of every module. Each module's tests/<module>.cmake appends its files to
# RGUI_TEST_SOURCES and adds the ctest entries that need an outside tool, only when
# find_program finds the tool (else: message(STATUS "<entry> skipped: <tool> not found")).
set(RGUI_TEST_SOURCES
    main.cpp
    runner.cpp
    test_build.cpp)
foreach(module gfx text pdf ui app linux fuzz)
    include(${CMAKE_CURRENT_SOURCE_DIR}/${module}.cmake)
endforeach()

add_executable(rayomd-gui-tests ${RGUI_TEST_SOURCES})
target_link_libraries(rayomd-gui-tests PRIVATE rayomd_gui_core)
if(TARGET rayomd_gui_linux)
    target_link_libraries(rayomd-gui-tests PRIVATE rayomd_gui_linux)
endif()
target_include_directories(rayomd-gui-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(rayomd-gui-tests PRIVATE
    RGUI_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    RGUI_BUILD_DIR="${CMAKE_BINARY_DIR}")
rgui_flags(rayomd-gui-tests)

add_test(NAME rayomd-gui-unit COMMAND rayomd-gui-tests)
