# SPDX-License-Identifier: Apache-2.0
# The Linux platform layer, except main_linux.cpp.
list(APPEND RGUI_LINUX_SOURCES
    platform/linux/loop.c
    platform/linux/ui_tasks.cpp
    platform/linux/wayland_window.cpp
    platform/linux/wl_wire.c
    platform/linux/wl_protocols.c)
