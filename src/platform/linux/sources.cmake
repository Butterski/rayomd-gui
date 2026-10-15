# SPDX-License-Identifier: Apache-2.0
# The Linux platform layer, except main_linux.cpp.
list(APPEND RGUI_LINUX_SOURCES
    platform/linux/compose.cpp
    platform/linux/data_transfer.cpp
    platform/linux/dbus_wire.c
    platform/linux/hypr_ipc.c
    platform/linux/omarchy.cpp
    platform/linux/open_external.c
    platform/linux/portal.cpp
    platform/linux/loop.c
    platform/linux/ui_tasks.cpp
    platform/linux/wayland_window.cpp
    platform/linux/wl_protocols.c
    platform/linux/wl_wire.c
    platform/linux/xkb_keymap.cpp)
