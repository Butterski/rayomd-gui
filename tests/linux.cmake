# SPDX-License-Identifier: Apache-2.0
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RGUI_TEST_SOURCES
        test_linux_loop.cpp
        test_linux_wire.cpp)
endif()
