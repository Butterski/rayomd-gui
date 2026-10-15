# SPDX-License-Identifier: Apache-2.0
list(APPEND RGUI_CORE_SOURCES
    gfx/canvas.cpp
    gfx/crc32.c
    gfx/kernels.cpp
    gfx/path.cpp
    gfx/png_write.cpp
    gfx/raster.cpp
    gfx/stroke.cpp
    gfx/svgpath.cpp
    gfx/x86_64/accumulate.S
    gfx/x86_64/blend.S
    gfx/x86_64/jpeg.S)
