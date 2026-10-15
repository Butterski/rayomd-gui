# SPDX-License-Identifier: Apache-2.0
list(APPEND RGUI_TEST_SOURCES
    test_jpeg.cpp
    test_pdf_golden.cpp
    test_pdf_limits.cpp
    test_pdf_reader.cpp
    test_pdf_render.cpp)

# The JPEG decoder against libjpeg-turbo's djpeg.
find_program(RGUI_DJPEG_PROGRAM djpeg)
if(RGUI_DJPEG_PROGRAM)
    add_test(NAME rayomd-gui-jpeg COMMAND rayomd-gui-tests jpeg.djpeg)
    set_tests_properties(rayomd-gui-jpeg PROPERTIES ENVIRONMENT "RGUI_DJPEG=${RGUI_DJPEG_PROGRAM}")
else()
    message(STATUS "rayomd-gui-jpeg skipped: djpeg not found")
endif()

# The preview renderer against poppler's pdftocairo.
find_program(RGUI_PDFTOCAIRO_PROGRAM pdftocairo)
if(RGUI_PDFTOCAIRO_PROGRAM)
    add_test(NAME rayomd-gui-golden COMMAND rayomd-gui-tests pdf.golden)
    set_tests_properties(rayomd-gui-golden PROPERTIES ENVIRONMENT "RGUI_PDFTOCAIRO=${RGUI_PDFTOCAIRO_PROGRAM}")
else()
    message(STATUS "rayomd-gui-golden skipped: pdftocairo not found")
endif()
