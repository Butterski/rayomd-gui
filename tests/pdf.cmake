# SPDX-License-Identifier: Apache-2.0
list(APPEND RGUI_TEST_SOURCES
    test_jpeg.cpp)

# The JPEG decoder against libjpeg-turbo's djpeg.
find_program(RGUI_DJPEG_PROGRAM djpeg)
if(RGUI_DJPEG_PROGRAM)
    add_test(NAME rayomd-gui-jpeg COMMAND rayomd-gui-tests jpeg.djpeg)
    set_tests_properties(rayomd-gui-jpeg PROPERTIES ENVIRONMENT "RGUI_DJPEG=${RGUI_DJPEG_PROGRAM}")
else()
    message(STATUS "rayomd-gui-jpeg skipped: djpeg not found")
endif()
