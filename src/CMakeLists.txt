# SPDX-License-Identifier: Apache-2.0

# Warnings and release flags for RayoMD GUI's own C, C++ and assembly.
function(rgui_flags target)
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wshadow>"
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
        -ffunction-sections
        -fdata-sections
        "$<$<CONFIG:Release>:-O2>")
endfunction()

# Every platform-independent source. Each module lists its own in <module>/sources.cmake,
# appending paths relative to this directory to RGUI_CORE_SOURCES.
set(RGUI_CORE_SOURCES)
foreach(module gfx text pdf ui app)
    include(${CMAKE_CURRENT_SOURCE_DIR}/${module}/sources.cmake)
endforeach()

add_library(rayomd_gui_core STATIC ${RGUI_CORE_SOURCES})
target_include_directories(rayomd_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rayomd_gui_core PUBLIC rayomd::engine)
target_compile_definitions(rayomd_gui_core PUBLIC RAYOMD_GUI_VERSION="${PROJECT_VERSION}")
rgui_flags(rayomd_gui_core)

# The Linux platform layer (platform/linux/sources.cmake sets RGUI_LINUX_SOURCES): a library the
# executable and the tests both link.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RGUI_LINUX_SOURCES)
    include(${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/sources.cmake)
    if(RGUI_LINUX_SOURCES)
        add_library(rayomd_gui_linux STATIC ${RGUI_LINUX_SOURCES})
        target_link_libraries(rayomd_gui_linux PUBLIC rayomd_gui_core)
        target_compile_definitions(rayomd_gui_linux PRIVATE _GNU_SOURCE)
        rgui_flags(rayomd_gui_linux)
    endif()
endif()
