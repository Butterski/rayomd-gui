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

# Every platform-independent source; each module keeps its own list.
set(RGUI_APP_SOURCES
    app/version.cpp)

add_library(rayomd_gui_core STATIC
    ${RGUI_APP_SOURCES})
target_include_directories(rayomd_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rayomd_gui_core PUBLIC rayomd::engine)
target_compile_definitions(rayomd_gui_core PUBLIC RAYOMD_GUI_VERSION="${PROJECT_VERSION}")
rgui_flags(rayomd_gui_core)
